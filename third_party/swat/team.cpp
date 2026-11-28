// Исполнитель тактик спецназа: «чувства» (Ч01–Ч20), «руки» пешком (Д01–Д24), программы тактик Т001–Т158.
// Слои (ТЗ-SQUAD-TACTICS §2): голова выбирает тактику → Team ведёт её 10 раз в секунду (кто куда встаёт, когда
// стрелять, когда менять позицию) и сам решает мелочи (перезарядка, укрытие под огнём, не стрелять в мирных) →
// руки каждый кадр ставят педа точно, приседают, выглядывают, дают короткие очереди. Тело — анимации игры.
#include "tactics.h"
#include "swat_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace swat
{
// ======================================================================================================
//                                                Данные
// ======================================================================================================
enum IntentKind
{
    I_IDLE, I_FOLLOW, I_MOVE, I_FIGHT, I_HOLD, I_SUPPRESS, I_FLANK, I_FALLBACK, I_ARREST_TALK, I_ARREST_COVER,
    I_CHASE, I_INTERCEPT, I_MEDIC, I_SCAVENGE, I_GUARD_BACK, I_WATCH, I_ENTER_CAR, I_IN_CAR, I_LEAVE_CAR,
    I_DRIVE, I_PILOT, I_BACKUP_CALL, I_GOTO_ARMORY, I_GIVE, I_ESCORT, I_REST, I_CHECK_BODY, I_PIE
};

struct Intent
{
    int kind = I_IDLE;
    V3 p; bool hasP = false;
    int target = 0, car = 0, seat = -1;
    float heading = -1, dist = 0;
    int mode = 0;                      // режим водителя/пилота и т.п.
    bool crouch = false, mayFire = true, sprint = false;
};

struct Threat
{
    int ref = 0; V3 pos, lastPos, vel; float score = 0, dist = 0;
    bool armed = false, aiming = false, shooting = false, elevated = false, inCar = false, visible = false, surrender = false;
    uint32_t lastSeen = 0, firstSeen = 0;
};

struct Unit
{
    int ref = 0; std::string name; int role = R_NONE; bool defender = false;
    Intent in;
    // руки
    int task = -1, taskTarget = 0; V3 taskP; uint32_t taskAt = 0;
    std::string doing = "ждёт";
    V3 cover; bool hasCover = false; uint32_t coverAt = 0, coverFor = 0;
    uint32_t peekAt = 0, burstUntil = 0, lastFireAt = 0, shiftAt = 0;
    int bursts = 0;
    bool ducked = false;
    float lastHp = 100; bool wounded = false, dead = false, reported = false;
    uint32_t deadAt = 0, lastSay = 0, reloadAt = 0;
    int fireTarget = 0;
    int bodyChecked = 0;
};

class TeamImpl
{
public:
    std::string tag;
    std::vector<Unit> u;
    int leader = 0;
    V3 home; float homeR = 0; bool hasHome = false;
    std::vector<V3> posts;
    V3 places[8]; bool hasPlace[8] = {};
    bool (*enemyFn)(uintptr_t) = nullptr;
    void (*sayFn)(int, const std::string&, const std::string&, bool) = nullptr;
    void (*backupFn)(int, const V3&) = nullptr;
    void (*arrestedFn)(int) = nullptr;
    void (*deliveredFn)(int) = nullptr;
    int  (*starsFn)(int) = nullptr;
    Order ord; uint32_t ordAt = 0; int ordStep = 0; uint32_t stepAt = 0;
    bool autoOn = true;
    std::vector<Threat> threats;
    std::deque<Event> events;
    uint32_t lastTick = 0, lastScan = 0, lastLos = 0, lastContact = 0, clearSince = 0;
    bool inCombat = false, announcedClear = true;
    int losCursor = 0;
    std::vector<int> prisoners;
    int arrestStage = 0; uint32_t arrestAt = 0;
    V3 lastKnown; uint32_t lastKnownAt = 0; int lastTargetRef = 0;
    uint32_t talkAt = 0;
    int defStage = 0;

    // --------------------------------- помощники ---------------------------------
    uint32_t Now() { return NowMs(); }
    static bool Ago(uint32_t t, uint32_t ms) { uint32_t n = NowMs(); return n < t || n - t >= ms; }
    Unit* Find(int ref) { for(auto& x : u) if(x.ref == ref) return &x; return nullptr; }
    uintptr_t LeaderPed() { return leader ? Ped(leader) : 0; }
    V3 Anchor() { uintptr_t l = LeaderPed(); if(l) return Pos(l); if(hasHome) return home; return u.empty() ? V3() : RefPos(u[0].ref); }
    float AnchorHeading() { uintptr_t l = LeaderPed(); return l ? EntHeading(l) : 0; }
    void Emit(const char* kind, const std::string& text, int who = 0)
    {
        Event e; e.kind = kind; e.text = text; e.who = who; e.ms = Now();
        events.push_back(e); while(events.size() > 24) events.pop_front();
        Log("ТАКТИКА", "[%s] событие %s: %s", tag.c_str(), kind, text.c_str());
    }
    void Say(Unit& x, const std::string& text, bool urgent = false)
    {
        if(!sayFn || (!urgent && !Ago(x.lastSay, 6000))) return;
        x.lastSay = Now();
        sayFn(x.ref, x.name, text, urgent);
    }
    Unit* ByRole(int role, int skip = 0) { for(auto& x : u) if(!x.dead && x.role == role && x.ref != skip) return &x; return nullptr; }
    std::vector<Unit*> Alive() { std::vector<Unit*> r; for(auto& x : u) if(!x.dead) r.push_back(&x); return r; }
    Threat* TopThreat() { Threat* b = nullptr; for(auto& t : threats) if(!t.surrender && (!b || t.score > b->score)) b = &t; return b; }
    Threat* ThreatOf(int ref) { for(auto& t : threats) if(t.ref == ref) return &t; return nullptr; }
    V3 ThreatCenter() { V3 c; int n = 0; for(auto& t : threats) if(!t.surrender) { c = Add(c, t.pos); n++; } return n ? Mul(c, 1.0f / n) : Anchor(); }

    // ======================================================================================================
    //                                   ЧУВСТВА (Ч01–Ч20)
    // ======================================================================================================
    bool Friendly(uintptr_t p)
    {
        if(!p) return false;
        if(p == Player()) return true;
        int r = Ref(p);
        for(auto& x : u) if(x.ref == r) return true;
        int t = PedType(p);
        return IsCopType(t) || t == 18 /*медик*/ || t == 19 /*пожарный*/;
    }
    bool OurSide(uintptr_t e)
    {   // Ч09: на кого направлен ствол — на нас (отряд, Артём) или на копов
        if(!e) return false;
        if(e == Player()) return true;
        int r = Ref(e);
        for(auto& x : u) if(x.ref == r) return true;
        return IsCopType(PedType(e));
    }
    // Ч03/Ч09: враг ли этот человек и насколько опасен
    bool IsEnemy(uintptr_t p, Threat& t)
    {
        if(!PedAlive(p) || Friendly(p)) return false;
        uintptr_t aim = AimTarget(p);
        int w = CurWeapon(p);
        t.armed = w >= 22 && w <= 38;                       // огнестрел
        t.aiming = aim && OurSide(aim);
        int ref = Ref(p);
        int stars = starsFn ? starsFn(ref) : 0;
        bool flagged = enemyFn && enemyFn(p);
        if(!(t.aiming || flagged || (stars >= 3 && t.armed))) return false;
        t.ref = ref;
        return true;
    }
    void Scan()
    {
        // раз в 0,5 с: кто вокруг опасен (Ч03, Ч04, Ч05, Ч09, Ч11, Ч14, Ч15, Ч20)
        V3 c = Anchor();
        std::vector<Threat> now;
        ForEachPed([&](uintptr_t p) {
            V3 pp = Pos(p);
            if(Dist(pp, c) > 85.0f) return;
            Threat t;
            if(!IsEnemy(p, t)) return;
            t.pos = pp; t.dist = Dist(pp, c);
            t.inCar = PedVehicle(p) != 0;
            t.elevated = pp.z - c.z > 3.5f;                     // Ч11/Ч15
            t.vel = Velocity(p);
            t.score = (t.armed ? 40 : 5) + (t.aiming ? 40 : 0) + 30.0f * (1 - std::min(t.dist, 80.0f) / 80.0f) + (t.elevated ? 10 : 0);
            Threat* old = ThreatOf(t.ref);
            t.firstSeen = old ? old->firstSeen : Now();
            t.lastSeen = old ? old->lastSeen : Now();
            t.visible = old ? old->visible : true;
            t.surrender = old ? old->surrender : false;
            t.lastPos = old ? old->lastPos : t.pos;
            now.push_back(t);
        });
        // Ч04 слух: стреляющие рядом — в список, даже если не видно, кто цель
        for(auto& t : now) t.shooting = Shooting(t.ref);
        // Ч05: память цели (кого видели — помним, куда двигался)
        for(auto& old : threats)
        {
            bool still = false; for(auto& t : now) if(t.ref == old.ref) still = true;
            if(!still && RefAlive(old.ref) && !Ago(old.lastSeen, 8000) && Dist(old.pos, c) < 120)
            { old.visible = false; now.push_back(old); }
        }
        threats.swap(now);
        if(!threats.empty())
        {
            Threat* tt = TopThreat();
            if(tt) { lastKnown = tt->pos; lastKnownAt = Now(); lastTargetRef = tt->ref; }
        }
    }
    void LosStep()
    {
        // Ч01: лучи по бюджету (~4 на тик): видит ли кто-то из отряда угрозу
        if(threats.empty() || u.empty()) return;
        for(int k = 0; k < 4; ++k)
        {
            losCursor = (losCursor + 1) % (int)threats.size();
            Threat& t = threats[losCursor];
            bool seen = false;
            for(auto& x : u)
            {
                if(x.dead) continue;
                V3 a = Add(RefPos(x.ref), Mk(0, 0, 0.6f)), b = Add(t.pos, Mk(0, 0, 0.6f));
                if(!Ray(a, b, nullptr, nullptr, true, true, false, true)) { seen = true; break; }
            }
            if(seen) { t.visible = true; t.lastSeen = Now(); t.lastPos = t.pos; }
            else if(Ago(t.lastSeen, 1500)) t.visible = false;
        }
    }
    // Ч02/Ч18: укрытие рядом от угрозы — точка, где стена/машина закрывает (на уровне груди присевшего)
    bool FindCover(const V3& from, const V3& threat, V3& out, float maxR = 14.0f, bool needPeek = true)
    {
        float best = 1e9; bool ok = false;
        float baseH = Heading(threat, from);
        for(int ring = 0; ring < 3; ++ring)
        {
            float r = 3.0f + ring * (maxR - 3.0f) / 2.0f;
            for(int i = 0; i < 8; ++i)
            {
                if(RaysThisFrame() > 26) return ok;
                float h = baseH + (i - 3.5f) * 30.0f;
                V3 cand = Add(from, Mul(Dir(h), r));
                cand.z = GroundZ(cand.x, cand.y, from.z + 2.0f);
                if(fabsf(cand.z - from.z) > 2.5f) continue;
                bool wz; float w = WaterZ(cand.x, cand.y, cand.z, &wz);       // Ч08: не в воду
                if(wz && w > cand.z + 0.3f) continue;
                V3 head = Add(cand, Mk(0, 0, 0.5f)), th = Add(threat, Mk(0, 0, 0.8f));
                if(!Ray(th, head, nullptr, nullptr, true, true, false, true)) continue;   // не закрыто
                if(needPeek)
                {   // выглянуть: шаг вбок — и цель видна
                    V3 side = Offset(cand, Heading(cand, threat), 1.2f, 0); side.z += 1.4f;
                    if(Ray(side, th, nullptr, nullptr, true, true, false, true)) { side = Offset(cand, Heading(cand, threat), -1.2f, 0); side.z += 1.4f;
                        if(Ray(side, th, nullptr, nullptr, true, true, false, true)) continue; }
                }
                bool taken = false;
                for(auto& x : u) if(x.hasCover && Dist2(x.cover, cand) < 2.0f) taken = true;
                if(taken) continue;
                float score = Dist(from, cand) + (Dist(cand, threat) < 8 ? 20 : 0);
                if(score < best) { best = score; out = cand; ok = true; }
            }
            if(ok) return true;
        }
        return ok;
    }
    // Ч16: мирные на линии огня
    bool CivilianInLine(const V3& a, const V3& b)
    {
        bool bad = false;
        V3 d = Sub(b, a); float L = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if(L < 1) return false;
        ForEachPed([&](uintptr_t p) {
            if(bad || !PedAlive(p) || p == Player()) return;
            int r = Ref(p); for(auto& x : u) if(x.ref == r) return;
            if(ThreatOf(r)) return;
            V3 q = Pos(p);
            float t = ((q.x - a.x) * d.x + (q.y - a.y) * d.y + (q.z - a.z) * d.z) / (L * L);
            if(t < 0.05f || t > 1.15f) return;
            V3 c = Add(a, Mul(d, t));
            if(Dist(c, q) < 1.3f) bad = true;
        });
        return bad;
    }
    // Ч08: опасное место — горящая машина рядом (взорвётся)
    bool Dangerous(const V3& p)
    {
        bool bad = false;
        ForEachVehicle([&](uintptr_t v) { if(!bad && VehHealth(v) > 0 && VehHealth(v) < 250 && Dist(Pos(v), p) < 9) bad = true; });
        return bad;
    }

    // ======================================================================================================
    //                                   РУКИ пешком (Д01–Д24)
    // ======================================================================================================
    enum TaskKind { T_NONE, T_GO, T_GO_AIM, T_AIM, T_SHOOT, T_SHOOT_AT, T_DUCK, T_FOLLOW, T_STAND, T_HEADING, T_KILL, T_GOTO_CHAR_AIM,
                    T_ENTER_DRV, T_ENTER_PASS, T_LEAVE, T_ANIM, T_FLEE, T_DRIVEBY, T_GOTO_OFFSET };
    bool Same(Unit& x, int k, int target, const V3& p, uint32_t refreshMs)
    {
        return x.task == k && x.taskTarget == target && Dist(x.taskP, p) < 1.0f && !Ago(x.taskAt, refreshMs);
    }
    void Mark(Unit& x, int k, int target, const V3& p) { x.task = k; x.taskTarget = target; x.taskP = p; x.taskAt = Now(); }

    // Д01/Д02: встать точно в точку (быстрый ход по обстановке) — точная доводка поворотом
    void GoTo(Unit& x, const V3& p, int moveState, float heading = -1)
    {
        uintptr_t ped = Ped(x.ref); if(!ped) return;
        float d = Dist2(Pos(ped), p);
        if(d < 0.7f)
        {
            if(heading >= 0 && fabsf(AngDiff(heading, EntHeading(ped))) > 20 && !Same(x, T_HEADING, 0, p, 2000))
            { Do(0x05D4, { Ai(x.ref), Af(heading) }); Mark(x, T_HEADING, 0, p); }   // TASK_ACHIEVE_HEADING
            return;
        }
        if(Same(x, T_GO, moveState, p, 4000)) return;
        Do(0x05D3, { Ai(x.ref), Af(p.x), Af(p.y), Af(p.z), Ai(moveState), Ai(20000) });   // TASK_GO_STRAIGHT_TO_COORD
        Mark(x, T_GO, moveState, p);
    }
    // Д03: присесть/встать
    void Duck(Unit& x, bool on)
    {
        if(x.ducked == on) return;
        Do(0x04EB, { Ai(x.ref), Ai(on ? 1 : 0) });                                       // TASK_TOGGLE_DUCK
        x.ducked = on;
    }
    // Д06: держать на мушке
    void AimAt(Unit& x, int target)
    {
        if(Same(x, T_AIM, target, V3(), 3000)) return;
        Do(0x0635, { Ai(x.ref), Ai(target), Ai(4000) });                                 // TASK_AIM_GUN_AT_CHAR
        Mark(x, T_AIM, target, V3());
    }
    // Д07/Д08: короткая очередь 2–4 выстрела / одиночный (стрелок)
    void Burst(Unit& x, int target, bool single)
    {
        uint32_t len = single ? 250 : (uint32_t)RndI(450, 800);
        Do(0x074D, { Ai(x.ref), Ai(target), Ai((int)len) });                             // TASK_SHOOT_AT_CHAR
        Mark(x, T_SHOOT, target, V3());
        x.burstUntil = Now() + len; x.lastFireAt = Now(); x.bursts++;
    }
    // Д09: подавляющий огонь по точке/окну
    void SuppressAt(Unit& x, const V3& p)
    {
        if(Same(x, T_SHOOT_AT, 0, p, 1800)) return;
        Do(0x0668, { Ai(x.ref), Af(p.x), Af(p.y), Af(p.z), Ai(1500) });                  // TASK_SHOOT_AT_COORD
        Mark(x, T_SHOOT_AT, 0, p); x.lastFireAt = Now();
    }
    // Д10: перезарядка заранее, в укрытии
    void MaybeReload(Unit& x, bool safe)
    {
        uintptr_t p = Ped(x.ref); if(!p) return;
        int w = CurWeapon(p); if(w < 22) return;
        int clip = ClipAmmo(p), size = ClipSize(w);
        if(safe && clip < size / 3 && Ago(x.reloadAt, 3000))
        {
            // у педов игра перезаряжает сама, когда магазин пуст: «отстреливаем» остаток в укрытии коротко в землю нельзя —
            // поэтому просто не высовываемся, пока не перезарядится (пауза в укрытии)
            x.reloadAt = Now(); x.doing = "перезаряжается";
            Do(0x05B9, { Ai(x.ref), Ai(900) });                                            // TASK_PAUSE
            Mark(x, T_STAND, 0, V3());
        }
    }
    // Д11: смена оружия по дистанции
    void PickWeapon(Unit& x, float dist)
    {
        uintptr_t p = Ped(x.ref); if(!p) return;
        int want = 0;
        if(x.role == R_MARKSMAN && dist > 20 && HasWeapon(p, 31)) want = 31;
        else if(dist < 7 && HasWeapon(p, 27)) want = 27;
        else if(dist < 7 && HasWeapon(p, 25)) want = 25;
        else if(dist > 25 && HasWeapon(p, 31)) want = 31;
        else if(HasWeapon(p, 29)) want = 29;
        else if(HasWeapon(p, 31)) want = 31;
        else if(HasWeapon(p, 24)) want = 24;
        else if(HasWeapon(p, 22)) want = 22;
        if(want && CurWeapon(p) != want) Do(0x01B9, { Ai(x.ref), Ai(want) });               // SET_CURRENT_CHAR_WEAPON
    }
    // Д12/Д13: идти к точке с оружием на цель (отход спиной вперёд / огонь на ходу)
    void MoveAiming(Unit& x, const V3& p, int target, int moveState)
    {
        if(Same(x, T_GO_AIM, target, p, 4000)) return;
        Do(0x06C2, { Ai(x.ref), Af(p.x), Af(p.y), Af(p.z), Ai(moveState), Af(0.5f), Af(0.8f), Ai(target), Af(0), Af(0), Af(0) });   // TASK_GO_TO_COORD_WHILE_AIMING
        Mark(x, T_GO_AIM, target, p);
    }
    // Д15: держать сектор
    void HoldSector(Unit& x, float heading)
    {
        V3 me = RefPos(x.ref);
        V3 look = Add(Add(me, Mul(Dir(heading), 15.0f)), Mk(0, 0, 1.0f));
        if(Same(x, T_AIM, -1, look, 5000)) return;
        Do(0x0667, { Ai(x.ref), Af(look.x), Af(look.y), Af(look.z), Ai(6000) });         // TASK_AIM_GUN_AT_COORD
        Mark(x, T_AIM, -1, look);
    }
    // Д16: подойти на «контакт» 2–3 м, держа на прицеле
    void Approach(Unit& x, int target, float r)
    {
        if(Same(x, T_GOTO_CHAR_AIM, target, V3(), 4000)) return;
        Do(0x07A3, { Ai(x.ref), Ai(target), Af(r), Af(r + 1.0f) });                      // TASK_GOTO_CHAR_AIMING
        Mark(x, T_GOTO_CHAR_AIM, target, V3());
    }
    void Follow(Unit& x, int target, float right, float back)
    {
        // идти за Артёмом со смещением в строю (Д02 по ситуации)
        uintptr_t l = Ped(target); if(!l) return;
        V3 slot = Offset(Pos(l), EntHeading(l), right, -back);
        slot.z = Pos(l).z;
        float d = Dist2(RefPos(x.ref), slot);
        int ms = d > 14 ? 7 : (d > 4 ? 6 : 4);
        if(d < 1.2f) { if(!Same(x, T_HEADING, 1, slot, 3000)) { Do(0x05D4, { Ai(x.ref), Af(EntHeading(l)) }); Mark(x, T_HEADING, 1, slot); } return; }
        if(Same(x, T_GO, ms, slot, 700) || (x.task == T_GO && Dist(x.taskP, slot) < 2.0f && !Ago(x.taskAt, 1500))) return;
        Do(0x05D3, { Ai(x.ref), Af(slot.x), Af(slot.y), Af(slot.z), Ai(ms), Ai(8000) });
        Mark(x, T_GO, ms, slot);
    }
    void Anim(Unit& x, const char* anim, const char* file, int ms, bool loop = false)
    {
        Do(0x0605, { Ai(x.ref), As(anim), As(file), Af(4.0f), Ai(loop), Ai(0), Ai(0), Ai(0), Ai(ms) });   // TASK_PLAY_ANIM
        Mark(x, T_ANIM, 0, V3());
    }

    // Бой «сам решает мелочи»: укрытие → выглянуть → очередь → назад; перезарядка, смена позиции, не по мирным
    void FightFrom(Unit& x, int target, bool allowMove)
    {
        uintptr_t me = Ped(x.ref); if(!me) return;
        Threat* t = ThreatOf(target);
        V3 tp = t ? t->pos : RefPos(target);
        V3 mp = Pos(me);
        float d = Dist(mp, tp);
        PickWeapon(x, d);
        // Ч16: мирные на линии — не стреляем (Т028)
        bool civ = CivilianInLine(Add(mp, Mk(0, 0, 0.7f)), Add(tp, Mk(0, 0, 0.7f)));
        // укрытие (Ч02): нет — ищем; есть — держимся
        if(allowMove && (!x.hasCover || Ago(x.coverAt, 20000) || Dist(x.cover, mp) > 18))
        {
            V3 c;
            if(FindCover(mp, tp, c)) { x.cover = c; x.hasCover = true; x.coverAt = Now(); }
            else x.hasCover = false;
        }
        if(allowMove && x.hasCover && Dist2(mp, x.cover) > 1.2f)
        {   // Д04: перебежка пригнувшись к укрытию
            Duck(x, false);
            MoveAiming(x, x.cover, target, 7);
            x.doing = "перебегает в укрытие";
            return;
        }
        if(x.hasCover)
        {
            Duck(x, true);
            MaybeReload(x, true);
            if(x.task == T_STAND && !Ago(x.taskAt, 900)) return;
            // Т025: после 3 серий — сменить позицию
            if(x.bursts >= 3 && allowMove) { x.bursts = 0; x.hasCover = false; x.coverAt = 0; x.doing = "меняет позицию"; return; }
            if(civ) { AimAt(x, target); x.doing = "не стреляет — мирные на линии"; return; }
            if(Now() > x.burstUntil + (uint32_t)(x.role == R_MARKSMAN ? 1400 : 900))
            {   // Д05: выглянуть и дать очередь
                Burst(x, target, x.role == R_MARKSMAN);
                x.doing = x.role == R_MARKSMAN ? "бьёт одиночными" : "ведёт огонь из укрытия";
            }
            return;
        }
        // укрытия нет: присесть и стрелять очередями (Д07), если цель рядом — на ходу (Д13)
        Duck(x, d > 12);
        if(civ) { AimAt(x, target); x.doing = "не стреляет — мирные на линии"; return; }
        if(Now() > x.burstUntil + 700) { Burst(x, target, x.role == R_MARKSMAN); x.doing = "ведёт огонь"; }
    }

    // ======================================================================================================
    //                                            РАЗДАЧА НАМЕРЕНИЙ
    // ======================================================================================================
    void SetAll(const Intent& in) { for(auto& x : u) if(!x.dead) x.in = in; }
    // строй: смещение (вправо, назад) по номеру в строю
    void Slot(int formation, int i, int n, float s, float& right, float& back)
    {
        switch(formation)
        {
            case 1: right = (i % 2 ? 0.8f : -0.8f); back = s * (i + 1); break;                       // колонна
            case 2: right = (i % 2 ? 1 : -1) * s * (i / 2 + 1); back = s * (i / 2 + 1); break;        // клин
            case 3: right = (i - (n - 1) * 0.5f) * s * 1.2f + (i >= (n - 1) * 0.5f ? s : -s) * 0.5f; back = 0.5f; break;  // шеренга
            case 4: { const float R[4] = { 0, s, 0, -s }, B[4] = { -s, 0, s, 0 }; right = R[i % 4]; back = B[i % 4]; break; } // ромб
            default: right = (i % 2 ? 1.5f : -1.5f); back = 2.5f + i * 1.5f; break;
        }
    }
    void DoFormation(int formation, float spacing)
    {
        auto al = Alive(); int n = (int)al.size();
        // ведущий впереди, замыкающий сзади
        std::stable_sort(al.begin(), al.end(), [](Unit* a, Unit* b) { auto rank = [](int r) { return r == R_LEAD ? 0 : r == R_REAR ? 9 : 5; }; return rank(a->role) < rank(b->role); });
        for(int i = 0; i < n; ++i)
        {
            Intent in; in.kind = I_FOLLOW; in.target = leader;
            float r, b; Slot(formation, i, n, spacing, r, b);
            in.p = Mk(r, b, 0); in.heading = (al[i]->role == R_REAR) ? 180.0f : 0.0f;
            al[i]->in = in;
        }
    }

    // Главная раздача: программа тактики → намерения бойцов
    void Program()
    {
        const TacticInfo* ti = FindTactic(ord.tactic);
        if(!ti) { DoFormation(4, 3.0f); return; }
        Threat* top = TopThreat();
        int tgt = ord.target ? ord.target : (top ? top->ref : 0);
        V3 tp = tgt ? RefPos(tgt) : (ord.hasPoint ? ord.point : Anchor());
        auto al = Alive();
        int n = (int)al.size();
        auto movers = [&](int& holderIdx) { holderIdx = 0; for(int i = 0; i < n; ++i) if(al[i]->role == R_MARKSMAN) holderIdx = i; };
        switch(ti->prog)
        {
            // ---------------- перемещение ----------------
            case P_FORMATION: DoFormation((int)ti->a, ti->b); break;
            case P_BOUND:
            {   // Т005/Т007/Т012: двойки — один прикрывает, другой бежит (смена каждые 3 с)
                V3 goal = ord.hasPoint ? ord.point : Add(Anchor(), Mul(Dir(AnchorHeading()), ti->b));
                int phase = (int)((Now() - ordAt) / 3000) % 2;
                for(int i = 0; i < n; ++i)
                {
                    Intent in;
                    bool runner = (i % 2) == phase;
                    V3 me = RefPos(al[i]->ref);
                    if(runner) { in.kind = I_MOVE; in.p = Add(me, Mul(Norm2(Sub(goal, me)), std::min(ti->b, Dist2(me, goal)))); in.sprint = true; in.crouch = true; }
                    else { in.kind = I_HOLD; in.p = me; in.heading = Heading(me, top ? top->pos : goal); in.crouch = true; }
                    al[i]->in = in;
                }
                break;
            }
            case P_WALL:
            {   // Т006: вдоль стены — смещаемся к ближайшей стене (луч вбок) и идём колонной
                DoFormation(1, 2.2f);
                uintptr_t l = LeaderPed();
                if(l)
                {
                    V3 lp = Pos(l); float h = EntHeading(l);
                    for(int s = -1; s <= 1; s += 2)
                    {
                        V3 hit; V3 side = Offset(lp, h, s * 6.0f, 0); side.z += 1;
                        if(Ray(Add(lp, Mk(0, 0, 1)), side, &hit)) { float off = s * (Dist2(lp, hit) - ti->b); for(auto* x : al) x->in.p.x += off; break; }
                    }
                }
                break;
            }
            case P_SHIELD_CAR:
            {   // Т008: идём за машиной (машина Артёма или ближайшая наша)
                uintptr_t l = LeaderPed(); uintptr_t v = l ? PedVehicle(l) : 0;
                if(!v) { DoFormation(1, 2.5f); break; }
                V3 vp = Pos(v); float vh = EntHeading(v);
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_MOVE; in.p = Offset(vp, vh, (i - 1) * 1.2f, -4.0f); in.crouch = true; al[i]->in = in; }
                break;
            }
            case P_SPLIT_BLOCK:
            {   // Т009: две группы обходят квартал слева и справа и встречаются у цели
                V3 goal = ord.hasPoint ? ord.point : tp; V3 me = Anchor(); float h = Heading(me, goal);
                for(int i = 0; i < n; ++i)
                {
                    float s = (i % 2) ? 1.0f : -1.0f;
                    Intent in; in.kind = I_MOVE; in.sprint = true;
                    V3 mid = Offset(Add(me, Mul(Sub(goal, me), 0.5f)), h, s * ti->b * 0.5f, 0);
                    in.p = Dist2(RefPos(al[i]->ref), mid) > 6 && (Now() - ordAt) < 25000 ? mid : goal;
                    al[i]->in = in;
                }
                break;
            }
            case P_CLIMB:
            {   // Т010/Т011: на крышу/с крыши — к точке (лестницы игры педы проходят сами), остальное как строй
                V3 goal = ord.hasPoint ? ord.point : Anchor();
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_MOVE; in.p = Offset(goal, 0, (i - 1) * 1.5f, 0); al[i]->in = in; }
                break;
            }
            case P_LISTEN:
            {   // Т013: стоп — слушать: присели и держат сектора
                V3 c = Anchor();
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_HOLD; in.p = RefPos(al[i]->ref); in.heading = AnchorHeading() + i * (360.0f / std::max(n, 1)); in.crouch = true; al[i]->in = in; }
                (void)c;
                if(Ago(ordAt, (uint32_t)(ti->b * 1000)) && ordStep == 0) { ordStep = 1; for(auto* x : al) { Say(*x, threats.empty() ? "Тихо." : "Слышу движение!"); break; } }
                break;
            }
            case P_SPREAD:
            {   // Т014: рассредоточиться (не стоять кучей)
                V3 c = Anchor();
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_MOVE; in.crouch = true; in.p = Add(c, Mul(Dir(i * 360.0f / std::max(n, 1) + 45), ti->b)); al[i]->in = in; }
                break;
            }
            case P_GATHER:
            {   // Т015/Т054: собраться у Артёма
                DoFormation(4, ti->b);
                if(ti->a > 0 && ordStep == 0) { ordStep = 1; if(n) Say(*al[0], "Чисто, выходим."); }
                break;
            }
            // ---------------- огневой контакт ----------------
            case P_TAKE_COVER: case P_RETURN_FIRE: case P_STAGGER_RELOAD:
            {   // Т016/Т017/Т034: каждый своё укрытие, ответный огонь по стрелкам; перезарядка по очереди
                for(int i = 0; i < n; ++i)
                {
                    Intent in; in.kind = I_FIGHT;
                    // распределить цели: каждый — свою (Ч13 сектора)
                    int k = 0; Threat* pick = nullptr;
                    for(auto& t : threats) if(!t.surrender) { if(k++ == i % std::max((int)threats.size(), 1)) pick = &t; }
                    in.target = pick ? pick->ref : tgt;
                    in.mayFire = !(ti->prog == P_STAGGER_RELOAD && (i % 2) == (int)((Now() / 2500) % 2));
                    al[i]->in = in;
                }
                break;
            }
            case P_SUPPRESS_FLANK: case P_PINCER:
            {   // Т018–Т021: стрелок (или первый) держит, остальные заходят сбоку (сторона из параметра/приказа)
                int holder = 0; movers(holder);
                float side = ord.side ? (float)ord.side : (ti->a != 0 ? ti->a : 1.0f);
                V3 from = Anchor(); float h = Heading(from, tp);
                for(int i = 0; i < n; ++i)
                {
                    Intent in;
                    if(i == holder) { in.kind = I_SUPPRESS; in.target = tgt; in.p = tp; }
                    else
                    {
                        float s = ti->prog == P_PINCER ? ((i % 2) ? 1.0f : -1.0f) : side;
                        in.kind = I_FLANK; in.target = tgt;
                        in.p = Offset(tp, h, s * 14.0f, -4.0f);
                    }
                    al[i]->in = in;
                }
                break;
            }
            case P_FALLBACK:
            {   // Т022: отход перекатами — по очереди: один стреляет, другой отходит на 8 м назад
                int phase = (int)((Now() - ordAt) / 2500) % 2;
                V3 away = ord.hasPoint ? ord.point : Add(Anchor(), Mul(Norm2(Sub(Anchor(), tp)), 25.0f));
                for(int i = 0; i < n; ++i)
                {
                    Intent in;
                    if((i % 2) == phase) { in.kind = I_FALLBACK; in.target = tgt; V3 me = RefPos(al[i]->ref); in.p = Add(me, Mul(Norm2(Sub(away, me)), ti->b)); }
                    else { in.kind = I_FIGHT; in.target = tgt; in.dist = -1; }   // без перемещения
                    al[i]->in = in;
                }
                break;
            }
            case P_COVER_WOUNDED: case P_MEDIC:
            {   // Т023/Т024/Т132: раненый уходит назад, медик к нему, остальные прикрывают
                Unit* w = nullptr; for(auto* x : al) if(x->wounded) w = x;
                if(!w) { ord.tactic = threats.empty() ? 4 : 16; ordStep = 0; break; }
                Unit* medic = ByRole(R_MEDIC, w->ref); if(!medic) for(auto* x : al) if(x != w) { medic = x; break; }
                for(auto* x : al)
                {
                    Intent in;
                    if(x == w) { in.kind = I_FALLBACK; in.target = tgt; in.p = Add(RefPos(x->ref), Mul(Norm2(Sub(RefPos(x->ref), tp)), 10.0f)); }
                    else if(x == medic && ti->prog == P_MEDIC) { in.kind = I_MEDIC; in.target = w->ref; }
                    else { in.kind = I_FIGHT; in.target = tgt; }
                    x->in = in;
                }
                break;
            }
            case P_SHIFT:
                for(auto* x : al) { x->hasCover = false; x->coverAt = 0; Intent in; in.kind = I_FIGHT; in.target = tgt; x->in = in; }
                break;
            case P_COUNTERSNIPE:
            {   // Т026: ищем стрелка на высоте; стрелок бьёт одиночными, остальные подавляют
                Threat* hi = nullptr; for(auto& t : threats) if(t.elevated && (!hi || t.score > hi->score)) hi = &t;
                int target = hi ? hi->ref : tgt;
                for(auto* x : al) { Intent in; in.kind = x->role == R_MARKSMAN ? I_FIGHT : I_SUPPRESS; in.target = target; in.p = hi ? hi->pos : tp; x->in = in; }
                break;
            }
            case P_FIRE_VEHICLE: case P_SHOOT_TIRES:
            {   // Т027/Т083: огонь по машине стрелков — по колёсам/двигателю (точка у капота и колёс)
                uintptr_t v = ord.targetCar ? Veh(ord.targetCar) : (tgt ? PedVehicle(Ped(tgt)) : 0);
                if(!v) { for(auto* x : al) { Intent in; in.kind = I_FIGHT; in.target = tgt; x->in = in; } break; }
                V3 vp = Pos(v); float vh = EntHeading(v);
                for(int i = 0; i < n; ++i)
                {
                    Intent in; in.kind = I_SUPPRESS; in.target = 0;
                    in.p = ti->prog == P_SHOOT_TIRES ? Offset(vp, vh, (i % 2 ? 0.9f : -0.9f), (i < 2 ? 1.4f : -1.4f)) : Offset(vp, vh, 0, 1.8f);
                    in.p.z = vp.z - (ti->prog == P_SHOOT_TIRES ? 0.4f : 0.0f);
                    al[i]->in = in;
                }
                break;
            }
            case P_HOLD_FIRE:
                for(auto* x : al) { Intent in; in.kind = I_HOLD; in.p = RefPos(x->ref); in.target = tgt; in.mayFire = false; in.heading = Heading(RefPos(x->ref), tp); x->in = in; }
                break;
            case P_CEASE_FIRE:
            {   // Т029/Т066: сдался — не стрелять, держать на мушке
                if(Threat* t = ThreatOf(tgt)) t->surrender = true;
                for(auto* x : al) { Intent in; in.kind = I_ARREST_COVER; in.target = tgt; x->in = in; }
                break;
            }
            case P_TAKE_ALIVE:
            {   // Т030: взять выжившего — не добивать, подходим и задерживаем
                for(int i = 0; i < n; ++i) { Intent in; in.kind = i == 0 ? I_ARREST_TALK : I_ARREST_COVER; in.target = tgt; al[i]->in = in; }
                break;
            }
            case P_PERIMETER:
            {   // Т031: периметр — каждый держит свой сектор вокруг точки
                V3 c = ord.hasPoint ? ord.point : Anchor();
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_HOLD; float h = i * 360.0f / std::max(n, 1); in.p = Add(c, Mul(Dir(h), ti->b)); in.heading = h; al[i]->in = in; }
                break;
            }
            case P_CHECK_BODIES: case P_SCAVENGE:
            {   // Т032/Т033: проверить тела, забрать оружие/патроны
                for(int i = 0; i < n; ++i) { Intent in; in.kind = ti->prog == P_SCAVENGE ? I_SCAVENGE : I_CHECK_BODY; al[i]->in = in; }
                break;
            }
            case P_FOCUS_FIRE:
            {   // Т035: огонь по цели Артёма
                uintptr_t pt = PlayerTarget();
                int t = pt ? Ref(pt) : tgt;
                for(auto* x : al) { Intent in; in.kind = I_FIGHT; in.target = t; x->in = in; }
                break;
            }
            case P_GUARD_BACK:
            {   // Т036/Т113: прикрыть спину Артёма — за ним, лицом назад
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_GUARD_BACK; in.target = leader; in.dist = ti->b + i * 1.5f; in.p = Mk((i - (n - 1) * 0.5f) * 2.0f, 0, 0); al[i]->in = in; }
                break;
            }
            case P_HOLD_POINT:
            {   // Т037/Т055/Т122: держать вход/улицу/точку
                V3 c = ord.hasPoint ? ord.point : Anchor();
                float face = threats.empty() ? AnchorHeading() + 180 : Heading(c, ThreatCenter());
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_HOLD; in.p = Offset(c, face, (i - (n - 1) * 0.5f) * 2.5f, -1.5f); in.heading = face; in.crouch = true; al[i]->in = in; }
                break;
            }
            case P_SUPPRESS_WINDOW:
            {   // Т038/Т052: подавление/проверка окна — точка (приказ) или где был стрелок
                V3 w = ord.hasPoint ? ord.point : (lastKnownAt ? lastKnown : tp);
                for(auto* x : al) { Intent in; in.kind = ti->a > 0 ? I_HOLD : I_SUPPRESS; in.p = ti->a > 0 ? RefPos(x->ref) : w; in.heading = Heading(RefPos(x->ref), w); x->in = in; }
                break;
            }
            case P_FLUSH:
            {   // Т039: вытеснить — двое давят огнём, один обходит
                for(int i = 0; i < n; ++i) { Intent in; in.kind = i == n - 1 ? I_FLANK : I_SUPPRESS; in.target = tgt; in.p = i == n - 1 ? Offset(tp, Heading(Anchor(), tp), 12.0f, 6.0f) : tp; al[i]->in = in; }
                break;
            }
            case P_CAR_FIGHT:
            {   // Т040: бой у машин — укрытие за ближайшей машиной (со стороны двигателя, Ч18)
                for(auto* x : al)
                {
                    Intent in; in.kind = I_FIGHT; in.target = tgt;
                    uintptr_t best = 0; float bd = 25;
                    V3 me = RefPos(x->ref);
                    ForEachVehicle([&](uintptr_t v) { float d = Dist(Pos(v), me); if(d < bd && VehHealth(v) > 300) { bd = d; best = v; } });
                    if(best) { V3 vp = Pos(best); V3 away = Norm2(Sub(vp, tp)); x->cover = Add(Offset(vp, EntHeading(best), 0, 1.4f), Mul(away, 1.6f)); x->hasCover = true; x->coverAt = Now(); }
                    x->in = in;
                }
                break;
            }
            // ---------------- углы и здания ----------------
            case P_PIE:
            {   // Т041: «нарезка пирога» — дуга вокруг угла с шагом, оружие на угол
                V3 corner = ord.hasPoint ? ord.point : Add(Anchor(), Mul(Dir(AnchorHeading()), 6.0f));
                float step = (float)((Now() - ordAt) / 1500 % 7);
                for(int i = 0; i < n; ++i)
                {
                    Intent in; in.kind = i == 0 ? I_PIE : I_HOLD;
                    float h = Heading(Anchor(), corner) - 60 + step * 20;
                    in.p = i == 0 ? Add(corner, Mul(Dir(h + 180), 4.0f)) : Offset(RefPos(al[0]->ref), Heading(Anchor(), corner), (i % 2 ? 1 : -1) * 1.2f, -1.5f * i);
                    in.heading = Heading(in.p, corner); in.crouch = i > 0;
                    al[i]->in = in;
                }
                break;
            }
            case P_STACK:
            {   // Т042/Т044/Т051: построение у двери — вдоль стены по обе стороны, оружие на дверь
                std::vector<Door> doors; V3 c = ord.hasPoint ? ord.point : Anchor();
                Doors(c, 25.0f, doors);
                V3 door = doors.empty() ? Add(Anchor(), Mul(Dir(AnchorHeading()), 4.0f)) : doors[0].at;
                float dh = Heading(Anchor(), door);
                for(int i = 0; i < n; ++i)
                {
                    Intent in; in.kind = I_HOLD; in.crouch = (i % 2) == 0;
                    float side = ti->a == 1 ? ((i % 2) ? 1.0f : -1.0f) : 1.0f;
                    in.p = Offset(door, dh, side * 1.6f, -1.0f - (ti->a == 1 ? 0 : i * 0.9f));
                    in.heading = Heading(in.p, door);
                    al[i]->in = in;
                }
                break;
            }
            case P_ENTER_AFTER:
            {   // Т043/Т053: вход за Артёмом — колонной вплотную (маркер интерьера игры переносит всех рядом)
                DoFormation(1, 1.6f);
                break;
            }
            case P_CLEAR_AREA:
            {   // Т045–Т047: зачистка — шеренгой по точкам вокруг центра, каждый со своим сектором
                V3 c = ord.hasPoint ? ord.point : Anchor();
                int k = (int)((Now() - ordAt) / 6000) % 4;
                for(int i = 0; i < n; ++i)
                {
                    Intent in; in.kind = threats.empty() ? I_MOVE : I_FIGHT; in.target = tgt;
                    float h = (k * 90.0f) + i * 25.0f;
                    in.p = Add(c, Mul(Dir(h), ti->b * (ti->a == 1 ? 0.8f : 0.5f)));
                    in.heading = h;
                    al[i]->in = in;
                }
                if(threats.empty() && Ago(ordAt, 24000) && ordStep == 0) { ordStep = 1; if(n) Say(*al[0], "Чисто!"); Emit("clear", "участок зачищен"); }
                break;
            }
            case P_STAIRS:
            {   // Т048: один вверх, один вниз (сектора по вертикали — смотрим вверх/вниз по лестнице: вперёд/назад)
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_FOLLOW; in.target = leader; in.p = Mk(0, 1.5f + i, 0); in.heading = (i % 2) ? 180.0f : 0.0f; al[i]->in = in; }
                break;
            }
            case P_ROOF_CHECK:
            {   // Т049: крыша — проверить углы и встать периметром
                V3 c = ord.hasPoint ? ord.point : Anchor();
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_HOLD; float h = 45 + i * 90.0f; in.p = Add(c, Mul(Dir(h), ti->b * 0.6f)); in.p.z = c.z; in.heading = h; al[i]->in = in; }
                break;
            }
            case P_BACK_DOOR:
            {   // Т050: контроль заднего выхода — один-два бойца по другую сторону здания (дальний вход рядом)
                std::vector<Door> doors; Doors(Anchor(), 60.0f, doors);
                V3 back = doors.size() > 1 ? doors[1].at : Add(Anchor(), Mul(Dir(AnchorHeading()), 30.0f));
                for(int i = 0; i < n; ++i) { Intent in; in.kind = i == 0 ? I_HOLD : I_FOLLOW; in.target = leader; in.p = i == 0 ? back : Mk(1.5f, 2.0f, 0); in.heading = Heading(back, Anchor()); al[i]->in = in; }
                break;
            }
            // ---------------- задержание ----------------
            case P_ARREST:
            {   // Т056–Т060/Т065/Т069/Т073/Т074: один говорит и задерживает, остальные держат на прицеле
                if(!tgt) { ord.tactic = 4; break; }
                int talker = 0; for(int i = 0; i < n; ++i) if(al[i]->role == R_LEAD) talker = i;
                for(int i = 0; i < n; ++i)
                {
                    Intent in; in.target = tgt;
                    in.kind = i == talker ? I_ARREST_TALK : I_ARREST_COVER;
                    if(ti->a == 6) in.crouch = true;                         // вооружённый — из укрытий
                    if(ti->a == 8 && i == n - 1 && threats.size() > 1) in.target = threats[1].ref;   // второй подозреваемый
                    al[i]->in = in;
                }
                break;
            }
            case P_ESCORT_CELL:
            {   // Т061–Т064: конвой задержанного — пешком/в машину/на базу
                int prisoner = tgt ? tgt : (prisoners.empty() ? 0 : prisoners.back());
                for(int i = 0; i < n; ++i)
                {
                    Intent in; in.kind = i == 0 ? I_ESCORT : I_FOLLOW; in.target = i == 0 ? prisoner : leader;
                    in.mode = (int)ti->a; in.p = Mk(i % 2 ? 1.6f : -1.6f, 2.5f, 0);
                    al[i]->in = in;
                }
                break;
            }
            case P_CHASE_FOOT:
            {   // Т067/Т089: быстрый бежит следом, остальные отрезают по прогнозу
                V3 vel = tgt ? Velocity(Ped(tgt)) : V3();
                for(int i = 0; i < n; ++i)
                {
                    Intent in; in.target = tgt;
                    if(i == 0) { in.kind = I_CHASE; }
                    else { in.kind = I_INTERCEPT; in.p = Add(tp, Add(Mul(vel, 4.0f + i * 2.0f), Mul(Dir(Heading(Mk(0,0,0), vel) + (i % 2 ? 90 : -90)), 6.0f))); }
                    al[i]->in = in;
                }
                break;
            }
            case P_INTERCEPT_FOOT:
            {   // Т068: перехват на следующей улице — к ближайшему узлу пеших дорог впереди беглеца
                V3 vel = tgt ? Velocity(Ped(tgt)) : V3();
                V3 ahead = Add(tp, Mul(vel, 6.0f)), node;
                if(ClosestPedNode(ahead, node)) ahead = node;
                for(int i = 0; i < n; ++i) { Intent in; in.target = tgt; in.kind = i == 0 ? I_CHASE : I_INTERCEPT; in.p = Offset(ahead, 0, (i - 1) * 3.0f, 0); al[i]->in = in; }
                break;
            }
            case P_CROWD:
            {   // Т070: толпа — встать шеренгой между толпой и Артёмом, без стрельбы, оттеснять (подходим вплотную)
                V3 c = tgt ? tp : Add(Anchor(), Mul(Dir(AnchorHeading()), 6.0f));
                float h = Heading(Anchor(), c);
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_HOLD; in.mayFire = false; in.p = Offset(Add(Anchor(), Mul(Dir(h), 3.0f)), h, (i - (n - 1) * 0.5f) * 1.4f, 0); in.heading = h; al[i]->in = in; }
                break;
            }
            case P_BRAWL:
            {   // Т071/Т072: драка — кулаками (убираем оружие), задерживаем
                for(auto* x : al) { Intent in; in.kind = I_ARREST_TALK; in.target = tgt; in.mode = 1; x->in = in; }
                break;
            }
            case P_HANDOVER:
            {   // Т075: передать задержанного подмоге — вызываем полицию, держим, пока не приедет
                if(ordStep == 0) { ordStep = 1; if(backupFn) backupFn(0, tp); if(n) Say(*al[0], "Пришлите машину за задержанным."); }
                for(auto* x : al) { Intent in; in.kind = I_ARREST_COVER; in.target = tgt; x->in = in; }
                break;
            }
            // ---------------- машины и погони ----------------
            case P_PURSUE: case P_INTERCEPT_CAR: case P_PIT: case P_BOX: case P_ROADBLOCK: case P_FELONY_STOP: case P_PULL_DRIVER:
            case P_RECOVER_CAR: case P_SHORTCUT: case P_EVADE_RAM: case P_CONVOY: case P_BLOCK_EXIT: case P_EVAC: case P_MOTO_SCOUT:
            case P_SWAP_DRIVER: case P_DEPLOY: case P_DRIVE_ME: case P_WAIT_CAR: case P_BRING_CAR:
                VehicleProgram(*ti, tgt, tp);
                break;
            case P_SEARCH:
            {   // Т088/Т123: прочёсывание от последней точки — веером по кругу
                V3 c = ord.hasPoint ? ord.point : (lastKnownAt ? lastKnown : tp);
                int k = (int)((Now() - ordAt) / 7000);
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_MOVE; float h = (k * 70.0f) + i * (360.0f / std::max(n, 1)); in.p = Add(c, Mul(Dir(h), ti->b * (0.3f + 0.2f * (k % 3)))); in.p.z = GroundZ(in.p.x, in.p.y, c.z + 5); al[i]->in = in; }
                if(!threats.empty()) { ord.tactic = 16; ordAt = Now(); Emit("found", "нашли цель"); }
                break;
            }
            case P_HELI_GUIDE: case P_HELI_WATCH: case P_HELI_GUNNER: case P_HELI_ROOF_DROP: case P_HELI_ROOF_PICK: case P_HELI_ORBIT:
            case P_HELI_FOLLOW: case P_HELI_DECOY: case P_HELI_LAND: case P_HELI_EVADE: case P_HELI_HOME:
                HeliProgram(*ti, tgt, tp);
                break;
            // ---------------- приказы Артёма ----------------
            case P_SHADOW:
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_FOLLOW; in.target = leader; in.p = Mk(i % 2 ? 0.9f : -0.9f, ti->b + i * 1.2f, 0); al[i]->in = in; }
                break;
            case P_TAKE_POS: case P_TAKE_ROOF:
            {   // Т115/Т116: позиция — куда целится Артём или метка; крыша — ближайшая высокая точка там же
                V3 pos = ord.hasPoint ? ord.point : V3(); bool ok = ord.hasPoint;
                if(!ok) { uintptr_t pt = PlayerTarget(); if(pt) { pos = Pos(pt); ok = true; } }
                if(!ok) ok = PlayerWaypoint(pos);
                if(!ok) pos = Add(Anchor(), Mul(Dir(AnchorHeading()), 15.0f));
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_HOLD; in.p = Offset(pos, Heading(Anchor(), pos), (i - (n - 1) * 0.5f) * 2.0f, 0); in.p.z = GroundZ(in.p.x, in.p.y, pos.z + (ti->prog == P_TAKE_ROOF ? 30 : 3)); in.heading = Heading(Anchor(), pos); in.crouch = true; al[i]->in = in; }
                break;
            }
            case P_CHECK_CAR:
            {   // Т117: проверить машину — один подходит на контакт, другой держит
                uintptr_t v = ord.targetCar ? Veh(ord.targetCar) : 0;
                if(!v) { V3 a = Anchor(); float bd = 30; ForEachVehicle([&](uintptr_t c) { float d = Dist(Pos(c), a); if(d < bd && d > 2) { bd = d; v = c; } }); }
                V3 vp = v ? Pos(v) : tp;
                uintptr_t drv = v ? VehDriver(v) : 0;
                for(int i = 0; i < n; ++i) { Intent in; in.kind = i == 0 ? (drv ? I_ARREST_TALK : I_MOVE) : I_HOLD; in.target = drv ? Ref(drv) : 0; in.p = i == 0 ? Offset(vp, v ? EntHeading(v) : 0, -1.8f, 0.5f) : Offset(vp, Heading(vp, Anchor()), (i % 2 ? 2 : -2), 6); in.heading = Heading(in.p, vp); al[i]->in = in; }
                break;
            }
            case P_FETCH_ARMOR:
            {   // Т118/Т133: в оружейную за бронёй/патронами и назад
                for(int i = 0; i < n; ++i) { Intent in; in.kind = (ti->a > 0 || i == 0) ? I_GOTO_ARMORY : I_FOLLOW; in.target = leader; in.p = Mk(i % 2 ? 1.5f : -1.5f, 2.5f, 0); al[i]->in = in; }
                break;
            }
            case P_CALL_BACKUP:
            {   // Т120: вызвать подмогу по рации
                if(ordStep == 0)
                {
                    ordStep = 1;
                    int kind = threats.size() >= 6 ? 1 : 0;
                    if(backupFn) backupFn(kind, threats.empty() ? Anchor() : ThreatCenter());
                    if(n) { Unit* r = ByRole(R_LEAD); Say(r ? *r : *al[0], kind ? "Центр, нужен S.W.A.T., много стволов!" : "Центр, нужна подмога!", true); }
                    Emit("backup", kind ? "вызван S.W.A.T." : "вызвана полиция");
                }
                for(auto* x : al) { Intent in; in.kind = threats.empty() ? I_FOLLOW : I_FIGHT; in.target = threats.empty() ? leader : tgt; in.p = Mk(1.5f, 2.5f, 0); x->in = in; }
                break;
            }
            case P_WATCH:
            {   // Т121: следить за ним — держим дистанцию и линию видимости, не нападаем
                for(int i = 0; i < n; ++i) { Intent in; in.kind = I_WATCH; in.target = tgt; in.dist = ti->b + i * 4; al[i]->in = in; }
                break;
            }
            case P_GIVE_ARMOR:
            {   // Т127/Д24: отдать броню Артёму (у кого больше)
                Unit* best = nullptr; for(auto* x : al) if(!best || Armour(Ped(x->ref)) > Armour(Ped(best->ref))) best = x;
                for(auto* x : al) { Intent in; in.kind = x == best ? I_GIVE : I_FOLLOW; in.target = leader; in.p = Mk(1.5f, 2.5f, 0); x->in = in; }
                break;
            }
            case P_TALK:
                TalkProgram((int)ti->a);
                for(auto* x : al) if(x->in.kind == I_IDLE) { Intent in; in.kind = I_FOLLOW; in.target = leader; in.p = Mk(x->ref % 2 ? 1.5f : -1.5f, 2.5f, 0); x->in = in; }
                break;
            default:
                DefenseProgram(*ti, tgt, tp);
                break;
        }
    }

    // ---------------- машины (Т076–Т100, Т119, Т124, Т125, Т139) ----------------
    int UnitCar(Unit& x) { uintptr_t p = Ped(x.ref); uintptr_t v = p ? PedVehicle(p) : 0; return v ? VehRef(v) : 0; }
    int NearCar(const V3& at, float r, bool policeOnly)
    {
        int best = 0; float bd = r;
        ForEachVehicle([&](uintptr_t v) {
            if(VehHealth(v) < 300 || IsHeli(v)) return;
            int m = Model(v);
            bool police = m == 596 || m == 597 || m == 598 || m == 599 || m == 427 || m == 490 || m == 528 || m == 601 || m == 523;
            if(policeOnly && !police) return;
            uintptr_t d = VehDriver(v); if(d && d != Player() && !Find(Ref(d))) return;   // занята чужим
            float dist = Dist(Pos(v), at); if(dist < bd) { bd = dist; best = VehRef(v); }
        });
        return best;
    }
    void VehicleProgram(const TacticInfo& ti, int tgt, const V3& tp)
    {
        auto al = Alive(); int n = (int)al.size(); if(!n) return;
        uintptr_t lp = LeaderPed();
        int leaderCar = lp && PedVehicle(lp) ? VehRef(PedVehicle(lp)) : 0;
        int targetCar = ord.targetCar ? ord.targetCar : (tgt && Ped(tgt) && PedVehicle(Ped(tgt)) ? VehRef(PedVehicle(Ped(tgt))) : 0);
        // своя машина отряда: где сидит водитель, иначе машина Артёма, иначе ближайшая полицейская/любая
        Unit* driver = ByRole(R_DRIVER); if(!driver) driver = al[0];
        int car = UnitCar(*driver);
        if(!car) car = NearCar(RefPos(driver->ref), 40, true);
        if(!car) car = NearCar(RefPos(driver->ref), 30, false);
        bool artemDrives = lp && leaderCar && VehDriver(Veh(leaderCar)) == lp;
        // все, кроме водителя, садятся к водителю или к Артёму
        for(auto* x : al)
        {
            Intent in;
            if(x == driver && car && !(artemDrives && car == leaderCar)) { in.kind = I_DRIVE; in.car = car; }
            else { in.kind = I_IN_CAR; in.car = artemDrives ? leaderCar : car; in.seat = -1; }
            in.target = tgt;
            x->in = in;
        }
        if(driver->in.kind != I_DRIVE) return;
        Intent& d = driver->in;
        d.target = targetCar; d.p = tp; d.dist = 0; d.heading = -1;
        switch(ti.prog)
        {
            case P_PURSUE:      d.mode = ti.a > 0 ? drv::D_OFFROAD : drv::D_FOLLOW; d.dist = ti.b; break;   // Т076/Т097
            case P_PIT:         d.mode = drv::D_PIT; d.dist = ord.side ? (float)ord.side : 1.0f; break;      // Т078
            case P_BOX:         d.mode = drv::D_SIDE; d.dist = 3.5f; break;                                  // Т079 (наша — сбоку; остальное — подмога)
            case P_EVADE_RAM:   d.mode = drv::D_SIDE; d.dist = -6.0f; break;                                 // Т090: уйти вбок
            case P_CONVOY:      d.mode = drv::D_CONVOY; d.target = targetCar ? targetCar : leaderCar; d.dist = ti.a > 0 ? 9.0f : 14.0f; break;   // Т091/Т092
            case P_INTERCEPT_CAR: case P_SHORTCUT:
            {   // Т077/Т086: перехват — узел дороги впереди по прогнозу (Ч06/Ч07)
                uintptr_t tv = targetCar ? Veh(targetCar) : 0;
                V3 ahead = tv ? Add(Pos(tv), Mul(Velocity(tv), 8.0f)) : tp, node; float hdg = 0;
                if(NthCarNode(ahead, 1, node, hdg)) ahead = node;
                d.mode = drv::D_BLOCK; d.p = ahead; d.heading = hdg;
                break;
            }
            case P_ROADBLOCK: case P_BLOCK_EXIT:
            {   // Т080/Т095/Т099: заслон поперёк дороги у ближайшего узла по пути цели
                V3 at = ord.hasPoint ? ord.point : tp, node; float hdg = 0;
                if(NthCarNode(at, 1, node, hdg)) at = node;
                d.mode = drv::D_BLOCK; d.p = at; d.heading = hdg;
                break;
            }
            case P_FELONY_STOP: case P_PULL_DRIVER:
            {   // Т081/Т082/Т084: опасная остановка — встаём клином за целью, все выходят с оружием
                uintptr_t tv = targetCar ? Veh(targetCar) : 0;
                if(tv && Speed(tv) < 2.0f)
                {
                    for(auto* x : al) { Intent in; in.kind = (x == al[0] && ti.prog == P_PULL_DRIVER) ? I_ARREST_TALK : I_ARREST_COVER; in.target = VehDriver(tv) ? Ref(VehDriver(tv)) : tgt; x->in = in; }
                    return;
                }
                d.mode = drv::D_FOLLOW; d.dist = 8.0f;
                break;
            }
            case P_RECOVER_CAR: d.mode = drv::D_FOLLOW; d.dist = 10.0f; break;                                  // Т085
            case P_EVAC:        d.mode = drv::D_ROUTE; d.p = hasPlace[0] ? places[0] : tp; d.target = 0; break;   // Т096
            case P_MOTO_SCOUT:  d.mode = drv::D_ROUTE; d.p = ord.hasPoint ? ord.point : tp; d.target = 0; break;  // Т098
            case P_SWAP_DRIVER: d.mode = drv::D_WAIT; break;                                                       // Т100: остановиться
            case P_DEPLOY:
            {   // Т093/Т094: подъехать, остановиться бортом и высадиться за машинами
                V3 at = ord.hasPoint ? ord.point : tp;
                d.mode = drv::D_STOP_AT; d.p = Add(at, Mul(Norm2(Sub(Anchor(), at)), 15.0f)); d.target = 0;
                if(car && drv::Arrived(car, 5)) for(auto* x : al) { Intent in; in.kind = I_LEAVE_CAR; in.target = tgt; x->in = in; }
                break;
            }
            case P_DRIVE_ME:
            {   // Т124: отвези меня — Артём садится пассажиром, водитель везёт к метке
                V3 to; bool has = ord.hasPoint; to = ord.point;
                if(!has) has = PlayerWaypoint(to);
                d.mode = has ? drv::D_ROUTE : drv::D_WAIT; d.p = to; d.target = 0;
                break;
            }
            case P_WAIT_CAR:    d.mode = drv::D_WAIT; break;                                                       // Т125/Т139
            case P_BRING_CAR:   d.mode = drv::D_STOP_AT; d.p = Add(Anchor(), Mul(Dir(AnchorHeading() + 90), 4.0f)); d.target = 0; break;   // Т119
            default:            d.mode = drv::D_FOLLOW; d.dist = 20; break;
        }
    }

    // ---------------- вертолёт (Т087, Т101–Т112, Т126) ----------------
    void HeliProgram(const TacticInfo& ti, int tgt, const V3& tp)
    {
        auto al = Alive(); int n = (int)al.size(); if(!n) return;
        Unit* pilot = ByRole(R_PILOT); if(!pilot) pilot = al[0];
        int heli = 0;
        if(uintptr_t pv = PedVehicle(Ped(pilot->ref))) if(IsHeli(pv)) heli = VehRef(pv);
        if(!heli)
        {   // ближайший вертолёт (площадка базы)
            float bd = 120; V3 me = RefPos(pilot->ref);
            ForEachVehicle([&](uintptr_t v) { if(!IsHeli(v) || VehHealth(v) < 300) return; uintptr_t d = VehDriver(v); if(d && d != Ped(pilot->ref)) return; float dd = Dist(Pos(v), me); if(dd < bd) { bd = dd; heli = VehRef(v); } });
        }
        if(!heli) { DoFormation(4, 3); Emit("noheli", "нет вертолёта рядом"); ord.tactic = 4; return; }
        int targetCar = ord.targetCar ? ord.targetCar : (tgt && Ped(tgt) && PedVehicle(Ped(tgt)) ? VehRef(PedVehicle(Ped(tgt))) : 0);
        for(auto* x : al)
        {
            Intent in; in.target = tgt;
            if(x == pilot) { in.kind = I_PILOT; in.car = heli; }
            else { in.kind = I_IN_CAR; in.car = heli; in.seat = -1; }
            x->in = in;
        }
        Intent& p = pilot->in;
        p.p = ord.hasPoint ? ord.point : tp; p.target = tgt; p.seat = targetCar; p.dist = ti.b; p.heading = 25;
        switch(ti.prog)
        {
            case P_HELI_WATCH:     p.mode = drv::H_ORBIT; p.heading = 45; break;
            case P_HELI_GUIDE:     p.mode = targetCar ? drv::H_FOLLOW : drv::H_ORBIT; p.heading = 40;
                                   if(ordStep == 0) { ordStep = 1; Say(*pilot, "Вижу цель сверху, веду!", true); } break;
            case P_HELI_GUNNER:    p.mode = drv::H_BROADSIDE; p.heading = 20;
                                   if(ti.a > 0) p.p = Anchor(); break;
            case P_HELI_ROOF_DROP: case P_HELI_ROOF_PICK: case P_HELI_LAND:
                                   p.mode = drv::H_LAND; break;
            case P_HELI_ORBIT:     p.mode = drv::H_ORBIT; break;
            case P_HELI_FOLLOW:    p.mode = drv::H_FOLLOW; break;
            case P_HELI_DECOY:     p.mode = drv::H_ORBIT; p.heading = 35; p.p = Add(tp, Mul(Dir(Heading(Anchor(), tp)), 50.0f)); break;
            case P_HELI_EVADE:     p.mode = drv::H_EVADE; break;
            case P_HELI_HOME:      p.mode = drv::H_LAND; p.p = hasPlace[3] ? places[3] : p.p; break;
        }
        // высадка: сели — пассажиры выходят (Т104, Т109, Т110)
        if((ti.prog == P_HELI_ROOF_DROP || ti.prog == P_HELI_LAND) && drv::Landed(heli))
            for(auto* x : al) if(x != pilot) { Intent in; in.kind = I_LEAVE_CAR; in.target = tgt; x->in = in; }
    }

    // ---------------- дисциплина и разговоры (Т128–Т140) ----------------
    void TalkProgram(int what)
    {
        if(ordStep) return;
        ordStep = 1;
        auto al = Alive(); if(al.empty()) return;
        switch(what)
        {
            case 0: Say(*al[0], threats.empty() ? "Центр, у нас тихо." : "Центр, ведём бой!", true); break;
            case 1: { int i = 1; for(auto* x : al) { char b[64]; snprintf(b, sizeof(b), "%s — на месте, номер %d.", x->name.c_str(), i++); Say(*x, b, true); } break; }
            case 2: Say(*al[0], "Чисто!", true); break;
            case 3: for(auto* x : al) { std::string s = std::string("Я — ") + RoleName(x->role) + "."; Say(*x, s, true); } break;
            case 4: Emit("debrief", "разбор после операции"); break;
            case 5: Emit("banter", "спокойно — можно пошутить"); break;
            case 6: break;
            case 7: Say(*al[0], "Так точно, Артём!", true); break;
            case 8: Emit("argue", "спор в характере"); break;
            case 9:
            {   // Т140: отдых на базе — к зоне отдыха
                for(auto* x : al) { Intent in; in.kind = I_REST; x->in = in; }
                break;
            }
        }
    }

    // ---------------- оборона базы (Т141–Т158) — исполнитель сам, без ИИ ----------------
    void DefenseProgram(const TacticInfo& ti, int tgt, const V3& tp)
    {
        auto al = Alive(); int n = (int)al.size();
        switch(ti.prog)
        {
            case P_DEF_ALARM: case P_DEF_ARM: case P_DEF_GATE: case P_DEF_POSTS: case P_DEF_FRANK: case P_DEF_WOUNDED:
            case P_DEF_BLOCK_GATE: case P_DEF_FLUSH: case P_DEF_CHASE_GATE: case P_DEF_PRISONERS: case P_DEF_BACKUP:
            case P_DEF_STOLEN_CAR: case P_DEF_JAILBREAK: case P_DEF_HELI_UP:
            {
                int pi = 0;
                for(int i = 0; i < n; ++i)
                {
                    Unit& x = *al[i]; Intent in; in.target = tgt;
                    uintptr_t p = Ped(x.ref);
                    bool armed = p && CurWeapon(p) >= 22;
                    if(x.wounded && hasPlace[0]) { in.kind = I_FALLBACK; in.p = places[0]; }                  // Т148
                    else if(!armed && hasPlace[1]) { in.kind = I_GOTO_ARMORY; }                                // Т142
                    else if(threats.empty() && !posts.empty()) { in.kind = I_HOLD; in.p = posts[pi++ % posts.size()]; in.heading = Heading(home, in.p); in.crouch = true; }   // Т144–Т146
                    else if(!threats.empty())
                    {
                        Threat* nearest = nullptr; float bd = 1e9; V3 me = Pos(p);
                        for(auto& t : threats) if(!t.surrender) { float d = Dist(t.pos, me); if(d < bd) { bd = d; nearest = &t; } }
                        in.target = nearest ? nearest->ref : tgt;
                        bool inside = nearest && hasHome && Dist2(nearest->pos, home) < homeR;
                        // Т150: прорвались во двор — обход; Т151: отступают — до ворот, не дальше
                        if(inside && i % 3 == 2) { in.kind = I_FLANK; in.p = Offset(nearest->pos, Heading(me, nearest->pos), 10, 0); }
                        else { in.kind = I_FIGHT; if(!posts.empty() && !inside) { x.cover = posts[i % posts.size()]; x.hasCover = true; x.coverAt = Now(); } }
                        if(hasHome && nearest && Dist2(nearest->pos, home) > homeR + 15) in.kind = I_HOLD, in.p = posts.empty() ? home : posts[i % posts.size()];
                    }
                    else { in.kind = I_HOLD; in.p = home; }
                    x.in = in;
                }
                break;
            }
            case P_DEF_ALLCLEAR: case P_DEF_STANDDOWN:
                for(auto* x : al) { Intent in; in.kind = I_IDLE; x->in = in; }
                break;
            default: break;
        }
    }

    // ======================================================================================================
    //                                      ИСПОЛНЕНИЕ НАМЕРЕНИЙ (руки)
    // ======================================================================================================
    void Execute(Unit& x)
    {
        uintptr_t p = Ped(x.ref);
        if(!p) return;
        Intent& in = x.in;
        uintptr_t pv = PedVehicle(p);
        // в машине, а намерение пешее — выйти (кроме «сидеть»)
        bool carIntent = in.kind == I_IN_CAR || in.kind == I_DRIVE || in.kind == I_PILOT || in.kind == I_ENTER_CAR;
        if(pv && !carIntent && in.kind != I_IDLE && in.kind != I_REST)
        {
            if(!Same(x, T_LEAVE, 0, V3(), 4000)) { Do(0x0633, { Ai(x.ref) }); Mark(x, T_LEAVE, 0, V3()); x.doing = "выходит из машины"; }   // TASK_LEAVE_ANY_CAR
            return;
        }
        switch(in.kind)
        {
            case I_IDLE: x.doing = x.defender ? "на посту" : "ждёт"; break;
            case I_FOLLOW:
            {
                Duck(x, false);
                int t = in.target ? in.target : leader;
                if(!t) break;
                uintptr_t lp = Ped(t);
                if(lp && PedVehicle(lp))
                {   // Артём сел в машину — к нему пассажиром (Д36)
                    Intent e; e.kind = I_IN_CAR; e.car = VehRef(PedVehicle(lp)); x.in = e; Execute(x); x.in = in; break;
                }
                Follow(x, t, in.p.x, in.p.y);
                x.doing = in.heading > 90 ? "замыкает, смотрит назад" : "идёт в строю";
                if(in.heading > 90 && Dist2(RefPos(x.ref), RefPos(t)) < 5 && x.task != T_GO) HoldSector(x, AnchorHeading() + 180);
                break;
            }
            case I_MOVE:
                Duck(x, false);
                if(Dangerous(in.p)) { in.p = Add(in.p, Mul(Norm2(Sub(RefPos(x.ref), in.p)), 8.0f)); }
                GoTo(x, in.p, in.sprint ? 7 : 6, in.heading);
                x.doing = in.crouch ? "перебегает" : "выдвигается";
                if(Dist2(RefPos(x.ref), in.p) < 1.0f && in.crouch) Duck(x, true);
                break;
            case I_HOLD:
            {
                float d = Dist2(RefPos(x.ref), in.p);
                if(d > 1.0f) { Duck(x, false); GoTo(x, in.p, d > 10 ? 7 : 6, in.heading); x.doing = "занимает позицию"; break; }
                Threat* t = in.target ? ThreatOf(in.target) : TopThreat();
                if(t && in.mayFire && t->visible) { FightFrom(x, t->ref, false); break; }
                Duck(x, in.crouch);
                if(t && !in.mayFire) { AimAt(x, t->ref); x.doing = "держит на прицеле, не стреляет"; }
                else { HoldSector(x, in.heading >= 0 ? in.heading : AnchorHeading()); x.doing = "держит сектор"; }
                break;
            }
            case I_FIGHT:
            {
                Threat* t = ThreatOf(in.target);
                if(!t) t = TopThreat();
                if(!t) { Duck(x, false); Follow(x, leader ? leader : x.ref, 1.5f, 2.5f); x.doing = "ищет цель"; break; }
                if(!in.mayFire) { Duck(x, true); x.doing = "перезаряжается по очереди"; MaybeReload(x, true); break; }
                FightFrom(x, t->ref, in.dist >= 0);
                break;
            }
            case I_SUPPRESS:
            {
                Duck(x, true);
                V3 at = in.target ? RefPos(in.target) : in.p;
                if(CivilianInLine(RefPos(x.ref), at)) { x.doing = "не стреляет — мирные на линии"; AimAt(x, in.target ? in.target : x.ref); break; }
                SuppressAt(x, Add(at, Mk(0, 0, 0.8f)));
                x.doing = "подавляет огнём";
                break;
            }
            case I_FLANK:
            {
                float d = Dist2(RefPos(x.ref), in.p);
                if(d > 2.0f) { Duck(x, false); MoveAiming(x, in.p, in.target, 7); x.doing = "заходит с фланга"; }
                else if(in.target) { FightFrom(x, in.target, false); x.doing = "бьёт с фланга"; }
                break;
            }
            case I_FALLBACK:
                Duck(x, false);
                if(in.target && RefAlive(in.target)) MoveAiming(x, in.p, in.target, 6); else GoTo(x, in.p, 7);
                x.doing = x.wounded ? "ранен, отходит" : "отходит";
                break;
            case I_ARREST_TALK: ArrestTalk(x, in); break;
            case I_ARREST_COVER:
                if(in.target && RefAlive(in.target))
                {
                    float d = Dist(RefPos(x.ref), RefPos(in.target));
                    if(d > 9) Approach(x, in.target, 6.0f); else AimAt(x, in.target);
                    Duck(x, in.crouch);
                    x.doing = "держит на прицеле";
                }
                break;
            case I_CHASE:
                if(in.target && RefAlive(in.target))
                {
                    if(!Same(x, T_FOLLOW, in.target, V3(), 5000)) { Do(0x0850, { Ai(x.ref), Ai(in.target) }); Mark(x, T_FOLLOW, in.target, V3()); }   // TASK_FOLLOW_FOOTSTEPS
                    x.doing = "бежит за беглецом";
                    if(Dist(RefPos(x.ref), RefPos(in.target)) < 3.0f) { Intent a = in; a.kind = I_ARREST_TALK; x.in = a; }
                }
                break;
            case I_INTERCEPT:
                GoTo(x, in.p, 7); x.doing = "отрезает путь";
                if(in.target && Dist(RefPos(x.ref), RefPos(in.target)) < 6) { Intent a = in; a.kind = I_ARREST_COVER; x.in = a; }
                break;
            case I_MEDIC: Medic(x, in.target); break;
            case I_SCAVENGE: case I_CHECK_BODY: Scavenge(x, in.kind == I_SCAVENGE); break;
            case I_GUARD_BACK:
            {   // за Артёмом, лицом назад
                int t = in.target ? in.target : leader; uintptr_t lp = Ped(t); if(!lp) break;
                V3 spot = Offset(Pos(lp), EntHeading(lp), in.p.x, -in.dist);
                if(Dist2(RefPos(x.ref), spot) > 1.5f) { GoTo(x, spot, Dist2(RefPos(x.ref), spot) > 8 ? 7 : 6); x.doing = "прикрывает спину"; }
                else { Threat* th = TopThreat(); if(th && th->visible) FightFrom(x, th->ref, false); else HoldSector(x, EntHeading(lp) + 180); x.doing = "прикрывает спину"; }
                break;
            }
            case I_WATCH:
            {
                if(!in.target || !RefAlive(in.target)) { x.doing = "потерял из виду"; break; }
                V3 tp = RefPos(in.target), me = RefPos(x.ref);
                float d = Dist2(me, tp);
                if(d > in.dist + 8) GoTo(x, Add(tp, Mul(Norm2(Sub(me, tp)), in.dist)), 6);
                else if(!Same(x, T_HEADING, 2, tp, 4000)) { Do(0x06A9, { Ai(x.ref), Af(tp.x), Af(tp.y), Af(tp.z + 1), Ai(5000) }); Mark(x, T_HEADING, 2, tp); }   // TASK_LOOK_AT_COORD
                x.doing = "следит";
                break;
            }
            case I_ENTER_CAR: case I_IN_CAR:
            {
                if(!in.car || !Veh(in.car)) { x.doing = "ищет машину"; break; }
                uintptr_t v = Veh(in.car);
                if(pv == v)
                {
                    x.doing = "в машине";
                    // Д34: пассажиры стреляют по колёсам/цели из окна
                    if(in.target && RefAlive(in.target) && Dist(Pos(v), RefPos(in.target)) < 35 && !Same(x, T_DRIVEBY, in.target, V3(), 6000))
                    {
                        V3 tp = RefPos(in.target);
                        Do(0x0713, { Ai(x.ref), Ai(in.target), Ai(-1), Af(tp.x), Af(tp.y), Af(tp.z), Af(30.0f), Ai(4), Ai(0), Ai(60) });   // TASK_DRIVE_BY
                        Mark(x, T_DRIVEBY, in.target, V3()); x.doing = "стреляет из окна";
                    }
                    break;
                }
                if(Same(x, T_ENTER_PASS, in.car, V3(), 6000)) { x.doing = "садится"; break; }
                // свободное место пассажира
                int seat = -1, maxp = VehMaxPassengers(v);
                for(int s = 0; s < maxp; ++s) if(!VehPassenger(v, s)) { bool taken = false; for(auto& o : u) if(&o != &x && o.task == T_ENTER_PASS && o.taskTarget == in.car && (int)o.taskP.x == s) taken = true; if(!taken) { seat = s; break; } }
                if(seat < 0) { x.doing = "нет места"; Follow(x, leader ? leader : x.ref, 2, 3); break; }
                Do(0x05CA, { Ai(x.ref), Ai(in.car), Ai(10000), Ai(seat) });                       // TASK_ENTER_CAR_AS_PASSENGER
                Mark(x, T_ENTER_PASS, in.car, Mk((float)seat, 0, 0)); x.doing = "садится";
                break;
            }
            case I_LEAVE_CAR:
                if(pv) { if(!Same(x, T_LEAVE, 0, V3(), 4000)) { Do(0x0633, { Ai(x.ref) }); Mark(x, T_LEAVE, 0, V3()); } x.doing = "высаживается"; }
                else { Intent f; f.kind = in.target ? I_FIGHT : I_HOLD; f.target = in.target; f.p = RefPos(x.ref); x.in = f; }
                break;
            case I_DRIVE:
            {
                uintptr_t v = Veh(in.car);
                if(!v) break;
                if(pv != v || VehDriver(v) != p)
                {
                    if(!Same(x, T_ENTER_DRV, in.car, V3(), 7000)) { Do(0x05CB, { Ai(x.ref), Ai(in.car), Ai(10000) }); Mark(x, T_ENTER_DRV, in.car, V3()); }   // TASK_ENTER_CAR_AS_DRIVER
                    x.doing = "садится за руль";
                    break;
                }
                // ждём пассажиров (Д36): кто едет с нами, но ещё не сел
                bool waiting = false;
                for(auto& o : u) if(!o.dead && &o != &x && o.in.kind == I_IN_CAR && o.in.car == in.car && PedVehicle(Ped(o.ref)) != v && Dist(RefPos(o.ref), Pos(v)) < 25) waiting = true;
                uintptr_t lp = LeaderPed();
                if(lp && ord.tactic != 98 && ord.tactic != 96 && PedVehicle(lp) != v && Dist(Pos(lp), Pos(v)) < 20 && (ord.tactic == 124 || ord.tactic == 125 || ord.tactic == 139)) waiting = true;
                if(waiting) drv::Drive(in.car, x.ref, drv::D_WAIT, Pos(v), 0, 0, 0);
                else drv::Drive(in.car, x.ref, in.mode ? in.mode : drv::D_FOLLOW, in.p, in.target, 32.0f, in.dist, in.heading);
                x.doing = std::string("за рулём: ") + drv::Doing(in.car);
                break;
            }
            case I_PILOT:
            {
                uintptr_t v = Veh(in.car);
                if(!v) break;
                if(pv != v)
                {
                    if(!Same(x, T_ENTER_DRV, in.car, V3(), 7000)) { Do(0x05CB, { Ai(x.ref), Ai(in.car), Ai(10000) }); Mark(x, T_ENTER_DRV, in.car, V3()); }
                    x.doing = "садится за штурвал";
                    break;
                }
                bool waiting = false;
                for(auto& o : u) if(!o.dead && &o != &x && o.in.kind == I_IN_CAR && o.in.car == in.car && PedVehicle(Ped(o.ref)) != v && Dist(RefPos(o.ref), Pos(v)) < 30) waiting = true;
                if(waiting && Pos(v).z - GroundZ(Pos(v).x, Pos(v).y, Pos(v).z) < 3) { x.doing = "ждёт посадки"; break; }
                drv::Fly(in.car, x.ref, in.mode, in.p, in.target, in.seat > 0 ? in.seat : 0, in.dist, in.heading);
                x.doing = std::string("пилот: ") + drv::HeliDoing(in.car);
                break;
            }
            case I_GOTO_ARMORY:
            {
                if(!hasPlace[1]) { x.doing = "оружейной нет"; break; }
                float d = Dist2(RefPos(x.ref), places[1]);
                if(d > 2.0f) { GoTo(x, places[1], 7); x.doing = "бежит в оружейную"; break; }
                // взял оружие и броню (оружейная базы — как у игрока: бесплатно, только полицейское)
                if(!HasWeapon(p, 31)) swat::GiveWeapon(x.ref, 31, 300);   // M4
                if(!HasWeapon(p, 29)) swat::GiveWeapon(x.ref, 29, 300);   // MP5
                if(!HasWeapon(p, 25)) swat::GiveWeapon(x.ref, 25, 60);    // дробовик
                if(Armour(p) < 50) Do(0x035F, { Ai(x.ref), Ai(100) });                // ADD_ARMOUR_TO_CHAR
                Do(0x01B9, { Ai(x.ref), Ai(31) });
                Say(x, "Взял ствол и броню.");
                Intent f; f.kind = x.defender ? I_IDLE : I_FOLLOW; f.target = leader; f.p = Mk(1.5f, 2.5f, 0); x.in = f;
                break;
            }
            case I_GIVE:
            {   // Д24: отдать броню Артёму
                int t = in.target ? in.target : leader; uintptr_t tp = Ped(t); if(!tp) break;
                if(Dist(RefPos(x.ref), Pos(tp)) > 2.2f) { GoTo(x, Pos(tp), 6); x.doing = "несёт броню"; break; }
                float a = Armour(p);
                if(a > 5)
                {
                    Anim(x, "DEALER_DEAL", "DEALER", 2000);
                    Do(0x035F, { Ai(t), Ai((int)a) });
                    *(float*)(p + 0x6B4) = 0;                    // CPed::m_fArmour — свою отдал
                    Say(x, "Держи, Артём, тебе нужнее.");
                }
                Intent f; f.kind = I_FOLLOW; f.target = leader; f.p = Mk(1.5f, 2.5f, 0); x.in = f;
                break;
            }
            case I_ESCORT: Escort(x, in); break;
            case I_REST:
                if(hasPlace[6]) { if(Dist2(RefPos(x.ref), places[6]) > 3) GoTo(x, Offset(places[6], 0, (x.ref % 3) - 1.0f, 0), 4); else if(x.task != T_ANIM || Ago(x.taskAt, 30000)) Anim(x, "LKUP_LOOP", "BAR", 30000, true); }
                x.doing = "отдыхает";
                break;
            case I_PIE:
            {
                GoTo(x, in.p, 4, in.heading);
                Duck(x, false);
                if(Dist2(RefPos(x.ref), in.p) < 1.2f) HoldSector(x, in.heading);
                x.doing = "режет пирог у угла";
                break;
            }
            default: break;
        }
    }

    // Д16–Д20: задержание — подойти, голосом и жестом, наручники, обыск
    void ArrestTalk(Unit& x, Intent& in)
    {
        int t = in.target;
        if(!t || !RefAlive(t)) { x.doing = "цели нет"; return; }
        Threat* th = ThreatOf(t);
        uintptr_t tp = Ped(t);
        float d = Dist(RefPos(x.ref), Pos(tp));
        bool armedShooting = th && th->shooting && !th->surrender;
        if(armedShooting) { FightFrom(x, t, true); x.doing = "открыл огонь — подозреваемый стреляет"; return; }   // Т073
        if(in.mode == 1 && CurWeapon(Ped(x.ref)) != 0) Do(0x01B9, { Ai(x.ref), Ai(0) });   // Т071: кулаки
        if(d > 3.0f) { Approach(x, t, 2.5f); x.doing = "подходит на контакт"; if(arrestStage == 0 && d < 12) { Say(x, "Полиция! Руки вверх!", true); arrestStage = 1; arrestAt = Now(); } return; }
        if(arrestStage < 2 && Ago(arrestAt, 1200))
        {   // Д17: «На землю!» — руки вверх
            Say(x, "На землю! Руки за голову!", true);
            Do(0x05C4, { Ai(t), Ai(8000) });                                                   // TASK_HANDS_UP
            if(th) th->surrender = true;
            arrestStage = 2; arrestAt = Now();
            return;
        }
        if(arrestStage == 2 && Ago(arrestAt, 2500))
        {   // Д18/Д19: наручники и обыск
            Anim(x, "plc_drgbst_02", "POLICE", 2500);
            Do(0x0605, { Ai(t), As("crm_drgbst_01"), As("POLICE"), Af(4.0f), Ai(0), Ai(0), Ai(0), Ai(1), Ai(-1) });   // задержанный стоит, руки за спиной
            Say(x, "Задержан. Обыскал — чисто.", true);
            arrestStage = 3; arrestAt = Now();
            bool known = false; for(int p : prisoners) if(p == t) known = true;
            if(!known) prisoners.push_back(t);
            if(arrestedFn) arrestedFn(t);
            Emit("arrest", x.name + " задержал подозреваемого", t);
            return;
        }
        if(arrestStage == 3) { AimAt(x, t); x.doing = "задержанный под охраной"; }
    }
    // Д20: вести задержанного рядом / посадить в машину / в камеру
    void Escort(Unit& x, Intent& in)
    {
        int t = in.target;
        if(!t || !RefAlive(t)) { x.doing = "задержанного нет"; return; }
        uintptr_t tp = Ped(t);
        // задержанный идёт рядом с конвоиром
        if(Dist(Pos(tp), RefPos(x.ref)) > 2.5f && !PedVehicle(tp))
            Do(0x06A8, { Ai(t), Ai(x.ref), Ai(-1), Af(1.2f), Af(150.0f) });                     // TASK_GOTO_CHAR_OFFSET — сзади-сбоку
        V3 goal = Anchor();
        if(in.mode >= 3 && hasPlace[2]) goal = places[2];
        uintptr_t lp = LeaderPed();
        if(in.mode >= 1 && lp && PedVehicle(lp) && !PedVehicle(tp))
        {   // Т061/Т062: в машину Артёма на заднее сиденье
            int car = VehRef(PedVehicle(lp));
            Do(0x05CA, { Ai(t), Ai(car), Ai(10000), Ai(1) });
            x.doing = "сажает задержанного в машину";
            Intent e; e.kind = I_IN_CAR; e.car = car; x.in = e;
            return;
        }
        if(Dist2(RefPos(x.ref), goal) > 3) GoTo(x, goal, 4);
        x.doing = "ведёт задержанного";
        if(in.mode >= 3 && hasPlace[2] && Dist2(Pos(tp), places[2]) < 4)
        {
            if(deliveredFn) deliveredFn(t);
            prisoners.erase(std::remove(prisoners.begin(), prisoners.end(), t), prisoners.end());
            Emit("delivered", "задержанный в камере", t);
            Intent f; f.kind = I_FOLLOW; f.target = leader; f.p = Mk(1.5f, 2.5f, 0); x.in = f;
        }
    }
    // Д22: перевязать своего
    void Medic(Unit& x, int ref)
    {
        Unit* w = Find(ref);
        uintptr_t wp = Ped(ref);
        if(!w || !wp) { x.doing = "раненого нет"; return; }
        if(Dist(RefPos(x.ref), Pos(wp)) > 1.8f) { Duck(x, false); GoTo(x, Pos(wp), 7); x.doing = "бежит к раненому"; return; }
        if(x.task == T_ANIM && !Ago(x.taskAt, 4000)) return;
        Duck(x, true);
        Anim(x, "CPR", "MEDIC", 4000);
        int hp = (int)Health(wp) + 40; if(hp > 100) hp = 100;
        Do(0x0223, { Ai(ref), Ai(hp) });                                                     // SET_CHAR_HEALTH
        w->wounded = false;
        Say(x, "Держись, перевязал!", true);
        Emit("healed", x.name + " перевязал " + w->name, ref);
        x.doing = "перевязывает";
    }
    // Д21: подобрать оружие/патроны (у убитого врага)
    void Scavenge(Unit& x, bool ammo)
    {
        V3 me = RefPos(x.ref); uintptr_t body = 0; float bd = 30;
        ForEachPed([&](uintptr_t p) { if(PedAlive(p) || Friendly(p)) return; int r = Ref(p); if(r == x.bodyChecked) return; float d = Dist(Pos(p), me); if(d < bd) { bd = d; body = p; } });
        if(!body) { x.doing = "тел рядом нет"; Follow(x, leader ? leader : x.ref, 1.5f, 2.5f); return; }
        if(bd > 1.6f) { GoTo(x, Pos(body), 6); x.doing = ammo ? "идёт за патронами" : "проверяет тело"; return; }
        Anim(x, "PICKUP_box", "CARRY", 1200);
        x.bodyChecked = Ref(body);
        uintptr_t me2 = Ped(x.ref);
        int w = CurWeapon(me2);
        if(w >= 22) swat::GiveWeapon(x.ref, w, ClipSize(w) * 2);                    // патроны из магазинов убитого
        Say(x, ammo ? "Забрал патроны." : "Готов. Оружие убрал.");
    }

    // ======================================================================================================
    //                                ПРАВИЛА без головы (§9) и события
    // ======================================================================================================
    void Rules()
    {
        // стреляют — Т016 + Т017; ранен свой — Т023 + Т024; бегущий — Т067; сдался — Т056–Т059; Артём в машине — Т036 + Т124;
        // тишина — Т004 / Т015
        uintptr_t lp = LeaderPed();
        bool wounded = false; for(auto& x : u) if(!x.dead && x.wounded) wounded = true;
        int want = 0;
        Threat* top = TopThreat();
        bool fleeing = top && !top->shooting && Speed(Ped(top->ref)) > 4.0f && !top->inCar;
        if(wounded && !threats.empty()) want = 24;
        else if(top && top->surrender) want = 56;
        else if(fleeing && starsFn && starsFn(top->ref) > 0) want = 67;
        else if(!threats.empty()) want = Ago(lastContact, 1500) && !inCombat ? 16 : 17;
        else if(lp && PedVehicle(lp)) want = 124;
        else want = 4;
        if(defender()) want = threats.empty() ? (defStage ? 153 : 155) : 144;
        if(want != ord.tactic && (ord.tactic == 0 || Ago(ordAt, 2500)))
        {
            Order o; o.tactic = want; o.why = "правило без головы";
            if(top) o.target = top->ref;
            SetOrder(o);
        }
    }
    bool defender() { for(auto& x : u) if(x.defender) return true; return false; }
    void SetOrder(const Order& o)
    {
        bool same = o.tactic == ord.tactic && o.target == ord.target;
        ord = o; if(!same) { ordAt = Now(); ordStep = 0; arrestStage = 0; }
        const TacticInfo* ti = FindTactic(o.tactic);
        Log("ТАКТИКА", "[%s] Т%03d %s%s%s", tag.c_str(), o.tactic, ti ? ti->name : "?", o.why.empty() ? "" : " — ", o.why.c_str());
    }

    void CheckHealth()
    {
        for(auto& x : u)
        {
            uintptr_t p = Ped(x.ref);
            bool alive = p && PedAlive(p);
            if(!alive && !x.dead) { x.dead = true; x.deadAt = Now(); Emit("died", x.name + " погиб", x.ref); x.doing = "погиб"; continue; }
            if(!alive) continue;
            float hp = Health(p);
            if(hp < 40 && !x.wounded) { x.wounded = true; Emit("wounded", x.name + " ранен", x.ref); Say(x, "Я ранен!", true); }
            if(hp > 60) x.wounded = false;
            x.lastHp = hp;
        }
    }

    void Tick10()
    {
        CheckHealth();
        if(Ago(lastScan, 500)) { lastScan = Now(); Scan(); }
        LosStep();
        bool combat = false; for(auto& t : threats) if(!t.surrender && (t.shooting || t.aiming || t.visible)) combat = true;
        if(combat && !inCombat) { inCombat = true; lastContact = Now(); announcedClear = false; Emit("contact", ContactText()); for(auto& x : u) if(!x.dead) { Say(x, ContactShout(), true); break; } }
        if(!combat && inCombat && Ago(lastContact, 3000)) { inCombat = false; clearSince = Now(); }
        if(combat) lastContact = Now();
        if(!inCombat && !announcedClear && Ago(clearSince, 6000))
        {   // Ч17: «чисто» — нет живых угроз 6 с
            announcedClear = true; Emit("clear", "угроз нет — чисто");
            for(auto& x : u) if(!x.dead) { Say(x, "Чисто!", true); break; }
            if(defender()) defStage = 1;
        }
        if(lastTargetRef && !RefAlive(lastTargetRef) && lastKnownAt) { Emit("target_down", "цель нейтрализована", lastTargetRef); lastTargetRef = 0; }
        else if(lastTargetRef && lastKnownAt && Ago(lastKnownAt, 10000) && !ThreatOf(lastTargetRef)) { Emit("lost", "цель ушла"); lastTargetRef = 0; }
        if(autoOn) Rules();
        Program();
    }
    std::string ContactText()
    {
        Threat* t = TopThreat(); if(!t) return "контакт";
        char b[200]; snprintf(b, sizeof(b), "контакт: %d угроз(ы), ближайшая в %.0f м%s%s", (int)threats.size(), t->dist, t->elevated ? ", стрелок на высоте" : "", t->inCar ? ", в машине" : "");
        return b;
    }
    std::string ContactShout()
    {
        Threat* t = TopThreat(); if(!t) return "Контакт!";
        if(t->elevated) return "Стрелок на крыше!";
        if(t->inCar) return "Огонь из машины!";
        return threats.size() > 2 ? "Контакт! Их много!" : "Контакт!";
    }

    std::string Situation(size_t maxLen)
    {
        std::string s; char b[300];
        V3 a = Anchor();
        uintptr_t lp = LeaderPed();
        if(lp) { snprintf(b, sizeof(b), "Артём: здоровье %.0f, броня %.0f, %s, смотрит на %.0f°%s.\n", Health(lp), Armour(lp), PedVehicle(lp) ? "в машине" : "пешком", EntHeading(lp), PlayerTarget() ? ", целится в кого-то" : ""); s += b; }
        V3 wp; if(PlayerWaypoint(wp)) { snprintf(b, sizeof(b), "Метка Артёма на карте: %.0f м.\n", Dist2(a, wp)); s += b; }
        s += "Отряд:\n";
        for(auto& x : u)
        {
            uintptr_t p = Ped(x.ref);
            if(x.dead || !p) { s += "- " + x.name + ": погиб (появится в медпункте базы)\n"; continue; }
            snprintf(b, sizeof(b), "- %s (%s): здоровье %.0f, броня %.0f, оружие %d (%d в магазине), %s, в %.0f м от Артёма\n", x.name.c_str(), RoleName(x.role),
                     Health(p), Armour(p), CurWeapon(p), ClipAmmo(p), x.doing.c_str(), Dist(Pos(p), a));
            s += b;
        }
        if(threats.empty()) s += "Угроз нет.\n";
        else
        {
            s += "Угрозы:\n";
            int k = 0;
            for(auto& t : threats)
            {
                if(k++ >= 6) break;
                int stars = starsFn ? starsFn(t.ref) : 0;
                snprintf(b, sizeof(b), "- #%d: %.0f м, направление %.0f°%s%s%s%s%s%s\n", t.ref, t.dist, Heading(a, t.pos),
                         t.armed ? ", вооружён" : "", t.aiming ? ", целится в нас" : "", t.shooting ? ", стреляет" : "",
                         t.elevated ? ", на высоте" : "", t.inCar ? ", в машине" : "", t.surrender ? ", сдался" : (t.visible ? "" : ", не видно"));
                s += b;
                if(stars) { snprintf(b, sizeof(b), "  розыск: %d зв.\n", stars); s += b; }
            }
        }
        if(!prisoners.empty()) { snprintf(b, sizeof(b), "Задержанных: %d.\n", (int)prisoners.size()); s += b; }
        const TacticInfo* ti = FindTactic(ord.tactic);
        if(ti) { snprintf(b, sizeof(b), "Сейчас выполняем: Т%03d %s.\n", ti->id, ti->name); s += b; }
        if(s.size() > maxLen) s.resize(maxLen);
        return s;
    }
};

// ======================================================================================================
//                                              Team
// ======================================================================================================
Team::Team(const char* tag) : m(new TeamImpl) { m->tag = tag ? tag : "отряд"; }
Team::~Team() { delete m; }
void Team::Add(int ref, const std::string& name, int role, bool defender)
{
    if(Has(ref)) return;
    Unit x; x.ref = ref; x.name = name; x.role = role; x.defender = defender;
    uintptr_t p = Ped(ref); x.lastHp = p ? Health(p) : 100;
    m->u.push_back(x);
    // свои: не бросают задачи из-за событий, не выпадают из машины
    Do(0x0961, { Ai(ref), Ai(1) });                       // SET_CHAR_KEEP_TASK
    Do(0x0526, { Ai(ref), Ai(1) });                       // SET_CHAR_STAY_IN_CAR_WHEN_JACKED
    Do(0x02E2, { Ai(ref), Ai(defender ? 65 : 75) });      // SET_CHAR_ACCURACY
    Do(0x081A, { Ai(ref), Ai(2) });                       // SET_CHAR_WEAPON_SKILL — профи
    Log("ТАКТИКА", "[%s] + %s (%s)%s", m->tag.c_str(), name.c_str(), RoleName(role), defender ? " — защитник" : "");
}
void Team::Remove(int ref)
{
    for(auto it = m->u.begin(); it != m->u.end(); ++it)
        if(it->ref == ref)
        {
            uintptr_t p = Ped(ref);
            if(p) { uintptr_t v = PedVehicle(p); if(v) { drv::Release(VehRef(v)); drv::Ground(VehRef(v)); } Do(0x0687, { Ai(ref) }); }   // CLEAR_CHAR_TASKS
            m->u.erase(it); return;
        }
}
void Team::Clear() { m->u.clear(); m->threats.clear(); m->events.clear(); m->prisoners.clear(); m->ord = Order(); }
bool Team::Has(int ref) const { for(auto& x : m->u) if(x.ref == ref) return true; return false; }
int  Team::Count() const { return (int)m->u.size(); }
void Team::SetRole(int ref, int role) { if(Unit* x = m->Find(ref)) x->role = role; }
void Team::SetLeader(int ref) { m->leader = ref; }
void Team::SetHome(const V3& h, float r) { m->home = h; m->homeR = r; m->hasHome = true; }
void Team::SetPosts(const std::vector<V3>& p) { m->posts = p; }
void Team::SetPlace(int id, const V3& p) { if(id >= 0 && id < 8) { m->places[id] = p; m->hasPlace[id] = true; } }
void Team::SetEnemyFilter(bool (*f)(uintptr_t)) { m->enemyFn = f; }
void Team::SetSay(void (*f)(int, const std::string&, const std::string&, bool)) { m->sayFn = f; }
void Team::SetBackup(void (*f)(int, const V3&)) { m->backupFn = f; }
void Team::SetArrest(void (*a)(int), void (*d)(int)) { m->arrestedFn = a; m->deliveredFn = d; }
void Team::SetStars(int (*f)(int)) { m->starsFn = f; }
void Team::Command(const Order& o) { m->SetOrder(o); }
void Team::Auto(bool on) { m->autoOn = on; }
bool Team::AutoOn() const { return m->autoOn; }
int  Team::Current() const { return m->ord.tactic; }
bool Team::InCombat() const { return m->inCombat; }
int  Team::Threats() const { return (int)m->threats.size(); }
bool Team::PopEvent(Event& e) { if(m->events.empty()) return false; e = m->events.front(); m->events.pop_front(); return true; }
std::string Team::Situation(int maxLen) { return m->Situation((size_t)maxLen); }
std::vector<UnitPublic> Team::Units() const
{
    std::vector<UnitPublic> r;
    for(auto& x : m->u)
    {
        UnitPublic p; p.ref = x.ref; p.name = x.name; p.role = x.role; p.doing = x.doing; p.alive = !x.dead;
        uintptr_t ped = Ped(x.ref); if(ped && !x.dead) { p.hp = Health(ped); p.armour = Armour(ped); p.ammo = TotalAmmo(ped); }
        r.push_back(p);
    }
    return r;
}
void Team::Tick()
{
    if(m->u.empty() || Paused() || !Player()) return;
    if(TeamImpl::Ago(m->lastTick, 100)) { m->lastTick = NowMs(); m->Tick10(); }
    for(auto& x : m->u) if(!x.dead) m->Execute(x);
}
}
