#include "life.h"
#include "sim/police.h"

#include <math.h>
#include <string.h>

#include <map>

#include "ai/claude.h"
#include "game/game.h"
#include "net/link.h"
#include "settings.h"
#include "talk/talk.h"
#include "trace/trace.h"
#include "util/log.h"

using world::Character;
using world::Fmt;

namespace life
{
namespace
{
    double g_nextSlow = 0, g_nextInit = 0, g_nextDirector = 0;
    int    g_directorHour = -1;
    int    g_aimRef = 0; double g_aimSince = 0;
    std::map<int, double> g_spawnTried;      // персонаж → когда пытались вывести
    std::map<int, bool> g_profileAsked;
    int    g_directorReq = 0;

    struct FacModels { int faction; int pedType; int models[3]; };
    const FacModels kFac[] = {
        { world::F_BALLAS, game::PT_BALLAS, { 102, 103, 104 } }, { world::F_GROVE, game::PT_GROVE, { 105, 106, 107 } },
        { world::F_VAGOS, game::PT_VAGOS, { 108, 109, 110 } },   { world::F_AZTECAS, game::PT_AZTECA, { 114, 115, 116 } },
        { world::F_RIFA, game::PT_RIFA, { 173, 174, 175 } },      { world::F_DANANG, game::PT_DANANG, { 121, 122, 123 } },
        { world::F_TRIADS, game::PT_TRIAD, { 117, 118, 120 } },   { world::F_MAFIA, game::PT_MAFIA, { 111, 112, 113 } },
        { world::F_POLICE, game::PT_COP, { 280, 281, 282 } },
    };
    const FacModels* FacInfo(int f) { for(auto& x : kFac) if(x.faction == f) return &x; return nullptr; }

    game::Vec Around(const game::Vec& at, float dist)
    {
        float a = (float)world::Rand(0, 628) / 100.0f;
        game::Vec p{ at.x + cosf(a) * dist, at.y + sinf(a) * dist, at.z };
        p.z = game::GroundZ(p.x, p.y, at.z) + 1.0f;
        return p;
    }

    bool NightTime() { int h = game::Hour(); return h >= 21 || h < 5; }

    // Хост: второй CJ, если он рядом (мир хоста — вокруг хоста; дальше 150 м там пусто и не прогружено)
    uintptr_t FriendNearHost()
    {
        if(coopnet::GetRole() != coopnet::HOST) return 0;
        uintptr_t p2 = game::Player2(), pl = game::Player();
        if(!p2 || !pl || !game::Alive(p2) || game::Dist(game::Pos(p2), game::Pos(pl)) > 150) return 0;
        return p2;
    }
    // Сообщение на экран тому CJ, кого касается (себе — уведомлением, другу — сообщением)
    void NotifyFor(int who, const std::string& text)
    {
        if(who == world::WHO_CJ || coopnet::GetRole() != coopnet::HOST) { talk::Notify(text); return; }
        json::Value m = json::Value::Object(); m["t"] = "note"; m["text"] = text; coopnet::Send(m);
    }

    // ---- персонажи в мире ----
    void SlowTick()
    {
        uintptr_t pl = game::Player(); if(!pl) return;
        game::Vec me = game::Pos(pl);
        auto& W = world::W();
        double now = talk::Now();
        for(auto& c : W.chars)
        {
            uintptr_t ped = c.pedRef ? game::FromRef(c.pedRef) : 0;
            if(c.pedRef && (!ped || game::Model(ped) != c.model)) { c.pedRef = 0; ped = 0; }   // игра удалила/переиспользовала слот
            if(ped && !game::Alive(ped))
            {
                if(c.alive)
                {
                    c.alive = false;
                    LOG_INFO("Персонаж #%d %s погиб", c.id, c.name.c_str());
                    if(c.rel[world::WHO_CJ].met) world::News(Fmt("%s погиб.", c.name.c_str()));
                }
                game::KeepPed(ped, false);      // тело — игре (иначе тела копятся в пуле до вылета)
                c.pedRef = 0; continue;
            }
            if(ped)
            {
                float d = game::Dist(game::Pos(ped), me);
                // далеко: отпускаем педа игре — она уберёт его сама и правильно (удалять самим нельзя: он может сидеть за рулём)
                // в коопе мир хоста держит и людей вокруг друга (до 210 м): его собеседника и тех, кто рядом с ним, не отпускаем
                uintptr_t p2 = coopnet::GetRole() == coopnet::HOST ? game::Player2() : 0;
                float d2 = p2 ? game::Dist(game::Pos(ped), game::Pos(p2)) : 1e9f;
                if(d > 160 && d2 > 120 && !game::OnScreen(ped) && talk::Current() != &c && talk::FriendConvChar() != c.id && !police::Arresting()) { game::KeepPed(ped, false); c.pedRef = 0; }
                continue;
            }
            // вывести персонажа «домой», когда игрок рядом
            if(!c.alive || c.jailUntilDay > world::Day() || c.home.x == 0) continue;
            if(!c.story.empty() || (c.model >= 290 && c.model <= 299)) continue;   // сюжетных (Свит, Смоук…) выводит только игра — иначе двойники
            if(game::OnMission() || game::CutsceneRunning() || police::Arresting()) continue;
            if(game::Dist2D(c.home, me) > 70 || game::Dist2D(c.home, me) < 25) continue;
            if(g_spawnTried.count(c.id) && now - g_spawnTried[c.id] < 300) continue;
            g_spawnTried[c.id] = now;
            int h = game::Hour();
            bool awake = c.faction == world::F_CIVIL ? (h >= 8 && h <= 22) : (h >= 12 || h <= 3);
            if(!awake || world::Rand(0, 100) > 60) continue;
            game::Vec at = c.home; at.z = game::GroundZ(at.x, at.y, at.z) + 1.0f;
            uintptr_t np = game::SpawnPed(c.pedType, c.model, at);
            if(np) { world::Bind(c, np); game::TaskWander(np); LOG_INFO("Персонаж #%d %s появился у себя (%s)", c.id, c.name.c_str(), c.homeZone.c_str()); }
        }
    }

    // ---- «приду к тебе» (обещал по телефону или в разговоре) ----
    // Раньше цель meet_player только записывалась: человек говорил «уже подхожу», а сам гулял (тест 0.4.1).
    // Теперь: далеко или не в мире — «приезжает» (появляется в 35–45 м вне кадра), дальше идёт/бежит к CJ;
    // дошёл — цель снята, ИИ говорит, что пришёл (и что CJ в машине — можно сесть).
    std::map<int, double> g_meetGo;
    void MeetGoal(Character& c)
    {
        uintptr_t pl = game::Player();
        uintptr_t tgt = c.goalTarget == world::WHO_CJ ? pl : FriendNearHost();
        if(!tgt || game::OnMission() || game::CutsceneRunning() || police::Arresting()) return;
        if(talk::Current() == &c && talk::InConversation() && !talk::PhoneMode()) return;   // уже говорит с ним рядом
        uintptr_t ped = world::PedOf(c);
        if(ped && !game::Alive(ped)) return;
        game::Vec tp = game::Pos(tgt);
        if(!ped || game::Dist(game::Pos(ped), tp) > 150)
        {
            game::Vec at = Around(tp, 40.0f);
            float sx, sy;
            for(int i = 0; i < 6 && game::WorldToScreen(at, sx, sy); ++i) at = Around(tp, 35.0f + i * 2);   // появиться за кадром, а не перед глазами
            if(!ped)
            {
                ped = game::SpawnPed(c.pedType, c.model, at);
                if(!ped) return;
                world::Bind(c, ped);
            }
            else if(!game::Vehicle(ped)) game::Teleport(ped, at);
            TRACE("ЖИЗНЬ", "#%d %s идёт к %s (обещал прийти)", c.id, c.name.c_str(), c.goalTarget == world::WHO_CJ ? "CJ" : "другу");
        }
        float d = game::Dist(game::Pos(ped), tp);
        if(d < 4.5f)
        {
            c.goal.clear();
            game::TaskTurnTo(ped, tgt);
            bool car = game::Vehicle(tgt) != 0;
            NotifyFor(c.goalTarget, Fmt("%s пришёл", c.nick.empty() ? c.name.c_str() : c.nick.c_str()));
            talk::React(c, c.goalTarget, std::string("Ты пришёл к ") + (c.goalTarget == world::WHO_CJ ? world::W().playerNick : world::W().friendNick) +
                        ", как обещал, — стоишь рядом с ним." + (car ? " Он сидит в машине: позовёт — садись (move get_in)." : " Скажи, что пришёл."), true);
            TRACE("ЖИЗНЬ", "#%d пришёл к CJ", c.id);
            return;
        }
        if(game::Vehicle(ped)) { if(!game::VehDriver(game::Vehicle(ped)) || game::VehDriver(game::Vehicle(ped)) != ped) game::TaskLeaveCar(ped); return; }
        double now = talk::Now();
        if(now - g_meetGo[c.id] < 2.0) return;                           // CJ двигается — цель обновляем раз в 2 с
        g_meetGo[c.id] = now;
        game::TaskGoTo(ped, tp, d > 25 ? 6 : 4, 3.0f);
    }

    // ---- цели (месть, засада) ----
    void Goals()
    {
        uintptr_t pl = game::Player(); if(!pl) return;
        auto& W = world::W();
        int nowMin = world::NowMin();
        for(auto& c : W.chars)
        {
            if(!c.alive || c.goal.empty()) continue;
            if(c.goalUntilMin && nowMin > c.goalUntilMin) { LOG_INFO("Цель #%d истекла: %s", c.id, c.goal.c_str()); c.goal.clear(); continue; }
            if(c.goal.rfind("meet_player", 0) == 0) { MeetGoal(c); continue; }
            bool hunt = c.goal.rfind("hunt_player", 0) == 0 || c.goal.rfind("revenge", 0) == 0;
            if(!hunt) continue;
            // цель — тот CJ, на кого зуб: у хоста друг — второй CJ (только когда вы рядом: мир хоста — вокруг хоста)
            uintptr_t tgt = c.goalTarget == world::WHO_CJ ? pl : FriendNearHost();
            if(!tgt) continue;
            if(game::OnMission() || talk::InConversation() || police::Arresting() || game::CutsceneRunning()) continue;
            if(world::Rand(0, 1000) > (NightTime() ? 25 : 8)) continue;   // раз в несколько минут
            uintptr_t ped = world::PedOf(c);
            if(!ped)
            {
                game::Vec at = Around(game::Pos(tgt), 30.0f);
                ped = game::SpawnPed(c.pedType, c.model, at);
                if(!ped) continue;
                world::Bind(c, ped);
            }
            game::GiveWeapon(ped, 22, 80); game::SetCurrentWeapon(ped, 22);
            game::TaskAttack(ped, tgt);
            if(c.faction != world::F_CIVIL && c.faction != world::F_POLICE) SpawnHelpers(c.faction, world::Rand(1, 2), true, tgt);
            NotifyFor(c.goalTarget, Fmt("%s пришёл за тобой!", c.nick.empty() ? c.name.c_str() : c.nick.c_str()));
            world::Remember(c, std::string("Пришёл мстить ") + (c.goalTarget == world::WHO_CJ ? W.playerNick : (W.friendNick.empty() ? std::string("другу CJ") : W.friendNick)));
            c.goal.clear();
        }
        // планы банд от режиссёра: засада
        for(int f = world::F_GROVE; f < world::F_COUNT; ++f)
        {
            auto& s = W.fac[f];
            if(s.plan.find("засада") == std::string::npos || f == W.playerGang || s.truce) continue;
            if(!NightTime() || game::OnMission() || police::Arresting() || game::CutsceneRunning() || world::Rand(0, 1000) > 6) continue;
            uintptr_t fr = FriendNearHost();
            // цель засады: если в плане назван друг — на него (ник друга не «CJ» и проверяется первым: «Кореш CJ» содержит «CJ»),
            // назван CJ — на хоста, не назван никто — на любого из двоих
            bool namesFriend = !W.friendNick.empty() && W.friendNick != W.playerNick && s.plan.find(W.friendNick) != std::string::npos;
            bool namesHost = !namesFriend && s.plan.find(W.playerNick) != std::string::npos;
            bool onFriend = fr && (namesFriend || (!namesHost && world::Rand(0, 1)));
            Ambush(f, world::Rand(3, 4), onFriend ? fr : 0);
            world::News(Fmt("%s устроили засаду на %s!", world::FactionName(f), onFriend ? W.friendNick.c_str() : W.playerNick.c_str()));
            s.plan.clear();
        }
    }

    // ---- инициатива: педы сами реагируют ----
    void Initiative()
    {
        int lvl = settings::Get().initiative;
        if(!lvl || !claude::Available() || talk::InConversation() || game::OnMission() || game::CutsceneRunning() || police::Arresting()) return;
        double now = talk::Now();
        if(now < g_nextInit) return;
        uintptr_t pl = game::Player(); if(!pl) return;
        double cooldown = lvl >= 2 ? 12 : 35;                    // люди подходят сами чаще (не экономим)

        // 1) игрок целится в человека
        uintptr_t t = game::PlayerTarget();
        if(t && !game::IsPlayer(t) && game::Alive(t) && game::Dist(game::Pos(t), game::Pos(pl)) < 15)
        {
            int ref = game::Ref(t);
            if(ref != g_aimRef) { g_aimRef = ref; g_aimSince = now; }
            else if(now - g_aimSince > 1.2)
            {
                Character& c = world::Promote(t);
                if(!c.aiProfile) RequestProfile(c);
                talk::React(c, world::WHO_CJ, talk::NameFor(world::WHO_CJ) + " навёл на тебя пистолет и держит на прицеле", true);
                g_nextInit = now + cooldown; g_aimRef = 0;
                return;
            }
        }
        else g_aimRef = 0;

        // 2) знакомые с сильными чувствами замечают игрока
        game::Vec me = game::Pos(pl);
        for(auto& c : world::W().chars)
        {
            uintptr_t ped = world::PedOf(c);
            if(!ped || game::Dist(game::Pos(ped), me) > 10 || !game::OnScreen(ped)) continue;
            const world::Relation& r = c.rel[world::WHO_CJ];
            if(!r.met) continue;
            bool strong = abs(r.liking) >= 40 || r.fear >= 50 || r.payroll || r.informant || !c.goal.empty();
            if(!strong) continue;
            int sinceMin = (world::Day() - c.lastTalkDay) * 1440 + (game::Hour() * 60 + game::Minute() - c.lastTalkMin);
            if(c.lastTalkDay >= 0 && sinceMin < 180) continue;
            talk::React(c, world::WHO_CJ, Fmt("Ты снова видишь %s рядом с собой (вы знакомы). Отреагируй, как сделал бы на самом деле.", talk::NameFor(world::WHO_CJ).c_str()), false);
            g_nextInit = now + cooldown;
            return;
        }
        // 3) то же — со вторым CJ (кооп, у хоста): знакомые друга замечают его так же
        if(uintptr_t p2 = FriendNearHost())
        {
            game::Vec fp = game::Pos(p2);
            for(auto& c : world::W().chars)
            {
                uintptr_t ped = world::PedOf(c);
                if(!ped || game::Dist(game::Pos(ped), fp) > 10) continue;
                const world::Relation& r = c.rel[world::WHO_FRIEND];
                if(!r.met) continue;
                bool strong = abs(r.liking) >= 40 || r.fear >= 50 || r.payroll || r.informant || (!c.goal.empty() && c.goalTarget == world::WHO_FRIEND);
                if(!strong) continue;
                int sinceMin = (world::Day() - c.lastTalkDay) * 1440 + (game::Hour() * 60 + game::Minute() - c.lastTalkMin);
                if(c.lastTalkDay >= 0 && sinceMin < 180) continue;
                talk::React(c, world::WHO_FRIEND, Fmt("Ты снова видишь %s рядом с собой (вы знакомы). Отреагируй, как сделал бы на самом деле.", world::W().friendNick.c_str()), false);
                g_nextInit = now + cooldown;
                return;
            }
        }
    }

    // ---- режиссёр города ----
    void Director()
    {
        if(!settings::Get().director || !claude::Available() || g_directorReq) return;
        int h = game::Hour();
        if(g_directorHour >= 0 && ((h - g_directorHour + 24) % 24) < 2) return;   // раз в 2 игровых часа — город живёт плотнее (не экономим)
        g_directorHour = h;
        RunDirector(false);
    }
}

void RunDirector(bool force)
{
    (void)force;
    auto& W = world::W();
    std::string state = Fmt("День %d, %02d:00. Игрок: %s (Grove Street). Розыск: %d зв.\nОтношения банд: %s\nДело полиции: интерес %d, улики %d, ССБ проверяет: %s.\n",
                            W.day, game::Hour(), W.playerNick.c_str(), game::Wanted(), world::FactionSummary(world::WHO_CJ).c_str(),
                            W.pcase[0].heat, W.pcase[0].evidence, W.pcase[0].iaInvestigation ? "да" : "нет");
    state += "Последние события:\n";
    int n = 0;
    for(auto it = W.events.rbegin(); it != W.events.rend() && n < 12; ++it, ++n)
        state += Fmt("- день %d, %s: %s (%s, тяжесть %d)\n", it->day, it->zone.c_str(), it->text.c_str(), it->reported ? "полиция знает" : "полиция не знает", it->severity);
    state += "Знакомые персонажи:\n";
    n = 0;
    for(auto& c : W.chars)
    {
        if(!c.rel[0].met || n++ > 15) continue;
        state += Fmt("- %s (%s, %s): доверие %d, симпатия %d%s%s%s\n", c.name.c_str(), world::FactionName(c.faction), c.alive ? "жив" : "мёртв",
                     c.rel[0].trust, c.rel[0].liking, c.rel[0].payroll ? ", на зарплате" : "", c.rel[0].informant ? ", информатор" : "",
                     c.goal.empty() ? "" : (", цель: " + c.goal).c_str());
    }
    claude::Request r;
    json::Value sys = json::Value::Object(); sys["type"] = "text";
    sys["text"] = "Ты — «режиссёр» живого города Сан-Андреас (GTA San Andreas, начало 90-х). По состоянию мира реши, что задумали банды и полиция "
                  "в ближайшие часы, какие слухи пошли и какие новости дойдут до игрока. Будь реалистичен и последователен: месть за убитых, "
                  "войны за районы, полиция копает, если есть улики, ССБ ловит продажных копов. Не всё касается игрока. Пиши по-русски коротко.";
    r.system = json::Value::Array(); r.system.push(sys);
    json::Value msg = json::Value::Object(); msg["role"] = "user"; msg["content"] = state;
    r.messages = json::Value::Array(); r.messages.push(msg);
    json::Value fmt; json::Parse(R"({"type":"json_schema","schema":{"type":"object","additionalProperties":false,"required":["factions","rumors","news","police"],
      "properties":{
        "factions":{"type":"array","items":{"type":"object","additionalProperties":false,"required":["faction","plan","tension_change","hostility_change"],
          "properties":{"faction":{"type":"string","enum":["Ballas","Vagos","Aztecas","Rifa","Da Nang Boys","Триады","Мафия","Grove Street"]},
          "plan":{"type":"string"},"tension_change":{"type":"integer"},"hostility_change":{"type":"integer"}}}},
        "rumors":{"type":"array","items":{"type":"string"}},
        "news":{"type":"array","items":{"type":"string"}},
        "police":{"type":"object","additionalProperties":false,"required":["heat_change","start_internal_affairs"],
          "properties":{"heat_change":{"type":"integer"},"start_internal_affairs":{"type":"boolean"}}}}}})", fmt);
    r.format = fmt; r.maxTokens = 6000; r.effort = "medium"; r.cache = false; r.tag = "режиссёр"; r.role = claude::ROLE_WORLD;   // в фоне — можно подумать
    g_directorReq = claude::Send(r, nullptr, [](claude::Result& res) {
        g_directorReq = 0;
        json::Value v;
        if(!res.ok || !json::Parse(res.text, v)) return;
        auto& W = world::W();
        for(auto& f : v["factions"].arr())
        {
            int id = -1;
            for(int i = 0; i < world::F_COUNT; ++i) if(f["faction"].str() == world::FactionName(i)) id = i;
            if(id <= world::F_POLICE) continue;
            auto& s = W.fac[id];
            s.plan = f["plan"].str();
            s.tension = world::Clamp(s.tension + world::Clamp(f["tension_change"].i(), -20, 20), 0, 100);
            s.hostility[0] = world::Clamp(s.hostility[0] + world::Clamp(f["hostility_change"].i(), -20, 20), 0, 100);
            LOG_INFO("Режиссёр: %s — %s", world::FactionName(id), s.plan.c_str());
        }
        for(auto& t : v["rumors"].arr()) { world::Rumor rm; rm.text = t.str(); rm.day = W.day; rm.reach = 2; W.rumors.push_back(rm); }
        for(auto& t : v["news"].arr()) world::News(t.str());
        auto& p = W.pcase[0];
        p.heat = world::Clamp(p.heat + world::Clamp(v["police"]["heat_change"].i(), -20, 20), 0, 100);
        if(v["police"]["start_internal_affairs"].b()) { p.iaInvestigation = true; world::Rumor rm; rm.text = "говорят, ССБ трясёт продажных копов"; rm.day = W.day; rm.reach = 3; W.rumors.push_back(rm); }
        if(coopnet::GetRole() == coopnet::HOST) { json::Value m = json::Value::Object(); m["t"] = "news"; json::Value a = json::Value::Array(); for(auto& t : v["news"].arr()) a.push(t); m["items"] = a; coopnet::Send(m); }
    });
}

void RequestProfile(Character& c)
{
    if(g_profileAsked[c.id] || !claude::Available()) return;
    g_profileAsked[c.id] = true;
    claude::Request r;
    json::Value sys = json::Value::Object(); sys["type"] = "text";
    sys["text"] = "Придумай короткий правдоподобный профиль жителя Сан-Андреаса (GTA San Andreas, начало 90-х) по данным. По-русски, без клише и пафоса. "
                  "Имя оставь, кличку придумай только если человек с улицы.";
    r.system = json::Value::Array(); r.system.push(sys);
    const world::Traits& t = c.tr;
    std::string info = Fmt("Имя: %s. Кто: %s (%s). Пол: %s, возраст %d. Район: %s. Черты 0-100: преданность %d, жадность %d, смелость %d, агрессия %d, честь %d, хитрость %d, болтливость %d, амбиции %d.",
                           c.name.c_str(), game::PedTypeName(c.pedType), world::FactionName(c.faction), c.female ? "женщина" : "мужчина", c.age,
                           c.homeZone.c_str(), t.loyalty, t.greed, t.courage, t.aggression, t.honor, t.cunning, t.talkative, t.ambition);
    json::Value msg = json::Value::Object(); msg["role"] = "user"; msg["content"] = info;
    r.messages = json::Value::Array(); r.messages.push(msg);
    json::Value fmt; json::Parse(R"({"type":"json_schema","schema":{"type":"object","additionalProperties":false,"required":["nick","backstory","dream","fear","speech","role"],
      "properties":{"nick":{"type":"string"},"backstory":{"type":"string"},"dream":{"type":"string"},"fear":{"type":"string"},"speech":{"type":"string"},"role":{"type":"string"}}}})", fmt);
    r.format = fmt; r.maxTokens = 4000; r.effort = "low"; r.cache = false; r.tag = "профиль"; r.role = claude::ROLE_WORLD;   // с размышлением — живее; запас под него
    int id = c.id;
    claude::Send(r, nullptr, [id](claude::Result& res) {
        Character* ch = world::Find(id);
        json::Value v;
        if(!ch || !res.ok || !json::Parse(res.text, v)) return;
        if(ch->nick.empty() && !v["nick"].str().empty() && (ch->faction != world::F_CIVIL || ch->pedType == game::PT_DEALER)) ch->nick = v["nick"].str();
        ch->backstory = v["backstory"].str(); ch->dream = v["dream"].str(); ch->fearOf = v["fear"].str();
        ch->speech = v["speech"].str(); ch->role = v["role"].str(); ch->aiProfile = true;
        LOG_INFO("Профиль #%d %s готов", ch->id, ch->name.c_str());
    });
}

void OnKill(uintptr_t victim, int weapon, int playerId)
{
    (void)weapon;
    if(!victim || playerId != 0) return;     // убийства друга в коопе считаются у него
    bool mission = game::OnMission();
    auto& W = world::W();
    int t = game::PedType(victim);
    int fac = world::FactionOfPedType(t);
    Character* c = world::FindByPed(victim);
    std::string who = c ? c->name : std::string(game::PedTypeName(t));
    int sev = t == game::PT_COP ? 5 : (game::IsGang(t) ? 3 : 4);
    world::Event& e = world::AddEvent("kill", Fmt("убил: %s", who.c_str()), game::Pos(victim), sev, world::WHO_CJ);
    e.victimFaction = fac; e.victimChar = c ? c->id : 0;
    if(c) { game::KeepPed(victim, false); c->alive = false; c->pedRef = 0; }
    if(mission) return;                      // по сюжету миссии: без мести, жара и вражды банд
    world::GangReactToKill(fac, world::WHO_CJ, sev >= 4 ? 2 : 1);
    if(fac == world::F_POLICE)
    {
        auto& p = W.pcase[0]; p.heat = world::Clamp(p.heat + 30, 0, 100);
        for(auto& ch : W.chars) if(ch.faction == world::F_POLICE && ch.rel[0].payroll) { world::ChangeRelation(ch, 0, -20, 0, -20, 10, "убил копа"); world::Remember(ch, "Игрок убил копа — это перебор"); }
    }
    // кто-то из своих жертвы может поклясться отомстить
    if(fac != world::F_CIVIL && fac != W.playerGang)
        for(auto& ch : W.chars)
            if(ch.alive && ch.faction == fac && ch.id != (c ? c->id : 0) && ch.goal.empty() && ch.tr.loyalty > 60 && world::Rand(0, 100) < 25)
            {
                ch.goal = Fmt("revenge: отомстить %s за %s", W.playerNick.c_str(), who.c_str());
                ch.goalUntilMin = world::NowMin() + 72 * 60; ch.goalTarget = world::WHO_CJ;
                world::Remember(ch, Fmt("%s убил нашего (%s). Я этого так не оставлю.", W.playerNick.c_str(), who.c_str()));
                break;
            }
}

void SpawnHelpers(int faction, int count, bool hostile, uintptr_t target)
{
    const FacModels* fm = FacInfo(faction);
    uintptr_t pl = target ? target : game::Player();
    if(!fm || !pl) return;
    for(int i = 0; i < count; ++i)
    {
        game::Vec at = Around(game::Pos(pl), 28.0f + i * 3);
        uintptr_t p = game::SpawnPed(fm->pedType, fm->models[world::Rand(0, 2)], at);
        if(!p) continue;
        if(hostile) { int w = world::Rand(0, 2) ? 22 : 28; game::GiveWeapon(p, w, 120); game::SetCurrentWeapon(p, w); game::TaskAttack(p, pl); }
        else game::TaskGoTo(p, game::Pos(pl), 6, 3.0f);
        game::KeepPed(p, false);      // временные: игра уберёт сама
    }
    LOG_INFO("Подмога %s: %d чел. (%s) к %s", world::FactionName(faction), count, hostile ? "враждебно" : "мирно", pl == game::Player() ? "игроку" : "второму CJ");
}

void Ambush(int faction, int count, uintptr_t target) { SpawnHelpers(faction, count, true, target); }

void HelpersForFriend(int faction, int count, bool hostile)
{
    uintptr_t p2 = FriendNearHost();
    if(!p2) { TRACE("СВОИ", "подмога другу: он далеко от хоста — не к кому"); return; }
    SpawnHelpers(faction, world::Clamp(count, 1, 4), hostile, p2);
}

void Frame()
{
    double now = talk::Now();
    if(now >= g_nextSlow)
    {
        g_nextSlow = now + 1.0;
        SlowTick();
        Goals();
        if(coopnet::GetRole() != coopnet::CLIENT) Director();
    }
    Initiative();
}

} // namespace life
