#include "police.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <math.h>
#include <algorithm>

#include <vector>

#include "ai/claude.h"
#include "game/game.h"
#include "jarvis/script.h"
#include "settings.h"
#include "sim/life.h"
#include "talk/talk.h"
#include "util/log.h"
#include "world/world.h"

using world::Fmt;

namespace police
{
namespace
{
    // ---------------- свидетели ----------------
    struct PendingReport
    {
        bool active = false;
        int callerRef = 0;
        double at = 0;
        uintptr_t wanted = 0;
        int type = 0; game::Vec pos; uint64_t victim = 0; bool imm = false;
        int eventId = 0;
    };
    PendingReport g_rep;
    bool g_bypass = false;
    double g_lastCrimeLog = 0;

    int Severity(int t)
    {
        switch(t)
        {
            case 2: case 21: return 1;
            case 6: case 10: case 15: case 20: case 22: return 2;
            case 4: case 13: case 17: case 18: return 3;
            case 3: case 5: case 11: case 12: case 14: case 16: case 19: return 4;
            default: return 1;
        }
    }
    const char* CrimeText(int t)
    {
        switch(t)
        {
            case 1: return "засветил оружие"; case 2: return "избил человека"; case 3: return "ударил полицейского";
            case 4: return "стрелял в человека"; case 5: return "стрелял в полицейского"; case 6: return "угнал машину";
            case 7: return "проехал на красный"; case 8: return "опасно водил"; case 9: return "превысил скорость";
            case 10: return "сбил человека"; case 11: return "сбил полицейского"; case 12: return "сбил вертолёт";
            case 13: return "поджёг человека"; case 14: return "поджёг полицейского"; case 15: return "поджёг машину";
            case 16: return "сбил самолёт"; case 17: return "устроил взрыв"; case 18: return "пырнул ножом человека";
            case 19: return "пырнул ножом полицейского"; case 20: return "уничтожил машину"; case 21: return "испортил имущество";
            case 22: return "целился в полицейского"; default: return "нарушил закон";
        }
    }

    typedef void (*Register_t)(uintptr_t, int, const game::Vec&, uint64_t, bool);
    Register_t RegisterCrime = nullptr, RegisterCrimeImm = nullptr;
    typedef void* (*FindWanted_t)(int);
    FindWanted_t FindWanted = nullptr;

    bool CopNear(const game::Vec& at, float r)
    {
        bool found = false;
        game::ForEachPed([&](uintptr_t p) { if(!found && game::PedType(p) == game::PT_COP && game::Alive(p) && game::Dist(game::Pos(p), at) < r) found = true; });
        return found;
    }

    void Process(uintptr_t self, int type, const game::Vec& pos, uint64_t victim, bool dontCare, bool imm)
    {
        Register_t orig = imm ? RegisterCrimeImm : RegisterCrime;
        uintptr_t mine = FindWanted ? (uintptr_t)FindWanted(-1) : 0;
        if(g_bypass || !settings::Get().enabled || !settings::Get().witnesses || self != mine || dontCare) { orig(self, type, pos, victim, dontCare); return; }

        int sev = Severity(type);
        uintptr_t pl = game::Player();
        game::Vec at = pl ? game::Pos(pl) : pos;

        if(CopNear(at, 40.0f))
        {   // копы видели сами
            orig(self, type, pos, victim, dontCare);
            auto& p = world::W().pcase[world::WHO_CJ];
            p.evidence = world::Clamp(p.evidence + 5 * sev, 0, 100);
            if(sev >= 2) { world::Event& e = world::AddEvent("crime", CrimeText(type), at, sev, world::WHO_CJ); e.reported = true; }
            return;
        }
        if(type >= 7 && type <= 9) return;     // ПДД без копов — никто не заметил
        if(type == 1) return;                  // оружие в руке без копов — не преступление для прохожих

        // свидетели
        uintptr_t caller = 0; float bestD = 35.0f; int count = 0;
        int playerGangPed = game::PT_GROVE;
        game::ForEachPed([&](uintptr_t p) {
            if(game::IsPlayer(p) || !game::Alive(p)) return;
            float d = game::Dist(game::Pos(p), at);
            if(d > 35.0f) return;
            int t = game::PedType(p);
            if(t == playerGangPed || t == game::PT_PROSTITUTE || t == game::PT_DEALER || t == game::PT_CRIMINAL) return; // свои/криминал не звонят
            count++;
            if(game::IsGang(t)) return;      // бандиты в полицию не звонят (но запоминают)
            if(d < bestD) { bestD = d; caller = p; }
        });
        double now = talk::Now();
        if(now - g_lastCrimeLog > 3) { g_lastCrimeLog = now; LOG_INFO("Преступление: %s, свидетелей %d, %s", CrimeText(type), count, caller ? "кто-то звонит в полицию" : "звонить некому"); }
        if(!count) return;                      // никто не видел — розыска нет

        world::Event* ev = nullptr;
        static double s_lastEvAt = -100; static int s_lastEvType = -1, s_lastEvId = 0;
        if(sev >= 2 && type == s_lastEvType && now - s_lastEvAt < 10)
        {   // та же очередь выстрелов — одно событие, а не по событию на пулю
            for(auto& e : world::W().events) if(e.id == s_lastEvId) { e.anonWitnesses = std::max(e.anonWitnesses, count); ev = &e; }
            s_lastEvAt = now;
        }
        else if(sev >= 2)
        {
            world::Event& e = world::AddEvent("crime", CrimeText(type), at, sev, world::WHO_CJ);
            s_lastEvAt = now; s_lastEvType = type; s_lastEvId = e.id;
            e.anonWitnesses = count;
            game::ForEachPed([&](uintptr_t p) {
                if(world::Character* c = world::FindByPed(p)) if(game::Dist(game::Pos(p), at) < 35) { world::AddWitness(e, c->id); world::Remember(*c, Fmt("Видел, как %s %s", world::W().playerNick.c_str(), CrimeText(type))); }
            });
            ev = &e;
        }
        if(!caller) return;
        if(g_rep.active) { if(Severity(type) > Severity(g_rep.type)) { g_rep.type = type; g_rep.pos = pos; g_rep.victim = victim; if(ev) g_rep.eventId = ev->id; } return; }
        g_rep.active = true; g_rep.callerRef = game::Ref(caller); g_rep.at = now + 7.0 + world::Rand(0, 4);
        g_rep.wanted = self; g_rep.type = type; g_rep.pos = pos; g_rep.victim = victim; g_rep.imm = imm; g_rep.eventId = ev ? ev->id : 0;
        game::TaskPhone(caller, 8000);
        talk::Notify("Свидетель достаёт телефон — звонит в полицию!");
    }

    void ProcessReport()
    {
        if(!g_rep.active || talk::Now() < g_rep.at) return;
        g_rep.active = false;
        {   // за эти секунды всё могло измениться: игрок погиб, арестован, в участке, катсцена, миссия
            uintptr_t p0 = game::Player();
            if(!p0 || !game::Alive(p0) || Arresting() || game::CutsceneRunning() || game::OnMission()) return;
        }
        if(FindWanted) g_rep.wanted = (uintptr_t)FindWanted(-1);   // не хранить старый указатель (могла быть загрузка)
        uintptr_t caller = game::FromRef(g_rep.callerRef);
        if(!caller || !game::Alive(caller)) { talk::Notify("Свидетель не успел позвонить"); return; }
        uintptr_t pl = game::Player();
        world::Character* c = pl ? world::FindByPed(caller) : nullptr;
        if(c && c->rel[world::WHO_CJ].fear > 60) { talk::Notify(Fmt("%s побоялся звонить", c->name.c_str())); return; }
        g_bypass = true;
        (g_rep.imm ? RegisterCrimeImm : RegisterCrime)(g_rep.wanted, g_rep.type, g_rep.pos, g_rep.victim, false);
        if(game::Wanted() == 0) game::SetWanted(1);
        g_bypass = false;
        for(auto& e : world::W().events) if(e.id == g_rep.eventId) e.reported = true;
        auto& p = world::W().pcase[world::WHO_CJ];
        p.heat = world::Clamp(p.heat + 5 * Severity(g_rep.type), 0, 100);
        p.evidence = world::Clamp(p.evidence + 3 * Severity(g_rep.type), 0, 100);
        talk::Notify("Свидетель сообщил в полицию — тебя ищут");
    }

    // ---------------- задержание ----------------
    enum Stage { ST_NONE, ST_HANDS, ST_APPROACH, ST_CUFF, ST_ESCORT, ST_RIDE, ST_STATION, ST_COURT, ST_JAIL, ST_RELEASE };
    Stage  g_st = ST_NONE;
    double g_stAt = 0;
    int    g_copRef = 0, g_carRef = 0;
    int    g_starsAtArrest = 0;
    uintptr_t g_car = 0;
    Scene  g_scene;
    std::string g_statement;
    int    g_courtReq = 0;
    std::vector<std::pair<std::string, std::string>> g_jailEpisodes;
    size_t g_jailIdx = 0;
    bool   g_inOrigArrest = false;
    bool   g_inHook = false;
    bool   g_passNextArrest = false;     // следующий вызов ArrestPlayer — обычный «Busted» игры (наш сценарий не берём)
    bool   g_locked = false;             // управление заблокировали МЫ (снимать только свою блокировку)
    bool   g_handsUp = false;
    bool   g_calledHelp = false;
    uint8_t* g_gameState = nullptr;      // CGameLogic::GameState: 2 — «арестован», игра сама затемнит и перенесёт в участок
    void (*SetPedState)(uintptr_t, int) = nullptr;

    // Игра уже считает игрока арестованным (состояние педа 63, GameState 2) — снимаем, иначе он заморожен
    // и через 3 с игра сама затемнит экран и перенесёт его, посреди нашего сценария.
    void CancelGameArrest()
    {
        if(uintptr_t pl = game::Player()) if(SetPedState) SetPedState(pl, 1 /*PED_IDLE*/);
        if(g_gameState && *g_gameState == 2) *g_gameState = 0;
    }
    void Lock(bool on) { if(on) { game::LockPlayer(true); g_locked = true; } else if(g_locked) { game::LockPlayer(false); g_locked = false; } }

    typedef void (*ArrestPlayer_t)(uintptr_t);
    ArrestPlayer_t ArrestPlayerOrig = nullptr;
    uintptr_t g_playerInfoForArrest = 0;

    void SetStage(Stage s) { g_st = s; g_stAt = talk::Now(); LOG_INFO("Задержание: этап %d", (int)s); }
    double InStage() { return talk::Now() - g_stAt; }

    uintptr_t Cop() { return game::FromRef(g_copRef); }

    uintptr_t FindCop(float r)
    {
        uintptr_t pl = game::Player();
        // сначала пеший коп (не тот, что проезжает мимо и не пилот вертолёта)
        uintptr_t c = game::NearestPed(game::Pos(pl), r, [](uintptr_t p) { return game::PedType(p) == game::PT_COP && game::Alive(p) && !game::Vehicle(p); });
        return c ? c : game::NearestPed(game::Pos(pl), r * 0.5f, [](uintptr_t p) { return game::PedType(p) == game::PT_COP && game::Alive(p); });
    }

    game::Vec NearestStation()
    {
        game::Vec pl = game::Pos(game::Player());
        static const game::Vec st[] = { { 1555.0f, -1675.6f, 16.2f }, { -1605.6f, 711.5f, 13.9f }, { 2337.0f, 2455.0f, 14.97f } };
        const game::Vec* best = &st[0];
        for(auto& s : st) if(game::Dist2D(s, pl) < game::Dist2D(*best, pl)) best = &s;
        return *best;
    }

    void Fallback(const char* why)
    {
        LOG_ERROR("Задержание: %s — обычный «Busted» игры", why);
        g_st = ST_NONE;
        Lock(false);
        uintptr_t pl = game::Player();
        if(g_inHook && ArrestPlayerOrig && g_playerInfoForArrest) { g_inOrigArrest = true; ArrestPlayerOrig(g_playerInfoForArrest); g_inOrigArrest = false; }
        else if(pl && SetPedState) { g_passNextArrest = true; SetPedState(pl, 63 /*PED_ARRESTED*/); }   // игра сама вызовет ArrestPlayer в следующем кадре
    }

    std::string ChargesText()
    {
        auto& p = world::W().pcase[world::WHO_CJ];
        std::string s;
        for(size_t i = 0; i < p.charges.size(); ++i) s += (i ? ", " : "") + p.charges[i];
        if(s.empty()) s = "сопротивление и нарушение порядка";
        return s;
    }

    void BeginStation()
    {
        uintptr_t pl = game::Player();
        game::Vec st = NearestStation();
        game::LoadScene(st);                     // мир вокруг участка — до переноса (иначе игрок в пустоте)
        if(game::Vehicle(pl) && script::Ready()) script::Do(0x0362, { script::Arg::I(game::Ref(pl)), script::Arg::F(st.x), script::Arg::F(st.y), script::Arg::F(st.z) });   // WARP_CHAR_FROM_CAR_TO_COORD: правильно освобождает место в машине
        else game::Teleport(pl, st);
        g_handsUp = false;
        auto& p = world::W().pcase[world::WHO_CJ];
        // статьи из недавних событий, о которых знает полиция
        for(auto& e : world::W().events)
            if(e.who == world::WHO_CJ && e.reported && e.day >= world::Day() - 3 && (e.kind == "crime" || e.kind == "kill"))
            {
                bool dup = false; for(auto& c : p.charges) if(c == e.text) dup = true;
                if(!dup) p.charges.push_back(e.text);
            }
        g_scene = Scene();
        g_scene.kind = SCENE_STATION;
        g_scene.title = "Участок: оформление";
        g_scene.text = Fmt("Тебя обыскали и сняли отпечатки. Оружие изъято.\nСтатьи: %s.\nУлик: %d/100. Розыск был: %d зв.\n\n"
                           "Можно позвонить адвокату или своему человеку в полиции.", ChargesText().c_str(), p.evidence, g_starsAtArrest);
        g_scene.canLawyer = true;
        SetStage(ST_STATION);
    }

    // формула без ИИ (если нет ключа или ошибка)
    void LocalVerdict()
    {
        auto& p = world::W().pcase[world::WHO_CJ];
        int score = p.evidence + g_starsAtArrest * 12 + (int)p.charges.size() * 6 - (g_statement.empty() ? 0 : 5);
        g_scene.fine = 0; g_scene.days = 0;
        if(score < 25) { g_scene.verdict = "Оправдан за недостатком улик."; }
        else if(score < 50) { g_scene.fine = 200 + score * 20; g_scene.verdict = Fmt("Штраф $%d.", g_scene.fine); }
        else if(score < 75) { g_scene.fine = 500; g_scene.days = 1 + score / 25; g_scene.verdict = Fmt("Срок: %d дн. и штраф $%d.", g_scene.days, g_scene.fine); }
        else { g_scene.days = 3 + score / 15; g_scene.verdict = Fmt("Срок: %d дн.", g_scene.days); }
    }

    void AskVerdict()
    {
        auto& W = world::W();
        auto& p = W.pcase[world::WHO_CJ];
        if(!claude::Available()) { LocalVerdict(); g_scene.text = "Судья зачитывает решение.\n\n" + g_scene.verdict; g_scene.waitingAi = false; return; }
        claude::Request r;
        json::Value sys = json::Value::Object(); sys["type"] = "text";
        sys["text"] = "Ты — судья округа Лос-Сантос (начало 90-х, GTA San Andreas). Ведёшь короткое заседание по делу Карла Джонсона. "
                      "Решай реалистично: улики, тяжесть, свидетели, речь обвиняемого, работа адвоката, прошлые сроки. Говори по-русски, сухо и по делу. "
                      "Срок — в игровых днях (1–30), штраф — в долларах. "
                      "speech — 2–3 коротких предложения (её озвучивают вслух), без перечисления статей.";
        r.system = json::Value::Array(); r.system.push(sys);
        std::string facts = Fmt("Статьи: %s. Улики: %d/100. Интерес полиции к нему: %d/100. Розыск при задержании: %d зв. Раньше сидел: %d дн. "
                                "Есть ли адвокат/связи: %s. Речь обвиняемого: %s",
                                ChargesText().c_str(), p.evidence, p.heat, g_starsAtArrest, p.jailDays,
                                g_calledHelp ? "да, был звонок" : "нет",
                                g_statement.empty() ? "(молчал)" : g_statement.c_str());
        json::Value msg = json::Value::Object(); msg["role"] = "user"; msg["content"] = facts;
        r.messages = json::Value::Array(); r.messages.push(msg);
        json::Value fmt; json::Parse(R"({"type":"json_schema","schema":{"type":"object","additionalProperties":false,
            "required":["speech","verdict","fine","days"],"properties":{"speech":{"type":"string"},
            "verdict":{"type":"string","enum":["acquit","fine","probation","jail"]},"fine":{"type":"integer"},"days":{"type":"integer"}}}})", fmt);
        r.format = fmt; r.maxTokens = 600; r.cache = false; r.tag = "суд"; r.effort = "medium";
        g_scene.waitingAi = true;
        g_courtReq = claude::Send(r, nullptr, [](claude::Result& res) {
            g_courtReq = 0; g_scene.waitingAi = false;
            json::Value v;
            if(!res.ok || !json::Parse(res.text, v)) { LocalVerdict(); g_scene.text = "Судья зачитывает решение.\n\n" + g_scene.verdict; return; }
            g_scene.fine = world::Clamp(v["fine"].i(), 0, 1000000);
            g_scene.days = world::Clamp(v["days"].i(), 0, 30);
            std::string vd = v["verdict"].str();
            if(vd == "acquit") { g_scene.fine = 0; g_scene.days = 0; g_scene.verdict = "Оправдан."; }
            else if(vd == "probation") { g_scene.days = 0; g_scene.verdict = Fmt("Условный срок%s.", g_scene.fine ? Fmt(" и штраф $%d", g_scene.fine).c_str() : ""); }
            else if(vd == "fine") { g_scene.days = 0; g_scene.verdict = Fmt("Штраф $%d.", g_scene.fine); }
            else g_scene.verdict = Fmt("Срок: %d дн.%s", g_scene.days, g_scene.fine ? Fmt(" Штраф $%d.", g_scene.fine).c_str() : "");
            g_scene.text = v["speech"].str() + "\n\nРешение: " + g_scene.verdict;
            talk::Say("Судья", v["speech"].str(), false, 0, 0.8f, 0.95f, true);
        });
        if(!g_courtReq) { g_scene.waitingAi = false; LocalVerdict(); g_scene.text = "Судья зачитывает решение.\n\n" + g_scene.verdict; }
    }

    void BeginCourt()
    {
        g_scene = Scene();
        g_scene.kind = SCENE_COURT;
        g_scene.title = "Суд округа Лос-Сантос";
        g_scene.text = Fmt("Слушается дело Карла Джонсона.\nОбвинение: %s.\n\nСкажи слово в свою защиту (кнопка «Говорить») или промолчи («Дальше»).", ChargesText().c_str());
        g_scene.canSpeak = true;
        g_statement.clear();
        SetStage(ST_COURT);
    }

    void Finish();
    void BeginJail()
    {
        auto& W = world::W();
        auto& p = W.pcase[world::WHO_CJ];
        if(g_scene.fine > 0) game::AddMoney(-std::min(g_scene.fine, game::Money()));
        if(g_scene.days <= 0) { Finish(); return; }      // оправдан / штраф / условно — на свободу (раньше игрок оставался заблокирован)
        p.jailDays += g_scene.days;
        W.day += g_scene.days;
        g_jailEpisodes.clear(); g_jailIdx = 0;
        int days = g_scene.days;
        g_scene = Scene(); g_scene.kind = SCENE_JAIL; g_scene.title = "Тюрьма"; g_scene.days = days;
        g_scene.text = "Срок идёт…";
        if(!claude::Available())
        {
            g_jailEpisodes.push_back({ "Первая ночь", "Сокамерник долго смотрел на тебя, потом спросил, правда ли ты из Grove. Ночь прошла спокойно." });
            g_jailEpisodes.push_back({ "Выход", "Охранник отдал вещи и буркнул: «Не возвращайся»." });
            SetStage(ST_JAIL); return;
        }
        claude::Request r;
        json::Value sys = json::Value::Object(); sys["type"] = "text";
        sys["text"] = "Ты описываешь короткие эпизоды отсидки Карла Джонсона (GTA San Andreas, начало 90-х) в окружной тюрьме. "
                      "2–3 эпизода, каждый 2–4 предложения, по-русски, реалистично и без пафоса: сокамерники, охрана, «смотрящий», драки, знакомства, предложения. "
                      "Для каждого укажи последствие: none, enemy (нажил врага), contact (полезное знакомство), deal (предложение дела после выхода), beaten (избили).";
        r.system = json::Value::Array(); r.system.push(sys);
        json::Value msg = json::Value::Object(); msg["role"] = "user";
        msg["content"] = Fmt("Срок: %d дн. Статьи: %s. Отношения с бандами: %s", g_scene.days, ChargesText().c_str(), world::FactionSummary(world::WHO_CJ).c_str());
        r.messages = json::Value::Array(); r.messages.push(msg);
        json::Value fmt; json::Parse(R"({"type":"json_schema","schema":{"type":"object","additionalProperties":false,"required":["episodes"],
            "properties":{"episodes":{"type":"array","items":{"type":"object","additionalProperties":false,"required":["title","text","effect","person"],
            "properties":{"title":{"type":"string"},"text":{"type":"string"},"effect":{"type":"string","enum":["none","enemy","contact","deal","beaten"]},
            "person":{"type":"string"}}}}}}})", fmt);
        r.format = fmt; r.maxTokens = 900; r.cache = false; r.tag = "тюрьма";
        g_scene.waitingAi = true;
        int id = claude::Send(r, nullptr, [](claude::Result& res) {
            g_scene.waitingAi = false;
            json::Value v;
            if(res.ok && json::Parse(res.text, v))
                for(auto& e : v["episodes"].arr())
                {
                    g_jailEpisodes.push_back({ e["title"].str(), e["text"].str() });
                    std::string eff = e["effect"].str(), who = e["person"].str();
                    if(eff == "contact" || eff == "deal") world::News("Знакомство в тюрьме: " + who + " — " + e["title"].str());
                    if(eff == "enemy") world::News("Враг из тюрьмы: " + who);
                }
            if(g_jailEpisodes.empty()) g_jailEpisodes.push_back({ "Срок", "Дни тянулись одинаково. Наконец тебя выпустили." });
            g_scene.title = g_jailEpisodes[0].first; g_scene.text = g_jailEpisodes[0].second;
        });
        if(!id) { g_scene.waitingAi = false; g_jailEpisodes.push_back({ "Срок", "Дни тянулись одинаково. Наконец тебя выпустили." }); }
        SetStage(ST_JAIL);
    }

    void Finish()
    {
        uintptr_t pl = game::Player();
        game::Vec st = NearestStation();
        st.x += 3.0f;
        game::LoadScene(st);
        game::Teleport(pl, st);
        game::ClearTask(pl);
        Lock(false);
        CancelGameArrest();
        game::SetWanted(0);
        auto& p = world::W().pcase[world::WHO_CJ];
        p.charges.clear(); p.evidence = world::Clamp(p.evidence - 40, 0, 100); p.heat = world::Clamp(p.heat - 30, 0, 100);
        g_scene = Scene(); g_scene.kind = SCENE_RELEASE; g_scene.title = "На свободе"; g_scene.text = "Ты вышел из участка. Изъятое оружие осталось у полиции.";
        SetStage(ST_RELEASE);
    }

    // ---------------- перехваты ----------------
    SDECL_HOOKv(RegisterCrimeH, uintptr_t self, int type, const game::Vec& pos, uint64_t victim, bool dc) { Process(self, type, pos, victim, dc, false); }
    SDECL_HOOKv(RegisterCrimeImmH, uintptr_t self, int type, const game::Vec& pos, uint64_t victim, bool dc) { Process(self, type, pos, victim, dc, true); }
    SDECL_HOOKv(ArrestPlayerH, uintptr_t self)
    {
        g_playerInfoForArrest = self;
        // обычный «Busted» игры: наш выключен, идёт миссия/катсцена (провал миссии — дело игры), или мы сами попросили
        if(g_inOrigArrest || g_passNextArrest || !settings::Get().enabled || !settings::Get().realArrest || game::OnMission() || game::CutsceneRunning())
        { g_passNextArrest = false; ArrestPlayerH(self); return; }
        if(g_st == ST_NONE) { g_inHook = true; StartArrest(0); g_inHook = false; }
        if(g_st != ST_NONE) CancelGameArrest();   // забираем арест себе: игра не должна держать игрока «арестованным»
    }
    SDECL_HOOKv(RegisterKillH, uintptr_t ped, int weapon, bool head, int playerId)
    {
        life::OnKill(ped, weapon, playerId);
        RegisterKillH(ped, weapon, head, playerId);
    }
}

bool Install(void* h)
{
    FindWanted = (FindWanted_t)aml->GetSym(h, "_Z16FindPlayerWantedi");
    g_gameState = (uint8_t*)aml->GetSym(h, "_ZN10CGameLogic9GameStateE");
    SetPedState = (void(*)(uintptr_t, int))aml->GetSym(h, "_ZN4CPed11SetPedStateE9ePedState");
    if(!g_gameState || !SetPedState) LOG_ERROR("Полиция: нет CGameLogic::GameState / CPed::SetPedState — арест может зависать");
    void* rc  = (void*)aml->GetSym(h, "_ZN7CWanted13RegisterCrimeE10eCrimeTypeRK7CVectoryb");
    void* rci = (void*)aml->GetSym(h, "_ZN7CWanted25RegisterCrime_ImmediatelyE10eCrimeTypeRK7CVectoryb");
    void* ap  = (void*)aml->GetSym(h, "_ZN11CPlayerInfo12ArrestPlayerEv");
    void* rk  = (void*)aml->GetSym(h, "_ZN7CDarkel20RegisterKillByPlayerEPK4CPed11eWeaponTypebi");
    if(rc && rci && FindWanted)
    {
        aml->Hook(rc, (void*)HookOf_RegisterCrimeH, (void**)&RegisterCrimeH);
        aml->Hook(rci, (void*)HookOf_RegisterCrimeImmH, (void**)&RegisterCrimeImmH);
        RegisterCrime = RegisterCrimeH; RegisterCrimeImm = RegisterCrimeImmH;
        LOG_INFO("Полиция: розыск по свидетелям включён");
    }
    else LOG_ERROR("Полиция: функции преступлений не найдены — розыск как в обычной игре");
    if(ap) { aml->Hook(ap, (void*)HookOf_ArrestPlayerH, (void**)&ArrestPlayerH); ArrestPlayerOrig = ArrestPlayerH; }
    if(rk) aml->Hook(rk, (void*)HookOf_RegisterKillH, (void**)&RegisterKillH);
    return true;
}

bool Arresting() { return g_st != ST_NONE; }
const Scene& CurrentScene() { return g_scene; }

const char* ArrestStage()
{
    switch(g_st)
    {
        case ST_HANDS: case ST_APPROACH: return "Руки вверх — к тебе идёт полицейский";
        case ST_CUFF: return "На тебя надевают наручники";
        case ST_ESCORT: return "Тебя ведут к машине";
        case ST_RIDE: return "Едете в участок. Можно поговорить с копом (кнопка «Говорить»)";
        default: return "";
    }
}

void Surrender()
{
    if(g_st != ST_NONE) return;
    if(game::Wanted() == 0) { talk::Notify("Тебя никто не ищет"); return; }
    StartArrest(0);
}

void StartArrest(uintptr_t cop)
{
    if(g_st != ST_NONE) return;
    uintptr_t pl = game::Player(); if(!pl) return;
    g_starsAtArrest = game::Wanted();
    if(game::OnMission() || game::CutsceneRunning()) { Fallback("идёт миссия"); return; }
    if(!cop) cop = FindCop(45.0f);
    if(!cop) { Fallback("рядом нет полицейского"); return; }
    g_calledHelp = false;
    talk::EndConversation();
    g_copRef = game::Ref(cop);
    world::Character& c = world::Promote(cop);
    world::Remember(c, Fmt("Задержал %s (розыск %d зв.)", world::W().playerNick.c_str(), g_starsAtArrest));
    Lock(true);
    // в машине: сначала выйти (руки поднимет в Frame, когда будет на ногах — иначе задача выхода перезаписывается)
    g_handsUp = false;
    if(game::Vehicle(pl)) game::TaskLeaveCar(pl);
    else { game::TaskAnim(pl, "handsup", "ped", false, 15000, true); g_handsUp = true; }
    game::TaskAimAt(cop, pl, 3000);
    talk::Say(c.nick.empty() ? c.name : c.nick, "Полиция! Руки, чтоб я видел! Не двигаться!", false, c.id, c.pitch, c.rate, true);
    SetStage(ST_HANDS);
}

void Release(const char* why)
{
    if(g_st == ST_NONE) return;
    LOG_INFO("Задержание прервано: %s", why);
    uintptr_t pl = game::Player();
    if(g_st >= ST_STATION) { Finish(); return; }
    if(game::Vehicle(pl)) game::TaskLeaveCar(pl); else game::ClearTask(pl);
    Lock(false);
    CancelGameArrest();
    game::SetWanted(0);
    if(uintptr_t cop = Cop()) game::TaskWander(cop);
    g_st = ST_NONE;
    talk::Notify(Fmt("Тебя отпустили (%s)", why));
}

void SceneContinue()
{
    if(g_scene.waitingAi) return;
    switch(g_st)
    {
        case ST_STATION: BeginCourt(); break;
        case ST_COURT:
            if(g_scene.canSpeak) { g_scene.canSpeak = false; g_scene.text = "Суд удаляется для решения…"; AskVerdict(); }
            else BeginJail();
            break;
        case ST_JAIL:
            if(g_jailIdx + 1 < g_jailEpisodes.size()) { ++g_jailIdx; g_scene.title = g_jailEpisodes[g_jailIdx].first; g_scene.text = g_jailEpisodes[g_jailIdx].second; }
            else Finish();
            break;
        case ST_RELEASE: g_scene = Scene(); g_st = ST_NONE; break;
        default: break;
    }
}

void SceneSpeak(const std::string& text)
{
    if(g_st != ST_COURT || !g_scene.canSpeak) return;
    g_statement = text;
    g_scene.canSpeak = false;
    g_scene.text = "«" + text + "»\n\nСуд удаляется для решения…";
    AskVerdict();
}

void SceneCallHelp()
{
    if(g_st != ST_STATION || !g_scene.canLawyer) return;
    g_scene.canLawyer = false; g_calledHelp = true;
    // свой человек в полиции может вытащить
    for(auto& c : world::W().chars)
    {
        const world::Relation& r = c.rel[world::WHO_CJ];
        if(c.alive && c.faction == world::F_POLICE && r.payroll && r.trust > 20)
        {
            int chance = 30 + c.rank * 20 + r.trust / 4 - world::W().pcase[world::WHO_CJ].evidence / 3;
            if(world::Rand(0, 100) < chance)
            {
                g_scene.text = Fmt("%s сделал пару звонков. Дело «потерялось». Тебя отпускают без суда.", c.name.c_str());
                world::Remember(c, "Вытащил игрока из участка");
                world::News(Fmt("%s вытащил тебя из участка. Он будет ждать благодарности.", c.name.c_str()));
                world::W().pcase[world::WHO_CJ].charges.clear();
                g_scene.kind = SCENE_RELEASE; g_stAt = talk::Now(); g_st = ST_RELEASE;
                Finish();
                return;
            }
            g_scene.text = Fmt("%s: «Сейчас не могу — на меня смотрят. Держись.»", c.name.c_str());
            return;
        }
    }
    int fee = 1500;
    if(game::Money() >= fee)
    {
        game::AddMoney(-fee);
        auto& p = world::W().pcase[world::WHO_CJ];
        p.evidence = world::Clamp(p.evidence - 25, 0, 100);
        g_scene.text = Fmt("Адвокат взял $%d и обещал развалить часть обвинения.", fee);
    }
    else g_scene.text = "На адвоката нет денег — будет государственный защитник.";
}

void Crime(int crimeType, int severity, const char* text, bool cops)
{
    world::Event& e = world::AddEvent("crime", text, game::Pos(game::Player()), severity, world::WHO_CJ);
    e.reported = cops;
    (void)crimeType;
}

void Reset()
{
    // загрузка сохранения / новая игра: ничего от прошлого ареста не тянуть
    if(g_st != ST_NONE) LOG_INFO("Задержание сброшено (загрузка игры)");
    g_st = ST_NONE; g_scene = Scene(); g_rep.active = false; g_jailEpisodes.clear(); g_jailIdx = 0;
    g_handsUp = false; g_passNextArrest = false; g_calledHelp = false;
    if(g_courtReq) { claude::Cancel(g_courtReq); g_courtReq = 0; }
    Lock(false);
}

void Frame()
{
    ProcessReport();
    if(g_st == ST_NONE) return;
    if(g_gameState && *g_gameState == 2) *g_gameState = 0;      // игра не должна сама затемнять и переносить, пока идёт наш сценарий
    uintptr_t pl = game::Player();
    uintptr_t cop = Cop();
    if(!pl) return;
    if(g_st == ST_HANDS && !g_handsUp && !game::Vehicle(pl)) { game::TaskAnim(pl, "handsup", "ped", false, 15000, true); g_handsUp = true; }
    switch(g_st)
    {
        case ST_HANDS:
            if(InStage() > 1.2) { if(!cop) { Fallback("коп пропал"); return; } game::TaskGoTo(cop, game::Pos(pl), 6, 1.2f); SetStage(ST_APPROACH); }
            break;
        case ST_APPROACH:
            if(!cop || !game::Alive(cop)) { Release("коп выбыл"); return; }
            if(game::Dist(game::Pos(cop), game::Pos(pl)) < 1.8f || InStage() > 10)
            {
                game::TaskAnim(cop, "ARRESTgun", "ped", false, 0, true);
                game::ClearWeapons(pl);
                game::SetWanted(0);
                auto& p = world::W().pcase[world::WHO_CJ];
                p.evidence = world::Clamp(p.evidence + 5, 0, 100);
                world::Character* c = world::FindByPed(cop);
                talk::Say(c ? (c->nick.empty() ? c->name : c->nick) : "Коп", "Руки за спину. Ты задержан. Имеешь право хранить молчание.", false, c ? c->id : 0, c ? c->pitch : 0.85f, 1.0f, true);
                SetStage(ST_CUFF);
            }
            break;
        case ST_CUFF:
            if(InStage() > 3.0)
            {
                game::ClearTask(pl);                  // опустить руки: дальше игрока ведут к машине
                // только патрульная машина (не мотоцикл, не чужая машина с водителем на светофоре)
                g_car = game::NearestPoliceCar(game::Pos(pl), 45.0f);
                if(!g_car)
                {   // коп на мотоцикле / пешком: вызывает патруль — короткая пауза и затемнение, дальше участок
                    talk::Notify("Коп вызвал патрульную машину — тебя везут в участок");
                    SetStage(ST_ESCORT); g_car = 0;
                    break;
                }
                if(game::VehDriver(g_car)) game::TaskEnterCar(cop, g_car, false, 0);   // напарник за рулём — коп садится рядом
                else game::TaskEnterCar(cop, g_car, true);
                game::TaskEnterCar(pl, g_car, false, 1);
                SetStage(ST_ESCORT);
            }
            break;
        case ST_ESCORT:
            if(!g_car && InStage() < 3.0) break;                   // ждём «патруль» (пауза перед затемнением)
            if(!g_car || InStage() > 16 || (game::Vehicle(pl) && game::Vehicle(cop)))
            {
                if(!game::Vehicle(pl)) { game::ClearTask(pl); BeginStation(); break; }   // не сел — не держим игрока, сразу участок
                if(uintptr_t v = game::Vehicle(pl))
                {   // везёт тот, кто за рулём (сам коп или его напарник)
                    uintptr_t drv = game::VehDriver(v);
                    if(drv && drv != pl) game::TaskDriveTo(drv, v, NearestStation(), 14.0f);
                }
                SetStage(ST_RIDE);
                if(cop) { world::Character& c = world::Promote(cop); talk::Notify(Fmt("В машине с копом %s. Можно попробовать договориться.", c.name.c_str())); }
            }
            break;
        case ST_RIDE:
            if(InStage() > 25) BeginStation();
            break;
        default: break;
    }
}

} // namespace police
