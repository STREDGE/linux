#include "coop.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <math.h>
#include <string.h>

#include "game/gta.h"

using namespace gta;

namespace coop
{
namespace
{
    constexpr int      kRemoteSlot       = 1;          // CWorld::Players[1]
    constexpr uint64_t kSendIntervalUs   = 50000;      // 20 пакетов в секунду
    constexpr uint64_t kStaleUs          = 3000000;    // нет данных 3 с — убрать второго CJ
    constexpr float    kActiveRadius     = 180.0f;     // дальше — CJ скрыт, видна только метка
    constexpr float    kSnapDistance     = 10.0f;      // дальше — мгновенно переносим
    constexpr float    kSmoothRate       = 12.0f;      // скорость сглаживания (1/с)
    constexpr uint64_t kEchoDelayUs      = 1000000;    // эхо-режим: повтор с задержкой 1 с
    constexpr float    kEchoOffset       = 1.5f;       // эхо-режим: сдвиг вбок, м
    constexpr float    kMinMoveRatio     = 0.05f;      // меньше — стоит на месте
    constexpr uint64_t kEchoSampleUs     = 50000;      // эхо-режим: запись 20 раз в секунду (буфер 3,2 с)
    constexpr uint64_t kStatusLogUs      = 15000000;   // раз в 15 с — строка состояния в лог

    net::Config  g_cfg;
    net::Session g_session;
    FLogger*     g_log = nullptr;

    // Состояние второго CJ
    uintptr_t g_remotePed   = 0;
    int       g_remoteRef   = -1;   // хэндл в пуле персонажей — проверка, что объект ещё жив
    const char* volatile g_where = "игра (не код мода)";
    bool      g_inWorld     = false;
    int       g_blip        = -1;
    CVector   g_shownPos{};
    float     g_shownHeading = 0.0f;
    uint64_t  g_lastFrameUs = 0;

    // «Виртуальный джойстик» второго CJ: игра читает его через перехваченные
    // CPad::GetPedWalkLeftRight / GetPedWalkUpDown / GetSprint для пульта 2
    // и сама включает анимации ходьбы, бега и спринта.
    int16_t   g_stickX = 0, g_stickY = 0;
    bool      g_sprint = false;
    uint64_t  g_nextSendUs  = 0;

    // Эхо-режим (проверка в одиночку): кольцевой буфер своих состояний
    struct EchoSample { uint64_t t; net::PlayerStatePacket s; };
    EchoSample g_echo[64];
    int        g_echoHead = 0;
    uint64_t   g_echoNextSampleUs = 0;
    uint64_t   g_nextStatusUs = 0;
    net::RemoteState g_echoRemote;

    void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool error, const char* fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        if(g_log) { if(error) g_log->Error("%s", buf); else g_log->Info("%s", buf); }
    }

    float WrapAngle(float a)
    {
        while(a >  (float)M_PI) a -= 2.0f * (float)M_PI;
        while(a < -(float)M_PI) a += 2.0f * (float)M_PI;
        return a;
    }

    float Dist2D(const CVector& a, const CVector& b)
    {
        float dx = a.x - b.x, dy = a.y - b.y;
        return sqrtf(dx * dx + dy * dy);
    }

    // ---------- Второй CJ ----------

    bool CheckRemoteAlive();

    void DespawnRemote(const char* reason)
    {
        if(!CheckRemoteAlive()) return;
        if(g_blip >= 0) { ClearBlip(g_blip); g_blip = -1; }
        if(SlotPed(kRemoteSlot) == g_remotePed)
        {
            if(!g_inWorld) WorldAdd(g_remotePed); // RemovePlayerPed сам вызывает CWorld::Remove
            RemovePlayerPed(kRemoteSlot);
        }
        g_remotePed = 0;
        g_remoteRef = -1;
        g_inWorld   = false;
        g_stickX = g_stickY = 0; g_sprint = false;
        Log(false, "Второй CJ убран (%s)", reason);
    }

    bool SpawnRemote(const CVector& pos, float heading)
    {
        if(SlotPed(kRemoteSlot))
        {
            Log(true, "Слот игрока 2 уже занят игрой — второй CJ не создан");
            return false;
        }
        SetupPlayerPed(kRemoteSlot);
        g_remotePed = SlotPed(kRemoteSlot);
        if(!g_remotePed)
        {
            Log(true, "SetupPlayerPed(1) не создал персонажа");
            return false;
        }
        // Как скриптовая команда CREATE_PLAYER: помечаем персонажем миссии.
        // Иначе игра считает его случайным прохожим и удаляет при очистке
        // (CPopulation::RemoveAllRandomPeds — например, при выходе из интерьера).
        PedSetCharCreatedBy(g_remotePed, 2);
        g_remoteRef    = GetPedRef(g_remotePed);
        g_inWorld      = true;
        g_shownPos     = pos;
        g_shownHeading = heading;

        g_blip = SetEntityBlip(BLIP_CHAR, g_remoteRef, 0, BLIP_DISPLAY_BOTH, (char*)"");
        if(g_blip >= 0)
        {
            ChangeBlipColour(g_blip, 0x40A0FFFF); // голубой
            ChangeBlipScale(g_blip, 3);
        }
        Log(false, "Второй CJ создан (метка %d)", g_blip);
        return true;
    }

    void PlaceRemote(const CVector& pos, float heading)
    {
        uintptr_t ped = g_remotePed;
        uintptr_t mat = Field<uintptr_t>(ped, off::EntMatrix);
        if(mat) MatrixSetRotateZOnly(mat, heading);
        else    Field<float>(ped, off::EntPlacementHead) = heading;
        SetPosition(ped, pos);

        Field<float>(ped, off::PedRotCurrent) = heading;
        Field<float>(ped, off::PedRotAiming)  = heading;
        Field<CVector>(ped, off::PhysMoveSpeed) = CVector{ 0, 0, 0 };
        Field<CVector>(ped, off::PhysTurnSpeed) = CVector{ 0, 0, 0 };
        Field<float>(ped, off::PedHealth) = 100.0f; // этап 1: друга нельзя убить у нас

        if(g_inWorld)
        {
            // К секторам карты персонажа перепривязывает сама игра (CWorld::Process)
            if(mat) MatrixUpdateRW(mat);
            if(Field<uintptr_t>(ped, off::EntRwObject)) EntityUpdateRwFrame(ped);
        }
    }

    // Игра могла сама удалить персонажа. Тогда забываем его и чистим слот,
    // чтобы ни мод, ни игра не обратились к освобождённой памяти.
    bool CheckRemoteAlive()
    {
        if(!g_remotePed) return false;
        bool inSlot = SlotPed(kRemoteSlot) == g_remotePed;
        bool inPool = GetPedFromRef(g_remoteRef) == g_remotePed;
        if(inSlot && inPool) return true;

        Log(true, "Второй CJ удалён игрой (в слоте: %s, в пуле: %s) — создам заново",
            inSlot ? "да" : "нет", inPool ? "да" : "нет");
        if(inSlot && !inPool) SlotPed(kRemoteSlot) = 0; // висячий указатель — убрать
        g_remotePed = 0; g_remoteRef = -1; g_inWorld = false; g_blip = -1;
        return false;
    }

    void UpdateRemote(const net::RemoteState& rs, uintptr_t localPed, float dt, uint64_t now)
    {
        bool fresh = rs.valid && now - rs.receivedUs < kStaleUs;
        CheckRemoteAlive();

        if(!fresh) { DespawnRemote(g_cfg.role == net::Role::Echo ? "эхо: нет данных" : "нет данных от друга"); return; }

        const net::PlayerStatePacket& s = rs.pkt;
        float   age    = (float)(now - rs.receivedUs) / 1e6f;
        if(age > 0.25f) age = 0.25f;
        CVector target { s.pos[0] + s.vel[0] * age, s.pos[1] + s.vel[1] * age, s.pos[2] + s.vel[2] * age };

        g_where = "создание второго CJ";
        if(!g_remotePed && !SpawnRemote(target, s.heading)) return;

        if(!PedIsAlive(g_remotePed)) { DespawnRemote("персонаж погиб у нас"); return; }

        // Сглаживание: плавно догоняем цель, при большом расхождении — переносим сразу
        if(Dist2D(g_shownPos, target) > kSnapDistance || fabsf(g_shownPos.z - target.z) > kSnapDistance)
        {
            g_shownPos = target;
            g_shownHeading = s.heading;
        }
        else
        {
            float k = 1.0f - expf(-kSmoothRate * dt);
            g_shownPos.x += (target.x - g_shownPos.x) * k;
            g_shownPos.y += (target.y - g_shownPos.y) * k;
            g_shownPos.z += (target.z - g_shownPos.z) * k;
            g_shownHeading = WrapAngle(g_shownHeading + WrapAngle(s.heading - g_shownHeading) * k);
        }

        // Далеко от нас — убираем из мира (там не загружена карта), метка остаётся
        g_where = "добавление/удаление из мира";
        bool wantInWorld = Dist2D(GetPosition(localPed), g_shownPos) < kActiveRadius;
        if(wantInWorld != g_inWorld)
        {
            if(wantInWorld) WorldAdd(g_remotePed); else WorldRemove(g_remotePed);
            g_inWorld = wantInWorld;
        }

        // Джойстик: направление — куда смотрит друг, сила — как сильно он
        // жмёт свой джойстик. Формула обратная PlayerControlZelda:
        // курс = atan2(-x, -y) - TheCamera.Orientation.
        float ratio = (s.flags & net::PSF_IN_VEHICLE) ? 0.0f : s.moveRatio;
        if(ratio > kMinMoveRatio)
        {
            float mag = ratio * off::StickScale;
            if(mag > 127.0f) mag = 127.0f;
            float a = g_shownHeading + Field<float>(TheCamera, off::CamOrientation);
            g_stickX = (int16_t)(-mag * sinf(a));
            g_stickY = (int16_t)(-mag * cosf(a));
            g_sprint = s.moveState >= 6;
        }
        else
        {
            g_stickX = g_stickY = 0;
            g_sprint = false;
        }

        g_where = "перенос второго CJ";
        PlaceRemote(g_shownPos, g_shownHeading);
    }

    net::PlayerStatePacket CaptureLocal(uintptr_t ped)
    {
        net::PlayerStatePacket s{};
        bool      inVeh = InVehicle(ped);
        uintptr_t ent   = inVeh ? Field<uintptr_t>(ped, off::PedVehicle) : ped;

        CVector p = GetPosition(ent);
        CVector v = Field<CVector>(ent, off::PhysMoveSpeed);
        s.pos[0] = p.x; s.pos[1] = p.y; s.pos[2] = p.z;
        s.vel[0] = v.x * 50.0f; s.vel[1] = v.y * 50.0f; s.vel[2] = v.z * 50.0f;
        s.heading = inVeh ? GetMatrixHeading(ent) : Field<float>(ped, off::PedRotCurrent);
        float hp = Field<float>(ped, off::PedHealth);
        s.health = (uint8_t)(hp < 0 ? 0 : hp > 255 ? 255 : hp);
        s.flags  = inVeh ? net::PSF_IN_VEHICLE : 0;
        s.moveState = (uint8_t)Field<int>(ped, off::PedMoveState);
        uintptr_t pd = Field<uintptr_t>(ped, off::PedPlayerData);
        s.moveRatio = (pd && !inVeh) ? Field<float>(pd, off::PlayerDataMoveBlendRatio) : 0.0f;
        return s;
    }

    const net::RemoteState& EchoUpdate(const net::PlayerStatePacket& local, uint64_t now)
    {
        // Пишем с фиксированной частотой, а не каждый кадр: при 60+ FPS
        // 64 кадра — меньше секунды, и задержанного сэмпла не находилось.
        if(now >= g_echoNextSampleUs)
        {
            g_echo[g_echoHead] = { now, local };
            g_echoHead = (g_echoHead + 1) % 64;
            g_echoNextSampleUs = now + kEchoSampleUs;
        }

        // Ищем самый свежий сэмпл старше kEchoDelayUs
        const EchoSample* best = nullptr;
        for(const auto& e : g_echo)
            if(e.t && now - e.t >= kEchoDelayUs && (!best || e.t > best->t)) best = &e;

        if(best)
        {
            g_echoRemote.pkt   = best->s;
            float h = best->s.heading;
            g_echoRemote.pkt.pos[0] += cosf(h) * kEchoOffset; // правее по направлению взгляда
            g_echoRemote.pkt.pos[1] += sinf(h) * kEchoOffset;
            g_echoRemote.receivedUs = now;
            g_echoRemote.valid = true;
        }
        return g_echoRemote;
    }

    void OnFrameImpl();
    void OnFrame()
    {
        OnFrameImpl();
        g_where = "игра (не код мода)";
    }

    void OnFrameImpl()
    {
        g_where = "кадр мода: начало";
        uint64_t now = net::NowUs();
        float dt = g_lastFrameUs ? (float)(now - g_lastFrameUs) / 1e6f : 0.0f;
        if(dt > 0.25f) dt = 0.25f;
        g_lastFrameUs = now;

        static bool firstFrame = true;
        if(firstFrame) { firstFrame = false; Log(false, "Первый игровой кадр — перехват CGame::Process работает"); }

        uintptr_t local = SlotPed(0);
        if(!local)
        {
            if(now >= g_nextStatusUs) { g_nextStatusUs = now + kStatusLogUs; Log(false, "Статус: игрок ещё не загружен"); }
            return;
        }

        net::PlayerStatePacket me = CaptureLocal(local);

        if(now >= g_nextStatusUs)
        {
            g_nextStatusUs = now + kStatusLogUs;
            if(CheckRemoteAlive())
                Log(false, "Статус: второй CJ есть, %s, до него %.0f м, его состояние %d; ты %s "
                           "(движение %d, сила %.2f, скорость %.1f м/с); джойстик 2: %d,%d%s; FPS ~%.0f",
                    g_inWorld ? "в мире" : "далеко (только метка)",
                    Dist2D(GetPosition(local), g_shownPos), Field<int>(g_remotePed, off::PedState),
                    InVehicle(local) ? "в машине" : "пешком", me.moveState, me.moveRatio,
                    sqrtf(me.vel[0] * me.vel[0] + me.vel[1] * me.vel[1]),
                    g_stickX, g_stickY, g_sprint ? " +спринт" : "", dt > 0 ? 1.0f / dt : 0.0f);
            else
                Log(false, "Статус: второго CJ нет, FPS ~%.0f", dt > 0 ? 1.0f / dt : 0.0f);
        }

        if(g_cfg.role == net::Role::Echo)
        {
            UpdateRemote(EchoUpdate(me, now), local, dt, now);
            return;
        }

        g_session.Poll();
        if(now >= g_nextSendUs)
        {
            g_session.SendPlayerState(me);
            g_nextSendUs = now + kSendIntervalUs;
        }
        UpdateRemote(g_session.Remote(), local, dt, now);
    }

    // Временно прячет второго CJ из слота игрока 2, чтобы игра
    // не включала свою логику режима на двоих (камера, «поводок», смерть).
    struct HideRemoteSlot
    {
        uintptr_t saved;
        HideRemoteSlot()  : saved(CheckRemoteAlive() ? g_remotePed : 0) { if(saved) SlotPed(kRemoteSlot) = 0; }
        ~HideRemoteSlot() { if(saved) SlotPed(kRemoteSlot) = saved; }
    };
}

// ---------- Перехваты ----------

DECL_HOOKv(GameProcess)
{
    GameProcess();
    OnFrame();
}

DECL_HOOKv(PadUpdatePads)
{
    PadUpdatePads();
    // Управление второго CJ отключено: иначе он повторял бы касания экрана
    if(g_remotePed) Field<uint16_t>(GetPad(kRemoteSlot), off::PadDisableCtrl) = 1;
}

// Пульт второго игрока: отдаём «виртуальный джойстик» друга
static bool IsRemotePad(uintptr_t pad) { return g_remotePed && pad == GetPad(kRemoteSlot); }
DECL_HOOK(int16_t, PadGetPedWalkLeftRight, uintptr_t self) { return IsRemotePad(self) ? g_stickX : PadGetPedWalkLeftRight(self); }
DECL_HOOK(int16_t, PadGetPedWalkUpDown,    uintptr_t self) { return IsRemotePad(self) ? g_stickY : PadGetPedWalkUpDown(self); }
DECL_HOOK(bool,    PadGetSprint, uintptr_t self, int mode) { return IsRemotePad(self) ? g_sprint : PadGetSprint(self, mode); }

DECL_HOOKb(IsCoopGameGoingOn)                         { return false; }
DECL_HOOKb(IsPlayerUse2PlayerControls, uintptr_t ped) { (void)ped; return false; }

DECL_HOOKv(GameLogicUpdate)             { g_where = "CGameLogic::Update (слот скрыт)"; { HideRemoteSlot h; GameLogicUpdate(); } g_where = "игра (не код мода)"; }
DECL_HOOKv(CameraCamControl, uintptr_t self) { g_where = "CCamera::CamControl (слот скрыт)"; { HideRemoteSlot h; CameraCamControl(self); } g_where = "игра (не код мода)"; }
DECL_HOOKv(PickupsUpdate)               { g_where = "CPickups::Update (слот скрыт)"; { HideRemoteSlot h; PickupsUpdate(); } g_where = "игра (не код мода)"; }
DECL_HOOKv(DarkelUpdate)                { g_where = "CDarkel::Update (слот скрыт)"; { HideRemoteSlot h; DarkelUpdate(); } g_where = "игра (не код мода)"; }

// Перед сохранением/загрузкой/рестартом второго CJ убираем,
// чтобы он не попал в сохранение и не остался висящим указателем.
DECL_HOOK(bool, GenericSave, int slot)      { DespawnRemote("сохранение игры"); return GenericSave(slot); }
DECL_HOOK(bool, GenericLoad, bool& ok)      { DespawnRemote("загрузка игры");   return GenericLoad(ok); }
DECL_HOOKv(ShutDownForRestart)              { DespawnRemote("рестарт игры");    ShutDownForRestart(); }

const char* Where() { return g_where; }

bool Install(void* h, const net::Config& cfg, FLogger* log)
{
    g_cfg = cfg;
    g_log = log;

    if(!gta::Init(h, 0))
    {
        Log(true, "Не все функции игры найдены — кооп выключен");
        return false;
    }

    struct HookDef { const char* sym; void* fn; void** orig; };
    const HookDef hooks[] = {
        { "_ZN5CGame7ProcessEv",                                     (void*)HookOf_GameProcess,                (void**)&GameProcess },
        { "_ZN4CPad10UpdatePadsEv",                                  (void*)HookOf_PadUpdatePads,              (void**)&PadUpdatePads },
        { "_ZN4CPad19GetPedWalkLeftRightEv",                         (void*)HookOf_PadGetPedWalkLeftRight,     (void**)&PadGetPedWalkLeftRight },
        { "_ZN4CPad16GetPedWalkUpDownEv",                            (void*)HookOf_PadGetPedWalkUpDown,        (void**)&PadGetPedWalkUpDown },
        { "_ZN4CPad9GetSprintEi",                                    (void*)HookOf_PadGetSprint,               (void**)&PadGetSprint },
        { "_ZN10CGameLogic17IsCoopGameGoingOnEv",                    (void*)HookOf_IsCoopGameGoingOn,          (void**)&IsCoopGameGoingOn },
        { "_ZN10CGameLogic26IsPlayerUse2PlayerControlsEP10CPlayerPed",(void*)HookOf_IsPlayerUse2PlayerControls, (void**)&IsPlayerUse2PlayerControls },
        { "_ZN10CGameLogic6UpdateEv",                                (void*)HookOf_GameLogicUpdate,            (void**)&GameLogicUpdate },
        { "_ZN7CCamera10CamControlEv",                               (void*)HookOf_CameraCamControl,           (void**)&CameraCamControl },
        { "_ZN8CPickups6UpdateEv",                                   (void*)HookOf_PickupsUpdate,              (void**)&PickupsUpdate },
        { "_ZN7CDarkel6UpdateEv",                                    (void*)HookOf_DarkelUpdate,               (void**)&DarkelUpdate },
        { "_ZN19CGenericGameStorage11GenericSaveEi",                 (void*)HookOf_GenericSave,                (void**)&GenericSave },
        { "_ZN19CGenericGameStorage11GenericLoadERb",                (void*)HookOf_GenericLoad,                (void**)&GenericLoad },
        { "_ZN5CGame18ShutDownForRestartEv",                         (void*)HookOf_ShutDownForRestart,         (void**)&ShutDownForRestart },
    };

    // Сначала проверяем, что все адреса есть, и только потом ставим перехваты
    for(const auto& d : hooks)
    {
        if(!aml->GetSym(h, d.sym))
        {
            Log(true, "Не найден символ для перехвата: %s — кооп выключен", d.sym);
            return false;
        }
    }
    for(const auto& d : hooks)
        aml->Hook((void*)aml->GetSym(h, d.sym), d.fn, d.orig);

    if(cfg.role == net::Role::Echo)
    {
        Log(false, "Режим ЭХО: второй CJ будет повторять твои движения с задержкой 1 с (проверка без друга)");
    }
    else if(!g_session.Start(cfg, log))
    {
        Log(true, "Сеть не запустилась — второй CJ не появится");
    }

    Log(false, "Этап 1 установлен: перехватов %d", (int)(sizeof(hooks) / sizeof(hooks[0])));
    return true;
}

} // namespace coop
