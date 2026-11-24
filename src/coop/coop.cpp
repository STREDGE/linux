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

    net::Config  g_cfg;
    net::Session g_session;
    FLogger*     g_log = nullptr;

    // Состояние второго CJ
    uintptr_t g_remotePed   = 0;
    bool      g_inWorld     = false;
    int       g_blip        = -1;
    CVector   g_shownPos{};
    float     g_shownHeading = 0.0f;
    uint64_t  g_lastFrameUs = 0;
    uint64_t  g_nextSendUs  = 0;

    // Эхо-режим (проверка в одиночку): кольцевой буфер своих состояний
    struct EchoSample { uint64_t t; net::PlayerStatePacket s; };
    EchoSample g_echo[64];
    int        g_echoHead = 0;
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

    void DespawnRemote(const char* reason)
    {
        if(!g_remotePed) return;
        if(g_blip >= 0) { ClearBlip(g_blip); g_blip = -1; }
        if(SlotPed(kRemoteSlot) == g_remotePed)
        {
            if(!g_inWorld) WorldAdd(g_remotePed); // RemovePlayerPed сам вызывает CWorld::Remove
            RemovePlayerPed(kRemoteSlot);
        }
        g_remotePed = 0;
        g_inWorld   = false;
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
        g_inWorld      = true;
        g_shownPos     = pos;
        g_shownHeading = heading;

        g_blip = SetEntityBlip(BLIP_CHAR, GetPedRef(g_remotePed), 0, BLIP_DISPLAY_BOTH, (char*)"");
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
            PhysicalRemoveAndAdd(ped); // перепривязать к секторам карты
            if(mat) MatrixUpdateRW(mat);
            if(Field<uintptr_t>(ped, off::EntRwObject)) EntityUpdateRwFrame(ped);
        }
    }

    void UpdateRemote(const net::RemoteState& rs, uintptr_t localPed, float dt, uint64_t now)
    {
        bool fresh = rs.valid && now - rs.receivedUs < kStaleUs;

        // Игра могла сама удалить персонажа (загрузка, рестарт)
        if(g_remotePed && SlotPed(kRemoteSlot) != g_remotePed)
        {
            g_remotePed = 0; g_inWorld = false; g_blip = -1;
            Log(false, "Игра удалила второго CJ, создам заново");
        }

        if(!fresh) { DespawnRemote("нет данных от друга"); return; }

        const net::PlayerStatePacket& s = rs.pkt;
        float   age    = (float)(now - rs.receivedUs) / 1e6f;
        if(age > 0.25f) age = 0.25f;
        CVector target { s.pos[0] + s.vel[0] * age, s.pos[1] + s.vel[1] * age, s.pos[2] + s.vel[2] * age };

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
        bool wantInWorld = Dist2D(GetPosition(localPed), g_shownPos) < kActiveRadius;
        if(wantInWorld != g_inWorld)
        {
            if(wantInWorld) WorldAdd(g_remotePed); else WorldRemove(g_remotePed);
            g_inWorld = wantInWorld;
        }

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
        return s;
    }

    const net::RemoteState& EchoUpdate(const net::PlayerStatePacket& local, uint64_t now)
    {
        g_echo[g_echoHead] = { now, local };
        g_echoHead = (g_echoHead + 1) % 64;

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

    void OnFrame()
    {
        uint64_t now = net::NowUs();
        float dt = g_lastFrameUs ? (float)(now - g_lastFrameUs) / 1e6f : 0.0f;
        if(dt > 0.25f) dt = 0.25f;
        g_lastFrameUs = now;

        uintptr_t local = SlotPed(0);
        if(!local) return;

        net::PlayerStatePacket me = CaptureLocal(local);

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
        HideRemoteSlot()  : saved(g_remotePed && SlotPed(kRemoteSlot) == g_remotePed ? g_remotePed : 0) { if(saved) SlotPed(kRemoteSlot) = 0; }
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

DECL_HOOKb(IsCoopGameGoingOn)                         { return false; }
DECL_HOOKb(IsPlayerUse2PlayerControls, uintptr_t ped) { (void)ped; return false; }

DECL_HOOKv(GameLogicUpdate)             { HideRemoteSlot h; GameLogicUpdate(); }
DECL_HOOKv(CameraCamControl, uintptr_t self) { HideRemoteSlot h; CameraCamControl(self); }
DECL_HOOKv(PickupsUpdate)               { HideRemoteSlot h; PickupsUpdate(); }
DECL_HOOKv(DarkelUpdate)                { HideRemoteSlot h; DarkelUpdate(); }

// Перед сохранением/загрузкой/рестартом второго CJ убираем,
// чтобы он не попал в сохранение и не остался висящим указателем.
DECL_HOOK(bool, GenericSave, int slot)      { DespawnRemote("сохранение игры"); return GenericSave(slot); }
DECL_HOOK(bool, GenericLoad, bool& ok)      { DespawnRemote("загрузка игры");   return GenericLoad(ok); }
DECL_HOOKv(ShutDownForRestart)              { DespawnRemote("рестарт игры");    ShutDownForRestart(); }

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
