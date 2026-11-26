#include "sim/crew.h"

#include <algorithm>
#include <map>

#include "game/game.h"
#include "net/link.h"
#include "sim/police.h"
#include "talk/talk.h"
#include "trace/trace.h"
#include "util/log.h"
#include "world/world.h"

using world::Character;
using world::Fmt;

namespace crew
{
namespace
{
    struct Hit { double at = -1e9; int weapon = 0; };
    std::map<int, Hit> g_hits[2];             // [0] игрок на этом телефоне, [1] друг: ref обидчика → последний удар
    float g_plHp[2] = { -1, -1 };

    struct Def
    {
        int attacker = 0, protectee = 0;      // ref врага; 0 — CJ этого телефона, 1 — друг
        double since = 0, until = 0, lostAt = 0;   // lostAt — с какого момента он не бьёт этого врага
        bool gaveGun = false;
    };
    std::map<int, Def> g_def;                 // ref защитника → что делает

    std::map<int, float> g_ownHp;             // свои рядом: ref → здоровье (кто их ранит)
    struct Hurt { double at = -1e9; bool armed = false; };
    // [0] CJ этого телефона, [1] друг (у хоста урон от друга по педу приходит событием SA Coop — обидчик = второй CJ)
    std::map<int, Hurt> g_ownHurtBy[2];       // ref своего → когда его ранил CJ (и было ли это оружием)
    double g_ownHurtAny[2] = { -1e9, -1e9 };  // когда CJ последний раз ранил кого-то из своих
    double g_ownHurtArmed[2] = { -1e9, -1e9 };// … оружием или машиной (не кулаками)

    std::map<int, double> g_shoutAt;          // ref врага → когда свои уже кричали (одна реплика на стычку)
    double g_next = 0;

    bool Own(uintptr_t p) { return world::FactionOfPedType(game::PedType(p)) == world::W().playerGang; }

    int ProtecteeIdx(uintptr_t pl) { return pl && pl == game::Player2() ? 1 : 0; }
    uintptr_t ProtecteeOf(int idx) { return idx == 1 ? game::Player2() : game::Player(); }

    bool IsGun(int w) { return w >= 22 && w <= 38; }

    const char* WeaponWord(int w)
    {
        if(w <= 1) return "кулаками";
        if(w == 4) return "с ножом";
        if(w == 8) return "с катаной";
        if(w >= 2 && w <= 15) return "с битой/дубинкой";
        if(w >= 22 && w <= 24) return "с пистолетом";
        if(w >= 25 && w <= 27) return "с дробовиком";
        if(w == 28 || w == 29 || w == 32) return "с автоматом-пистолетом (узи)";
        if(w == 30 || w == 31) return "с автоматом";
        if(w == 33 || w == 34) return "с винтовкой";
        if(w >= 35) return "с тяжёлым оружием";
        return "с оружием";
    }

    std::string Who(uintptr_t p)
    {
        if(Character* ch = world::FindByPed(p)) return Fmt("#%d %s", ch->id, ch->nick.empty() ? ch->name.c_str() : ch->nick.c_str());
        return game::PedTypeName(game::PedType(p));
    }

    // Удары по игрокам: кто бил (по данным игры: последний ранивший и время)
    void TrackPlayerHits(double now)
    {
        uint32_t tnow = game::TimeMs();
        for(int i = 0; i < 2; ++i)
        {
            uintptr_t pl = ProtecteeOf(i);
            if(!pl || !game::Alive(pl)) { g_plHp[i] = -1; continue; }
            float hp = game::Health(pl) + game::Armour(pl);
            if(g_plHp[i] >= 0 && hp < g_plHp[i] - 0.5f)
            {
                int wpn = 0; uint32_t tm = 0; uintptr_t by = game::LastDamager(pl, wpn, tm);
                bool fresh = tm && tnow >= tm && tnow - tm < 1500;
                if(fresh && by && game::IsPedEntity(by) && by != game::Player() && by != game::Player2())
                {
                    Hit& h = g_hits[i][game::Ref(by)];
                    if(now - h.at > 3)
                    {
                        TRACE("СВОИ", "на %s напал %s (%s, оружие %d)", i ? "друга" : "игрока", game::EntName(by).c_str(), Who(by).c_str(), wpn);
                        if(i == 0) talk::OnPlayerAttacked(by, Who(by), wpn);
                    }
                    h.at = now; h.weapon = wpn;
                }
            }
            g_plHp[i] = hp;
            for(auto it = g_hits[i].begin(); it != g_hits[i].end();) { if(now - it->second.at > 30) it = g_hits[i].erase(it); else ++it; }
        }
    }

    // CJ ранил своего — запоминаем (тогда свои вправе ответить ему и оружием, а не только словами)
    void TrackOwnHurt(double now)
    {
        uintptr_t pl = game::Player(); if(!pl) return;
        uintptr_t p2 = game::Player2();
        game::Vec me = game::Pos(pl);
        uint32_t tnow = game::TimeMs();
        std::map<int, float> next;
        game::ForEachPed([&](uintptr_t p) {
            if(p == pl || game::IsPlayer(p) || p == p2 || !Own(p)) return;
            if(game::Dist(game::Pos(p), me) > 50 && !(p2 && game::Dist(game::Pos(p), game::Pos(p2)) < 50)) return;
            int ref = game::Ref(p); if(!ref) return;
            float hp = game::Health(p);
            auto it = g_ownHp.find(ref);
            if(it != g_ownHp.end() && hp < it->second - 0.5f)
            {
                int wpn = 0; uint32_t tm = 0; uintptr_t by = game::LastDamager(p, wpn, tm);
                bool fresh = tm && tnow >= tm && tnow - tm < 1500;
                int who = -1;
                if(fresh && by && (by == pl || (game::Vehicle(pl) && by == game::Vehicle(pl)))) who = 0;
                else if(fresh && by && p2 && (by == p2 || (game::Vehicle(p2) && by == game::Vehicle(p2)))) who = 1;
                if(who >= 0)
                {
                    if(now - g_ownHurtAny[who] > 5) TRACE("СВОИ", "%s ранил своего %s (%s, оружие %d) — свои могут ответить", who ? "друг" : "игрок", game::EntName(p).c_str(), Who(p).c_str(), wpn);
                    bool armed = wpn >= 2;                              // 0/1 — кулаки/кастет; дальше оружие, машина, взрыв
                    Hurt& h = g_ownHurtBy[who][ref];
                    h.at = now; h.armed = h.armed || armed;
                    g_ownHurtAny[who] = now; if(armed) g_ownHurtArmed[who] = now;
                }
            }
            next[ref] = hp;
        });
        g_ownHp.swap(next);
    }

    void Shout(uintptr_t defender, uintptr_t attacker, bool gun, double now)
    {
        int aref = game::Ref(attacker);
        auto it = g_shoutAt.find(aref);
        if(it != g_shoutAt.end() && now - it->second < 20) return;
        g_shoutAt[aref] = now;
        static const char* kFists[] = { "Эй! Ты чё на брата полез?!", "Отвали от него!", "Это наш, понял?! Сюда иди!", "Руки убрал от него!" };
        static const char* kGuns[] = { "По нашим стреляют! Валим их!", "Прикрываем брата!", "Ложись, CJ! Мы его сделаем!", "Наших бьют! Стволы!" };
        const char* line = gun ? kGuns[world::Rand(0, 3)] : kFists[world::Rand(0, 3)];
        Character* ch = world::FindByPed(defender);
        std::string name = ch ? (ch->nick.empty() ? ch->name : ch->nick) : std::string("Свой из Grove");
        talk::SayNear(ch, name, line, defender);     // слышат оба CJ, кто рядом (у второго — субтитр и голос)
    }

    // Отбой: враг упал, убежал, защитник занят другим или прошло время — каждый возвращается к своим делам
    void Release(double now)
    {
        for(auto it = g_def.begin(); it != g_def.end();)
        {
            uintptr_t p = game::FromRef(it->first);
            Def& d = it->second;
            if(!p || !game::Alive(p)) { it = g_def.erase(it); continue; }
            uintptr_t a = game::FromRef(d.attacker), prot = ProtecteeOf(d.protectee);
            const char* why = nullptr;
            if(!a || !game::Alive(a)) why = "враг повержен";
            else if(prot && game::Dist(game::Pos(a), game::Pos(prot)) > 60) why = "враг убежал";
            else if(now > d.until) why = "время вышло";
            else if(game::FightTarget(p) != a)
            {   // короткие «провалы» (упал от удара, увернулся) — не отбой; 2,5 с подряд не бьёт его — бросил
                if(d.lostAt == 0) d.lostAt = now;
                else if(now - d.since > 3 && now - d.lostAt > 2.5) why = "дерётся с другим / бросил";
            }
            else d.lostAt = 0;
            if(!why) { ++it; continue; }
            if(d.gaveGun) game::SetCurrentWeapon(p, 0);                 // ствол обратно за пояс
            if(game::ActiveTaskType(p) == 1000 && game::FightTarget(p) == a) game::ClearTask(p);
            TRACE("СВОИ", "%s (%s) — отбой: %s, дальше по своим делам", game::EntName(p).c_str(), Who(p).c_str(), why);
            it = g_def.erase(it);
        }
    }

    // Свои рядом сами вступаются за CJ / друга
    void Backup(double now, bool onlyLocal)
    {
        auto& W = world::W();
        for(int i = 0; i < (onlyLocal ? 1 : 2); ++i)
        {
            uintptr_t prot = ProtecteeOf(i);
            if(!prot || !game::Alive(prot)) continue;
            game::Vec at = game::Pos(prot);
            std::vector<uintptr_t> att = AttackersOf(prot);
            int handled = 0;
            for(uintptr_t a : att)
            {
                if(handled >= 2) break;
                int at_ = game::PedType(a);
                if(at_ == game::PT_COP) continue;                            // полиция — не уличная драка
                if(Own(a)) continue;                                         // сцепился со своим — разберутся сами
                if(game::OnMission() && game::IsScriptPed(a)) continue;      // враги миссии — дело сценария, не ломаем его
                ++handled;
                int aref = game::Ref(a);
                int have = 0; for(auto& kv : g_def) if(kv.second.attacker == aref) ++have;
                if(have >= 3) continue;
                int aw = 0, aa = 0; game::CurWeapon(a, aw, aa);
                auto hit = g_hits[i].find(aref);
                bool gun = IsGun(aw) || (hit != g_hits[i].end() && IsGun(hit->second.weapon));
                struct C { uintptr_t p; float d; };
                std::vector<C> cand;
                game::ForEachPed([&](uintptr_t p) {
                    if(p == prot || p == a || game::IsPlayer(p) || p == game::Player() || p == game::Player2()) return;
                    if(!Own(p) || !game::Alive(p) || game::Vehicle(p) || game::IsScriptPed(p)) return;
                    int ref = game::Ref(p); if(!ref || g_def.count(ref)) return;
                    float d = game::Dist(game::Pos(p), at); if(d > 25.0f) return;
                    if(game::FightTarget(p)) return;                         // уже дерётся / целится в кого-то
                    { auto h = g_ownHurtBy[i].find(ref); if(h != g_ownHurtBy[i].end() && now - h->second.at < 300) return; }   // этот CJ только что бил его самого
                    if(Character* ch = world::FindByPed(p)) if(ch->rel[i].liking < -30) return;   // в обиде на него
                    cand.push_back({ p, d });
                });
                std::sort(cand.begin(), cand.end(), [](const C& x, const C& y) { return x.d < y.d; });
                for(auto& c : cand)
                {
                    if(have >= 3) break;
                    if(!Defend(c.p, a, -1, i ? "на друга напали" : "на CJ напали")) continue;
                    ++have;
                    Shout(c.p, a, gun, now);
                    if(Character* ch = world::FindByPed(c.p))
                    {
                        world::Remember(*ch, Fmt("Вступился за %s: на него напал %s", i ? "друга CJ" : W.playerNick.c_str(), Who(a).c_str()));
                        world::ChangeRelation(*ch, i, 0, 0, 2, 0, "");
                    }
                }
            }
        }
    }
}

void Reset()
{
    g_hits[0].clear(); g_hits[1].clear(); g_plHp[0] = g_plHp[1] = -1;
    g_def.clear(); g_ownHp.clear(); g_shoutAt.clear();
    for(int i = 0; i < 2; ++i) { g_ownHurtBy[i].clear(); g_ownHurtAny[i] = g_ownHurtArmed[i] = -1e9; }
}

std::vector<uintptr_t> AttackersOf(uintptr_t pl)
{
    std::vector<uintptr_t> out;
    if(!pl) return out;
    int idx = ProtecteeOf(1) == pl ? 1 : 0;
    double now = talk::Now();
    game::Vec me = game::Pos(pl);
    uintptr_t car = game::Vehicle(pl);
    std::vector<std::pair<float, uintptr_t>> v;
    game::ForEachPed([&](uintptr_t p) {
        if(p == pl || p == game::Player() || p == game::Player2() || game::IsPlayer(p) || !game::Alive(p)) return;
        float d = game::Dist(game::Pos(p), me); if(d > 45) return;
        auto h = g_hits[idx].find(game::Ref(p));
        bool hit = h != g_hits[idx].end() && now - h->second.at < 6;
        uintptr_t ft = game::FightTarget(p);
        bool target = ft && (ft == pl || (car && ft == car));
        if(hit || target) v.push_back({ d, p });
    });
    std::sort(v.begin(), v.end());
    for(auto& x : v) out.push_back(x.second);
    return out;
}

std::string Describe(uintptr_t pl, uintptr_t from)
{
    std::vector<uintptr_t> att = AttackersOf(pl);
    if(att.empty()) return "";
    int idx = ProtecteeOf(1) == pl ? 1 : 0;
    double now = talk::Now();
    std::string s = " НА НЕГО СЕЙЧАС НАПАДАЮТ:";
    int n = 0;
    for(uintptr_t a : att)
    {
        if(++n > 3) break;
        int w = 0, am = 0; game::CurWeapon(a, w, am);
        auto h = g_hits[idx].find(game::Ref(a));
        bool hit = h != g_hits[idx].end() && now - h->second.at < 6;
        int t = game::PedType(a);
        const char* tag = t == game::PT_COP ? " — полицейский" : Own(a) ? " — твой же из банды" : "";
        s += Fmt(" [%s%s, %s, %s", Who(a).c_str(), tag, WeaponWord(w), hit ? "бьёт его" : "лезет на него / целится");
        if(from) s += Fmt(", в %.0f м от тебя", game::Dist(game::Pos(a), game::Pos(from)));
        s += "]";
    }
    int helping = 0;
    for(auto& kv : g_def) if(kv.second.protectee == idx) ++helping;
    if(helping) s += Fmt(" Свои уже впряглись: %d чел.", helping);
    s += " Вступиться — attack / aim_weapon с target his_attacker.";
    return s;
}

bool Defend(uintptr_t ped, uintptr_t attacker, int weapon, const char* why)
{
    if(!ped || !attacker || !game::Alive(ped) || !game::Alive(attacker) || game::Vehicle(ped) || game::IsScriptPed(ped)) return false;
    int ref = game::Ref(ped), aref = game::Ref(attacker);
    if(!ref || !aref) return false;
    Def d; d.attacker = aref; d.since = talk::Now(); d.until = d.since + 45;
    uintptr_t p2 = game::Player2();
    d.protectee = 0;
    if(p2) { for(uintptr_t a : AttackersOf(p2)) if(a == attacker) d.protectee = 1; }
    int cw = 0, ca = 0; game::CurWeapon(ped, cw, ca);
    if(weapon < 0)
    {   // сам по ситуации: на кулаки — кулаки, на ствол — ствол
        int aw = 0, aa = 0; game::CurWeapon(attacker, aw, aa);
        bool gun = IsGun(aw);
        for(auto& h : g_hits) { auto it = h.find(aref); if(it != h.end() && IsGun(it->second.weapon)) gun = true; }
        if(gun && !IsGun(cw)) { game::GiveWeapon(ped, 22, 34); game::SetCurrentWeapon(ped, 22); d.gaveGun = true; }
        else if(!gun && IsGun(cw)) game::SetCurrentWeapon(ped, 0);    // на кулачную драку ствол не тянут
    }
    else if(weapon == 0) game::SetCurrentWeapon(ped, 0);
    // иначе оружие уже выдал вызвавший (ИИ выбрал)
    if(!game::TaskAttack(ped, attacker)) return false;
    g_def[ref] = d;
    TRACE("СВОИ", "%s (%s) вступился: бьёт %s (%s) — %s", game::EntName(ped).c_str(), Who(ped).c_str(), game::EntName(attacker).c_str(), Who(attacker).c_str(), why ? why : "");
    LOG_INFO("Свои: %s вступился против %s (%s)", Who(ped).c_str(), Who(attacker).c_str(), why ? why : "");
    return true;
}

bool PlayerArmedVsOwn(uintptr_t ped, int who)
{
    if(who < 0 || who > 1) who = 0;
    double now = talk::Now();
    uintptr_t pl = who ? game::Player2() : game::Player();
    if(now - g_ownHurtArmed[who] < 120 && ped && pl && game::Dist(game::Pos(ped), game::Pos(pl)) < 40) return true;
    auto it = g_ownHurtBy[who].find(game::Ref(ped));
    return it != g_ownHurtBy[who].end() && it->second.armed && now - it->second.at < 300;
}

bool Defending(uintptr_t ped) { return ped && g_def.count(game::Ref(ped)); }

void Frame()
{
    double now = talk::Now();
    if(now < g_next) return;
    g_next = now + 0.2;
    if(!game::Player() || game::GamePaused()) return;
    TrackPlayerHits(now);
    TrackOwnHurt(now);
    Release(now);
    if(game::CutsceneRunning() || police::Arresting() || talk::MissionLock()) return;   // миссия — сюжет как в игре
    // Хост (и одиночка) прикрывает обоих CJ своими педами — друг видит это на копиях.
    // У друга — только его самого и только своими людьми: когда вы рядом, люди у него — копии хоста
    // (createdBy 2, как у миссии) и в защитники не берутся; разъехались — у друга своё население, его и зовём.
    Backup(now, coopnet::GetRole() == coopnet::CLIENT);
}
}
