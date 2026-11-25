#include "crowd.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <math.h>
#include <stdarg.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "game/gta.h"
#include "coop/sync.h"

using namespace gta;

namespace coop { namespace crowd {
namespace
{
    constexpr float    kRange        = 120.0f;   // педы вокруг друга
    constexpr int      kMaxPeds      = 48;
    constexpr uint64_t kSendUs       = 100000;   // 10 раз/с
    constexpr uint64_t kCopyStaleUs  = 1500000;  // копию без вестей 1,5 с — убрать
    constexpr uintptr_t kPedStride   = 0x9C8;    // запись пула педов
    constexpr int      kMaxVehAround = 20;       // машин вокруг друга (общие машины)
    // Телефон хоста держит людей только вокруг хоста (так устроена игра: население — вокруг игрока).
    // Поэтому общий мир людей — когда вы рядом; разъехались — у друга снова своё население.
    constexpr float    kShareOn      = 150.0f;
    constexpr float    kShareOff     = 210.0f;

    FLogger*      g_log = nullptr;
    net::Session* g_session = nullptr;
    bool          g_ok = false;
    uintptr_t     g_pedPoolSym = 0, g_vehPoolSym = 0;

    // функции игры
    uintptr_t (*AddPed)(int pedType, unsigned model, const CVector& pos, bool) = nullptr;
    void      (*RemovePed)(uintptr_t ped) = nullptr;
    void*     ctorGoTo = nullptr;    // CTaskComplexGoToPointAndStandStill(int moveState, const CVector&, float, float, bool, bool)
    void*     ctorStand = nullptr;   // CTaskSimpleStandStill(int ms, bool, bool, float)
    void*     ctorKill = nullptr;    // CTaskComplexKillPedOnFoot(CPed*, int, uint, uint, uint, int)
    void*     ctorArrest = nullptr;  // CTaskComplexArrestPed(CPed*)
    void      (*ArrestPlayer)(uintptr_t playerInfo) = nullptr;

    void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool error, const char* fmt, ...)
    {
        char buf[512];
        va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        if(g_log) { if(error) g_log->Error("%s", buf); else g_log->Info("%s", buf); }
    }

    float Dist(const CVector& a, const CVector& b) { float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return sqrtf(dx * dx + dy * dy + dz * dz); }

    struct Pool { uintptr_t objs = 0, flags = 0; int size = 0; };
    bool GetPool(uintptr_t sym, Pool& p)
    {
        uintptr_t pp = sym ? *(uintptr_t*)sym : 0;
        if(!pp) return false;
        p.objs = Field<uintptr_t>(pp, 0); p.flags = Field<uintptr_t>(pp, 8); p.size = Field<int>(pp, 0x10);
        return p.objs && p.flags && p.size > 0 && p.size <= 8192;
    }

    void* NewTask() { return TaskNew ? TaskNew(0x80) : nullptr; }
    void  Assign(uintptr_t ped, void* t) { uintptr_t m = TaskManager(ped); if(m && t) TaskMgrSetTask(m, t, 3, false); }

    // ---------- общее ----------
    net::Role g_role = net::Role::Off;
    bool      g_active = false;
    uint64_t  g_now = 0;
    uintptr_t g_local = 0, g_remote = 0;
    const net::PlayerStatePacket* g_rs = nullptr;

    // ---------- хост ----------
    struct Track { uintptr_t ped; int ref; uint16_t id; uint64_t seenUs; uint64_t copTaskUs; };
    std::vector<Track> g_tracks;
    uint16_t g_nextId = 1;
    uint32_t g_seq = 0;
    uint64_t g_nextSendUs = 0, g_nextCopUs = 0, g_nextTimeUs = 0, g_lastBustedUs = 0;

    Track* FindTrack(uintptr_t ped) { for(auto& t : g_tracks) if(t.ped == ped) return &t; return nullptr; }

    bool PedValid(uintptr_t ped, int ref) { return ped && GetPedFromRef(ref) == ped; }

    void HostSend()
    {
        if(!g_rs || !g_session) return;
        CVector cp{ g_rs->pos[0], g_rs->pos[1], g_rs->pos[2] };
        Pool p;
        if(!GetPool(g_pedPoolSym, p)) return;
        struct Cand { uintptr_t ped; float d; };
        std::vector<Cand> cands;
        for(int i = 0; i < p.size; ++i)
        {
            if(Field<uint8_t>(p.flags, i) & 0x80) continue;
            uintptr_t ped = p.objs + (uintptr_t)i * kPedStride;
            if(ped == g_local || ped == g_remote) continue;
            int type = Field<int>(ped, off::PedType);
            if(type == 0 || type == 1) continue;
            if(!Field<uintptr_t>(ped, off::EntRwObject)) continue;
            float d = Dist(GetPosition(ped), cp);
            if(d > kRange) continue;
            cands.push_back({ ped, d });
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
        if((int)cands.size() > kMaxPeds) cands.resize(kMaxPeds);

        // номера
        for(auto& c : cands)
        {
            Track* t = FindTrack(c.ped);
            if(t && !PedValid(t->ped, t->ref)) { t->ped = 0; t = nullptr; }
            if(!t)
            {
                Track nt{ c.ped, GetPedRef(c.ped), g_nextId, g_now, 0 };
                if(!(g_nextId = (uint16_t)(g_nextId + 1))) g_nextId = 1;
                g_tracks.push_back(nt);
            }
            else t->seenUs = g_now;
        }
        // забыть тех, кого давно нет рядом
        for(size_t i = 0; i < g_tracks.size();)
        {
            Track& t = g_tracks[i];
            if(!t.ped || !PedValid(t.ped, t.ref) || net::Since(g_now, t.seenUs) > 3000000) { g_tracks.erase(g_tracks.begin() + i); continue; }
            ++i;
        }

        net::PedsPacket pk;
        int n = 0, sent = 0;
        auto flush = [&](bool last) {
            pk.hdr = net::MakeHeader(net::PKT_PEDS);
            pk.seq = ++g_seq; pk.count = (uint8_t)n; pk.last = last ? 1 : 0; pk.pad = 0;
            g_session->SendGame(&pk, net::kPedsHeaderSize + n * (int)sizeof(net::PedEntry));
            sent += n; n = 0;
        };
        for(auto& c : cands)
        {
            Track* t = FindTrack(c.ped);
            if(!t) continue;
            uintptr_t ped = c.ped;
            net::PedEntry& e = pk.peds[n];
            memset(&e, 0, sizeof(e));
            e.id = t->id;
            e.model = (uint16_t)ModelOf(ped);
            e.pedType = (uint8_t)Field<int>(ped, off::PedType);
            bool dead = !PedIsAlive(ped);
            e.flags = dead ? net::PDF_DEAD : 0;
            if(Field<uint8_t>(ped, off::PedFlags587) & 0x04) e.flags |= net::PDF_DUCK;
            if(Field<uint8_t>(ped, off::PedFlagsWord) & 0x10) e.flags |= net::PDF_AIM;
            e.moveState = (uint8_t)Field<int>(ped, off::PedMoveState);
            e.weapon = (uint8_t)CurrentWeaponType(ped);
            float hp = Field<float>(ped, off::PedHealth);
            e.health = (uint8_t)(hp < 0 ? 0 : hp > 255 ? 255 : hp);
            e.seat = net::kNoSeat;
            uintptr_t veh = InVehicle(ped) ? Field<uintptr_t>(ped, off::PedVehicle) : 0;
            if(veh)
            {
                uint16_t vid = sync::CrowdVehicleId(veh);
                if(vid) { e.flags |= net::PDF_IN_VEHICLE; e.vehId = vid; int s = SeatOf(ped, veh); e.seat = (uint8_t)(s < 0 ? 0 : s); }
            }
            CVector pos = GetPosition(ped);
            e.pos[0] = pos.x; e.pos[1] = pos.y; e.pos[2] = pos.z;
            e.heading = Field<float>(ped, off::PedRotCurrent);
            CVector ms = Field<CVector>(ped, off::PhysMoveSpeed);
            float vx = ms.x * 50.0f * 100.0f, vy = ms.y * 50.0f * 100.0f;
            e.vel[0] = (int16_t)(vx < -32000 ? -32000 : vx > 32000 ? 32000 : vx);
            e.vel[1] = (int16_t)(vy < -32000 ? -32000 : vy > 32000 ? 32000 : vy);
            if(++n == net::kPedsPerPacket) flush(false);
        }
        flush(true);

        // машины вокруг друга — в общие машины (без водителя-игрока тоже: припаркованные, трафик)
        Pool vp;
        if(GetPool(g_vehPoolSym, vp))
        {
            struct VC { uintptr_t veh; float d; };
            std::vector<VC> vs;
            for(int i = 0; i < vp.size; ++i)
            {
                if(Field<uint8_t>(vp.flags, i) & 0x80) continue;
                uintptr_t veh = vp.objs + (uintptr_t)i * 0xC68;
                if(Field<int>(veh, off::VehBaseType) == 6) continue;   // поезда — нет
                float d = Dist(GetPosition(veh), cp);
                if(d < kRange) vs.push_back({ veh, d });
            }
            std::sort(vs.begin(), vs.end(), [](const VC& a, const VC& b) { return a.d < b.d; });
            for(int i = 0; i < (int)vs.size() && i < kMaxVehAround; ++i) sync::CrowdVehicleId(vs[i].veh);
        }
        static uint64_t nextLog = 0;
        if(g_now >= nextLog) { nextLog = g_now + 5000000; Log(false, "[ЛЮДИ] хост: другу видно педов %d, всего на учёте %d", sent, (int)g_tracks.size()); }
    }

    // Копы хоста ловят и второго CJ, если у друга розыск
    void HostCops()
    {
        if(!g_rs || !g_remote || !PedIsAlive(g_remote) || g_now < g_nextCopUs) return;
        g_nextCopUs = g_now + 1000000;
        int wanted = g_rs->wanted;
        if(wanted <= 0) return;
        CVector rp = GetPosition(g_remote), lp = g_local ? GetPosition(g_local) : rp;
        int assigned = 0;
        for(auto& t : g_tracks)
        {
            if(!PedValid(t.ped, t.ref) || !PedIsAlive(t.ped)) continue;
            if(Field<int>(t.ped, off::PedType) != 6 || InVehicle(t.ped)) continue;   // коп пешком
            float dr = Dist(GetPosition(t.ped), rp);
            if(dr > 45.0f) continue;
            // ближе к тебе и у тебя есть розыск — пусть занимается тобой, как обычно
            float dl = Dist(GetPosition(t.ped), lp);
            if(dl < dr && FindPlayerWanted && Field<int>(FindPlayerWanted(0), off::WantedStars) > 0) continue;
            if(t.copTaskUs && net::Since(g_now, t.copTaskUs) < 6000000) { ++assigned; continue; }
            void* task = NewTask();
            if(!task) break;
            if(wanted == 1 && assigned == 0 && dr < 20.0f && ctorArrest)
                ((void(*)(void*, uintptr_t))ctorArrest)(task, g_remote);
            else if(ctorKill)
                ((void(*)(void*, uintptr_t, int, unsigned, unsigned, unsigned, int))ctorKill)(task, g_remote, 0, 0, 0, 0, 4);
            Assign(t.ped, task);
            t.copTaskUs = g_now;
            if(++assigned >= 2 + wanted) break;
        }
        // скрутили второго CJ — у друга «Арестован»
        if(Field<int>(g_remote, off::PedState) == 63 && !InVehicle(g_remote) && net::Since(g_now, g_lastBustedUs) > 15000000)
        {
            g_lastBustedUs = g_now;
            net::BustedEvent e{ net::REL_BUSTED, {} };
            g_session->SendReliable(&e, sizeof(e));
            Log(false, "[ЛЮДИ] Копы скрутили второго CJ — у друга «Арестован»");
        }
    }

    // Время и погода — как у хоста
    uint8_t* g_clockH = nullptr; uint8_t* g_clockM = nullptr;
    int16_t* g_wOld = nullptr; int16_t* g_wNew = nullptr; float* g_wInterp = nullptr;
    void HostTime()
    {
        if(g_now < g_nextTimeUs || !g_clockH || !g_wOld) return;
        g_nextTimeUs = g_now + 2000000;
        net::WorldTimePacket t{};
        t.hdr = net::MakeHeader(net::PKT_WORLD_TIME);
        t.hours = *g_clockH; t.minutes = *g_clockM;
        t.oldWeather = *g_wOld; t.newWeather = *g_wNew; t.interp = g_wInterp ? *g_wInterp : 0;
        g_session->SendGame(&t, sizeof(t));
    }

    // ---------- клиент ----------
    struct Copy
    {
        uint16_t id; uintptr_t ped = 0; int ref = -1;
        net::PedEntry st{}; uint64_t stUs = 0;
        uint64_t requestUs = 0; uint64_t goUs = 0; CVector goTarget{}; int goMove = 0;
        bool dead = false; bool killed = false; int weapon = -1;
    };
    std::vector<Copy> g_copies;
    bool g_popOff = false;                // своё население выключено
    bool g_allowCopyDamage = false;

    Copy* FindCopy(uint16_t id) { for(auto& c : g_copies) if(c.id == id) return &c; return nullptr; }
    bool CopyValid(Copy& c) { if(!c.ped) return false; if(GetPedFromRef(c.ref) == c.ped) return true; c.ped = 0; c.ref = -1; return false; }

    void DeleteCopy(Copy& c)
    {
        if(CopyValid(c))
        {
            if(InVehicle(c.ped)) sync::WarpPedOut(c.ped);
            RemovePed(c.ped);
        }
        c.ped = 0; c.ref = -1;
    }

    // Убрать своё (случайное) население один раз, когда включаем общий мир
    void ClearOwnPopulation()
    {
        int peds = 0, vehs = 0;
        Pool p;
        if(GetPool(g_pedPoolSym, p))
            for(int i = 0; i < p.size; ++i)
            {
                if(Field<uint8_t>(p.flags, i) & 0x80) continue;
                uintptr_t ped = p.objs + (uintptr_t)i * kPedStride;
                if(ped == g_local || ped == g_remote) continue;
                int type = Field<int>(ped, off::PedType);
                if(type == 0 || type == 1 || Field<uint8_t>(ped, off::PedCreatedBy) != 1) continue;
                if(InVehicle(ped)) continue;            // водители уйдут вместе с машинами
                RemovePed(ped); ++peds;
            }
        vehs = sync::ClearRandomVehicles();
        Log(false, "[ЛЮДИ] Твоё население выключено: убрано педов %d, машин %d — дальше люди и машины с телефона хоста", peds, vehs);
    }

    void ApplyCopy(Copy& c)
    {
        const net::PedEntry& s = c.st;
        CVector target{ s.pos[0], s.pos[1], s.pos[2] };
        if(!CopyValid(c))
        {
            if(c.killed) return;
            int model = s.model;
            if(!ModelLoaded(model))
            {
                if(!c.requestUs) { RequestModel(model, 2); c.requestUs = g_now; return; }
                if(net::Since(g_now, c.requestUs) < 1000000) return;
                LoadAllRequestedModels(false);
                if(!ModelLoaded(model)) return;
            }
            if(Dist(target, GetPosition(g_local)) > kRange + 20) return;
            uintptr_t ped = AddPed(s.pedType, (unsigned)model, target, false);
            if(!ped) return;
            PedSetCharCreatedBy(ped, 2);
            c.ped = ped; c.ref = GetPedRef(ped); c.weapon = -1; c.goUs = 0;
            Field<float>(ped, off::PedRotCurrent) = s.heading; Field<float>(ped, off::PedRotAiming) = s.heading;
        }
        uintptr_t ped = c.ped;

        // оружие (для вида и повтора выстрелов)
        if(s.weapon != c.weapon) { sync::EnsurePedWeapon(ped, s.weapon); c.weapon = s.weapon; }

        // смерть — как у хоста
        if(s.flags & net::PDF_DEAD)
        {
            if(PedIsAlive(ped))
            {
                g_allowCopyDamage = true;
                GenerateDamageEvent(ped, 0, s.weapon, 1000, 3, 0);
                g_allowCopyDamage = false;
                if(PedIsAlive(ped)) Field<float>(ped, off::PedHealth) = 0;
            }
            return;
        }
        Field<float>(ped, off::PedHealth) = s.health < 1 ? 1.0f : (float)s.health;

        // в машине
        if(s.flags & net::PDF_IN_VEHICLE)
        {
            uintptr_t veh = sync::VehicleById(s.vehId);
            if(veh)
            {
                uintptr_t cur = InVehicle(ped) ? Field<uintptr_t>(ped, off::PedVehicle) : 0;
                if(cur != veh) sync::WarpPedInto(ped, veh, s.seat);
                return;
            }
            // машины ещё нет у нас — пусть стоит, где пакет
        }
        else if(InVehicle(ped)) sync::WarpPedOut(ped);

        // пешком: близко — идём задачей игры (анимации ходьбы/бега), далеко — переносим
        CVector cur = GetPosition(ped);
        float vx = s.vel[0] / 100.0f, vy = s.vel[1] / 100.0f;
        CVector aim{ target.x + vx * 0.3f, target.y + vy * 0.3f, target.z };
        float d = Dist(cur, aim);
        if(d > 8.0f)
        {
            SetPosition(ped, target);
            Field<float>(ped, off::PedRotCurrent) = s.heading; Field<float>(ped, off::PedRotAiming) = s.heading;
            if(Field<uintptr_t>(ped, off::EntMatrix)) { MatrixSetRotateZOnly(Field<uintptr_t>(ped, off::EntMatrix), s.heading); }
            c.goUs = 0;
            return;
        }
        int move = s.moveState >= 7 ? 7 : s.moveState >= 6 ? 6 : s.moveState >= 4 ? 4 : 0;
        if(move && d > 0.5f)
        {
            if(!c.goUs || net::Since(g_now, c.goUs) > 400000 || Dist(aim, c.goTarget) > 1.5f || move != c.goMove)
            {
                void* t = NewTask();
                if(t)
                {
                    ((void(*)(void*, int, const CVector&, float, float, bool, bool))ctorGoTo)(t, move, aim, 0.5f, 2.0f, false, false);
                    Assign(ped, t);
                }
                c.goUs = g_now; c.goTarget = aim; c.goMove = move;
            }
        }
        else
        {
            if(c.goMove)
            {
                void* t = NewTask();
                if(t) { ((void(*)(void*, int, bool, bool, float))ctorStand)(t, 1000000, true, false, 8.0f); Assign(ped, t); }
                c.goMove = 0;
            }
            float diff = s.heading - Field<float>(ped, off::PedRotCurrent);
            while(diff > 3.1416f) diff -= 6.2832f;
            while(diff < -3.1416f) diff += 6.2832f;
            if(fabsf(diff) > 0.3f) { Field<float>(ped, off::PedRotAiming) = s.heading; }
            if(d > 0.5f) SetPosition(ped, CVector{ cur.x + (target.x - cur.x) * 0.2f, cur.y + (target.y - cur.y) * 0.2f, cur.z });
        }
    }

    void ClientFrame()
    {
        for(size_t i = 0; i < g_copies.size();)
        {
            Copy& c = g_copies[i];
            if(net::Since(g_now, c.stUs) > kCopyStaleUs) { DeleteCopy(c); g_copies.erase(g_copies.begin() + i); continue; }
            ApplyCopy(c);
            ++i;
        }
        static uint64_t nextLog = 0;
        if(g_now >= nextLog)
        {
            nextLog = g_now + 5000000;
            int alive = 0; for(auto& c : g_copies) if(c.ped) ++alive;
            Log(false, "[ЛЮДИ] друг: копий педов хоста %d (на карте %d)", (int)g_copies.size(), alive);
        }
    }

    void ClearCopies()
    {
        for(auto& c : g_copies) DeleteCopy(c);
        g_copies.clear();
    }
}

// Своё население у друга выключено, пока общий мир включён
DECL_HOOKv(PopulationUpdate, bool generate)    { if(g_popOff) return; PopulationUpdate(generate); }
DECL_HOOKv(GenerateRandomCars)                 { if(g_popOff) return; GenerateRandomCars(); }
DECL_HOOKv(CarGeneratorsProcess)               { if(g_popOff) return; CarGeneratorsProcess(); }
DECL_HOOKv(GenerateRoadBlocks)                 { if(g_popOff) return; GenerateRoadBlocks(); }
// Копии педов хоста не думают сами (не убегают, не дерутся): их поведение приходит с телефона хоста
DECL_HOOKv(HandleEvents, uintptr_t handler)
{
    if(g_popOff && handler && IsCopy(Field<uintptr_t>(handler, 0))) return;
    HandleEvents(handler);
}

bool Install(void* h, FLogger* log)
{
    g_log = log;
    auto sym = [&](const char* s) { return (void*)aml->GetSym(h, s); };
    g_pedPoolSym = (uintptr_t)sym("_ZN6CPools11ms_pPedPoolE");
    g_vehPoolSym = (uintptr_t)sym("_ZN6CPools15ms_pVehiclePoolE");
    AddPed     = (uintptr_t(*)(int, unsigned, const CVector&, bool))sym("_ZN11CPopulation6AddPedE8ePedTypejRK7CVectorb");
    RemovePed  = (void(*)(uintptr_t))sym("_ZN11CPopulation9RemovePedEP4CPed");
    ctorGoTo   = sym("_ZN34CTaskComplexGoToPointAndStandStillC2EiRK7CVectorffbb");
    ctorStand  = sym("_ZN21CTaskSimpleStandStillC2Eibbf");
    ctorKill   = sym("_ZN25CTaskComplexKillPedOnFootC2EP4CPedijjji");
    ctorArrest = sym("_ZN21CTaskComplexArrestPedC2EP4CPed");
    ArrestPlayer = (void(*)(uintptr_t))sym("_ZN11CPlayerInfo12ArrestPlayerEv");
    g_clockH = (uint8_t*)sym("_ZN6CClock18ms_nGameClockHoursE");
    g_clockM = (uint8_t*)sym("_ZN6CClock20ms_nGameClockMinutesE");
    g_wOld = (int16_t*)sym("_ZN8CWeather14OldWeatherTypeE");
    g_wNew = (int16_t*)sym("_ZN8CWeather14NewWeatherTypeE");
    g_wInterp = (float*)sym("_ZN8CWeather18InterpolationValueE");
    g_ok = g_pedPoolSym && g_vehPoolSym && AddPed && RemovePed && ctorGoTo && ctorStand && TaskNew && VehiclesReady() && CombatReady();
    if(!g_ok) { Log(true, "[ЛЮДИ] Не все функции найдены — у каждого свои педы (как раньше)"); return false; }
    struct HookDef { const char* sym; void* fn; void** orig; };
    const HookDef hooks[] = {
        { "_ZN11CPopulation6UpdateEb",              (void*)HookOf_PopulationUpdate,     (void**)&PopulationUpdate },
        { "_ZN8CCarCtrl18GenerateRandomCarsEv",     (void*)HookOf_GenerateRandomCars,   (void**)&GenerateRandomCars },
        { "_ZN17CTheCarGenerators7ProcessEv",       (void*)HookOf_CarGeneratorsProcess, (void**)&CarGeneratorsProcess },
        { "_ZN11CRoadBlocks18GenerateRoadBlocksEv", (void*)HookOf_GenerateRoadBlocks,   (void**)&GenerateRoadBlocks },
        { "_ZN13CEventHandler12HandleEventsEv",     (void*)HookOf_HandleEvents,         (void**)&HandleEvents },
    };
    for(const auto& d : hooks) if(!sym(d.sym)) { Log(true, "[ЛЮДИ] нет %s — у каждого свои педы", d.sym); g_ok = false; return false; }
    for(const auto& d : hooks) aml->Hook(sym(d.sym), d.fn, d.orig);
    Log(false, "[ЛЮДИ] Общий мир людей готов");
    return true;
}

void SetSession(net::Session* s) { g_session = s; }

void Frame(net::Role role, bool connected, uintptr_t local, uintptr_t remote, const net::PlayerStatePacket* rs, uint64_t now)
{
    if(!g_ok) return;
    g_role = role; g_now = now; g_local = local; g_remote = remote; g_rs = rs;
    bool wasActive = g_active;
    g_active = connected && local && (role == net::Role::Host || role == net::Role::Client);
    if(!g_active)
    {
        if(wasActive) Reset("связь пропала");
        return;
    }
    float apart = rs ? Dist(GetPosition(local), CVector{ rs->pos[0], rs->pos[1], rs->pos[2] }) : 1e9f;
    static bool near = false;
    bool wasNear = near;
    near = near ? apart < kShareOff : apart < kShareOn;
    if(near != wasNear)
        Log(false, "[ЛЮДИ] %s (между вами %.0f м)", near ? "вы рядом — люди и машины общие" : "разъехались — у каждого свои люди", apart);
    if(role == net::Role::Host)
    {
        HostTime();
        if(!near) return;
        if(now >= g_nextSendUs) { g_nextSendUs = now + kSendUs; HostSend(); }
        HostCops();
    }
    else
    {
        if(!near)
        {
            if(g_popOff) { g_popOff = false; ClearCopies(); Log(false, "[ЛЮДИ] Своё население снова включено"); }
            return;
        }
        if(!g_popOff) { g_popOff = true; ClearOwnPopulation(); }
        ClientFrame();
    }
}

void OnPacket(uint8_t type, const uint8_t* data, int len)
{
    if(!g_ok || g_role != net::Role::Client) return;
    if(type == net::PKT_PEDS && len >= net::kPedsHeaderSize)
    {
        const auto* p = (const net::PedsPacket*)data;
        if(p->count > net::kPedsPerPacket || net::kPedsHeaderSize + p->count * (int)sizeof(net::PedEntry) != len) return;
        for(int i = 0; i < p->count; ++i)
        {
            const net::PedEntry& e = p->peds[i];
            Copy* c = FindCopy(e.id);
            if(!c) { Copy nc; nc.id = e.id; g_copies.push_back(nc); c = &g_copies.back(); }
            c->st = e; c->stUs = g_now ? g_now : net::NowUs();
        }
    }
    else if(type == net::PKT_WORLD_TIME && len == sizeof(net::WorldTimePacket))
    {
        const auto* t = (const net::WorldTimePacket*)data;
        if(g_clockH) { *g_clockH = t->hours; *g_clockM = t->minutes; }
        if(g_wOld) { *g_wOld = t->oldWeather; *g_wNew = t->newWeather; if(g_wInterp) *g_wInterp = t->interp; }
    }
}

void Reset(const char* why)
{
    if(!g_copies.empty() || g_popOff) Log(false, "[ЛЮДИ] Общий мир людей выключен (%s)", why);
    ClearCopies();
    g_popOff = false;
    g_tracks.clear();
    g_active = false;
}

bool Active() { return g_active; }
bool ClientMode() { return g_active && g_role == net::Role::Client; }

uint16_t TrackedId(uintptr_t ped)
{
    if(!g_active || g_role != net::Role::Host) return 0;
    Track* t = FindTrack(ped);
    return t ? t->id : 0;
}
bool IsCopy(uintptr_t ped) { return CopyId(ped) != 0; }
uint16_t CopyId(uintptr_t ped)
{
    if(!ped) return 0;
    for(auto& c : g_copies) if(c.ped == ped) return c.id;
    return 0;
}
uintptr_t CopyById(uint16_t id)
{
    Copy* c = FindCopy(id);
    return (c && CopyValid(*c)) ? c->ped : 0;
}
bool AllowCopyDamage() { return g_allowCopyDamage; }
uintptr_t TrackedPed(uint16_t id)
{
    for(auto& t : g_tracks) if(t.id == id && PedValid(t.ped, t.ref)) return t.ped;
    return 0;
}

bool RemoteArrestable()
{
    return g_active && g_role == net::Role::Host && g_rs && g_rs->wanted > 0;
}

// Друг: «Арестован» по событию хоста
void OnBusted(uintptr_t local)
{
    if(!ArrestPlayer || !local || !Players) return;
    Log(false, "[ЛЮДИ] Копы хоста скрутили тебя — «Арестован»");
    ArrestPlayer(Players);   // CWorld::Players[0]
}

}}
