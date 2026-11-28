// Розыск NPC и подмога (v2, SA Story 0.4.0). Подозреваемый для города — как CJ с розыском в обычной игре.
//
// Что поменялось после первых игр (лог 16:25–16:29):
//  * вылет: полицейские шли «задерживать» прохожего с прицелом (задача игры «идти к цели целясь»), а игра удалила
//    этого прохожего как ненужного — задача обратилась к пустоте. Теперь подозреваемый становится «сюжетным»
//    (игра его не удаляет), мы никого не удаляем, пока на него кто-то нацелен, и задержание — родное (06E4);
//    плюс страховка в story.cpp (перехват задачи игры).
//  * автосохранение (оно идёт на каждый скриншот) раньше стирало всех подозреваемых и подмогу. Теперь — нет.
//  * выдача: цель — тот, на кого смотрит камера (человек или машина — тогда водитель), до 80 м, или в захвате прицела.
//  * подозреваемый убегает родной задачей «бежать любым способом» (0751): пешком, на машине, угоняет, отстреливается.
//  * полиция: машины преследуют машину подозреваемого (родные задания игры: таран/блок), пешие — задерживают
//    (родное задержание) или стреляют (3+ звезды); вертолёт — родная погоня полицейского вертолёта (0727).
//  * подмога по рации: едет к Артёму, охраняет его и идёт следом, вертолёт держится над ним и бьёт угрозы;
//    «Отпустить подмогу» в рации; через 3 минуты уезжают сами. Никто не исчезает на глазах.
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
        bool surrendered = false, custody = false;
        uint32_t nextWave = 0, taskAt = 0, aimedSince = 0, copsNearSince = 0, givenAt = 0, lostSince = 0;
        int fleeStars = -1;                     // с какими звёздами выдана задача бегства (-1 — ещё не выдана)
        std::string why;
    };
    std::map<int, Suspect> g_sus;

    struct Unit
    {
        int ref = 0, car = 0, target = 0, kind = 0, seat = 0;   // seat: 0 — водитель/пилот, 1… — пассажир
        uint32_t born = 0, taskAt = 0, doneAt = 0, orderAt = 0;
        bool backup = false, leaving = false;
        int order = 0, orderArg = 0;            // последний выданный приказ (чтобы не дёргать каждые 2 с)
    };
    std::vector<Unit> g_units;
    std::vector<int> g_cars;
    bool g_release = false;                     // «Отпустить подмогу»

    struct Call { bool on = false; int kind = 0; V3 at; int blip = 0; bool spawned = false; uint32_t start = 0; std::vector<int> refs; std::string text; };
    Call g_call;

    bool LoadModels(const std::vector<int>& m)
    {
        bool all = true;
        for(int x : m) if(!Is(0x0248, { Ai(x) })) { Do(0x0247, { Ai(x) }); all = false; }
        return all;
    }
    void Unload(const std::vector<int>& m) { for(int x : m) Do(0x0249, { Ai(x) }); }

    int CarOf(int ref) { uintptr_t p = Ped(ref); uintptr_t v = p ? PedVehicle(p) : 0; return v ? VehRef(v) : -1; }
    bool Drives(int ref) { uintptr_t p = Ped(ref); uintptr_t v = p ? PedVehicle(p) : 0; return v && VehDriver(v) == p; }

    // приказ один раз (повтор — только если поменялся или прошло ms)
    bool Order(Unit& u, int order, int arg, uint32_t ms)
    {
        if(u.order == order && u.orderArg == arg && !Ago(u.orderAt, ms)) return false;
        u.order = order; u.orderArg = arg; u.orderAt = Now();
        return true;
    }

    // перед тем как отпустить/убрать человека — все наши, кто на него нацелен, бросают задачу (иначе задачи игры
    // обращаются к пустому месту — это и был вылет)
    void ClearTargeting(int ref)
    {
        for(auto& u : g_units)
            if(u.target == ref)
            {
                u.target = 0; u.order = 0; u.taskAt = 0;
                if(Is(0x056D, { Ai(u.ref) }) && RefAlive(u.ref)) Do(0x0687, { Ai(u.ref) });   // CLEAR_CHAR_TASKS
            }
    }

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
            case 1: return { 427, 285, 31, 4, 6 };   // S.W.A.T. (Enforcer)
            case 2: return { 490, 286, 29, 3, 6 };   // FBI (Rancher)
            case 3: return { 470, 287, 31, 3, 6 };   // армия (Patriot)
            case 4: return { 497, 285, 31, 3, 6 };   // полицейский вертолёт: пилот + 2 стрелка S.W.A.T.
            case 5: return { 416, 274, 0, 2, 18 };   // скорая
            default: return { 596, 280, 22, 2, 6 }; // патруль
        }
    }
    const char* KindName(int k) { static const char* n[] = { "патруль", "S.W.A.T.", "FBI", "армия", "вертолёт", "скорая" }; return n[k < 0 || k > 5 ? 0 : k]; }

    // Отряд приезжает: машина «из-за угла» + люди внутри; target — кого ловить (0 — подмога Артёму)
    bool SpawnUnit(int kind, const V3& near, int target, bool backup)
    {
        Squad sq = SquadFor(kind);
        std::vector<int> models = { sq.car, sq.ped };
        if(kind == 4) models.push_back(280);
        if(!LoadModels(models)) Do(0x038B, {});                                                 // LOAD_ALL_MODELS_NOW — нужна сейчас
        V3 at; float h = 0;
        bool heli = kind == 4;
        if(heli) { at = Add(near, Mul(Dir(Rnd(0, 360)), 170.0f)); at.z = std::max(near.z, GroundZ(at.x, at.y, near.z)) + 45; h = Heading(at, near); }
        else if(!OffscreenNode(near, 70, 160, at, h) && !OffscreenNode(near, 120, 320, at, h))
        {   // скрытой точки рядом нет (например, игрок на базе: вокруг открытая площадка) — берём дорогу подальше
            V3 probe = base::Inside(layout::G(near), 10.0f) ? layout::WV(0, 110.0f) : Add(near, Mul(Dir(Rnd(0, 360)), 180.0f));
            if(!NthCarNode(probe, 1, at, h)) { LOG("РОЗЫСК", "%s: рядом нет дороги — не может проехать", KindName(kind)); return false; }
        }
        int car = OutI(0x00A5, { Ai(sq.car), Af(at.x), Af(at.y), Af(at.z + (heli ? 0 : 0.5f)), Aoi() });   // CREATE_CAR
        if(car <= 0) return false;
        Do(0x0175, { Ai(car), Af(h) });
        if(heli) Do(0x0825, { Ai(car) });                                                         // SET_HELI_BLADES_FULL_SPEED
        else Do(0x0397, { Ai(car), Ai(1) });                                                      // SWITCH_CAR_SIREN
        g_cars.push_back(car);
        int made = 0;
        for(int i = 0; i < sq.count; ++i)
        {
            int model = heli && i == 0 ? 280 : sq.ped;                                            // пилот — полицейский
            int ped = i == 0 ? OutI(0x0129, { Ai(car), Ai(sq.pedType), Ai(model), Aoi() })      // CREATE_CHAR_INSIDE_CAR
                             : OutI(0x01C8, { Ai(car), Ai(sq.pedType), Ai(model), Ai(i - 1), Aoi() });   // CREATE_CHAR_AS_PASSENGER
            if(ped <= 0) continue;
            made++;
            if(sq.weapon) { swat::GiveWeapon(ped, sq.weapon, 600); Do(0x01B9, { Ai(ped), Ai(sq.weapon) }); }
            Do(0x02E2, { Ai(ped), Ai(kind >= 1 && kind <= 4 ? 75 : 55) });                       // SET_CHAR_ACCURACY
            Do(0x0526, { Ai(ped), Ai(1) });                                                       // SET_CHAR_STAY_IN_CAR_WHEN_JACKED
            Do(0x087E, { Ai(ped), Ai(0) });                                                       // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD 0
            Unit u; u.ref = ped; u.car = car; u.target = target; u.kind = kind; u.seat = i; u.born = Now(); u.backup = backup;
            g_units.push_back(u);
        }
        Unload(models);
        if(!made) { Do(0x00A6, { Ai(car) }); g_cars.pop_back(); return false; }
        LOG("РОЗЫСК", "%s «%s» едет из-за угла (%.0f м)%s", backup ? "подмога" : "реакция города", KindName(kind), Dist2(at, near), target ? "" : " — к Артёму");
        return true;
    }

    int UnitsFor(int ref) { int n = 0; for(auto& u : g_units) if(u.target == ref && !u.leaving) n++; return n; }
    bool HasHeliFor(int ref) { for(auto& u : g_units) if(u.target == ref && u.kind == 4 && !u.leaving) return true; return false; }

    void Wave(Suspect& s)
    {
        V3 at = RefPos(s.ref);
        if(UnitsFor(s.ref) >= 1 + s.stars * 2 || (int)g_units.size() > 20) return;
        switch(s.stars)
        {
            case 1: SpawnUnit(0, at, s.ref, false); break;
            case 2: SpawnUnit(0, at, s.ref, false); SpawnUnit(0, at, s.ref, false); break;
            case 3: SpawnUnit(0, at, s.ref, false); if(!HasHeliFor(s.ref)) SpawnUnit(4, at, s.ref, false); else SpawnUnit(0, at, s.ref, false); break;
            case 4: SpawnUnit(1, at, s.ref, false); SpawnUnit(0, at, s.ref, false); break;
            case 5: SpawnUnit(2, at, s.ref, false); if(!HasHeliFor(s.ref)) SpawnUnit(4, at, s.ref, false); else SpawnUnit(1, at, s.ref, false); break;
            default: SpawnUnit(3, at, s.ref, false); SpawnUnit(1, at, s.ref, false); if(!HasHeliFor(s.ref)) SpawnUnit(4, at, s.ref, false); break;
        }
    }

    bool Ours(uintptr_t p)
    {
        int ref = Ref(p);
        if(base::StaffIndexOf(ref) >= 0) return true;
        for(auto& u : g_units) if(u.ref == ref) return true;
        return false;
    }

    // кого бить подмоге: разыскиваемые и те, кто стреляет/целится в Артёма или полицию рядом
    int ThreatNear(const V3& at, float r)
    {
        int best = 0; float bd = r;
        uintptr_t pl = Player();
        ForEachPed([&](uintptr_t p) {
            if(!PedAlive(p) || p == pl) return;
            int t = PedType(p); if(t == 6 || t == 18 || t <= 1) return;
            if(Ours(p)) return;
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

    void Release(Unit& u)
    {
        if(Is(0x056D, { Ai(u.ref) })) Do(0x01C2, { Ai(u.ref) });                                   // MARK_CHAR_AS_NO_LONGER_NEEDED
    }

    // уехать: водитель — катается по городу, пешие — садятся в свою машину (если рядом)
    void LeaveOrder(Unit& u)
    {
        uintptr_t p = Ped(u.ref); if(!p) return;
        uintptr_t v = PedVehicle(p);
        if(v)
        {
            if(VehDriver(v) == p && u.kind != 4)
            {
                Do(0x0397, { Ai(VehRef(v)), Ai(0) });                                             // сирену выключить
                Do(0x00A8, { Ai(VehRef(v)) });                                                    // CAR_WANDER_RANDOMLY
                Do(0x00AD, { Ai(VehRef(v)), Af(14.0f) });                                         // SET_CAR_CRUISE_SPEED
            }
            if(u.kind == 4 && u.seat == 0)
            {   // вертолёт улетает
                V3 away = Add(Pos(v), Mul(Norm2(Sub(Pos(v), Pos(Player()))), 600.0f)); away.z = Pos(v).z + 40;
                Do(0x04A2, { Ai(VehRef(v)), Af(away.x), Af(away.y), Af(away.z), Af(40.0f), Af(80.0f) });   // HELI_GOTO_COORDS
            }
            return;
        }
        uintptr_t cv = Is(0x056E, { Ai(u.car) }) ? Veh(u.car) : 0;
        if(cv && Dist(Pos(cv), Pos(p)) < 60)
        {
            if(u.seat == 0) Do(0x05CB, { Ai(u.ref), Ai(u.car), Ai(20000) });                       // TASK_ENTER_CAR_AS_DRIVER
            else Do(0x05CA, { Ai(u.ref), Ai(u.car), Ai(20000), Ai(u.seat - 1) });                 // TASK_ENTER_CAR_AS_PASSENGER
        }
        else Do(0x05DA, { Ai(u.ref), Af(Pos(Player()).x), Af(Pos(Player()).y), Af(Pos(Player()).z), Af(80.0f), Ai(60000) });   // TASK_FLEE_POINT — уйти
    }

    void UnitsFrame()
    {
        uintptr_t pl = Player(); V3 pp = Pos(pl);
        int plRef = PlayerRef();
        uintptr_t plCar = PedVehicle(pl);
        for(auto it = g_units.begin(); it != g_units.end();)
        {
            Unit& u = *it;
            if(!Is(0x056D, { Ai(u.ref) }) || !RefAlive(u.ref)) { Release(u); it = g_units.erase(it); continue; }   // погиб/исчез
            uintptr_t p = Ped(u.ref);
            V3 me = Pos(p);
            // цель ещё актуальна?
            if(u.target > 0)
            {
                auto s = g_sus.find(u.target);
                bool keep = s != g_sus.end() ? (!s->second.custody && s->second.stars > 0 && RefAlive(u.target))
                                             : (u.backup && RefAlive(u.target) && Dist(RefPos(u.target), pp) < 120);   // угроза у подмоги
                if(!keep) { u.target = 0; u.order = 0; }
            }
            if(!u.leaving)
            {
                if(u.backup)
                {
                    if(!u.target) u.target = ThreatNear(pp, 70);
                    if(g_release || Ago(u.born, 180000)) { u.leaving = true; u.doneAt = Now(); u.target = 0; LeaveOrder(u); }
                }
                else if(!u.target)
                {   // свой подозреваемый всё — помочь с другим поблизости или уехать
                    int best = 0; float bd = 400;
                    for(auto& kv : g_sus) if(!kv.second.custody && !kv.second.surrendered && kv.second.stars > 0) { float d = Dist(RefPos(kv.first), me); if(d < bd) { bd = d; best = kv.first; } }
                    if(best) u.target = best;
                    else { u.leaving = true; u.doneAt = Now(); LeaveOrder(u); }
                }
            }
            if(u.leaving)
            {
                bool gone = (Dist(me, pp) > 70 && !OnScreen(me, 3)) || Ago(u.doneAt, 90000);
                if(gone) { Release(u); it = g_units.erase(it); continue; }
                if(Ago(u.taskAt, 8000)) { u.taskAt = Now(); LeaveOrder(u); }
                ++it; continue;
            }
            if(!Ago(u.taskAt, 2000)) { ++it; continue; }
            u.taskAt = Now();
            uintptr_t v = PedVehicle(p);
            bool driver = v && VehDriver(v) == p;
            int tgt = u.target;
            V3 goal = tgt ? RefPos(tgt) : pp;
            int tCar = tgt ? CarOf(tgt) : -1;
            auto sit = tgt ? g_sus.find(tgt) : g_sus.end();
            int stars = sit != g_sus.end() ? sit->second.stars : (tgt ? 3 : 0);
            bool surrendered = sit != g_sus.end() && sit->second.surrendered;
            bool shoot = tgt && !surrendered && (stars >= 3 || u.backup);

            if(u.kind == 4)
            {   // вертолёт: пилот — родная погоня полицейского вертолёта или держится над Артёмом; стрелки бьют с борта
                if(u.seat == 0 && v)
                {
                    if(tgt) { if(Order(u, 1, tgt, 15000)) Do(0x0727, { Ai(VehRef(v)), Ai(tgt), Ai(-1), Af(20.0f) }); }   // POLICE_HELI_CHASE_ENTITY (как в сюжете игры)
                    else if(Order(u, 2, 0, 15000)) Do(0x0726, { Ai(VehRef(v)), Ai(plRef), Ai(-1), Af(28.0f) });   // HELI_FOLLOW_ENTITY — над Артёмом
                }
                else if(v && shoot && Dist(me, goal) < 90 && Order(u, 3, tgt, 6000))
                    Do(0x0713, { Ai(u.ref), Ai(tCar > 0 ? -1 : tgt), Ai(tCar > 0 ? tCar : -1), Af(0), Af(0), Af(0), Af(80.0f), Ai(4), Ai(0), Ai(70) });   // TASK_DRIVE_BY
                ++it; continue;
            }
            if(v)
            {
                if(driver)
                {
                    if(u.kind == 5)
                    {   // скорая: к Артёму
                        if(Dist2(me, pp) < 18) { if(Order(u, 4, 0, 30000)) Do(0x068B, { Ai(VehRef(v)) }); }   // TASK_EVERYONE_LEAVE_CAR
                        else if(Order(u, 5, (int)(pp.x / 10) * 1000 + (int)(pp.y / 10), 4000)) Do(0x05D1, { Ai(u.ref), Ai(VehRef(v)), Af(pp.x), Af(pp.y), Af(pp.z), Af(30.0f), Ai(0), Ai(-1), Ai(2) });
                    }
                    else if(tCar > 0)
                    {   // подозреваемый на машине — преследовать: чётные таранят, нечётные перекрывают (родные задания игры)
                        int mission = (u.ref & 1) ? 17 : 15;                                    // 15 RAMCAR_FARAWAY, 17 BLOCKCAR_FARAWAY
                        if(Order(u, 6, tCar, 10000)) Do(0x06E1, { Ai(u.ref), Ai(VehRef(v)), Ai(tCar), Ai(mission), Af(40.0f), Ai(2) });   // TASK_CAR_MISSION
                    }
                    else
                    {
                        float d = Dist2(me, goal);
                        bool follow = !tgt && u.backup && plCar;                                 // подмога: Артём едет — едем следом
                        if(!follow && (d < 25 || (Speed(v) < 0.5f && Ago(u.born, 10000) && d < 70))) { if(Order(u, 4, 0, 20000)) Do(0x068B, { Ai(VehRef(v)) }); }   // приехали — все из машины
                        else if(follow && d < 18) { /* рядом с машиной Артёма — стоим */ }
                        else if(Order(u, 5, (int)(goal.x / 8) * 1000 + (int)(goal.y / 8), 3000))
                            Do(0x05D1, { Ai(u.ref), Ai(VehRef(v)), Af(goal.x), Af(goal.y), Af(goal.z), Af(follow ? 32.0f : 36.0f), Ai(0), Ai(-1), Ai(2) });   // TASK_CAR_DRIVE_TO_COORD
                    }
                }
                else
                {   // пассажир: по машине подозреваемого — стрельба с борта (3+ звезды), машина встала — выйти
                    if(tCar > 0 && shoot && Dist(me, goal) < 60) { if(Order(u, 3, tgt, 6000)) Do(0x0713, { Ai(u.ref), Ai(-1), Ai(tCar), Af(0), Af(0), Af(0), Af(60.0f), Ai(4), Ai(u.seat == 1 ? 1 : 0), Ai(60) }); }
                    else if(!VehDriver(v) && Order(u, 4, 0, 10000)) Do(0x0633, { Ai(u.ref) });          // TASK_LEAVE_ANY_CAR
                }
                ++it; continue;
            }
            // пешком
            if(u.kind == 5)
            {   // скорая: подлечить Артёма
                if(Dist(me, pp) > 3) { if(Order(u, 7, 0, 5000)) Do(0x05D3, { Ai(u.ref), Af(pp.x), Af(pp.y), Af(pp.z), Ai(6), Ai(10000) }); }
                else
                {
                    Do(0x0605, { Ai(u.ref), As("CPR"), As("MEDIC"), Af(4), Ai(0), Ai(0), Ai(0), Ai(0), Ai(3000) });
                    Do(0x0223, { Ai(plRef), Ai(176) });
                    ui::Notice("Медик: подлатал тебя");
                    u.leaving = true; u.doneAt = Now();
                }
                ++it; continue;
            }
            uintptr_t cv = Is(0x056E, { Ai(u.car) }) ? Veh(u.car) : 0;
            if(tgt)
            {
                float d = Dist(me, goal);
                if(tCar > 0 && d > 35 && cv && Dist(Pos(cv), me) < 50)
                {   // подозреваемый уехал — обратно в свою машину
                    if(Order(u, 8, tCar, 15000)) { if(u.seat == 0) Do(0x05CB, { Ai(u.ref), Ai(u.car), Ai(15000) }); else Do(0x05CA, { Ai(u.ref), Ai(u.car), Ai(15000), Ai(u.seat - 1) }); }
                }
                else if(shoot) { if(Order(u, 9, tgt, 15000)) Do(0x05E2, { Ai(u.ref), Ai(tgt) }); }  // TASK_KILL_CHAR_ON_FOOT
                else if(tCar <= 0) { if(Order(u, 10, tgt, 8000)) Do(0x06E4, { Ai(u.ref), Ai(tgt) }); }   // TASK_CHAR_ARREST_CHAR — родное задержание
            }
            else if(u.backup)
            {   // охрана Артёма: держатся рядом; Артём уехал — по машинам и следом
                float d = Dist(me, pp);
                if(plCar && d > 25 && cv && Dist(Pos(cv), me) < 60)
                {
                    if(Order(u, 8, 0, 15000)) { if(u.seat == 0) Do(0x05CB, { Ai(u.ref), Ai(u.car), Ai(15000) }); else Do(0x05CA, { Ai(u.ref), Ai(u.car), Ai(15000), Ai(u.seat - 1) }); }
                }
                else if(d > 9) { if(Order(u, 11, 0, 6000)) Do(0x06A8, { Ai(u.ref), Ai(plRef), Ai(-1), Af(4.0f), Af((float)((u.ref * 67) % 360)) }); }   // TASK_GOTO_CHAR_OFFSET
            }
            ++it;
        }
        // машины, в которых никого из наших — отпустить игре
        for(auto it = g_cars.begin(); it != g_cars.end();)
        {
            bool used = false; for(auto& u : g_units) if(u.car == *it) used = true;
            if(!used) { if(Is(0x056E, { Ai(*it) })) Do(0x01C3, { Ai(*it) }); it = g_cars.erase(it); } else ++it;
        }
        if(g_release && g_units.empty()) g_release = false;
    }

    void RemoveSuspect(int ref, const char* why)
    {
        auto it = g_sus.find(ref);
        if(it == g_sus.end()) return;
        if(it->second.blip > 0) Do(0x0164, { Ai(it->second.blip) });
        ClearTargeting(ref);
        if(Is(0x056D, { Ai(ref) }))
        {
            Do(0x09B6, { Ai(ref), Ai(0) });                                                           // SET_CHAR_WANTED_BY_POLICE 0
            Do(0x0961, { Ai(ref), Ai(0) });                                                           // SET_CHAR_KEEP_TASK 0
            Do(0x01C2, { Ai(ref) });                                                                  // отпустить игре (не удаляем!)
        }
        LOG("РОЗЫСК", "#%d снят с розыска: %s", ref, why);
        g_sus.erase(it);
    }

    void Flee(Suspect& s, int plRef)
    {
        // родная задача «бежать любым способом»: пешком/на машине, с 2 звёзд угоняет машины, с 3 — отстреливается
        bool shoot = s.stars >= 3;
        float steal = s.stars >= 2 ? 50.0f : 0.0f;
        Do(0x0751, { Ai(s.ref), Ai(plRef), Af(1000.0f), Ai(999999), Ai(shoot ? 1 : 0), Ai(3000), Ai(5000), Af(steal) });   // TASK_FLEE_CHAR_ANY_MEANS
        Do(0x0961, { Ai(s.ref), Ai(1) });                                                             // SET_CHAR_KEEP_TASK
        s.fleeStars = s.stars; s.taskAt = Now();
    }

    void SuspectsFrame()
    {
        uintptr_t pl = Player(); V3 pp = Pos(pl);
        int plRef = PlayerRef();
        bool showArrest = false; int arrestRef = 0;
        std::vector<std::pair<int, const char*>> gone;
        for(auto& kv : g_sus)
        {
            Suspect& s = kv.second;
            if(!Is(0x056D, { Ai(s.ref) })) { gone.push_back({ s.ref, "исчез" }); continue; }
            if(!RefAlive(s.ref)) { gone.push_back({ s.ref, "погиб" }); continue; }
            uintptr_t p = Ped(s.ref);
            V3 sp = Pos(p);
            float d = Dist(sp, pp);
            if(d < 80 && s.stars > 0) ui::Stars(layout::G(sp), s.stars, s.surrendered || s.custody);
            if(s.custody)
            {   // задержан Артёмом: идёт за ним, садится к нему в машину
                if(Ago(s.taskAt, 2000))
                {
                    s.taskAt = Now();
                    uintptr_t pv = PedVehicle(pl);
                    if(pv && !PedVehicle(p)) Do(0x05CA, { Ai(s.ref), Ai(VehRef(pv)), Ai(10000), Ai(VehPassenger(pv, 1) ? 2 : 1) });   // заднее сиденье
                    else if(!pv && PedVehicle(p)) Do(0x0633, { Ai(s.ref) });
                    else if(!pv && d > 2.5f) Do(0x06A8, { Ai(s.ref), Ai(plRef), Ai(-1), Af(1.6f), Af(160.0f) });   // TASK_GOTO_CHAR_OFFSET
                }
                if(d > 120) { s.custody = false; s.surrendered = false; s.fleeStars = -1; ui::Notice("Задержанный сбежал!"); LOG("РОЗЫСК", "#%d сбежал из-под конвоя", s.ref); }
                continue;
            }
            // задержан полицией (родное задержание)
            if(Is(0x0741, { Ai(s.ref) }))                                                              // HAS_CHAR_BEEN_ARRESTED
            {
                ui::Notice("Полиция задержала подозреваемого");
                gone.push_back({ s.ref, "задержан полицией" });
                continue;
            }
            // сдаётся: Артём держит на прицеле рядом (1–2 зв. — сразу, 3+ — если ранен/без оружия), или окружили
            bool aimed = d < 15 && Is(0x0457, { Ai(0), Ai(s.ref) });                                  // IS_PLAYER_TARGETTING_CHAR
            int copsNear = 0; for(auto& u : g_units) if(!u.leaving && !PedVehicle(Ped(u.ref)) && Dist(RefPos(u.ref), sp) < 8) copsNear++;
            if(!s.surrendered && !PedVehicle(p))
            {
                bool weak = s.stars <= 2 || Health(p) < 50 || CurWeapon(p) < 22;
                if(aimed) { if(!s.aimedSince) s.aimedSince = Now(); if(Ago(s.aimedSince, weak ? 1200u : 4000u) && (weak || Health(p) < 80)) Surrendered(s.ref); }
                else s.aimedSince = 0;
                if(copsNear >= 2 && (weak || s.stars <= 3)) { if(!s.copsNearSince) s.copsNearSince = Now(); if(Ago(s.copsNearSince, 2500)) Surrendered(s.ref); }
                else s.copsNearSince = 0;
            }
            if(s.surrendered)
            {
                if(Ago(s.taskAt, 7000)) { s.taskAt = Now(); Do(0x05C4, { Ai(s.ref), Ai(8000) }); }   // TASK_HANDS_UP
                if(d < 3.0f && !PedVehicle(pl)) { showArrest = true; arrestRef = s.ref; }
                continue;
            }
            // бегство: выдать (или обновить при смене звёзд / раз в 40 с)
            if(s.fleeStars != s.stars || Ago(s.taskAt, 40000)) Flee(s, plRef);
            // ушёл: далеко и не виден 40 с
            if(d > 380 && !OnScreen(sp, 2)) { if(!s.lostSince) s.lostSince = Now(); if(Ago(s.lostSince, 40000)) { ui::Notice("Подозреваемый скрылся"); gone.push_back({ s.ref, "скрылся" }); continue; } }
            else s.lostSince = 0;
            // реакция города волнами
            if(Now() >= s.nextWave && d < 450) { s.nextWave = Now() + (uint32_t)std::max(12, 40 - s.stars * 5) * 1000u; Wave(s); }
        }
        for(auto& g : gone) RemoveSuspect(g.first, g.second);
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

    // ---------------------------------------------------------------- вызовы (кабинет Фрэнка)
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
            if(g_call.kind == 1)
            {
                int car = OutI(0x00A5, { Ai(RndI(0, 1) ? 410 : 412), Af(g_call.at.x), Af(g_call.at.y), Af(g_call.at.z + 0.5f), Aoi() });
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

    // ---------------------------------------------------------------- выдача розыска
    // Цель (ТЗ §8): человек в захвате прицела; иначе тот, на кого смотрит камера (до 80 м; машина — её водитель);
    // иначе тот, кто стоит прямо перед Артёмом. Своих (сотрудники базы, подмога, полиция-юниты) — нельзя.
    struct Pick { uintptr_t ped = 0; bool driver = false; };
    Pick PickStarsTarget()
    {
        Pick r;
        uintptr_t pl = Player(); if(!pl) return r;
        V3 me = Pos(pl);
        auto ok = [&](uintptr_t p) { return p && p != pl && PedAlive(p) && PedType(p) > 1 && !Ours(p); };
        uintptr_t t = PlayerTargetPed();
        if(ok(t) && Dist(Pos(t), me) < 80) { r.ped = t; r.driver = PedVehicle(t) != 0; return r; }
        V3 cp, cf;
        if(Camera(cp, cf))
        {
            float bestScore = 1e9f;
            auto consider = [&](uintptr_t ped, const V3& at, bool driver) {
                V3 rel = Sub(at, cp);
                float along = rel.x * cf.x + rel.y * cf.y + rel.z * cf.z;
                if(along < 2.0f || along > 80.0f) return;
                V3 c = Add(cp, Mul(cf, along));
                float off = Dist(c, at);
                float allow = 1.2f + along * 0.05f + (driver ? 1.0f : 0.0f);
                if(off > allow) return;
                float score = off / allow + along * 0.004f;
                if(score >= bestScore) return;
                if(Ray(Add(cp, Mul(cf, 1.0f)), Add(at, Mk(0, 0, driver ? 0.3f : 0.6f)), nullptr, nullptr, true, false, false, false)) return;   // за стеной — нет
                bestScore = score; r.ped = ped; r.driver = driver;
            };
            ForEachPed([&](uintptr_t p) { if(ok(p) && !PedVehicle(p)) consider(p, Add(Pos(p), Mk(0, 0, 0.5f)), false); });
            ForEachVehicle([&](uintptr_t v) { uintptr_t d = VehDriver(v); if(ok(d) && PedVehicle(pl) != v) consider(d, Pos(v), true); });
            if(r.ped) return r;
        }
        if(PedVehicle(pl)) return r;
        float h = EntHeading(pl) * 0.0174533f; float fx = -sinf(h), fy = cosf(h);
        float bestScore = 1e9f;
        ForEachPed([&](uintptr_t p) {
            if(!ok(p) || PedVehicle(p)) return;
            V3 v = Pos(p); float d = Dist(v, me);
            if(d > 5.0f) return;
            float facing = ((v.x - me.x) * fx + (v.y - me.y) * fy) / (d + 0.01f);
            if(facing < 0.3f) return;
            float score = d * (1.6f - facing);
            if(score < bestScore) { bestScore = score; r.ped = p; r.driver = false; }
        });
        return r;
    }
    void StarsButton()
    {
        if(ui::MenuOpen() || game::CutsceneRunning()) return;
        Pick pk = PickStarsTarget();
        if(!pk.ped) return;
        int ref = Ref(pk.ped);
        auto it = g_sus.find(ref);
        int cur = it == g_sus.end() ? 0 : it->second.stars;
        if(it != g_sus.end() && it->second.custody) return;
        // над выбранным — «ЦЕЛЬ» (видно, кому дашь розыск)
        if(!cur) { game::Vec w = layout::G(Pos(pk.ped)); w.z += 1.6f; ui::Sign(w, "ЦЕЛЬ", pk.driver ? "водитель" : "", 0xFFFFFF); }
        ui::ActionButton(1, cur ? "Розыск: " + std::to_string(cur) : std::string("Розыск"), [ref, cur]() {
            static const char* kItems[] = { "Снять розыск", "1 — ближайший патруль", "2 — ещё патрули", "3 — + вертолёт, стреляют",
                                            "4 — S.W.A.T.", "5 — FBI и вертолёт", "6 — армия" };
            std::vector<std::string> items(kItems, kItems + 7);
            std::string title = cur ? "РОЗЫСК — сейчас " + std::to_string(cur) + " зв." : std::string("РОЗЫСК — СКОЛЬКО ЗВЁЗД?");
            ui::OpenMenu(title, items, [ref](int i) {
                if(i < 0) return;
                ui::CloseMenu();
                if(!Is(0x056D, { Ai(ref) }) || !RefAlive(ref)) { ui::Notice("Человек пропал"); return; }     // DOES_CHAR_EXIST
                if(i == 0) { SetStars(ref, 0, "снят Артёмом"); ui::Notice("Розыск снят"); return; }
                SetStars(ref, i, "по приказу Артёма");
                ui::Notice(std::string("Розыск: ") + std::to_string(i) + (i == 1 ? " звезда" : i < 5 ? " звезды" : " звёзд"));
                ui::Subtitle("Артём", i >= 4 ? "Центр, особо опасный! Всем постам!" : "Центр, объявляю в розыск.", 2.5f, 0x5AAAFF);
            }, "Наведи камеру на человека или машину. Город реагирует по звёздам, как на CJ.");
        });
    }

    void RadioButton()
    {
        if(ui::MenuOpen() || game::CutsceneRunning()) return;
        ui::ActionButton(2, "Рация", []() {
            int mine = 0; for(auto& u : g_units) if(u.backup && !u.leaving) mine++;
            std::vector<std::string> items = { "Полиция", "S.W.A.T.", "FBI", "Армия", "Вертолёт", "Скорая", mine ? "Отпустить подмогу (" + std::to_string(mine) + ")" : std::string("Отпустить подмогу"), "Тревога на базе" };
            ui::OpenMenu("РАЦИЯ — ПОДМОГА", items, [](int i) {
                if(i < 0) return;
                ui::CloseMenu();
                V3 p = Pos(Player());
                if(i == 7) { base::RaiseAlarm("Артём по рации: тревога на базе"); return; }
                if(i == 6)
                {
                    int n = 0; for(auto& u : g_units) if(u.backup && !u.leaving) n++;
                    if(!n) { ui::Notice("Подмоги рядом нет"); return; }
                    g_release = true;
                    ui::Subtitle("Артём", "Центр, подмога свободна. Спасибо, парни.", 3.0f, 0x5AAAFF);
                    LOG("РАЦИЯ", "подмога отпущена (%d)", n);
                    return;
                }
                CallBackup(i, p.x, p.y, p.z, "рация Артёма");
            }, "Подмога едет из-за угла, охраняет тебя и бьёт тех, кто стреляет.");
        });
    }
}

void Init() {}
void Reset() { g_sus.clear(); g_units.clear(); g_cars.clear(); g_call = Call(); g_release = false; }

int Stars(int ref) { auto it = g_sus.find(ref); return it == g_sus.end() ? 0 : it->second.stars; }
void SetStars(int ref, int stars, const char* why)
{
    if(ref <= 0) return;
    if(stars <= 0) { RemoveSuspect(ref, why ? why : "снят"); return; }
    if(stars > 6) stars = 6;
    uintptr_t p = Ped(ref);
    if(!p) return;
    Suspect& s = g_sus[ref];
    bool fresh = s.ref == 0;
    s.ref = ref; s.stars = stars; s.why = why ? why : "";
    if(fresh)
    {
        s.givenAt = Now(); s.nextWave = Now() + 2500;                                               // первая машина — через 2–3 с
        s.blip = OutI(0x0187, { Ai(ref), Aoi() });                                                   // ADD_BLIP_FOR_CHAR (красная)
        bool converted = MakeMissionPed(p);                                                          // игра его больше не удалит сама
        LOG("РОЗЫСК", "#%d: под защитой от удаления игрой (%s)", ref, converted ? "был прохожим" : "уже сюжетный");
    }
    s.surrendered = false; s.fleeStars = -1;                                                         // новые звёзды — снова бежит
    Do(0x09B6, { Ai(ref), Ai(1) });                                                                   // SET_CHAR_WANTED_BY_POLICE
    LOG("РОЗЫСК", "#%d: %d зв. (%s)%s", ref, stars, s.why.c_str(), PedVehicle(p) ? " — за рулём" : "");
}
void CallBackup(int kind, float x, float y, float z, const char* why)
{
    V3 at = Mk(x, y, z);
    if(kind < 0 || kind > 5) kind = 0;
    g_release = false;
    bool ok = SpawnUnit(kind, at, 0, true);
    if(kind == 1 || kind == 3) ok = SpawnUnit(0, at, 0, true) || ok;
    static const char* said[] = { "Центр: патруль выехал.", "Центр: S.W.A.T. на подходе.", "Центр: FBI выдвигается.",
                                  "Центр: армия выдвигается.", "Центр: вертолёт поднят, будет над тобой.", "Центр: скорая едет." };
    ui::Subtitle("Рация", ok ? said[kind] : "Центр: все заняты, держитесь!", 3.0f, 0x9AD08A);
    LOG("РАЦИЯ", "подмога %d (%s): %s", kind, why ? why : "", ok ? "выехала" : "не вышло");
}
bool StartCall()
{
    if(g_call.on) return false;
    V3 at; float h;
    V3 base = Mk(layout::kCx, layout::kCy, layout::kZ);
    bool ok = false;
    for(int i = 0; i < 10 && !ok; ++i)
    {
        V3 probe = Add(base, Mul(Dir(Rnd(0, 360)), Rnd(400, 900)));
        if(probe.x < 100 || probe.x > 2900 || probe.y < -2800 || probe.y > -900) continue;
        ok = NthCarNode(probe, 1, at, h);
    }
    if(!ok) return false;
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
    Do(0x0961, { Ai(ref), Ai(0) });
    Do(0x0687, { Ai(ref) });                                                                      // CLEAR_CHAR_TASKS
    Do(0x05C4, { Ai(ref), Ai(8000) });                                                            // TASK_HANDS_UP
    Do(0x048F, { Ai(ref) });                                                                      // REMOVE_ALL_CHAR_WEAPONS
    for(auto& u : g_units) if(u.target == ref) u.order = 0;                                        // полиция — сразу на задержание
    LOG("РОЗЫСК", "#%d сдался", ref);
}
void Arrested(int ref)
{
    auto it = g_sus.find(ref);
    if(it == g_sus.end()) { SetStars(ref, 1, "задержан"); it = g_sus.find(ref); if(it == g_sus.end()) return; }
    it->second.custody = true; it->second.surrendered = true; it->second.taskAt = 0;
    ClearTargeting(ref);
    if(it->second.blip > 0) { Do(0x0165, { Ai(it->second.blip), Ai(2) }); }                       // метка зелёная
    Do(0x09B6, { Ai(ref), Ai(0) });
    Do(0x0961, { Ai(ref), Ai(0) });
    ui::Objective("Отвези задержанного в камеру базы", 6.0f);
    LOG("РОЗЫСК", "#%d задержан — вести в камеру базы", ref);
}
void Delivered(int ref)
{
    auto it = g_sus.find(ref);
    int stars = it == g_sus.end() ? 1 : it->second.stars;
    int reward = std::min(1500, 300 + 200 * stars);
    ClearTargeting(ref);
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
// Сохранение: метки людей в файл не пишем (после загрузки они повисли бы на чужих людях) — снимаем и сразу ставим
// обратно после записи. Людей и подмогу НЕ трогаем (раньше автосохранение на каждый скриншот стирало весь розыск).
void BeforeSave()
{
    for(auto& kv : g_sus) if(kv.second.blip > 0) { Do(0x0164, { Ai(kv.second.blip) }); kv.second.blip = 0; }
    if(g_call.blip > 0) { Do(0x0164, { Ai(g_call.blip) }); g_call.blip = -1; }
}
void AfterSave()
{
    for(auto& kv : g_sus)
        if(kv.second.blip == 0 && Is(0x056D, { Ai(kv.first) }))
        {
            kv.second.blip = OutI(0x0187, { Ai(kv.first), Aoi() });
            if(kv.second.custody && kv.second.blip > 0) Do(0x0165, { Ai(kv.second.blip), Ai(2) });
        }
    if(g_call.blip < 0 && g_call.on && !g_call.spawned) g_call.blip = OutI(0x018A, { Af(g_call.at.x), Af(g_call.at.y), Af(g_call.at.z), Aoi() });
    else if(g_call.blip < 0) g_call.blip = 0;
}

void Frame()
{
    if(!Player()) return;
    WHERE("розыск: подозреваемые"); SuspectsFrame();
    WHERE("розыск: подмога"); UnitsFrame();
    WHERE("розыск: вызов"); CallFrame();
    WHERE("розыск: кнопки"); StarsButton();
    RadioButton();
}
}
