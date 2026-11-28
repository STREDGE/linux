// Розыск NPC и подмога. Подозреваемый для города — как CJ с розыском в обычной игре: по звёздам приезжают патрули,
// вертолёт, S.W.A.T., FBI, армия — «из-за угла», вне поля зрения. Герой — на стороне полиции.
#include "wanted.h"

#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <swat/sgame.h>

#include "base.h"
#include "layout.h"
#include "log.h"
#include "story.h"
#include "ui.h"

using namespace swat;

namespace wanted
{
namespace
{
    uint32_t Now() { return NowMs(); }
    bool Ago(uint32_t t, uint32_t ms) { uint32_t n = Now(); return n < t || n - t >= ms; }

    struct Suspect
    {
        int ref = 0, stars = 0, blip = 0;
        bool surrendered = false, custody = false, byPolice = false, call = false;
        uint32_t nextWave = 0, taskAt = 0, aimedSince = 0, policeNear = 0;
        std::string why;
    };
    std::map<int, Suspect> g_sus;

    struct Unit { int ref = 0, car = 0, target = 0; int kind = 0; uint32_t born = 0, taskAt = 0; bool onFoot = false, backup = false; V3 guard; };
    std::vector<Unit> g_units;
    std::vector<int> g_cars;

    // вызов (кабинет Фрэнка)
    struct Call { bool on = false; int kind = 0; V3 at; int blip = 0; bool spawned = false; uint32_t start = 0; std::vector<int> refs; std::string text; };
    Call g_call;

    bool LoadModels(const std::vector<int>& m)
    {
        bool all = true;
        for(int x : m) if(!Is(0x0248, { Ai(x) })) { Do(0x0247, { Ai(x) }); all = false; }
        return all;
    }
    void Unload(const std::vector<int>& m) { for(int x : m) Do(0x0249, { Ai(x) }); }

    // точка «из-за угла»: узел дороги на нужном расстоянии, которого не видно на экране
    bool OffscreenNode(const V3& near, float minD, float maxD, V3& out, float& heading)
    {
        for(int tries = 0; tries < 12; ++tries)
        {
            V3 probe = Add(near, Mul(Dir(Rnd(0, 360)), Rnd(minD, maxD)));
            V3 n; float h = 0;
            if(!NthCarNode(probe, RndI(1, 3), n, h)) continue;
            float d = Dist2(n, near);
            if(d < minD * 0.7f || d > maxD * 1.4f) continue;
            if(OnScreen(Add(n, Mk(0, 0, 1)), 4.0f)) continue;
            out = n; heading = h; return true;
        }
        return false;
    }

    struct Squad { int car; int ped; int weapon; int count; int pedType; };
    Squad SquadFor(int kind)
    {
        switch(kind)
        {
            case 1: return { 427, 285, 31, 4, 6 };   // S.W.A.T.
            case 2: return { 490, 286, 29, 3, 6 };   // FBI
            case 3: return { 470, 287, 31, 3, 6 };   // армия (Patriot)
            case 4: return { 497, 280, 29, 2, 6 };   // полицейский вертолёт
            case 5: return { 416, 274, 0, 2, 18 };   // скорая
            default: return { 596, 280, 22, 2, 6 }; // патруль
        }
    }

    // Отряд приезжает: машина «из-за угла» + люди внутри; target — кого ловить (0 — охранять точку)
    bool SpawnUnit(int kind, const V3& near, int target, bool backup)
    {
        Squad sq = SquadFor(kind);
        if(!LoadModels({ sq.car, sq.ped })) { Do(0x038B, {}); }                             // LOAD_ALL_MODELS_NOW — подмога нужна сейчас
        V3 at; float h = 0;
        bool heli = kind == 4;
        if(heli) { at = Add(near, Mul(Dir(Rnd(0, 360)), 140.0f)); at.z = near.z + 45; }
        else if(!OffscreenNode(near, 70, 140, at, h)) { LOG("РОЗЫСК", "подмога (%d): нет точки «из-за угла» — позже", kind); return false; }
        int car = OutI(0x00A5, { Ai(sq.car), Af(at.x), Af(at.y), Af(at.z + (heli ? 0 : 0.5f)), Aoi() });
        if(car <= 0) return false;
        Do(0x0175, { Ai(car), Af(heli ? Heading(at, near) : h) });
        if(!heli && kind != 5) Do(0x0397, { Ai(car), Ai(1) });                                   // SWITCH_CAR_SIREN
        g_cars.push_back(car);
        for(int i = 0; i < sq.count; ++i)
        {
            int ped = i == 0 ? OutI(0x0129, { Ai(car), Ai(sq.pedType), Ai(sq.ped), Aoi() })     // CREATE_CHAR_INSIDE_CAR
                             : OutI(0x01C8, { Ai(car), Ai(sq.pedType), Ai(sq.ped), Ai(i - 1), Aoi() });   // CREATE_CHAR_AS_PASSENGER
            if(ped <= 0) continue;
            if(sq.weapon) { swat::GiveWeapon(ped, sq.weapon, 600); Do(0x01B9, { Ai(ped), Ai(sq.weapon) }); }
            Do(0x02E2, { Ai(ped), Ai(kind >= 1 && kind <= 3 ? 70 : 50) });
            Do(0x0961, { Ai(ped), Ai(1) });                                                     // SET_CHAR_KEEP_TASK
            Unit u; u.ref = ped; u.car = car; u.target = target; u.kind = kind; u.born = Now(); u.backup = backup; u.guard = near;
            g_units.push_back(u);
        }
        // водитель едет к цели (вертолёт — следует сверху)
        int driver = g_units.empty() ? 0 : g_units.back().ref;
        for(auto& u : g_units) if(u.car == car) { driver = u.ref; break; }
        if(heli)
        {
            Do(0x0825, { Ai(car) });                                                            // SET_HELI_BLADES_FULL_SPEED
            if(target > 0) Do(0x0726, { Ai(car), Ai(target), Ai(-1), Af(25.0f) });               // HELI_FOLLOW_ENTITY
            else Do(0x04A2, { Ai(car), Af(near.x), Af(near.y), Af(near.z + 30), Af(25.0f), Af(50.0f) });
        }
        else
        {
            V3 to = target > 0 ? RefPos(target) : near;
            Do(0x05D1, { Ai(driver), Ai(car), Af(to.x), Af(to.y), Af(to.z), Af(30.0f), Ai(0), Ai(-1), Ai(2) });   // TASK_CAR_DRIVE_TO_COORD
        }
        Unload({ sq.car, sq.ped });
        static const char* names[] = { "патруль", "S.W.A.T.", "FBI", "армия", "вертолёт", "скорая" };
        LOG("РОЗЫСК", "%s «%s» едет из-за угла (%.0f м)%s", backup ? "подмога" : "реакция города", names[kind < 0 || kind > 5 ? 0 : kind], Dist2(at, near), target ? "" : " — к точке");
        return true;
    }

    int UnitsFor(int target) { int n = 0; for(auto& u : g_units) if(u.target == target && RefAlive(u.ref)) n++; return n; }

    // волна реакции города по звёздам (1 патруль … 4 S.W.A.T. … 5 FBI … 6 армия)
    void Wave(Suspect& s)
    {
        V3 at = RefPos(s.ref);
        if(UnitsFor(s.ref) >= 2 + s.stars * 2 || (int)g_units.size() > 16) return;
        switch(s.stars)
        {
            case 1: SpawnUnit(0, at, s.ref, false); break;
            case 2: SpawnUnit(0, at, s.ref, false); SpawnUnit(0, at, s.ref, false); break;
            case 3: SpawnUnit(0, at, s.ref, false); SpawnUnit(4, at, s.ref, false); break;
            case 4: SpawnUnit(1, at, s.ref, false); SpawnUnit(0, at, s.ref, false); break;
            case 5: SpawnUnit(2, at, s.ref, false); SpawnUnit(2, at, s.ref, false); break;
            default: SpawnUnit(3, at, s.ref, false); SpawnUnit(1, at, s.ref, false); break;
        }
    }

    // кого бить подмоге: стреляющие/целящиеся в Артёма и разыскиваемые рядом
    int ThreatNear(const V3& at, float r)
    {
        int best = 0; float bd = r;
        uintptr_t pl = Player();
        ForEachPed([&](uintptr_t p) {
            if(!PedAlive(p) || p == pl) return;
            int t = PedType(p); if(t == 6 || t == 18) return;
            int ref = Ref(p);
            auto it = g_sus.find(ref);
            bool wanted = it != g_sus.end() && it->second.stars > 0 && !it->second.surrendered && !it->second.custody;
            uintptr_t aim = AimTarget(p);
            bool hostile = aim && (aim == pl || PedType(aim) == 6);
            if(!wanted && !hostile && !(Shooting(ref) && Dist(Pos(p), Pos(pl)) < 40)) return;
            float d = Dist(Pos(p), at);
            if(d < bd) { bd = d; best = ref; }
        });
        return best;
    }

    void UnitsFrame()
    {
        V3 pp = Pos(Player());
        for(auto it = g_units.begin(); it != g_units.end();)
        {
            Unit& u = *it;
            if(!Is(0x056D, { Ai(u.ref) }) || !RefAlive(u.ref))
            {   // погиб/исчез — отпускаем
                if(Is(0x056D, { Ai(u.ref) })) Do(0x01C2, { Ai(u.ref) });
                it = g_units.erase(it); continue;
            }
            uintptr_t p = Ped(u.ref);
            V3 me = Pos(p);
            // цель: подозреваемый или угроза у точки охраны
            if(u.target > 0)
            {
                auto s = g_sus.find(u.target);
                if(s == g_sus.end() || !RefAlive(u.target) || s->second.custody || s->second.stars <= 0) u.target = 0;
            }
            if(u.target == 0 && u.backup) u.target = ThreatNear(u.guard, 70);
            bool done = u.target == 0 && (!u.backup || Ago(u.born, 70000));
            if(done && Dist(me, pp) > 60 && !OnScreen(me, 2))
            {   // работа сделана — уезжают (игрок не видит — убираем)
                Do(0x009B, { Ai(u.ref) });
                it = g_units.erase(it); continue;
            }
            if(!Ago(u.taskAt, 3500)) { ++it; continue; }
            u.taskAt = Now();
            uintptr_t v = PedVehicle(p);
            V3 goal = u.target ? RefPos(u.target) : (u.backup ? u.guard : pp);
            if(u.kind == 4) { ++it; continue; }                                                   // вертолёт ведёт сама команда следования
            if(v)
            {
                float d = Dist2(me, goal);
                if(d < 28 || Speed(v) < 0.5f) { Do(0x0633, { Ai(u.ref) }); }                     // TASK_LEAVE_ANY_CAR — приехали
                else if(VehDriver(v) == p) Do(0x05D1, { Ai(u.ref), Ai(VehRef(v)), Af(goal.x), Af(goal.y), Af(goal.z), Af(30.0f), Ai(0), Ai(-1), Ai(2) });
                ++it; continue;
            }
            if(u.kind == 5)
            {   // скорая: к Артёму, подлечить
                if(Dist(me, pp) > 3) Do(0x05D3, { Ai(u.ref), Af(pp.x), Af(pp.y), Af(pp.z), Ai(6), Ai(10000) });
                else { Do(0x0605, { Ai(u.ref), As("CPR"), As("MEDIC"), Af(4), Ai(0), Ai(0), Ai(0), Ai(0), Ai(3000) }); Do(0x0223, { Ai(PlayerRef()), Ai(176) }); u.target = 0; u.backup = false; }
                ++it; continue;
            }
            if(u.target)
            {
                auto s = g_sus.find(u.target);
                int stars = s != g_sus.end() ? s->second.stars : 3;
                bool surrendered = s != g_sus.end() && s->second.surrendered;
                if(stars >= 3 && !surrendered) Do(0x05E2, { Ai(u.ref), Ai(u.target) });          // TASK_KILL_CHAR_ON_FOOT
                else Do(0x07A3, { Ai(u.ref), Ai(u.target), Af(2.0f), Af(4.0f) });               // TASK_GOTO_CHAR_AIMING — задержать
            }
            else if(u.backup)
            {
                if(Dist(me, u.guard) > 6) Do(0x05D3, { Ai(u.ref), Af(u.guard.x + Rnd(-4, 4)), Af(u.guard.y + Rnd(-4, 4)), Af(u.guard.z), Ai(6), Ai(10000) });
                else Do(0x0667, { Ai(u.ref), Af(u.guard.x + Rnd(-20, 20)), Af(u.guard.y + Rnd(-20, 20)), Af(u.guard.z + 1), Ai(4000) });   // TASK_AIM_GUN_AT_COORD
            }
            ++it;
        }
        // машины, из которых все вышли и которые далеко — отпустить
        for(auto it = g_cars.begin(); it != g_cars.end();)
        {
            bool used = false; for(auto& u : g_units) if(u.car == *it) used = true;
            if(!used) { if(Is(0x056E, { Ai(*it) })) Do(0x01C3, { Ai(*it) }); it = g_cars.erase(it); } else ++it;
        }
    }

    void RemoveSuspect(int ref, const char* why)
    {
        auto it = g_sus.find(ref);
        if(it == g_sus.end()) return;
        if(it->second.blip > 0) Do(0x0164, { Ai(it->second.blip) });
        if(Is(0x056D, { Ai(ref) })) Do(0x09B6, { Ai(ref), Ai(0) });                               // SET_CHAR_WANTED_BY_POLICE 0
        LOG("РОЗЫСК", "#%d снят с розыска: %s", ref, why);
        g_sus.erase(it);
    }

    void SuspectsFrame()
    {
        uintptr_t pl = Player(); V3 pp = Pos(pl);
        int plRef = PlayerRef();
        bool showArrest = false; int arrestRef = 0;
        std::vector<int> dead;
        for(auto& kv : g_sus)
        {
            Suspect& s = kv.second;
            if(!Is(0x056D, { Ai(s.ref) }) || !RefAlive(s.ref)) { dead.push_back(s.ref); continue; }
            uintptr_t p = Ped(s.ref);
            V3 sp = Pos(p);
            float d = Dist(sp, pp);
            if(d < 70 && s.stars > 0) ui::Stars(layout::G(sp), s.stars, s.surrendered || s.custody);
            if(s.custody)
            {   // задержан: идёт за Артёмом, садится к нему в машину
                if(Ago(s.taskAt, 2000))
                {
                    s.taskAt = Now();
                    uintptr_t pv = PedVehicle(pl);
                    if(pv && !PedVehicle(p)) Do(0x05CA, { Ai(s.ref), Ai(VehRef(pv)), Ai(10000), Ai(VehPassenger(pv, 1) ? 2 : 1) });   // заднее сиденье
                    else if(!pv && PedVehicle(p)) Do(0x0633, { Ai(s.ref) });
                    else if(!pv && d > 2.5f) Do(0x06A8, { Ai(s.ref), Ai(plRef), Ai(-1), Af(1.6f), Af(160.0f) });   // TASK_GOTO_CHAR_OFFSET
                }
                if(d > 120) { s.custody = false; s.surrendered = false; ui::Notice("Задержанный сбежал!"); LOG("РОЗЫСК", "#%d сбежал из-под конвоя", s.ref); }
                continue;
            }
            // сдаётся: Артём держит на прицеле рядом (1–2 зв. — сразу, 3+ — если ранен/без оружия)
            bool aimed = d < 14 && Is(0x0457, { Ai(0), Ai(s.ref) });                             // IS_PLAYER_TARGETTING_CHAR
            if(aimed && !s.surrendered)
            {
                if(!s.aimedSince) s.aimedSince = Now();
                bool weak = s.stars <= 2 || Health(p) < 50 || CurWeapon(p) < 22;
                if(weak && Ago(s.aimedSince, 1200)) { Surrendered(s.ref); }
            }
            else if(!aimed) s.aimedSince = 0;
            if(s.surrendered)
            {
                if(Ago(s.taskAt, 7000)) { s.taskAt = Now(); Do(0x05C4, { Ai(s.ref), Ai(8000) }); }       // TASK_HANDS_UP
                if(d < 3.0f && !PedVehicle(pl)) { showArrest = true; arrestRef = s.ref; }
                // полиция рядом — задержит сама
                bool cop = false; for(auto& u : g_units) if(u.target == s.ref && Dist(RefPos(u.ref), sp) < 3) cop = true;
                if(cop) { if(!s.policeNear) s.policeNear = Now(); if(Ago(s.policeNear, 6000) && d > 8) { s.byPolice = true; dead.push_back(s.ref); ui::Notice("Полиция задержала подозреваемого"); } }
                continue;
            }
            // поведение: бежать или драться (как CJ против копов)
            if(Ago(s.taskAt, 7000) && d < 160)
            {
                s.taskAt = Now();
                uintptr_t v = PedVehicle(p);
                if(v && VehDriver(v) == p)
                {
                    V3 away = Add(sp, Mul(Norm2(Sub(sp, pp)), 400.0f));
                    Do(0x05D1, { Ai(s.ref), Ai(VehRef(v)), Af(away.x), Af(away.y), Af(away.z), Af(38.0f), Ai(0), Ai(-1), Ai(2) });
                }
                else if(s.stars >= 3 && CurWeapon(p) >= 22) Do(0x05E2, { Ai(s.ref), Ai(plRef) });   // отстреливается
                else Do(0x05DB, { Ai(s.ref), Ai(plRef), Af(90.0f), Ai(12000) });                  // TASK_FLEE_CHAR
            }
            // реакция города волнами
            if(Now() >= s.nextWave && d < 260) { s.nextWave = Now() + (uint32_t)(42000 - s.stars * 4000); Wave(s); }
        }
        for(int r : dead)
        {
            auto it = g_sus.find(r);
            bool police = it != g_sus.end() && it->second.byPolice;
            if(police && Is(0x056D, { Ai(r) })) Do(0x009B, { Ai(r) });
            RemoveSuspect(r, police ? "задержан полицией" : "погиб/исчез");
        }
        if(showArrest) ui::ActionButton(0, "Задержать", [arrestRef]() {
            auto it = g_sus.find(arrestRef);
            if(it == g_sus.end()) return;
            int me = PlayerRef();
            Do(0x0605, { Ai(me), As("plc_drgbst_02"), As("POLICE"), Af(4), Ai(0), Ai(0), Ai(0), Ai(0), Ai(2000) });
            Do(0x0605, { Ai(arrestRef), As("crm_drgbst_01"), As("POLICE"), Af(4), Ai(0), Ai(0), Ai(0), Ai(1), Ai(2000) });
            Arrested(arrestRef);
            ui::Subtitle("Артём", "Ты задержан. Пошли.", 2.5f, 0x5AAAFF);
        });
    }

    // ---------------- вызовы ----------------
    const char* kCallText[] = {
        "Диспетчер: вооружённое ограбление, подозреваемый скрылся пешком. 2 звезды.",
        "Диспетчер: угон машины, подозреваемый за рулём. 2 звезды.",
        "Диспетчер: стрельба на улице, двое вооружённых. 3 звезды.",
        "Диспетчер: торговец на углу, без оружия. 1 звезда.",
        "Диспетчер: перестрелка банд, автоматы. 4 звезды.",
    };
    void CallFrame()
    {
        if(!g_call.on) return;
        V3 pp = Pos(Player());
        if(!g_call.spawned && Dist2(pp, g_call.at) < 220)
        {
            std::vector<int> models = { 102, 103, 104, 105, 106, 410, 412 };
            if(!LoadModels(models)) return;
            g_call.spawned = true;
            int n = g_call.kind == 2 ? 2 : (g_call.kind == 4 ? 4 : 1);
            int stars = g_call.kind == 3 ? 1 : (g_call.kind == 2 ? 3 : (g_call.kind == 4 ? 4 : 2));
            int car = 0;
            if(g_call.kind == 1)
            {
                car = OutI(0x00A5, { Ai(RndI(0, 1) ? 410 : 412), Af(g_call.at.x), Af(g_call.at.y), Af(g_call.at.z + 0.5f), Aoi() });
                if(car > 0) { int d = OutI(0x0129, { Ai(car), Ai(20), Ai(105), Aoi() }); if(d > 0) { g_call.refs.push_back(d); SetStars(d, stars, "угон"); } Do(0x01C3, { Ai(car) }); }
            }
            else
                for(int i = 0; i < n; ++i)
                {
                    V3 at = Add(g_call.at, Mk(Rnd(-4, 4), Rnd(-4, 4), 0.5f));
                    int ped = OutI(0x009A, { Ai(g_call.kind == 4 ? 7 + i % 2 * 2 : 20), Ai(models[i % 5]), Af(at.x), Af(at.y), Af(at.z), Aoi() });
                    if(ped <= 0) continue;
                    int w = g_call.kind == 3 ? 0 : (g_call.kind == 4 ? 30 : (g_call.kind == 2 ? 28 : 22));
                    if(w) swat::GiveWeapon(ped, w, 300);
                    g_call.refs.push_back(ped);
                    SetStars(ped, stars, g_call.kind == 4 ? "перестрелка банд" : (g_call.kind == 2 ? "стрельба" : (g_call.kind == 3 ? "торговля" : "ограбление")));
                }
            Unload(models);
            if(g_call.blip > 0) { Do(0x0164, { Ai(g_call.blip) }); g_call.blip = 0; }
            LOG("ВЫЗОВ", "на месте: %d подозреваемых", (int)g_call.refs.size());
        }
        if(g_call.spawned)
        {
            int left = 0;
            for(int r : g_call.refs) { auto it = g_sus.find(r); if(it != g_sus.end() && !it->second.custody && RefAlive(r)) left++; }
            if(left == 0)
            {
                int reward = 300 + 250 * (int)g_call.refs.size();
                game::AddMoney(reward); base::AddEarned(reward);
                ui::Result(true, "ВЫЗОВ ЗАКРЫТ", "+$" + std::to_string(reward));
                Do(0x0394, { Ai(1) });                                                           // PLAY_MISSION_PASSED_TUNE
                LOG("ВЫЗОВ", "закрыт, награда $%d", reward);
                g_call = Call();
            }
        }
        if(g_call.on && Ago(g_call.start, 420000))
        {
            ui::Notice("Вызов отменён: время вышло");
            if(g_call.blip > 0) Do(0x0164, { Ai(g_call.blip) });
            LOG("ВЫЗОВ", "отменён по времени");
            g_call = Call();
        }
    }

    // кнопка «Рация»
    void RadioButton()
    {
        if(ui::MenuOpen() || game::CutsceneRunning()) return;
        ui::ActionButton(2, "Рация", []() {
            ui::OpenMenu("РАЦИЯ — ПОДМОГА", { "Полиция", "S.W.A.T.", "FBI", "Армия", "Вертолёт", "Скорая", "Тревога на базе" }, [](int i) {
                if(i < 0) return;
                ui::CloseMenu();
                V3 p = Pos(Player());
                if(i == 6) { base::RaiseAlarm("Артём по рации: тревога на базе"); return; }
                CallBackup(i, p.x, p.y, p.z, "рация Артёма");
            }, "Подмога появится из-за угла рядом с тобой.");
        });
    }
}

void Init() {}
void Reset() { g_sus.clear(); g_units.clear(); g_cars.clear(); g_call = Call(); }

int Stars(int ref) { auto it = g_sus.find(ref); return it == g_sus.end() ? 0 : it->second.stars; }
void SetStars(int ref, int stars, const char* why)
{
    if(ref <= 0) return;
    if(stars <= 0) { RemoveSuspect(ref, why ? why : "снят"); return; }
    if(stars > 6) stars = 6;
    Suspect& s = g_sus[ref];
    bool fresh = s.ref == 0;
    s.ref = ref; s.stars = stars; s.why = why ? why : "";
    if(fresh) { s.nextWave = Now() + 6000; s.blip = OutI(0x0187, { Ai(ref), Aoi() }); }          // ADD_BLIP_FOR_CHAR (красная)
    Do(0x09B6, { Ai(ref), Ai(1) });                                                               // SET_CHAR_WANTED_BY_POLICE
    LOG("РОЗЫСК", "#%d: %d зв. (%s)", ref, stars, s.why.c_str());
}
void CallBackup(int kind, float x, float y, float z, const char* why)
{
    V3 at = Mk(x, y, z);
    if(kind < 0 || kind > 5) kind = 0;
    bool ok = SpawnUnit(kind, at, 0, true);
    if(kind == 1 || kind == 3) ok = SpawnUnit(0, at, 0, true) || ok;
    static const char* said[] = { "Центр: патруль выехал.", "Центр: S.W.A.T. на подходе.", "Центр: FBI выдвигается.",
                                  "Центр: армия выдвигается.", "Центр: вертолёт поднят.", "Центр: скорая едет." };
    ui::Subtitle("Рация", ok ? said[kind] : "Центр: все заняты, держитесь!", 3.0f, 0x9AD08A);
    LOG("РАЦИЯ", "подмога %d (%s): %s", kind, why ? why : "", ok ? "выехала" : "не вышло");
}
bool StartCall()
{
    if(g_call.on) return false;
    V3 pp = Pos(Player());
    V3 at; float h;
    // в Лос-Сантосе, 400–900 м от базы
    V3 base = Mk(layout::kCx, layout::kCy, layout::kZ);
    bool ok = false;
    for(int i = 0; i < 10 && !ok; ++i)
    {
        V3 probe = Add(base, Mul(Dir(Rnd(0, 360)), Rnd(400, 900)));
        if(probe.x < 100 || probe.x > 2900 || probe.y < -2800 || probe.y > -900) continue;
        ok = NthCarNode(probe, 1, at, h);
    }
    if(!ok) return false;
    (void)pp;
    g_call = Call(); g_call.on = true; g_call.kind = RndI(0, 4); g_call.at = at; g_call.start = Now(); g_call.text = kCallText[g_call.kind];
    g_call.blip = OutI(0x018A, { Af(at.x), Af(at.y), Af(at.z), Aoi() });                         // ADD_BLIP_FOR_COORD
    ui::Subtitle("Рация", g_call.text, 6.0f, 0x9AD08A);
    LOG("ВЫЗОВ", "%s Место (%.0f, %.0f)", g_call.text.c_str(), at.x, at.y);
    return true;
}
void Surrendered(int ref)
{
    auto it = g_sus.find(ref); if(it == g_sus.end()) return;
    if(it->second.surrendered) return;
    it->second.surrendered = true; it->second.taskAt = 0;
    Do(0x0687, { Ai(ref) });
    Do(0x05C4, { Ai(ref), Ai(8000) });
    Do(0x048F, { Ai(ref) });                                                                      // REMOVE_ALL_CHAR_WEAPONS
    LOG("РОЗЫСК", "#%d сдался", ref);
}
void Arrested(int ref)
{
    auto it = g_sus.find(ref);
    if(it == g_sus.end()) { SetStars(ref, 1, "задержан"); it = g_sus.find(ref); if(it == g_sus.end()) return; }
    it->second.custody = true; it->second.surrendered = true; it->second.taskAt = 0;
    if(it->second.blip > 0) { Do(0x0165, { Ai(it->second.blip), Ai(2) }); }                       // метка зелёная
    Do(0x09B6, { Ai(ref), Ai(0) });
    ui::Objective("Отвези задержанного в камеру базы", 6.0f);
    LOG("РОЗЫСК", "#%d задержан — вести в камеру базы", ref);
}
void Delivered(int ref)
{
    auto it = g_sus.find(ref);
    int stars = it == g_sus.end() ? 1 : it->second.stars;
    int reward = std::min(1500, 300 + 200 * stars);
    base::DeliverPrisoner(ref);
    game::AddMoney(reward); base::AddEarned(reward);
    ui::Notice("Задержанный в камере: +$" + std::to_string(reward), 5.0f);
    Do(0x0394, { Ai(1) });
    LOG("РОЗЫСК", "#%d в камере, награда $%d", ref, reward);
    if(it != g_sus.end()) { if(it->second.blip > 0) Do(0x0164, { Ai(it->second.blip) }); g_sus.erase(it); }
    for(auto& r : g_call.refs) if(r == ref) r = -r;
}
void DeliverAtCell()
{
    int n = 0;
    std::vector<int> refs;
    for(auto& kv : g_sus) if(kv.second.custody && Dist(RefPos(kv.first), Pos(Player())) < 20) refs.push_back(kv.first);
    for(int r : refs) { if(PedVehicle(Ped(r))) Do(0x0633, { Ai(r) }); Delivered(r); n++; }
    if(!n) ui::Notice("Здесь сдают задержанных. Привези подозреваемого (Задержать — когда он сдался).");
}
int Custody() { int n = 0; for(auto& kv : g_sus) if(kv.second.custody) n++; return n; }
void BeforeSave()
{
    // в сохранение не тащим подозреваемых и подмогу
    for(auto& u : g_units) if(Is(0x056D, { Ai(u.ref) })) Do(0x01C2, { Ai(u.ref) });
    for(int c : g_cars) if(Is(0x056E, { Ai(c) })) Do(0x01C3, { Ai(c) });
    for(auto& kv : g_sus) { if(kv.second.blip > 0) Do(0x0164, { Ai(kv.second.blip) }); if(Is(0x056D, { Ai(kv.first) })) Do(0x01C2, { Ai(kv.first) }); }
    if(g_call.blip > 0) Do(0x0164, { Ai(g_call.blip) });
    Reset();
}

void Frame()
{
    if(!Player()) return;
    WHERE("розыск: подозреваемые"); SuspectsFrame();
    WHERE("розыск: подмога"); UnitsFrame();
    WHERE("розыск: вызов"); CallFrame();
    RadioButton();
}
}
