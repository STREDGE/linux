#include "coop.h"
#include "trace/ctrace.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <math.h>
#include <string.h>

#include <mutex>
#include <vector>

#include "game/gta.h"
#include "voice/voice.h"
#include "net/sacoop_api.h"
#include "coop/sync.h"
#include "coop/world.h"
#include "coop/crowd.h"
#include "coop/mission.h"
#include "net/tailscale.h"
#include <stdarg.h>
#include <mod/interface.h>

using namespace gta;

namespace coop
{
namespace
{
    constexpr int      kRemoteSlot       = 1;          // CWorld::Players[1]
    constexpr uint64_t kSendIntervalUs   = 50000;      // 20 пакетов в секунду
    constexpr uint64_t kStaleUs          = 60000000;   // нет данных 60 с — убрать второго CJ (пока друг свернул игру, он «замирает»)
    constexpr float    kActiveRadius     = 180.0f;     // дальше — CJ скрыт, видна только метка
    constexpr float    kSnapDistance     = 10.0f;      // дальше — мгновенно переносим
    constexpr float    kSmoothRate       = 12.0f;      // скорость сглаживания (1/с)
    constexpr uint64_t kEchoDelayUs      = 500000;     // эхо-режим: повтор с задержкой 0,5 с
    constexpr float    kEchoOffset       = 3.5f;       // эхо-режим: постоянный сдвиг на восток, м (машина двойника не наезжает на твою)
    constexpr float    kMinMoveRatio     = 0.05f;      // меньше — стоит на месте
    constexpr uint64_t kEchoSampleUs     = 50000;      // эхо-режим: запись 20 раз в секунду (буфер 3,2 с)
    constexpr uint64_t kStatusLogUs      = 15000000;   // раз в 15 с — строка состояния в лог

    net::Config  g_cfg{ net::Role::Off };
    net::Session g_session;

    // Смена режима из меню: кладём сюда, применяем в игровом кадре
    std::mutex   g_cfgMutex;
    net::Config  g_pendingCfg;
    bool         g_hasPending = false;
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
    uint8_t*  g_userPause = nullptr;   // CTimer::m_UserPause
    uint8_t*  g_cutsceneRunning = nullptr; // CCutsceneMgr::ms_running
    uint64_t  g_localSinceUs = 0;          // с какого момента игрок в игре (без заставки)
    constexpr uint64_t kSpawnDelayUs = 3000000; // 3 с
    bool      g_localTalking = false;  // кнопка микрофона зажата
    bool      g_extTalking = false;    // зажата кнопка «Говорить» другого мода (SA Living, API v2)
    bool      g_talkOwned = false;     // другой мод рисует свою кнопку — золотую прячем
    bool      g_lockLocal = false;     // редактор раскладки: управление CJ выключено
    bool      g_friendlyFire = false;  // свой тумблер «огонь по своим»
    bool      g_sharedWanted = true;   // свой тумблер «общий розыск»
    uint64_t  g_lastGameFrameUs = 0;   // когда последний раз шёл игровой кадр (иначе — меню, сеть из MenuTick)
    struct GiveReq { bool toFriend; int weapon, ammo; };
    std::vector<GiveReq> g_giveQueue;   // выдача оружия из меню (под g_cfgMutex)
    std::vector<net::Event> g_echoEvents;
    bool      g_echoAnnounced = false;

    // «Виртуальный джойстик» второго CJ: игра читает его через перехваченные
    // CPad::GetPedWalkLeftRight / GetPedWalkUpDown / GetSprint для пульта 2
    // и сама включает анимации ходьбы, бега и спринта.
    int16_t   g_stickX = 0, g_stickY = 0;
    bool      g_sprint = false;
    bool      (*g_origGetSprint)(uintptr_t pad, int mode) = nullptr; // оригинал CPad::GetSprint

    // Прыжки: свои считаем в перехвате CPad::JumpJustDown (пульт 1),
    // а второму CJ «нажимаем» прыжок, когда счётчик друга изменился.
    uint32_t  g_frameNo = 0;
    uint32_t  g_lastLocalJumpFrame = 0;
    uint8_t   g_localJumpCount = 0;
    bool      g_haveRemoteJumpCount = false;
    uint8_t   g_remoteJumpCount = 0;
    bool      g_pendingJump = false;

    // Проверка формулы джойстика: куда игра повела второго CJ (m_fAimingRotation,
    // посчитан из нашего джойстика) против того, куда мы хотели.
    float     g_intendedHeading = 0.0f;
    bool      g_stickActive = false;
    float     g_headingErrSum = 0.0f;
    int       g_headingErrCount = 0;
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
        ctrace::FromLog(error, buf);
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
        sync::BeforeRemoteDespawn(g_remotePed);   // высадить из машины, если сидит
        if(SlotPed(kRemoteSlot) == g_remotePed)
        {
            if(!g_inWorld) WorldAdd(g_remotePed); // RemovePlayerPed сам вызывает CWorld::Remove
            RemovePlayerPed(kRemoteSlot);
        }
        g_remotePed = 0;
        g_remoteRef = -1;
        g_inWorld   = false;
        g_stickX = g_stickY = 0; g_sprint = false;
        g_pendingJump = false; g_haveRemoteJumpCount = false;
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

    void PlaceRemote(const CVector& pos, float heading, float health)
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
        Field<float>(ped, off::PedHealth) = health < 1.0f ? 1.0f : health; // здоровье — как у друга (урон считает его телефон)

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
        if(inPool && SlotPed(kRemoteSlot) == 0)
        {   // персонаж жив, слот на миг опустел (кто-то спрятал) — возвращаем, а не создаём второго
            SlotPed(kRemoteSlot) = g_remotePed;
            return true;
        }

        Log(true, "Второй CJ удалён игрой (в слоте: %s, в пуле: %s) — создам заново",
            inSlot ? "да" : "нет", inPool ? "да" : "нет");
        if(inSlot && !inPool) SlotPed(kRemoteSlot) = 0; // висячий указатель — убрать
        g_remotePed = 0; g_remoteRef = -1; g_inWorld = false; g_blip = -1;
        return false;
    }

    void UpdateRemote(const net::RemoteState& rs, uintptr_t localPed, float dt, uint64_t now)
    {
        bool fresh = rs.valid && net::Since(now, rs.receivedUs) < kStaleUs;
        CheckRemoteAlive();

        if(!fresh) { DespawnRemote(g_cfg.role == net::Role::Echo ? "эхо: нет данных" : "нет данных от друга"); return; }

        // Во время заставки второго CJ нет (заставка сама расставляет персонажей)
        if(g_cutsceneRunning && *g_cutsceneRunning) { DespawnRemote("заставка"); return; }
        // Не создавать сразу после загрузки/новой игры: модель CJ ещё собирается,
        // и копия получилась бы невидимой. Ждём, пока игрок пробудет в игре пару секунд.
        if(!g_remotePed && net::Since(now, g_localSinceUs) < kSpawnDelayUs) return;

        const net::PlayerStatePacket& s = rs.pkt;
        float   age    = (float)net::Since(now, rs.receivedUs) / 1e6f;
        bool    frozen = age > 0.6f;            // друг свернул игру / пауза: стоит на месте, не идёт
        if(age > 0.25f) age = 0.25f;
        net::PlayerStatePacket sf = s;
        if(frozen) { sf.vel[0] = sf.vel[1] = sf.vel[2] = 0; sf.moveRatio = 0; sf.flags &= ~net::PSF_SPRINT; }
        CVector target { sf.pos[0] + sf.vel[0] * age, sf.pos[1] + sf.vel[1] * age, sf.pos[2] + sf.vel[2] * age };

        g_where = "создание второго CJ";
        if(!g_remotePed && !SpawnRemote(target, s.heading)) return;
        Field<uint8_t>(g_remotePed, off::EntArea) = s.area;   // сразу в том же интерьере, что друг

        // Смерть: друг погиб — второй CJ лежит (уложит sync); друг ожил — создаём заново
        bool friendDead = (s.flags & net::PSF_DEAD) != 0;
        if(!PedIsAlive(g_remotePed))
        {
            if(friendDead) return;
            DespawnRemote("друг снова в игре после смерти");
            return;
        }
        if(friendDead) return;   // sync уложит его в этом же кадре

        // Второго CJ скрутили у нас (так быть не должно) — пересоздать, иначе зависнет.
        // Только пешком и дольше 3 с: в машине состояние 63 игра ставит водителю, когда к нему
        // садится пассажир, — это не арест (0.8.5 из-за этого зря пересоздавала второго CJ).
        {
            static uint64_t cuffedSinceUs = 0;
            bool cuffed = Field<int>(g_remotePed, off::PedState) == 63 && !InVehicle(g_remotePed) && !sync::RemoteBusy();
            if(!cuffed) cuffedSinceUs = 0;
            else if(!cuffedSinceUs) cuffedSinceUs = now;
            else if(now - cuffedSinceUs > 3000000) { cuffedSinceUs = 0; DespawnRemote("скручен полицией у нас — пересоздаю"); return; }
        }

        // Интерьер: второго CJ видно, только если вы в одном здании (игра сама не рисует чужую зону)
        Field<uint8_t>(g_remotePed, off::EntArea) = s.area;

        // В машине / садится / выходит — им управляет sync (задачи игры), не трогаем
        if(sync::RemoteBusy())
        {
            g_shownPos = GetPosition(g_remotePed);
            g_shownHeading = Field<float>(g_remotePed, off::PedRotCurrent);
            g_stickX = g_stickY = 0; g_sprint = false; g_stickActive = false;
            if(!g_inWorld) { WorldAdd(g_remotePed); g_inWorld = true; }
            g_haveRemoteJumpCount = false;
            return;
        }

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
        if(g_stickActive && g_inWorld)
        {
            float err = WrapAngle(Field<float>(g_remotePed, off::PedRotAiming) - g_intendedHeading);
            g_headingErrSum += fabsf(err) * 57.2958f;
            ++g_headingErrCount;
        }

        float ratio = (sf.flags & net::PSF_IN_VEHICLE) ? 0.0f : sf.moveRatio;
        if(ratio > kMinMoveRatio)
        {
            float mag = ratio * off::StickScale;
            if(mag > 127.0f) mag = 127.0f;
            float a = g_shownHeading + Field<float>(TheCamera, off::CamOrientation);
            g_stickX = (int16_t)(-mag * sinf(a));
            g_stickY = (int16_t)(-mag * cosf(a));
            g_sprint = (sf.flags & net::PSF_SPRINT) != 0;
            g_intendedHeading = g_shownHeading;
            g_stickActive = true;
        }
        else
        {
            g_stickX = g_stickY = 0;
            g_sprint = false;
            g_stickActive = false;
        }

        if(!g_haveRemoteJumpCount) { g_haveRemoteJumpCount = true; g_remoteJumpCount = s.jumpCount; }
        else if(s.jumpCount != g_remoteJumpCount)
        {
            g_remoteJumpCount = s.jumpCount;
            if(!(s.flags & net::PSF_IN_VEHICLE)) g_pendingJump = true;
        }

        g_where = "перенос второго CJ";
        PlaceRemote(g_shownPos, g_shownHeading, (float)s.health);
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
        float ar = Field<float>(ped, off::PedArmour);
        s.armour = (uint8_t)(ar < 0 ? 0 : ar > 255 ? 255 : ar);
        s.flags  = inVeh ? net::PSF_IN_VEHICLE : 0;
        if(g_localTalking) s.flags |= net::PSF_TALKING;
        s.moveState = (uint8_t)Field<int>(ped, off::PedMoveState);
        s.jumpCount = g_localJumpCount;
        if(!inVeh && g_origGetSprint && g_origGetSprint(GetPad(0), 0)) s.flags |= net::PSF_SPRINT;
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

        // «Двойник ждёт в машине»: ты вышел из-за руля — двойник не повторяет выход, а остаётся
        // за рулём своей машины, чтобы можно было сесть к нему пассажиром. Отпускаем, когда ты
        // снова сел за руль (своей машины или его), отошёл дальше 60 м или прошло 2 минуты.
        static bool     hold = false;
        static uint64_t holdSinceUs = 0;
        static net::PlayerStatePacket held{}, prev{};
        static bool     havePrev = false;
        if(best)
        {
            const net::PlayerStatePacket& s = best->s;
            bool prevDriving = havePrev && prev.vehId && prev.seat == 0;
            bool nowOnFoot   = s.seat == net::kNoSeat;
            if(!hold && prevDriving && nowOnFoot && !(s.flags & net::PSF_DEAD))
            {
                hold = true; held = prev; holdSinceUs = now;
                held.vel[0] = held.vel[1] = held.vel[2] = 0; held.moveRatio = 0;
                Log(false, "Эхо: двойник остаётся за рулём — садись к нему пассажиром (кнопка «сесть» у его машины)");
            }
            if(hold)
            {
                float dx = local.pos[0] - (held.pos[0] + kEchoOffset), dy = local.pos[1] - held.pos[1];
                bool far = dx * dx + dy * dy > 60.0f * 60.0f;
                bool driving = local.seat == 0;
                if(far || driving || (local.flags & net::PSF_DEAD) || now - holdSinceUs > 120000000ull)
                {
                    hold = false;
                    Log(false, "Эхо: двойник снова повторяет за тобой");
                }
            }
            prev = s; havePrev = true;
            g_echoRemote.pkt   = hold ? held : s;
            g_echoRemote.pkt.pos[0] += kEchoOffset; // всегда на восток — траектория повторяется точно
            g_echoRemote.receivedUs = now;
            g_echoRemote.valid = true;
        }
        return g_echoRemote;
    }

    // ---------- встроенный Tailscale (точка хоста, поиск по ID) ----------
    constexpr int kTsPort = 7777;          // порт точки в сети Tailscale
    bool     g_tsHost = false;             // хост: точку держим открытой
    int      g_tsHostBridge = 0;
    bool     g_tsClientWait = false;       // друг: ищем точку хоста по ID
    char     g_tsClientId[8] = "";
    uint64_t g_tsNextTryUs = 0;
    char     g_tsInfo[192] = "";           // что сейчас происходит (для меню)
    int      g_tsLastFind = -9;
    ts::State g_tsLastState = ts::State::Off;

    bool IsPointId(const char* s)
    {
        if(!s || strlen(s) != 6) return false;
        for(int i = 0; i < 6; ++i) if(s[i] < '0' || s[i] > '9') return false;
        return true;
    }
    void TsInfo(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    void TsInfo(const char* fmt, ...)
    {
        va_list a; va_start(a, fmt); vsnprintf(g_tsInfo, sizeof(g_tsInfo), fmt, a); va_end(a);
    }

    void TsTick(uint64_t now)
    {
        if(!g_tsHost && !g_tsClientWait) return;
        if(now < g_tsNextTryUs) return;
        g_tsNextTryUs = now + 500000;
        ts::Status st = ts::GetStatus();
        if(st.state != g_tsLastState)
        {
            g_tsLastState = st.state;
            Log(st.state == ts::State::Error, "[TAILSCALE] %s%s%s", ts::StateText(st.state), st.error[0] ? ": " : "", st.error);
            if(st.state == ts::State::Running) Log(false, "[TAILSCALE] Мой адрес в сети: %s", st.ip);
        }
        if(st.state != ts::State::Running)
        {
            if(st.state == ts::State::NeedsLogin) TsInfo("Войди в Tailscale (один раз) — кнопка «Войти»");
            else if(st.state == ts::State::NeedsApproval) TsInfo("Телефон ждёт одобрения в аккаунте Tailscale (сайт → Machines)");
            else if(st.state == ts::State::Error) TsInfo("Tailscale: %s", st.error);
            else TsInfo("Tailscale %s", ts::StateText(st.state));
            return;
        }
        if(g_tsHost && !g_tsHostBridge)
        {
            int p = ts::OpenBridge(kTsPort, g_cfg.port);
            if(p > 0)
            {
                g_tsHostBridge = p;
                Log(false, "[TAILSCALE] Точка открыта: ID %s (%s:%d) → игра 127.0.0.1:%u", g_cfg.myId, st.ip, kTsPort, (unsigned)g_cfg.port);
                TsInfo("Точка открыта — жду друга");
            }
            else if(p < 0) TsInfo("Не удалось открыть точку: %s", st.error);
        }
        else if(g_tsHost) TsInfo(g_session.Connected() ? "Друг в лобби" : "Точка открыта — жду друга");
        if(g_tsClientWait)
        {
            char ip[64] = "";
            int r = ts::FindHost(g_tsClientId, ip, sizeof(ip));
            if(r != g_tsLastFind)
            {
                g_tsLastFind = r;
                Log(false, "[TAILSCALE] Точка %s: %s%s", g_tsClientId,
                    r == 1 ? "в сети, адрес " : r == 2 ? "есть, но выключена" : r == 0 ? "нет в аккаунте" : "сеть не готова", r == 1 ? ip : "");
            }
            if(r == 1)
            {
                int p = ts::OpenBridge(kTsPort, 0);
                if(p > 0)
                {
                    ts::SetPeer(ip, kTsPort);
                    net::Config c = g_cfg;
                    snprintf(c.hostAddress, sizeof(c.hostAddress), "127.0.0.1");
                    c.port = (uint16_t)p; c.relay[0] = 0;
                    if(g_session.Start(c, g_log)) TsInfo("Нашёл точку %s — стучусь к хосту", g_tsClientId);
                    else TsInfo("Сеть не запустилась");
                    g_tsClientWait = false;
                }
            }
            else if(r == 2) { TsInfo("Точка %s выключена — пусть хост нажмёт «Открыть точку»", g_tsClientId); g_tsNextTryUs = now + 2000000; }
            else if(r == 0) { TsInfo("Точки %s нет. Вы оба должны войти в один аккаунт Tailscale", g_tsClientId); g_tsNextTryUs = now + 2000000; }
        }
    }

    void ApplyPendingConfig()
    {
        net::Config c;
        {
            std::lock_guard<std::mutex> lock(g_cfgMutex);
            if(!g_hasPending) return;
            c = g_pendingCfg;
            g_hasPending = false;
        }

        DespawnRemote("смена режима");
        sync::Reset("смена режима");
        crowd::Reset("смена режима"); mission::Reset("смена режима");
        world::OnSessionReset();
        g_session.Stop();
        voice::Stop();
        memset(g_echo, 0, sizeof(g_echo));
        g_echoHead = 0;
        g_echoRemote = net::RemoteState();
        g_echoAnnounced = false;
        g_cfg = c;
        g_tsHost = false; g_tsHostBridge = 0; g_tsClientWait = false; g_tsLastFind = -9; g_tsInfo[0] = 0;
        ts::CloseBridge();

        switch(c.role)
        {
            case net::Role::Off:
                Log(false, "Режим: выключено");
                break;
            case net::Role::Echo:
                Log(false, "Режим: проверка в одиночку — второй CJ повторяет тебя через 0,5 с, на 3,5 м восточнее (его машина — в 6 м справа от твоей)");
                break;
            case net::Role::Host:
            case net::Role::Client:
                Log(false, "Режим: %s", c.role == net::Role::Host ? "хост" : "подключение к хосту");
                if(c.role == net::Role::Client && !c.relay[0] && IsPointId(c.hostAddress))
                {   // друг ввёл ID точки: сначала встроенный Tailscale находит хоста
                    snprintf(g_tsClientId, sizeof(g_tsClientId), "%s", c.hostAddress);
                    g_tsClientWait = true; g_tsNextTryUs = 0;
                    ts::Start(c.myId);
                    TsInfo("Включаю Tailscale…");
                    Log(false, "[TAILSCALE] Ищу точку хоста %s (мой ID %s)", g_tsClientId, c.myId);
                    break;
                }
                if(!g_session.Start(c, g_log)) Log(true, "Сеть не запустилась — второй CJ не появится");
                if(c.role == net::Role::Host && !c.relay[0])
                {   // хост: открыть точку во встроенном Tailscale (обычный адрес тоже работает)
                    g_tsHost = true; g_tsNextTryUs = 0;
                    ts::Start(c.myId);
                    TsInfo("Включаю Tailscale…");
                }
                break;
        }
    }

    // Голос: зажата кнопка и друг на связи — пишем микрофон и шлём; пришедшее — в динамик
    void PumpVoice()
    {
        voice::SetLoopback(false, 0);
        voice::SetTransmit((g_localTalking || g_extTalking) && g_session.Connected());
        voice::Pump();
        uint8_t buf[net::kVoiceMaxBytes]; uint16_t seq; uint8_t spurt;
        while(int n = voice::PopEncodedFrame(buf, sizeof(buf), &seq, &spurt))
            g_session.SendVoice(seq, spurt, buf, n);
    }

    // ---------- подробный лог коопа (ctrace) ----------
    const char* RoleName(net::Role r)
    {
        switch(r) { case net::Role::Off: return "выкл"; case net::Role::Echo: return "эхо"; case net::Role::Host: return "хост"; case net::Role::Client: return "друг"; }
        return "?";
    }
    const char* LinkName(net::Link l)
    {
        switch(l)
        {
            case net::Link::Idle: return "ждёт друга"; case net::Link::Requesting: return "стучится к хосту";
            case net::Link::Pending: return "запрос ждёт ответа"; case net::Link::Connected: return "принят"; case net::Link::Denied: return "отказ";
        }
        return "?";
    }
    const char* PktName(int t)
    {
        static const char* n[] = { "?", "PING", "PONG", "STATE", "HELLO", "ACCEPT", "DENY", "VOICE", "MOD", "VEH", "VEH_GONE", "SHOT",
                                   "REL", "ACK", "WORLD_REQ", "WORLD_CHUNK", "WORLD_ACK", "PEDS", "TIME", "BLIPS", "LOBBY" };
        return t >= 0 && t < (int)(sizeof(n) / sizeof(n[0])) ? n[t] : "?";
    }

    struct TraceState
    {
        int       link = -1, connected = -1, role = -1, running = -1, tsState = -1;
        uint64_t  nextPlayerUs = 0, nextNetUs = 0, nextPerfUs = 0, lastFrameUs = 0, worstUs = 0, prevStatsUs = 0;
        int       frames = 0;
        net::Session::Stats prev;
        int       remoteSpawned = -1, inWorld = -1;
    };
    TraceState g_tr;

    template<size_t N> uint32_t Delta(const uint32_t (&cur)[N], const uint32_t (&prev)[N], size_t i) { return cur[i] >= prev[i] ? cur[i] - prev[i] : cur[i]; }

    // Связь: смена этапа — сразу; счётчики пакетов, пинг, потери — раз в 5 с. Зовётся и из игры, и из меню.
    void TraceNet(uint64_t now)
    {
        if(!ctrace::Started()) return;
        int link = (int)g_session.GetLink(), conn = g_session.Connected() ? 1 : 0, role = (int)g_cfg.role, run = g_session.Running() ? 1 : 0;
        // состояние Tailscale — раз в секунду (запрос уходит в Go, каждый кадр дорого)
        static ts::Status tst; static uint64_t nextTsUs = 0;
        if(now >= nextTsUs) { nextTsUs = now + 1000000; tst = ts::GetStatus(); }
        int tsState = (int)tst.state;
        if(link != g_tr.link || conn != g_tr.connected || role != g_tr.role || run != g_tr.running || tsState != g_tr.tsState)
        {
            CTRACE("СВЯЗЬ", "роль %s, сокет %s, этап «%s», на связи %s, друг «%s», посредник %s%s; Tailscale: %s%s%s, адрес %s; пинг %d мс",
                   RoleName(g_cfg.role), run ? "открыт" : "закрыт", LinkName(g_session.GetLink()), conn ? "ДА" : "нет", g_session.PeerNick(),
                   g_session.UsingRelay() ? "да, комната " : "нет", g_session.UsingRelay() ? g_session.RoomCode() : "",
                   ts::StateText(tst.state), tst.error[0] ? " — " : "", tst.error, tst.ip[0] ? tst.ip : "-", g_session.LastRttMs());
            g_tr.link = link; g_tr.connected = conn; g_tr.role = role; g_tr.running = run; g_tr.tsState = tsState;
        }
        if(now < g_tr.nextNetUs) return;
        g_tr.nextNetUs = now + 5000000;
        const net::Session::Stats& st = g_session.GetStats();
        if(!run) { g_tr.prev = st; g_tr.prevStatsUs = now; return; }
        double sec = g_tr.prevStatsUs ? net::Since(now, g_tr.prevStatsUs) / 1e6 : 5.0;
        if(sec <= 0) sec = 5.0;
        char out[700] = "", in[700] = "";
        size_t lo = 0, li = 0;
        uint32_t bOut = 0, bIn = 0;
        for(size_t t = 0; t < 32; ++t)
        {
            uint32_t po = Delta(st.sentPk, g_tr.prev.sentPk, t), bo = Delta(st.sentB, g_tr.prev.sentB, t);
            uint32_t pi = Delta(st.recvPk, g_tr.prev.recvPk, t), bi = Delta(st.recvB, g_tr.prev.recvB, t);
            bOut += bo; bIn += bi;
            if(po && lo < sizeof(out) - 40) lo += snprintf(out + lo, sizeof(out) - lo, " %s:%u", PktName((int)t), po);
            if(pi && li < sizeof(in) - 40)  li += snprintf(in + li, sizeof(in) - li, " %s:%u", PktName((int)t), pi);
        }
        char mods[300] = ""; size_t lm = 0;
        for(size_t ch = 0; ch < 256; ++ch)
        {
            uint32_t mo = Delta(st.modOut, g_tr.prev.modOut, ch), mi = Delta(st.modIn, g_tr.prev.modIn, ch);
            if((mo || mi) && lm < sizeof(mods) - 40) lm += snprintf(mods + lm, sizeof(mods) - lm, " канал %u: >%u <%u", (unsigned)ch, mo, mi);
        }
        uint32_t pings = st.pingSent >= g_tr.prev.pingSent ? st.pingSent - g_tr.prev.pingSent : st.pingSent;
        uint32_t pongs = st.pongRecv >= g_tr.prev.pongRecv ? st.pongRecv - g_tr.prev.pongRecv : st.pongRecv;
        uint64_t lastRecv = g_session.LastRecvUs();
        CTRACE("СЕТЬ", "за %.1f с: пинг %d мс, ответов на пинг %u из %u (потеря %.0f%%), отправлено %.1f КБ/с:%s | принято %.1f КБ/с:%s | "
                       "надёжные: отпр %u, прин %u, повторов %u, дублей %u, выброшено %u, в очереди %d | последний пакет %.1f с назад%s%s%s",
               sec, g_session.LastRttMs(), pongs, pings, pings ? 100.0 * (pings > pongs ? pings - pongs : 0) / pings : 0.0,
               bOut / 1024.0 / sec, out[0] ? out : " —", bIn / 1024.0 / sec, in[0] ? in : " —",
               st.relSent - (st.relSent >= g_tr.prev.relSent ? g_tr.prev.relSent : 0), st.relRecv - (st.relRecv >= g_tr.prev.relRecv ? g_tr.prev.relRecv : 0),
               st.relResend - (st.relResend >= g_tr.prev.relResend ? g_tr.prev.relResend : 0), st.relDup - (st.relDup >= g_tr.prev.relDup ? g_tr.prev.relDup : 0),
               st.relDropped - (st.relDropped >= g_tr.prev.relDropped ? g_tr.prev.relDropped : 0), g_session.ReliableQueue(),
               lastRecv ? net::Since(now, lastRecv) / 1e6 : -1.0, mods[0] ? " | SACoopNet:" : "", mods, st.badVersion ? " | ЕСТЬ ПАКЕТЫ ДРУГОЙ ВЕРСИИ ПРОТОКОЛА" : "");
        g_tr.prev = st; g_tr.prevStatsUs = now;
    }

    // Кадры: подвисания сразу, FPS раз в 5 с
    void TracePerf(uint64_t now)
    {
        if(!ctrace::Started()) return;
        if(g_tr.lastFrameUs)
        {
            uint64_t d = net::Since(now, g_tr.lastFrameUs);
            if(d > g_tr.worstUs) g_tr.worstUs = d;
            if(d > 150000) CTRACE("ПОДВИСАНИЕ", "кадр %.0f мс", d / 1000.0);
        }
        g_tr.lastFrameUs = now; ++g_tr.frames;
        if(now >= g_tr.nextPerfUs)
        {
            if(g_tr.nextPerfUs)
                CTRACE("FPS", "%.0f кадр/с (худший кадр %.0f мс), общих машин %d, общий мир людей %s, второй CJ %s",
                       g_tr.frames / 5.0, g_tr.worstUs / 1000.0, sync::TrackedVehicles(), crowd::Active() ? "включён" : "нет",
                       g_remotePed ? (g_inWorld ? "в мире" : "далеко (метка)") : "нет");
            g_tr.nextPerfUs = now + 5000000; g_tr.frames = 0; g_tr.worstUs = 0;
        }
    }

    // Оба игрока 4 раза в секунду: свой CJ (что отправлено) и друг (что пришло и где стоит его копия)
    void TracePlayers(uint64_t now, uintptr_t local, const net::PlayerStatePacket& me, const net::RemoteState* rs)
    {
        if(!ctrace::Started() || now < g_tr.nextPlayerUs) return;
        g_tr.nextPlayerUs = now + 250000;
        CVector lp = GetPosition(local);
        float spd = sqrtf(me.vel[0] * me.vel[0] + me.vel[1] * me.vel[1] + me.vel[2] * me.vel[2]);
        char veh[96] = "";
        if(me.seat != net::kNoSeat || me.vehId)
        {
            uintptr_t v = InVehicle(local) ? Field<uintptr_t>(local, off::PedVehicle) : 0;
            snprintf(veh, sizeof(veh), " машина=%04X м=%d место=%d садится=%d hpм=%.0f", (unsigned)me.vehId, v ? ModelOf(v) : 0,
                     me.seat == net::kNoSeat ? -1 : (int)me.seat, me.enterSeat == net::kNoSeat ? -1 : (int)me.enterSeat, v ? Field<float>(v, off::VehHealth) : 0.0f);
        }
        bool padOff = Field<uint16_t>(GetPad(0), off::PadDisableCtrl) != 0;
        bool cut = g_cutsceneRunning && *g_cutsceneRunning;
        bool fade = TheCamera && Field<uint8_t>(TheCamera, 0x57);   // CCamera::m_bFading
        CTRACE("ИГРОК", "(%.1f,%.1f,%.1f) h=%.0f v=%.1f hp=%u бр=%u w=%u зв=%u st=%d mv=%u сила=%.2f i=%u%s%s%s%s%s%s%s%s%s",
               lp.x, lp.y, lp.z, me.heading * 57.29578f, spd, (unsigned)me.health, (unsigned)me.armour, (unsigned)me.weapon, (unsigned)me.wanted,
               Field<int>(local, off::PedState), (unsigned)me.moveState, me.moveRatio, (unsigned)me.area, veh,
               (me.flags & net::PSF_AIMING) ? " ЦЕЛИТСЯ" : "", (me.flags & net::PSF_DUCK) ? " ПРИСЕЛ" : "", (me.flags & net::PSF_TALKING) ? " ГОВОРИТ" : "",
               (me.flags & net::PSF_DEAD) ? " МЁРТВ" : "", cut ? " КАТСЦЕНА" : "", fade ? " ЗАТЕМНЕНИЕ" : "", padOff ? " УПРАВЛЕНИЕ-ВЫКЛ" : "",
               mission::ClientCutscene() ? " КАТСЦЕНА-ХОСТА" : "");
        int spawned = g_remotePed ? 1 : 0, inWorld = g_inWorld ? 1 : 0;
        if(spawned != g_tr.remoteSpawned || inWorld != g_tr.inWorld)
        {
            CTRACE("ДРУГ", "второй CJ: %s", !spawned ? "нет на карте" : inWorld ? "на карте" : "далеко — только метка");
            g_tr.remoteSpawned = spawned; g_tr.inWorld = inWorld;
        }
        if(!rs || !rs->valid) return;
        const net::PlayerStatePacket& p = rs->pkt;
        float rspd = sqrtf(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2]);
        CVector pp{ p.pos[0], p.pos[1], p.pos[2] };
        char copy[200];
        if(g_remotePed && GetPedFromRef(g_remoteRef) == g_remotePed)
        {
            CVector cp = GetPosition(g_remotePed);
            float dx = cp.x - pp.x, dy = cp.y - pp.y, dz = cp.z - pp.z;
            snprintf(copy, sizeof(copy), " | копия (%.1f,%.1f,%.1f) расхожд=%.1f м st=%d hp=%.0f%s%s", cp.x, cp.y, cp.z, sqrtf(dx * dx + dy * dy + dz * dz),
                     Field<int>(g_remotePed, off::PedState), Field<float>(g_remotePed, off::PedHealth), InVehicle(g_remotePed) ? " в_машине" : "",
                     sync::RemoteBusy() ? " занят_машиной" : "");
        }
        else snprintf(copy, sizeof(copy), " | копии нет");
        char rveh[64] = "";
        if(p.seat != net::kNoSeat || p.vehId)
            snprintf(rveh, sizeof(rveh), " машина=%04X место=%d садится=%d", (unsigned)p.vehId, p.seat == net::kNoSeat ? -1 : (int)p.seat,
                     p.enterSeat == net::kNoSeat ? -1 : (int)p.enterSeat);
        CTRACE("ДРУГ", "пакет #%u %.0f мс назад (%.1f,%.1f,%.1f) h=%.0f v=%.1f hp=%u бр=%u w=%u зв=%u mv=%u сила=%.2f i=%u%s%s%s%s%s%s%s до тебя %.0f м%s",
               p.seq, net::Since(now, rs->receivedUs) / 1000.0, pp.x, pp.y, pp.z, p.heading * 57.29578f, rspd, (unsigned)p.health, (unsigned)p.armour,
               (unsigned)p.weapon, (unsigned)p.wanted, (unsigned)p.moveState, p.moveRatio, (unsigned)p.area, rveh,
               (p.flags & net::PSF_AIMING) ? " ЦЕЛИТСЯ" : "", (p.flags & net::PSF_DUCK) ? " ПРИСЕЛ" : "", (p.flags & net::PSF_TALKING) ? " ГОВОРИТ" : "",
               (p.flags & net::PSF_DEAD) ? " МЁРТВ" : "", (p.flags & net::PSF_FF) ? " огонь_по_своим" : "", (p.flags & net::PSF_SHARED_WANTED) ? " общий_розыск" : "",
               Dist2D(lp, pp), copy);
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
        ++g_frameNo;
        ApplyPendingConfig();
        TsTick(net::NowUs());
        uint64_t now = net::NowUs();
        float dt = g_lastFrameUs ? (float)(now - g_lastFrameUs) / 1e6f : 0.0f;
        if(dt > 0.25f) dt = 0.25f;
        g_lastFrameUs = now;

        static bool firstFrame = true;
        if(firstFrame) { firstFrame = false; Log(false, "Первый игровой кадр — перехват CGame::Process работает"); }
        TracePerf(now);
        TraceNet(now);

        uintptr_t local = SlotPed(0);
        bool canSpawnNow = local && Field<uintptr_t>(local, off::EntRwObject) && !(g_cutsceneRunning && *g_cutsceneRunning);
        if(!canSpawnNow || g_localSinceUs == 0) g_localSinceUs = now;   // отсчёт «игрок в игре» начинается заново
        if(!local)
        {
            if(now >= g_nextStatusUs) { g_nextStatusUs = now + kStatusLogUs; Log(false, "Статус: игрок ещё не загружен"); }
            return;
        }

        net::PlayerStatePacket me = CaptureLocal(local);
        sync::Ctx sc;
        sc.role = g_cfg.role; sc.local = local; sc.now = now; sc.dt = dt;
        sc.friendlyFireLocal = g_friendlyFire; sc.sharedWantedLocal = g_sharedWanted; sc.echoOffset = kEchoOffset;
        sc.remote = CheckRemoteAlive() ? g_remotePed : 0;
        sync::Capture(sc, me);

        // Выдача оружия из меню CO-OP (только хост)
        {
            std::vector<GiveReq> q;
            { std::lock_guard<std::mutex> lock(g_cfgMutex); q.swap(g_giveQueue); }
            for(const auto& r : q)
            {
                if(g_cfg.role != net::Role::Host) { Log(true, "[ОРУЖИЕ] выдавать может только хост"); continue; }
                if(r.toFriend) sync::SendGiveWeapon(r.weapon, r.ammo);
                else           sync::GiveWeaponLocal(r.weapon, r.ammo, "Выдано себе");
            }
        }

        if(now >= g_nextStatusUs)
        {
            g_nextStatusUs = now + kStatusLogUs;
            if(sync::TrackedVehicles()) Log(false, "Статус: общих машин %d", sync::TrackedVehicles());
            if(!CheckRemoteAlive())
                Log(false, "Статус: второго CJ нет, FPS ~%.0f", dt > 0 ? 1.0f / dt : 0.0f);
            else
                Log(false, "Статус: второй CJ есть, %s, до него %.0f м, его состояние %d; ты %s "
                           "(движение %d, сила %.2f, скорость %.1f м/с, прыжков %u, состояние %d%s); джойстик 2: %d,%d%s; FPS ~%.0f",
                    g_inWorld ? "в мире" : "далеко (только метка)",
                    Dist2D(GetPosition(local), g_shownPos), Field<int>(g_remotePed, off::PedState),
                    InVehicle(local) ? "в машине" : "пешком", me.moveState, me.moveRatio,
                    sqrtf(me.vel[0] * me.vel[0] + me.vel[1] * me.vel[1]), (unsigned)me.jumpCount,
                    Field<int>(local, off::PedState), Field<uint16_t>(GetPad(0), off::PadDisableCtrl) ? ", УПРАВЛЕНИЕ ВЫКЛЮЧЕНО ИГРОЙ" : "",
                    g_stickX, g_stickY, g_sprint ? " +спринт" : "", dt > 0 ? 1.0f / dt : 0.0f);
            if(g_headingErrCount)
                Log(false, "Проверка джойстика: среднее расхождение курса %.0f° (норма — до ~20°, около 180° — перепутан знак)",
                    g_headingErrSum / g_headingErrCount);
            g_headingErrSum = 0; g_headingErrCount = 0;
        }

        if(g_cfg.role == net::Role::Off) { sync::Frame(sc); return; }

        if(g_cfg.role == net::Role::Echo)
        {
            const net::RemoteState& ers = EchoUpdate(me, now);
            UpdateRemote(ers, local, dt, now);
            sc.connected = true;
            sc.remote = CheckRemoteAlive() ? g_remotePed : 0;
            sc.rs = (ers.valid && net::Since(now, ers.receivedUs) < kStaleUs) ? &ers.pkt : nullptr;
            g_where = "синхронизация машин/оружия (эхо)";
            sync::Frame(sc);
            TracePlayers(now, local, me, &ers);
            // Голос в одиночку: говоришь — через 0,5 с слышишь себя (как двойник повторяет движения)
            voice::SetLoopback(true, (int)(kEchoDelayUs / 1000));
            voice::SetTransmit(g_localTalking || g_extTalking);
            voice::Pump();
            if(g_remotePed && !g_echoAnnounced)
            {
                g_echoAnnounced = true;
                net::Event e{ net::EventType::Connected, "Двойник", -1 };
                g_echoEvents.push_back(e);
            }
            return;
        }

        g_session.Poll();
        g_lastGameFrameUs = now;
        world::Tick(g_cfg.role, g_session.Connected(), true, now);
        world::RunDeferredLoad(false);
        if(now >= g_nextSendUs)
        {
            g_session.SendPlayerState(me);
            g_nextSendUs = now + kSendIntervalUs;
        }
        PumpVoice();
        const net::RemoteState& rs = g_session.Remote();
        UpdateRemote(rs, local, dt, now);
        sc.connected = g_session.Connected();
        sc.remote = CheckRemoteAlive() ? g_remotePed : 0;
        sc.rs = (rs.valid && net::Since(now, rs.receivedUs) < kStaleUs) ? &rs.pkt : nullptr;
        g_where = "синхронизация машин/оружия";
        sync::Frame(sc);
        g_where = "общий мир людей";
        crowd::Frame(g_cfg.role, sc.connected, local, sc.remote, sc.rs, now);
        g_where = "миссии вместе";
        mission::Frame(g_cfg.role, sc.connected, local, sc.remote, sc.rs, now);
        g_where = "подробный лог коопа";
        TracePlayers(now, local, me, &rs);

        // Загрузились в мир хоста — встать рядом с ним (один раз)
        if(world::TeleportPending() && sc.rs && !InVehicle(local) && PedTeleport)
        {
            const net::PlayerStatePacket& h = *sc.rs;
            uint8_t myArea = CurrArea ? *CurrArea : 0;
            if(h.area == myArea)
            {
                float hh = h.heading;
                float side = (h.flags & net::PSF_IN_VEHICLE) ? 3.5f : 1.8f;
                CVector to{ h.pos[0] + cosf(hh) * side, h.pos[1] + sinf(hh) * side, h.pos[2] + 0.5f };
                PedTeleport(local, to, false);
                Log(false, "[МИР] Перенесён к хосту: %.1f, %.1f, %.1f", to.x, to.y, to.z);
                world::TeleportDone();
            }
        }
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
    static bool lockedBefore = false;
    if(g_lockLocal) Field<uint16_t>(GetPad(0), off::PadDisableCtrl) = 1;
    else if(lockedBefore) Field<uint16_t>(GetPad(0), off::PadDisableCtrl) = 0; // вернуть управление
    lockedBefore = g_lockLocal;
}

// Пульт второго игрока: отдаём «виртуальный джойстик» друга
static bool IsRemotePad(uintptr_t pad) { return g_remotePed && pad == GetPad(kRemoteSlot); }
DECL_HOOK(int16_t, PadGetPedWalkLeftRight, uintptr_t self) { return IsRemotePad(self) ? g_stickX : PadGetPedWalkLeftRight(self); }
DECL_HOOK(int16_t, PadGetPedWalkUpDown,    uintptr_t self) { return IsRemotePad(self) ? g_stickY : PadGetPedWalkUpDown(self); }
DECL_HOOK(bool,    PadGetSprint, uintptr_t self, int mode) { return IsRemotePad(self) ? g_sprint : PadGetSprint(self, mode); }
DECL_HOOK(bool,    PadJumpJustDown, uintptr_t self)
{
    if(IsRemotePad(self))
    {
        bool j = g_pendingJump;
        g_pendingJump = false;
        return j;
    }
    bool r = PadJumpJustDown(self);
    if(r && self == GetPad(0) && g_lastLocalJumpFrame != g_frameNo)
    {
        g_lastLocalJumpFrame = g_frameNo;
        ++g_localJumpCount;
    }
    return r;
}

// CPlayerInfo::Process для второго игрока не выполняем: им управляет мод. Иначе игра «за него»
// прятала общую кнопку «сесть» (CWidgetButtonEnterCar) и могла засчитать ему твоё нажатие.
DECL_HOOKv(PlayerInfoProcess, uintptr_t self, int player)
{
    if(g_remotePed && self == Players + off::PlayerInfoSize && SlotPed(kRemoteSlot) == g_remotePed)
    {
        static bool logged = false;
        if(!logged) { logged = true; Log(false, "Обработка игрока 2 (CPlayerInfo::Process) отключена — вторым CJ управляет мод"); }
        return;
    }
    PlayerInfoProcess(self, player);
}

DECL_HOOKb(IsCoopGameGoingOn)                         { return false; }

// «Поводок» режима на двоих: игра не даёт уйти/уехать дальше заданного расстояния от игрока 2
// (CPlayerTaskSimpleOnFoot / CAutomobile::ProcessControlInputs). У нас игроки ходят где хотят.
DECL_HOOK(bool, AllowedToGoInDirection, uintptr_t ped, CVector dir, float dist)
{
    if(g_remotePed) return true;
    return AllowedToGoInDirection(ped, dir, dist);
}
DECL_HOOKb(IsPlayerUse2PlayerControls, uintptr_t ped) { (void)ped; return false; }

DECL_HOOKv(GameLogicUpdate)             { g_where = "CGameLogic::Update (слот скрыт)"; { HideRemoteSlot h; GameLogicUpdate(); } g_where = "игра (не код мода)"; }
DECL_HOOKv(CameraCamControl, uintptr_t self) { g_where = "CCamera::CamControl (слот скрыт)"; { HideRemoteSlot h; CameraCamControl(self); } g_where = "игра (не код мода)"; }
DECL_HOOKv(PickupsUpdate)               { g_where = "CPickups::Update (слот скрыт)"; { HideRemoteSlot h; PickupsUpdate(); } g_where = "игра (не код мода)"; }
DECL_HOOKv(DarkelUpdate)                { g_where = "CDarkel::Update (слот скрыт)"; { HideRemoteSlot h; DarkelUpdate(); } g_where = "игра (не код мода)"; }

// Перед сохранением/загрузкой/рестартом второго CJ убираем,
// чтобы он не попал в сохранение и не остался висящим указателем.
DECL_HOOK(bool, GenericLoad, bool& ok)
{
    g_localSinceUs = 0; DespawnRemote("загрузка игры"); crowd::Reset("загрузка игры"); mission::Reset("загрузка игры"); sync::Reset("загрузка игры");
    bool r = GenericLoad(ok);
    world::AfterGameLoaded();
    return r;
}
DECL_HOOKv(ShutDownForRestart)              { g_localSinceUs = 0; DespawnRemote("рестарт игры"); crowd::Reset("рестарт игры"); mission::Reset("рестарт игры"); sync::Reset("рестарт игры"); ShutDownForRestart(); }

const char* Where() { return g_where; }

// ---------- канал данных для других модов (SA Living) ----------
namespace
{
    void (*g_modRecv[256])(const void*, int) = {};
    void ModSinkDispatch(uint8_t ch, const uint8_t* data, int len) { if(g_modRecv[ch]) g_modRecv[ch](data, len); }
    int  Api_GetRole()
    {
        switch(g_cfg.role) { case net::Role::Echo: return 1; case net::Role::Host: return 2; case net::Role::Client: return 3; default: return 0; }
    }
    bool Api_Connected() { return g_session.Connected(); }
    bool Api_Send(uint8_t ch, const void* d, int len) { return g_session.SendMod(ch, d, len); }
    void Api_SetReceiver(uint8_t ch, void (*fn)(const void*, int)) { g_modRecv[ch] = fn; }
    const char* Api_PeerNick() { return g_session.PeerNick(); }
    const char* Api_MyNick() { return g_cfg.nick; }
    // версия 2: один микрофон и одна кнопка на оба мода
    void Api_SetMicTap(void (*fn)(const int16_t*, int, int)) { voice::SetMicTap(fn); CTRACE("ГОЛОС", "другой мод %s копию микрофона", fn ? "берёт" : "больше не берёт"); }
    void Api_SetTalk(bool on) { if(on != g_extTalking) CTRACE("ГОЛОС", "кнопка «Говорить» SA Living %s", on ? "зажата" : "отпущена"); g_extTalking = on; }
    void Api_SetTalkButtonOwner(bool owned) { if(owned != g_talkOwned) CTRACE("ГОЛОС", "золотая кнопка %s", owned ? "спрятана — говорит кнопка SA Living" : "снова своя"); g_talkOwned = owned; if(!owned) g_extTalking = false; }
    bool Api_VoiceAvailable() { return voice::Available(); }
    SACoopNetApi g_netApi = { 2, Api_GetRole, Api_Connected, Api_Send, Api_SetReceiver, Api_PeerNick, Api_MyNick,
                              Api_SetMicTap, Api_SetTalk, Api_SetTalkButtonOwner, Api_VoiceAvailable };
}

static void GameSinkDispatch(uint8_t type, const uint8_t* data, int len)
{
    if(type == net::PKT_WORLD_REQ || type == net::PKT_WORLD_CHUNK || type == net::PKT_WORLD_ACK || type == net::PKT_LOBBY) world::OnPacket(type, data, len);
    else if(type == net::PKT_PEDS || type == net::PKT_WORLD_TIME) crowd::OnPacket(type, data, len);
    else if(type == net::PKT_BLIPS || (type == net::PKT_RELIABLE && len >= 1 && mission::HandlesReliable(data[0]))) mission::OnPacket(type, data, len);
    else sync::OnPacket(type, data, len);
}

bool Install(void* h, FLogger* log)
{
    g_log = log;

    g_userPause = (uint8_t*)aml->GetSym(h, "_ZN6CTimer11m_UserPauseE");
    g_cutsceneRunning = (uint8_t*)aml->GetSym(h, "_ZN12CCutsceneMgr10ms_runningE");
    voice::Init(log);
    g_session.SetVoiceSink(voice::PushReceivedFrame);
    g_session.SetModSink(ModSinkDispatch);
    g_session.SetGameSink(GameSinkDispatch);
    sync::SetSession(&g_session);
    world::SetSession(&g_session);
    RegisterInterface(SACOOP_NET_INTERFACE, &g_netApi);
    Log(false, "Канал данных для других модов (SACoopNet) зарегистрирован");
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
        { "_ZN4CPad12JumpJustDownEv",                                (void*)HookOf_PadJumpJustDown,            (void**)&PadJumpJustDown },
        { "_ZN11CPlayerInfo7ProcessEi",                              (void*)HookOf_PlayerInfoProcess,          (void**)&PlayerInfoProcess },
        { "_ZN10CGameLogic34IsPlayerAllowedToGoInThisDirectionEP10CPlayerPed7CVectorf", (void*)HookOf_AllowedToGoInDirection, (void**)&AllowedToGoInDirection },
        { "_ZN10CGameLogic17IsCoopGameGoingOnEv",                    (void*)HookOf_IsCoopGameGoingOn,          (void**)&IsCoopGameGoingOn },
        { "_ZN10CGameLogic26IsPlayerUse2PlayerControlsEP10CPlayerPed",(void*)HookOf_IsPlayerUse2PlayerControls, (void**)&IsPlayerUse2PlayerControls },
        { "_ZN10CGameLogic6UpdateEv",                                (void*)HookOf_GameLogicUpdate,            (void**)&GameLogicUpdate },
        { "_ZN7CCamera10CamControlEv",                               (void*)HookOf_CameraCamControl,           (void**)&CameraCamControl },
        { "_ZN8CPickups6UpdateEv",                                   (void*)HookOf_PickupsUpdate,              (void**)&PickupsUpdate },
        { "_ZN7CDarkel6UpdateEv",                                    (void*)HookOf_DarkelUpdate,               (void**)&DarkelUpdate },
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
    g_origGetSprint = PadGetSprint;
    sync::Install(h, g_log);   // машины, оружие, урон (если чего-то нет — кооп пешком, как раньше)
    world::Install(h, g_log);  // зайти сразу в мир хоста
    crowd::Install(h, g_log);  // одинаковые люди и машины вокруг друга
    crowd::SetSession(&g_session);
    mission::Install(h, g_log);  // миссии вместе (ведёт хост)
    mission::SetSession(&g_session);

    Log(false, "Кооп установлен: перехватов %d", (int)(sizeof(hooks) / sizeof(hooks[0])));
    return true;
}

void SetConfig(const net::Config& cfg)
{
    CTRACE("КНОПКА", "режим коопа → %s, адрес хоста «%s», порт %u, ник «%s»", RoleName(cfg.role), cfg.hostAddress, (unsigned)cfg.port, cfg.nick);
    std::lock_guard<std::mutex> lock(g_cfgMutex);
    g_pendingCfg = cfg;
    g_hasPending = true;
}

net::Config GetConfig()
{
    std::lock_guard<std::mutex> lock(g_cfgMutex);
    return g_hasPending ? g_pendingCfg : g_cfg;
}

void KeepWorldRunningInMenu()
{
    if(!g_userPause) return;
    bool pending;
    { std::lock_guard<std::mutex> lock(g_cfgMutex); pending = g_hasPending; }
    bool coopActive = !pending && (g_cfg.role == net::Role::Host || g_cfg.role == net::Role::Client);
    static bool weUnpaused = false;   // это мы сняли паузу, пока открыто меню
    if(coopActive && SlotPed(0))
    {
        if(*g_userPause)
        {
            *g_userPause = 0;
            static bool logged = false;
            if(!logged) { logged = true; Log(false, "Кооп: меню открыто, но игра не на паузе — мир и связь работают"); }
        }
        weUnpaused = true;
    }
    else if(weUnpaused)
    {   // кооп выключили прямо в меню — вернуть обычную паузу сразу
        *g_userPause = 1;
        weUnpaused = false;
        Log(false, "Кооп выключен: меню снова ставит игру на паузу");
    }
}

Team GetTeam()
{
    Team t;
    t.role = g_cfg.role;
    bool pending;
    { std::lock_guard<std::mutex> lock(g_cfgMutex); pending = g_hasPending; }
    uintptr_t me = SlotPed(0);
    if(pending || !me || g_cfg.role == net::Role::Off) return t;

    t.show = true;
    snprintf(t.myNick, sizeof(t.myNick), "%s", g_cfg.nick);
    t.myHealth = Field<float>(me, off::PedHealth);
    t.myArmour = Field<float>(me, off::PedArmour);

    if(g_cfg.role == net::Role::Echo)
    {
        if(g_remotePed)
        {
            t.mate.present = true;
            snprintf(t.mate.nick, sizeof(t.mate.nick), "Двойник");
            t.mate.health = t.myHealth; t.mate.armour = t.myArmour;
            t.mate.distM = Dist2D(GetPosition(me), g_shownPos);
            t.mate.talking = voice::Available() ? voice::Playing() : false; // двойник «говорит», когда звучит твоё эхо
        }
        return t;
    }

    const net::RemoteState& rs = g_session.Remote();
    bool connected = g_session.Connected();
    if(connected || (rs.valid && g_session.PeerNick()[0]))
    {
        t.mate.present = true;
        t.mate.lost = !connected;
        snprintf(t.mate.nick, sizeof(t.mate.nick), "%s", g_session.PeerNick());
        if(rs.valid)
        {
            t.mate.health = rs.pkt.health; t.mate.armour = rs.pkt.armour;
            CVector p{ rs.pkt.pos[0], rs.pkt.pos[1], rs.pkt.pos[2] };
            t.mate.distM = Dist2D(GetPosition(me), p);
            t.mate.talking = (rs.pkt.flags & net::PSF_TALKING) != 0;
        }
        t.mate.pingMs = g_session.LastRttMs();
    }
    return t;
}

bool PopEvent(net::Event& out)
{
    if(!g_echoEvents.empty()) { out = g_echoEvents.front(); g_echoEvents.erase(g_echoEvents.begin()); return true; }
    return g_session.PopEvent(out);
}

bool PendingRequest(char* nick, size_t len, float* secondsLeft)
{
    if(g_cfg.role != net::Role::Host || g_session.GetLink() != net::Link::Pending) return false;
    snprintf(nick, len, "%s", g_session.PendingNick());
    float el = (float)(net::NowUs() - g_session.PendingSinceUs()) / 1e6f;
    *secondsLeft = el < 15 ? 15 - el : 0;
    return true;
}

void AnswerJoin(bool accept) { CTRACE("КНОПКА", "запрос «%s»: %s", g_session.PendingNick(), accept ? "ПРИНЯТЬ" : "отклонить"); g_session.AnswerJoin(accept); }
bool TalkButtonOwned() { return g_talkOwned; }
void SetTalking(bool talking) { if(talking != g_localTalking) CTRACE("ГОЛОС", "кнопка микрофона %s", talking ? "зажата" : "отпущена"); g_localTalking = talking; }
void SetFriendlyFire(bool on) { if(on != g_friendlyFire) CTRACE("КНОПКА", "огонь по своим: %s", on ? "вкл" : "выкл"); g_friendlyFire = on; }
void SetSharedWanted(bool on) { if(on != g_sharedWanted) CTRACE("КНОПКА", "общий розыск: %s", on ? "вкл" : "выкл"); g_sharedWanted = on; }

void GiveWeapon(bool toFriend, int weapon, int ammo)
{
    CTRACE("КНОПКА", "выдать оружие %d (патроны %d) %s", weapon, ammo, toFriend ? "другу" : "себе");
    std::lock_guard<std::mutex> lock(g_cfgMutex);
    g_giveQueue.push_back(GiveReq{ toFriend, weapon, ammo });
}
bool CanGiveToFriend() { return g_cfg.role == net::Role::Host && g_session.Connected(); }
bool PopNotice(char* out, size_t len) { return world::PopNotice(out, len) || sync::PopNotice(out, (int)len); }

void MenuTick()
{
    uint64_t now = net::NowUs();
    if(g_lastGameFrameUs && net::Since(now, g_lastGameFrameUs) < 300000) return;   // игра идёт — всё в OnFrame
    if(SlotPed(0)) return;                                                          // игра загружена (пауза)
    ApplyPendingConfig();
    TsTick(now);
    if(g_cfg.role != net::Role::Host && g_cfg.role != net::Role::Client) return;
    g_session.Poll();
    TraceNet(now);
    world::Tick(g_cfg.role, g_session.Connected(), false, now);
}

bool MenuUpdateTick() { return world::RunDeferredLoad(true); }
bool CanSaveHostWorld() { return g_cfg.role == net::Role::Client && g_session.Connected(); }
void SaveHostWorld() { world::RequestSaveCopy(); }

JoinProgress GetJoinProgress()
{
    JoinProgress jp;
    bool pending;
    { std::lock_guard<std::mutex> lock(g_cfgMutex); pending = g_hasPending; }
    if(!pending && g_cfg.role == net::Role::Client && g_tsClientWait)
    {
        jp.active = true; jp.percent = 3;
        snprintf(jp.title, sizeof(jp.title), "Поиск точки хоста %s", g_tsClientId);
        snprintf(jp.detail, sizeof(jp.detail), "%s", g_tsInfo[0] ? g_tsInfo : "Включаю Tailscale…");
        return jp;
    }
    if(pending || g_cfg.role != net::Role::Client || !g_session.Running()) return jp;
    world::Progress p = world::GetProgress(g_session.GetLink());
    jp.active = p.active; jp.percent = p.percent;
    snprintf(jp.title, sizeof(jp.title), "%s", p.title);
    snprintf(jp.detail, sizeof(jp.detail), "%s", p.detail);
    return jp;
}

void CancelJoin()
{
    net::Config c = GetConfig();
    c.role = net::Role::Off;
    SetConfig(c);
    Log(false, "[МИР] Подключение отменено игроком");
}

NetInfo GetNetInfo()
{
    NetInfo n;
    ts::Status st = ts::GetStatus();
    n.tsState = st.state;
    snprintf(n.loginUrl, sizeof(n.loginUrl), "%s", st.loginUrl);
    snprintf(n.tsIp, sizeof(n.tsIp), "%s", st.ip);
    snprintf(n.info, sizeof(n.info), "%s", g_tsInfo);
    net::Config c = GetConfig();
    snprintf(n.myId, sizeof(n.myId), "%s", c.myId);
    n.pointOpen = g_tsHostBridge > 0 && g_cfg.role == net::Role::Host;
    n.searching = g_tsClientWait;
    return n;
}
LobbyInfo GetLobbyInfo()
{
    LobbyInfo l;
    l.active = (g_cfg.role == net::Role::Host || g_cfg.role == net::Role::Client) && g_session.Connected();
    l.isHost = g_cfg.role == net::Role::Host;
    snprintf(l.peerNick, sizeof(l.peerNick), "%s", g_session.PeerNick());
    l.pingMs = g_session.LastRttMs();
    l.inGame = SlotPed(0) != 0;
    l.lobby = world::GetLobby();
    return l;
}
std::vector<world::SaveEntry> ListSaves() { return world::ListSaves(); }
const char* SavesFolder() { return world::SavesFolder(); }
bool Launch(const char* path, const char* name, char* why, size_t whyLen)
{
    bool ok = world::HostLaunch(path, name, why, whyLen);
    CTRACE("КНОПКА", "«Запуск» лобби: %s (%s) → %s%s%s", name ? name : "?", path ? path : "текущая игра", ok ? "ок" : "НЕТ", ok ? "" : ": ", ok ? "" : why);
    return ok;
}

void StartTailscale() { CTRACE("КНОПКА", "включить Tailscale"); ts::Start(GetConfig().myId); }
bool TailscaleLogout() { bool ok = ts::Logout(); CTRACE("КНОПКА", "выйти из аккаунта Tailscale: %s", ok ? "ок" : "не вышло"); return ok; }

bool GameLoaded() { return SlotPed(0) != 0; }
void LockLocalControls(bool lock) { g_lockLocal = lock; }

uintptr_t HideRemoteBegin()
{
    if(!CheckRemoteAlive()) return 0;
    SlotPed(kRemoteSlot) = 0;
    return g_remotePed;
}
void HideRemoteEnd(uintptr_t token)
{
    if(token && token == g_remotePed && !SlotPed(kRemoteSlot)) SlotPed(kRemoteSlot) = token;
}

Status GetStatus()
{
    Status st{};
    st.link = g_session.GetLink();
    snprintf(st.peerNick, sizeof(st.peerNick), "%s", g_session.PeerNick());
    { std::lock_guard<std::mutex> lock(g_cfgMutex); st.pending = g_hasPending; }
    st.role = g_cfg.role;
    st.running = g_session.Running();
    st.connected = g_session.Connected();
    st.pingMs = g_session.LastRttMs();
    st.remoteSpawned = g_remotePed != 0;
    strncpy(st.host, g_cfg.hostAddress, sizeof(st.host) - 1);
    snprintf(st.room, sizeof(st.room), "%s", g_session.RoomCode());
    st.relay = g_session.UsingRelay();
    return st;
}

void StatusText(char* out, size_t len)
{
    bool pending;
    { std::lock_guard<std::mutex> lock(g_cfgMutex); pending = g_hasPending; }
    if(pending) { snprintf(out, len, "Applies when you load a game"); return; }

    switch(g_cfg.role)
    {
        case net::Role::Off:
            snprintf(out, len, "Off");
            break;
        case net::Role::Echo:
            snprintf(out, len, g_remotePed ? "Solo test: 2nd CJ is near you" : "Solo test: load a game");
            break;
        case net::Role::Host:
            if(!g_session.Running())       snprintf(out, len, "Host: network error");
            else if(g_session.Connected()) snprintf(out, len, "Host: friend connected");
            else                           snprintf(out, len, "Host: waiting for friend");
            break;
        case net::Role::Client:
            if(!g_session.Running())       snprintf(out, len, "Join: bad host address");
            else if(g_session.Connected())
            {
                int rtt = g_session.LastRttMs();
                if(rtt >= 0) snprintf(out, len, "Connected, ping %d ms", rtt);
                else         snprintf(out, len, "Connected");
            }
            else snprintf(out, len, "Connecting to %s...", g_cfg.hostAddress);
            break;
    }
}

} // namespace coop
