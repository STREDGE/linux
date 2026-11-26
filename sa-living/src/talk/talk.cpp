#include "talk.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <functional>
#include <map>
#include <vector>

#include "game/game.h"
#include "jarvis/script.h"
#include "net/link.h"
#include "platform/bridge.h"
#include "settings.h"
#include "sim/life.h"
#include "sim/crew.h"
#include "sim/police.h"
#include "speech/speech.h"
#include "util/log.h"
#include "trace/trace.h"

using world::Character;
using world::Fmt;

namespace talk
{
namespace
{
    // ---- разговор ----
    bool g_memberAct = false;
    double g_nextWarm = 0, g_lastTalkReq = -1000;   // прогрев кэша правил педов
    struct Conv
    {
        int  charId = 0;
        int  who = world::WHO_CJ;
        bool phone = false;
        bool active = false;
        int  reqId = 0;
        double sentAt = 0;            // когда ушёл запрос хосту (кооп, reqId=-1)
        std::string spoken;           // уже озвученная часть текущего ответа
        std::string buf;              // хвост, ещё не озвученный
        double lastAt = 0;
        int  offer = 0;               // игрок протянул деньги (ждём answer_offer)
        int  demand = 0;              // пед требует деньги
        std::string demandWhy;
        bool endAfterSpeech = false;
        bool remote = false;          // мы — друг: ответы идут через хоста
        std::vector<int> group;       // групповой разговор: кто ещё рядом из его компании (номера персонажей)
    };
    Conv g_c;

    // ---- распознавание ----
    bool g_listening = false, g_waitFinal = false;
    std::string g_partial;
    double g_sttUntil = 0;

    std::function<void(const std::string&)> g_sink;
    std::function<void(const std::string&)> g_capture;    // разовый приёмник (Джарвис, проверка микрофона)
    bool g_captureActive = false;
    double g_heardAt = 0;            // когда игрок договорил (для замера «ответ через N с»)
    bool   g_firstVoice = false;
    std::deque<Sub> g_subs;
    std::deque<std::string> g_notes;
    int g_ttsSeq = 0;

    struct Delayed { double at; std::function<void()> fn; };
    std::vector<Delayed> g_later;

    void Later(double sec, std::function<void()> fn) { g_later.push_back({ Now() + sec, std::move(fn) }); }

    const char* WhoName(int who)
    {
        auto& w = world::W();
        return who == world::WHO_CJ ? w.playerNick.c_str() : (w.friendNick.empty() ? "друг CJ" : w.friendNick.c_str());
    }

    uintptr_t PlayerFor(int who)
    {
        (void)who;   // друг на своём телефоне тоже «игрок 0»
        return game::Player();
    }

    // ---------- подсказка: обстановка ----------
    std::string Situation(Character& c, int who, bool phone)
    {
        std::string s = Fmt("[Обстановка: день %d, %02d:%02d, район %s.", world::Day(), game::Hour(), game::Minute(),
                            game::ZoneName().empty() ? "неизвестен" : game::ZoneName().c_str());
        if(phone) { s += " Это ТЕЛЕФОННЫЙ разговор: ты не рядом с собеседником, действовать телом не можешь."; s += "]"; return s; }
        uintptr_t ped = world::PedOf(c), pl = PlayerFor(who);
        if(ped && pl)
        {
            float d = game::Dist(game::Pos(ped), game::Pos(pl));
            s += Fmt(" Собеседник (%s) в %.0f м от тебя", WhoName(who), d);
            if(game::PlayerInVehicle()) s += ", сидит в машине";
            s += ".";
            if(game::Vehicle(ped)) s += " ТЫ СИДИШЬ ЗА РУЛЁМ/В МАШИНЕ (выйти — move get_out).";
            int at = game::ActiveTaskType(ped);
            if(at == 936) s += " Ты сейчас идёшь за ним.";
            else if(at == 933) s += " Ты сейчас держишь его на прицеле.";
            if(game::PlayerTarget() == ped) s += " ОН ЦЕЛИТСЯ В ТЕБЯ ИЗ ОРУЖИЯ.";
            s += crew::Describe(pl, ped);                                   // кто сейчас бьёт собеседника (чтобы вступиться — а не бить его самого)
            int w = game::Wanted();
            if(w > 0) s += Fmt(" Его разыскивает полиция: %d зв.", w);
            int money = game::Money();
            if(money > 20000) s += " Выглядит очень при деньгах."; else if(money > 3000) s += " Похоже, при деньгах.";
            // кто рядом
            // Свидетели — только те, кто реально рядом (до 12 м) и не сидит в машине: прохожие в 25 м
            // ничего не слышат, а коп «при свидетелях» отказывался от любой взятки (тест 0.2.7).
            int same = 0, cops = 0, people = 0, far = 0;
            game::Vec me = game::Pos(ped);
            uintptr_t myCar = game::Vehicle(ped);
            bool together = myCar && myCar == game::Vehicle(pl);
            game::ForEachPed([&](uintptr_t p) {
                if(p == ped || game::IsPlayer(p) || !game::Alive(p)) return;
                float d = game::Dist(game::Pos(p), me);
                if(d > 30) return;
                if(d > 12 || game::Vehicle(p)) { far++; return; }
                int t = game::PedType(p);
                if(t == game::PT_COP) cops++;
                else if(world::FactionOfPedType(t) == c.faction && c.faction != world::F_CIVIL) same++;
                else people++;
            });
            if(together) s += " Вы ВДВОЁМ в машине — вас никто не слышит и не видит.";
            else
            {
                if(same) s += Fmt(" Рядом (слышат вас) твоих (%s): %d.", world::FactionName(c.faction), same);
                if(cops) s += Fmt(" Рядом (слышат вас) полицейских: %d.", cops);
                if(people) s += Fmt(" Рядом (слышат вас) посторонних: %d.", people);
                if(!same && !cops && !people) s += far ? Fmt(" Рядом никого — люди только вдалеке (%d), разговор никто не слышит.", far) : " Свидетелей вокруг нет.";
            }
        }
        if(g_c.active && g_c.charId == c.id && !g_c.group.empty())
        {
            s += " РЯДОМ СТОЯТ И СЛЫШАТ РАЗГОВОР (могут вмешаться: group_say / group_act):";
            for(int id : g_c.group)
                if(Character* m = world::Find(id))
                {
                    const char* temper = m->tr.aggression > 65 ? "вспыльчивый" : m->tr.courage < 35 ? "трусоватый" : m->tr.greed > 65 ? "жадный" : m->tr.talkative > 65 ? "болтливый" : "спокойный";
                    const char* role = m->faction == c.faction && c.faction != world::F_CIVIL ? (c.faction == world::F_POLICE ? "твой коллега" : "свой") : game::PedTypeName(m->pedType);
                    s += Fmt(" [#%d %s — %s%s, %s, симпатия к собеседнику %d]", m->id, m->nick.empty() ? m->name.c_str() : m->nick.c_str(), role,
                             m->rank >= 2 ? " (старший)" : "", temper, m->rel[who].liking);
                }
        }
        s += Fmt(" У тебя при себе $%d.]", c.money);
        return s;
    }

    // ---------- история диалога ----------
    void TrimDialog(json::Value& d)
    {
        if(!d.isArray()) { d = json::Value::Array(); return; }
        auto& a = d.arr();
        while(a.size() > 18)
        {
            a.erase(a.begin());
            // первым должно идти сообщение пользователя без «висящих» tool_result
            while(!a.empty() && a[0]["role"].str() != "user") a.erase(a.begin());
            if(!a.empty() && a[0]["content"].isArray())
            {
                json::Value nc = json::Value::Array();
                for(auto& b : a[0]["content"].arr()) if(b["type"].str() != "tool_result") nc.push(b);
                if(nc.size() == 0) { a.erase(a.begin()); continue; }
                a[0]["content"] = nc;
            }
        }
    }

    void AppendUser(json::Value& d, const std::string& text)
    {
        if(!d.isArray()) d = json::Value::Array();
        json::Value blk = json::Value::Object(); blk["type"] = "text"; blk["text"] = text;
        auto& a = d.arr();
        if(!a.empty() && a.back()["role"].str() == "user")
        {
            json::Value& c = a.back()["content"];
            if(!c.isArray()) { json::Value t = json::Value::Object(); t["type"] = "text"; t["text"] = c.str(); json::Value arr = json::Value::Array(); arr.push(t); c = arr; }
            c.push(blk);
            return;
        }
        json::Value m = json::Value::Object(); m["role"] = "user"; json::Value content = json::Value::Array(); content.push(blk); m["content"] = content;
        d.push(m);
    }

    // ---------- голоса ----------
    unsigned HashStr(const std::string& s) { unsigned h = 2166136261u; for(unsigned char c : s) { h ^= c; h *= 16777619u; } return h; }

    void SpeakAs(Character* c, const std::string& who, const std::string& text)
    {
        std::string t = text;
        while(!t.empty() && (t[0] == ' ' || t[0] == '\n')) t.erase(0, 1);
        if(t.empty() || !settings::Get().voices) return;
        if(settings::Get().engine == 0)
        {
            if(!speech::TtsReady()) return;
            std::string key; float p = 1, r = 1;
            if(c) { EnsureVoice(*c); key = c->voice; p = c->vpitch; r = c->vrate; }
            else key = speech::PickVoice(false, HashStr(who));
            speech::Voice v;
            if(speech::FindVoice(key, v)) { p *= v.pitch; r *= v.speed; }
            if(!g_firstVoice && g_heardAt > 0) { g_firstVoice = true; LOG_INFO("Замер: от конца твоей фразы до первой реплики в очереди голоса — %.2f с", Now() - g_heardAt); }
            speech::Speak(t, key, p, r, Fmt("u%d", ++g_ttsSeq));
        }
        else if(bridge::TtsReady())
            bridge::TtsSpeak(t, c ? c->pitch : 1.0f, c ? c->rate : 1.0f, Fmt("u%d", ++g_ttsSeq), true);
    }

    void StopVoice() { speech::StopSpeaking(); bridge::TtsStop(); }

    // ---------- озвучка по фразам ----------
    void SubAppend(Character* c, const std::string& delta);

    // Поза собеседника на время разговора (раньше все стояли «смирно» — TaskStand):
    //  слушает: коп — держит на прицеле, если игрок в розыске, иначе полицейская стойка; бандит/барыга — уличная стойка;
    //           женщина, пожилой — свои стойки; остальные — обычная;
    //  говорит: жестикулирует (IDLE_chat) по длине фразы.
    // Не трогаем, если пед занят своим (идёт за CJ, бежит, дерётся, целится), сидит в машине или он из миссии.
    bool CanPose(uintptr_t ped)
    {
        if(!ped || !game::Alive(ped) || game::Vehicle(ped) || game::IsScriptPed(ped)) return false;
        if(game::FightTarget(ped)) return false;                               // дерётся / целится — позы поверх драки ломали анимацию
        int at = game::ActiveTaskType(ped);
        return at == -1 || at == 203 || at == 912 || at == 401 || at == 920;   // нет задачи / стоит / гуляет / анимация / поворот
    }
    void FaceCJ(uintptr_t ped)
    {
        game::Vec a = game::Pos(ped), b = game::Pos(game::Player());
        game::SetHeading(ped, atan2f(-(b.x - a.x), b.y - a.y));
    }
    void ListenPose(Character* c, uintptr_t ped, int ms)
    {
        if(!CanPose(ped)) return;
        FaceCJ(ped);
        int t = game::PedType(ped);
        if(t == game::PT_COP)
        {
            if(game::Wanted() > 0) { game::TaskAimAt(ped, game::Player(), ms); return; }        // в розыске — на прицеле
            if(game::TaskAnim(ped, "Coplook_loop", "COP_AMBIENT", true, ms)) return;
            game::TaskAnim(ped, "IDLE_stance", "ped", true, ms); return;
        }
        if(game::IsGang(t) || t == game::PT_DEALER || t == game::PT_CRIMINAL) { if(game::TaskAnim(ped, "Idle_Gang1", "ped", true, ms)) return; }
        else if(c && c->female) { if(game::TaskAnim(ped, "woman_idlestance", "ped", true, ms)) return; }
        else if(c && c->age >= 55) { if(game::TaskAnim(ped, "idlestance_old", "ped", true, ms)) return; }
        if(!game::TaskAnim(ped, "IDLE_stance", "ped", true, ms)) game::TaskStand(ped, ms);
    }
    void TalkPose(Character* c, uintptr_t ped, const std::string& text)
    {
        if(!CanPose(ped) || game::ActiveTaskType(ped) == 933) return;
        int ms = 1200 + (int)text.size() * 45;                         // примерно по длине фразы (байты UTF-8 ≈ 2 на букву)
        if(ms > 9000) ms = 9000;
        FaceCJ(ped);
        if(game::PedType(ped) == game::PT_COP && game::Wanted() > 0) return;   // коп с пистолетом наготове не машет руками
        game::TaskAnim(ped, "IDLE_chat", "ped", true, ms);
        (void)c;
    }

    // Фраза — это ремарка/рассуждение, а не прямая речь? («Чен замирает и оглядывается…», «CJ — брат по Grove?»)
    // Такое модель иногда пишет в ответ — вслух и в субтитры не пускаем.
    bool IsNarration(Character* c, const std::string& raw)
    {
        std::string s = raw; while(!s.empty() && (s[0] == ' ' || s[0] == '\n')) s.erase(0, 1);
        if(s.empty()) return true;
        if(s[0] == '*' || s[0] == '(' || s[0] == '[') return true;
        if(s.find("от лица") != std::string::npos || s.compare(0, 10, "Ответ") == 0) return true;   // «Ответ от лица Андре: …»
        if(!c) return false;
        auto startsWord = [&](const std::string& w) { return w.size() > 1 && s.compare(0, w.size(), w) == 0 && (s.size() == w.size() || s[w.size()] == ' ' || s[w.size()] == ','); };
        std::string first = c->name.substr(0, c->name.find(' '));
        if(startsWord(first) || startsWord(c->name) || (!c->nick.empty() && startsWord(c->nick))) return true;   // говорит о себе в третьем лице
        if(s.compare(0, 3, "CJ ") == 0 && s.find('?') != std::string::npos && s.find("CJ,") == std::string::npos) return true;    // «CJ — брат…?» — рассуждение о собеседнике
        return false;
    }
    void SpeakChunk(Character* c, const std::string& text)
    {
        if(IsNarration(c, text)) { TRACE("РЕМАРКА", "не озвучено (ремарка/рассуждение): %s", text.c_str()); return; }
        SubAppend(c, text);
        SpeakAs(c, c ? c->name : "", text);
        if(c && !g_c.phone) TalkPose(c, world::PedOf(*c), text);
    }

    size_t SentenceEnd(const std::string& s)
    {
        for(size_t i = 0; i < s.size(); ++i)
        {
            char ch = s[i];
            if((ch == '.' || ch == '!' || ch == '?' || ch == '\n') && i > 8) return i + 1;
        }
        return s.size() > 160 ? s.size() : std::string::npos;
    }

    int Cps(const std::string& s) { int n = 0; for(unsigned char ch : s) if((ch & 0xC0) != 0x80) ++n; return n; }   // символов (не байтов)
    double SubDur(const std::string& s) { double d = 2.5 + Cps(s) / 14.0; return d < 3 ? 3 : d > 10 ? 10 : d; }

    void SubAppend(Character* c, const std::string& delta)
    {
        if(!settings::Get().subtitles) return;
        if(g_subs.empty() || g_subs.back().player || g_subs.back().charId != (c ? c->id : 0) || g_subs.back().until < Now())
        {
            Sub s; s.who = c ? (c->nick.empty() ? c->name : c->nick) : "?"; s.player = false; s.charId = c ? c->id : 0;
            g_subs.push_back(s);
        }
        g_subs.back().text += delta;
        g_subs.back().until = Now() + SubDur(g_subs.back().text);
        while(g_subs.size() > 12) g_subs.pop_front();
    }

    // ---------- исполнение «мировых» инструментов (у хоста / в одиночке) ----------
    std::string ExecuteWorld(Character& c, int who, const std::string& n, const json::Value& in, bool& physical)
    {
        auto& W = world::W();
        world::Relation& r = c.rel[who];
        physical = false;
        if(n == "group_say" || n == "group_act")
        {
            Character* m = world::Find(in["who"].i());
            bool inGroup = false; for(int id : g_c.group) if(m && id == m->id) inGroup = true;
            if(!m || !inGroup || !(g_c.active && g_c.charId == c.id)) return "такого человека рядом нет";
            uintptr_t mped = world::PedOf(*m);
            if(n == "group_say")
            {
                std::string text = in["text"].str();
                if(text.empty()) return "пусто";
                Say(m->nick.empty() ? m->name : m->nick, text, false, m->id, m->pitch, m->rate, true);
                m->lastTalkDay = world::Day();
                return "сказал";
            }
            if(!mped) return "он уже ушёл";
            struct MemberScope { MemberScope() { g_memberAct = true; } ~MemberScope() { g_memberAct = false; } } scope;   // действие одного из компании не обрывает весь разговор
            const std::string& a = in["action"].str();
            json::Value arg = json::Value::Object();
            if(a == "call_police") { json::Value cp = json::Value::Object(); cp["who"] = "police"; cp["message"] = ""; return ExecutePhysical(m, mped, who, "call", cp); }
            static const char* moves[] = { "follow_player", "come_closer", "go_away", "flee", "wait_here" };
            for(const char* mv : moves) if(a == mv) { arg["mode"] = a; arg["run"] = a == "flee"; return ExecutePhysical(m, mped, who, "move", arg); }
            if(a == "attack" || a == "aim_weapon") { arg["weapon"] = in["weapon"].s(a == "attack" ? "fists" : "pistol"); arg["target"] = in["target"].s("him"); return ExecutePhysical(m, mped, who, a, arg); }
            if(a == "holster") return ExecutePhysical(m, mped, who, "holster", arg);
            arg["kind"] = a;
            return ExecutePhysical(m, mped, who, "gesture", arg);
        }
        if(n == "relationship")
        {
            world::ChangeRelation(c, who, in["trust"].i(), in["respect"].i(), in["liking"].i(), in["fear"].i(), in["why"].str().c_str());
            return "ok";
        }
        if(n == "remember") { world::Remember(c, in["fact"].str()); return "запомнил"; }
        if(n == "set_goal")
        {
            int h = world::Clamp(in["hours"].i(24), 1, 24 * 14);
            c.goal = in["kind"].str() + ": " + in["text"].str();
            c.goalUntilMin = world::NowMin() + h * 60;
            c.goalTarget = who;
            world::Remember(c, "Моя цель: " + in["text"].str());
            return Fmt("цель поставлена на %d ч", h);
        }
        if(n == "share_contact")
        {
            r.contact = true;
            if(who == world::WHO_CJ) Notify(Fmt("Номер %s добавлен в телефон", c.name.c_str()));
            return "номер дан";
        }
        if(n == "share_info")
        {
            world::News(Fmt("Сведения от %s: %s", c.name.c_str(), in["text"].str().c_str()));
            if(who == world::WHO_CJ) Notify("Записано в заметки (телефон > Новости)");
            r.favors++;
            return "рассказал";
        }
        if(n == "spread_rumor")
        {
            world::Rumor rm; rm.text = in["text"].str(); rm.zone = c.homeZone; rm.day = W.day; rm.reach = 1;
            W.rumors.push_back(rm);
            return "слух пошёл";
        }
        if(n == "agree_deal")
        {
            std::string k = in["kind"].str(); int price = world::Clamp(in["price"].i(), 0, 1000000);
            if(k == "payroll") { r.payroll = true; r.weekly = price; r.paidDay = W.day - 8; world::Remember(c, Fmt("Договорились: работаю на %s за $%d в неделю (%s)", WhoName(who), price, in["details"].str().c_str())); if(who == world::WHO_CJ) Notify(Fmt("%s работает на тебя: $%d в неделю. Заплати первую неделю.", c.name.c_str(), price)); }
            else if(k == "informant") { r.informant = true; r.contact = true; world::Remember(c, "Тайно сливаю сведения " + std::string(WhoName(who))); }
            else if(k == "truce")
            {
                if(c.rank < 3) return "ты не можешь решать за всю банду — только старшие";
                world::FactionState& f = W.fac[c.faction]; f.truce = true; f.truceUntil = W.day + 7; f.tension = world::Clamp(f.tension - 40, 0, 100);
                f.hostility[who] = world::Clamp(f.hostility[who] - 30, 0, 100);
                world::News(Fmt("Перемирие с %s на 7 дней (договорился %s)", world::FactionName(c.faction), c.name.c_str()));
            }
            else if(k == "defect")
            {
                int old = c.faction;
                if(old == W.playerGang) return "он и так из твоих";
                W.fac[old].hostility[who] = world::Clamp(W.fac[old].hostility[who] + 20, 0, 100);
                c.role = Fmt("бывший %s, перешёл к %s", world::FactionName(old), world::FactionName(W.playerGang));
                c.faction = W.playerGang; c.exposed = true; c.secretlyLoyal = false;
                world::News(Fmt("%s ушёл из %s к тебе. Его бывшие будут его искать.", c.name.c_str(), world::FactionName(old)));
            }
            else world::Remember(c, Fmt("Договорились (%s, $%d): %s", k.c_str(), price, in["details"].str().c_str()));
            if(price > 0 && k != "payroll") { g_c.demand = price; g_c.demandWhy = k; }
            return "договорились";
        }
        if(n == "end_deal")
        {
            r.payroll = false; r.informant = false;
            world::Remember(c, "Разорвал договор с " + std::string(WhoName(who)) + ": " + in["why"].str());
            if(who == world::WHO_CJ) Notify(Fmt("%s больше на тебя не работает", c.name.c_str()));
            return "договор разорван";
        }
        if(n == "betray")
        {
            std::string how = in["how"].str();
            world::Remember(c, "Я сдал " + std::string(WhoName(who)) + " (" + how + "): " + in["details"].str());
            LOG_INFO("Предательство: #%d %s → %s", c.id, c.name.c_str(), how.c_str());
            if(how == "tell_gang") { auto& f = W.fac[c.faction]; f.hostility[who] = world::Clamp(f.hostility[who] + 15, 0, 100); f.plan = "засада на " + std::string(WhoName(who)) + ": " + in["details"].str(); }
            else if(how == "ambush") { c.goal = "hunt_player: засада на " + std::string(WhoName(who)); c.goalUntilMin = world::NowMin() + 48 * 60; c.goalTarget = who; }
            else if(how == "tell_police") { auto& p = W.pcase[who]; p.heat = world::Clamp(p.heat + 20, 0, 100); p.evidence = world::Clamp(p.evidence + 15, 0, 100); }
            return "сделано тайно";
        }
        if(n == "police_action")
        {
            if(c.faction != world::F_POLICE) return "ты не полицейский";
            std::string a = in["action"].str(); int amt = in["amount"].i();
            auto& p = W.pcase[who];
            if(a == "add_charge") { p.charges.push_back(in["note"].str()); p.heat = world::Clamp(p.heat + 5 * world::Clamp(amt, 1, 5), 0, 100); return "статья добавлена"; }
            if(a == "drop_case")
            {
                if(c.rank < 2) return "у тебя нет такой власти — только детектив и выше";
                p.charges.clear(); p.evidence = 0; p.heat = world::Clamp(p.heat - 40, 0, 100);
                r.favors++; if(W.pcase[who].iaInvestigation == false && world::Rand(0, 100) < 15) W.pcase[who].iaInvestigation = true;
                world::News(Fmt("%s развалил дело против тебя", c.name.c_str()));
                return "дело развалено";
            }
            if(a == "warn_raid") { world::News(Fmt("%s: %s", c.name.c_str(), in["note"].str().c_str())); r.favors++; return "предупредил"; }
            physical = true;  // остальное — у того, кто говорит (розыск, арест, штраф)
            return "";
        }
        physical = true;
        return "";
    }

    // ---------- ход ----------
    void FinishTurn(Character* c, int who, claude::Result& res, bool remoteFriend, json::Value* outActions);

    bool SendTurn(Character& c, int who, const std::string& userText, bool important, bool remoteFriend, std::function<void(json::Value)> remoteDone = nullptr)
    {
        if(!claude::HasKey()) { if(!remoteFriend) Notify("Нет ключа ИИ: Меню > ИИ > Вставить ключ"); return false; }
        if(claude::LimitReached()) { if(!remoteFriend) Notify("Лимит запросов ИИ на сегодня исчерпан"); return false; }
        bool phone = !remoteFriend && g_c.phone && g_c.charId == c.id;

        json::Value dialog = c.dialog; TrimDialog(dialog);
        std::string turn = remoteFriend ? userText : (Situation(c, who, phone) + "\n" + userText);
        AppendUser(dialog, turn);

        claude::Request req;
        json::Value rules = json::Value::Object(); rules["type"] = "text"; rules["text"] = WorldRules();
        json::Value cc = json::Value::Object(); cc["type"] = "ephemeral"; rules["cache_control"] = cc;
        json::Value card = json::Value::Object(); card["type"] = "text";
        card["text"] = "КТО ТЫ\n" + world::Card(c, who) + "\nЧТО ТЫ ЗНАЕШЬ О СОБЕСЕДНИКЕ И ГОРОДЕ\n" + world::KnownFacts(c, who);
        req.system = json::Value::Array(); req.system.push(rules); req.system.push(card);
        req.tools = Tools();
        req.messages = dialog;
        req.maxTokens = 1200;
        req.tag = "разговор";
        // Живой разговор: ответ нужен за секунду-две. «Быстро» — без размышления; «Умнее» — с коротким размышлением.
        g_lastTalkReq = Now();
        req.effort = "low";
        // С коротким размышлением: без него модель вслух проговаривала ремарки и свои рассуждения (тест 0.2.6).
        // Задержка с размышлением на low по логам 1,2–2 с — разница с «без» небольшая.
        req.noThinking = false;
        (void)important;

        int charId = c.id;
        std::string userTurn = turn;
        if(!remoteFriend)
        {
            g_c.spoken.clear(); g_c.buf.clear();
            g_firstVoice = false;
        }
        int id = claude::Send(req,
            [charId, remoteFriend](const std::string& delta) {
                if(remoteFriend) return;
                Character* ch = world::Find(charId);
                g_c.buf += delta;              // субтитры — по готовым фразам (SpeakChunk), чтобы ремарки не мелькали на экране
                size_t e;
                while((e = SentenceEnd(g_c.buf)) != std::string::npos)
                {
                    SpeakChunk(ch, g_c.buf.substr(0, e));
                    g_c.spoken += g_c.buf.substr(0, e);
                    g_c.buf.erase(0, e);
                }
            },
            [charId, who, userTurn, remoteFriend, remoteDone](claude::Result& r) {
                Character* ch = world::Find(charId);
                if(!ch) return;
                if(!remoteFriend) { if(!g_c.buf.empty()) { SpeakChunk(ch, g_c.buf); g_c.buf.clear(); } g_c.reqId = 0; g_c.lastAt = Now(); }
                if(!r.ok)
                {
                    if(!remoteFriend) Notify("ИИ: " + r.error);
                    if(remoteDone) { json::Value e = json::Value::Object(); e["error"] = r.error; remoteDone(e); }
                    return;
                }
                // история: реплика пользователя + ответ
                AppendUser(ch->dialog, userTurn);
                json::Value am = json::Value::Object(); am["role"] = "assistant"; am["content"] = r.assistantContent;
                if(r.assistantContent.size()) ch->dialog.push(am);
                ch->lastTalkDay = world::Day(); ch->lastTalkMin = game::Hour() * 60 + game::Minute();
                json::Value actions = json::Value::Array();
                FinishTurn(ch, who, r, remoteFriend, &actions);
                TrimDialog(ch->dialog);
                if(remoteDone)
                {
                    json::Value rep = json::Value::Object();
                    rep["text"] = r.text; rep["actions"] = actions; rep["charId"] = ch->id;
                    rep["name"] = ch->nick.empty() ? ch->name : ch->nick; rep["pitch"] = ch->pitch; rep["rate"] = ch->rate;
                    remoteDone(rep);
                }
            });
        if(!id) { if(!remoteFriend) Notify("ИИ недоступен (нет связи или ключа)"); return false; }
        if(!remoteFriend) g_c.reqId = id;
        return true;
    }

    void FinishTurn(Character* c, int who, claude::Result& r, bool remoteFriend, json::Value* outActions)
    {
        json::Value results = json::Value::Array();
        for(auto& call : r.tools)
        {
            std::string res;
            if(!call.inputOk) res = "ошибка: параметры не разобраны";
            else
            {
                bool physical = false;
                res = ExecuteWorld(*c, who, call.name, call.input, physical);
                if(physical)
                {
                    if(remoteFriend)
                    {
                        json::Value a = json::Value::Object(); a["name"] = call.name; a["input"] = call.input; outActions->push(a);
                        res = "выполняется у собеседника";
                    }
                    else res = ExecutePhysical(c, world::PedOf(*c), who, call.name, call.input);
                }
            }
            json::Value tr = json::Value::Object();
            tr["type"] = "tool_result"; tr["tool_use_id"] = call.id; tr["content"] = res.empty() ? "ok" : res;
            results.push(tr);
        }
        if(results.size())
        {
            json::Value m = json::Value::Object(); m["role"] = "user"; m["content"] = results;
            c->dialog.push(m);
        }
    }

    // ---------- выбор собеседника ----------
    uintptr_t PickTarget()
    {
        uintptr_t pl = game::Player(); if(!pl) return 0;
        game::Vec me = game::Pos(pl);
        float h = game::Heading(pl);
        float fx = -sinf(h), fy = cosf(h);
        uintptr_t target = game::PlayerTarget();
        if(target && game::Alive(target) && !game::IsScriptPed(target) && game::Dist(game::Pos(target), me) < 15) return target;
        uintptr_t best = 0; float bestScore = 1e9f;
        game::ForEachPed([&](uintptr_t p) {
            if(p == pl || game::IsPlayer(p) || !game::Alive(p) || game::IsScriptPed(p)) return;   // педы миссий — не трогаем (сломается сценарий)
            game::Vec v = game::Pos(p);
            float d = game::Dist(v, me);
            if(d > 8.0f) return;
            float dx = (v.x - me.x) / (d + 0.01f), dy = (v.y - me.y) / (d + 0.01f);
            float facing = dx * fx + dy * fy;          // 1 — прямо перед CJ
            float score = d * (1.6f - facing);
            if(score < bestScore) { bestScore = score; best = p; }
        });
        return best;
    }

    // Кто рядом слышит разговор (до 4 человек в 10 м, пешком): свои и коллеги — первыми, потом остальные.
    // Раньше слышали только «свои» той же банды и только двое — в участке другие копы даже не поворачивались.
    // Вызывается при начале разговора и при каждом нажатии «Говорить»: кто отошёл — выбывает, кто подошёл — слышит.
    void ScanListeners(uintptr_t mainPed)
    {
        Character* mc = Current(); if(!mc || !mainPed) return;
        int mainId = mc->id, fac = mc->faction;
        game::Vec at = game::Pos(mainPed);
        // выбывшие
        for(size_t i = 0; i < g_c.group.size();)
        {
            Character* m = world::Find(g_c.group[i]); uintptr_t mp = m ? world::PedOf(*m) : 0;
            if(!mp || !game::Alive(mp) || game::Dist(game::Pos(mp), at) > 15.0f)
            {   // выбыл из разговора — наша поза на нём не остаётся
                if(mp && game::Alive(mp) && !game::Vehicle(mp)) { int t = game::ActiveTaskType(mp); if(t == 401 || t == 203 || t == 920 || t == 933) game::ClearTask(mp); }
                g_c.group.erase(g_c.group.begin() + i);
            }
            else ++i;
        }
        struct N { uintptr_t p; float score; };
        std::vector<N> near;
        game::ForEachPed([&](uintptr_t p) {
            if(p == mainPed || game::IsPlayer(p) || !game::Alive(p) || game::Vehicle(p) || game::IsScriptPed(p)) return;
            if(game::FightTarget(p)) return;                         // дерётся / целится (напал на CJ, вступился) — не «слушатель»
            float d = game::Dist(game::Pos(p), at); if(d > 10.0f) return;
            if(world::Character* ex = world::FindByPed(p)) for(int id : g_c.group) if(id == ex->id) return;
            int f = world::FactionOfPedType(game::PedType(p));
            near.push_back({ p, d - (f == fac ? 6.0f : 0.0f) });     // свои/коллеги — в первую очередь
        });
        std::sort(near.begin(), near.end(), [](const N& a, const N& b) { return a.score < b.score; });
        std::vector<std::string> used; if(Character* m0 = world::Find(mainId)) used.push_back(m0->voice);
        for(int id : g_c.group) if(Character* m = world::Find(id)) used.push_back(m->voice);
        for(auto& n : near)
        {
            if(g_c.group.size() >= 4) break;
            Character& m = world::Promote(n.p);
            if(m.id == mainId) continue;
            EnsureVoice(m, used); used.push_back(m.voice);
            m.rel[world::WHO_CJ].met = true;
            g_c.group.push_back(m.id);
            game::TaskTurnTo(n.p, game::Player());
            TRACE("РАЗГОВОР", "слышит: #%d %s (%s)", m.id, m.name.c_str(), game::PedTypeName(m.pedType));
        }
    }

    void StartWith(uintptr_t ped)
    {
        Character& c = world::Promote(ped);
        if(!c.aiProfile) life::RequestProfile(c);
        if(g_c.active && g_c.charId != c.id) EndConversation();
        if(!g_c.active || g_c.charId != c.id)
        {
            g_c = Conv(); g_c.active = true; g_c.charId = c.id; g_c.who = world::WHO_CJ; g_c.remote = coopnet::GetRole() == coopnet::CLIENT;
            if(!game::Vehicle(ped)) { game::TaskTurnTo(ped, game::Player()); }
            c.rel[world::WHO_CJ].met = true;
            EnsureVoice(c);
            int mainId = c.id;
            if(!g_c.remote) ScanListeners(ped);                       // может сдвинуть массив персонажей — c больше не трогаем
            Character* mc = world::Find(mainId);
            LOG_INFO("Разговор с #%d %s (%s), рядом слышат: %d чел.", mainId, mc ? mc->name.c_str() : "?", mc ? game::PedTypeName(mc->pedType) : "?", (int)g_c.group.size());
        }
        g_c.lastAt = Now();
    }
}

double Now() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

void Init() {}

bool Listening() { return g_listening; }
const std::string& Partial() { return g_partial; }
bool InConversation() { return g_c.active; }
bool Thinking() { return g_c.reqId != 0; }
bool PhoneMode() { return g_c.active && g_c.phone; }
Character* Current() { return g_c.active ? world::Find(g_c.charId) : nullptr; }
uintptr_t CurrentPed() { Character* c = Current(); return c ? world::PedOf(*c) : 0; }
const std::deque<Sub>& Subs() { return g_subs; }
int PendingDemand() { return g_c.active ? g_c.demand : 0; }

void Notify(const std::string& s) { g_notes.push_back(s); if(g_notes.size() > 8) g_notes.pop_front(); LOG_INFO("Уведомление: %s", s.c_str()); }
bool PopNotify(std::string& out) { if(g_notes.empty()) return false; out = g_notes.front(); g_notes.pop_front(); return true; }

void Say(const std::string& who, const std::string& text, bool player, int charId, float pitch, float rate, bool voice)
{
    // длинную речь (судья) показываем по частям, по предложениям, одна за другой — а не стеной текста на минуту
    std::vector<std::string> parts; std::string cur;
    for(size_t i = 0; i < text.size(); ++i)
    {
        cur += text[i];
        bool end = (text[i] == '.' || text[i] == '!' || text[i] == '?' || text[i] == '\n') && (i + 1 >= text.size() || text[i + 1] == ' ' || text[i + 1] == '\n');
        if((end && Cps(cur) >= 60) || Cps(cur) >= 160) { while(!cur.empty() && (cur[0] == ' ' || cur[0] == '\n')) cur.erase(0, 1); if(!cur.empty()) parts.push_back(cur); cur.clear(); }
    }
    while(!cur.empty() && (cur[0] == ' ' || cur[0] == '\n')) cur.erase(0, 1);
    if(!cur.empty()) parts.push_back(cur);
    double t = Now();
    for(auto& p : parts)
    {
        Sub s; s.who = who; s.text = p; s.player = player; s.charId = charId; s.from = t; s.until = t + SubDur(p);
        t = s.until;
        g_subs.push_back(s);
    }
    while(g_subs.size() > 12) g_subs.pop_front();
    (void)pitch; (void)rate;
    if(voice) SpeakAs(world::Find(charId), who, text);
}

void EnsureVoice(Character& c, const std::vector<std::string>& avoid)
{
    if(!c.story.empty())
    {   // сюжетный: голос общий для всех сохранений (если игрок назначил)
        world::StoryVoice sv;
        if(world::GetStoryVoice(c.story, sv)) { c.voice = sv.voice; c.vpitch = sv.pitch; c.vrate = sv.rate; }
    }
    speech::Voice v;
    if(!c.voice.empty() && speech::FindVoice(c.voice, v)) return;
    if(speech::Voices().empty()) return;
    unsigned seed = c.story.empty() ? (unsigned)c.id * 2654435761u + (unsigned)c.model * 40503u : HashStr(c.story);
    c.voice = speech::PickVoice(c.female, seed >> 7, avoid);
    c.vpitch = 0.93f + ((seed >> 3) % 15) / 100.0f;      // 0.93 … 1.07: из одного голоса — разные люди
    c.vrate  = 0.96f + ((seed >> 13) % 12) / 100.0f;     // 0.96 … 1.07
    if(c.age >= 50) { c.vpitch -= 0.04f; c.vrate -= 0.03f; }
    else if(c.age <= 22) c.vpitch += 0.03f;
    LOG_INFO("Голос: #%d %s — «%s», высота %.2f, скорость %.2f", c.id, c.name.c_str(), c.voice.c_str(), c.vpitch, c.vrate);
}

// После разговора снять «разговорные» задачи (стоять, жест, целиться, телефон, повернуться), чтобы пед
// вернулся к своим делам. Идти за CJ (936) оставляем — если согласился пойти. Педов в машинах не трогаем.
static void ReleaseTask(uintptr_t ped)
{
    if(!ped || game::Vehicle(ped) || game::IsScriptPed(ped)) return;
    int t = game::ActiveTaskType(ped);
    if(t == 203 || t == 401 || t == 920 || t == 933 || t == 1600) game::TaskWander(ped);
}

void EndConversation()
{
    if(!g_c.active) return;
    if(g_c.reqId) claude::Cancel(g_c.reqId);
    if(Character* c = Current())
    {
        uintptr_t ped = world::PedOf(*c);
        if(ped && !g_c.phone) ReleaseTask(ped);
    }
    for(int id : g_c.group)
        if(Character* m = world::Find(id)) { uintptr_t mp = world::PedOf(*m); if(mp) ReleaseTask(mp); }
    g_c = Conv();
}

void StartPhoneCall(int charId)
{
    Character* c = world::Find(charId);
    if(!c || !c->alive) { Notify("Номер не отвечает"); return; }
    if(c->jailUntilDay > world::Day()) { Notify(Fmt("%s сейчас в тюрьме", c->name.c_str())); return; }
    EndConversation();
    g_c.active = true; g_c.charId = charId; g_c.phone = true; g_c.remote = coopnet::GetRole() == coopnet::CLIENT;
    if(uintptr_t pl = game::Player()) if(!game::Vehicle(pl)) game::TaskPhone(pl, 4000);
    Notify(Fmt("Звонок: %s. Зажми «Говорить»", c->name.c_str()));
}

void SetSpeechSink(std::function<void(const std::string&)> fn) { g_sink = std::move(fn); }

static bool BeginListen()
{
    if(g_waitFinal || g_listening) { Notify("Секунду, распознаю прошлую фразу…"); return false; }
    StopVoice();
    g_partial.clear();
    if(settings::Get().engine == 0)
    {
        speech::Status st = speech::Get();
        if(!st.asrReady)
        {
            Notify(st.loading ? "Речь ещё загружается — подожди несколько секунд" : st.asrFiles ? "Распознавание не запустилось: Настройки > AI" : "Распознавание речи не установлено: Настройки > AI > «Скачать распознавание»");
            return false;
        }
        if(!speech::Listen()) { Notify("Микрофон недоступен — проверь разрешение (Настройки > AI)"); return false; }
    }
    else
    {
        if(!bridge::Ready()) { Notify("Мост Android не запустился — голос недоступен"); return false; }
        bridge::SttStart(settings::Get().lang.c_str());
    }
    g_listening = true; g_waitFinal = false;
    return true;
}

static void EndListen()
{
    if(!g_listening) return;
    g_listening = false; g_waitFinal = true; g_sttUntil = Now() + (settings::Get().engine == 0 ? 20 : 6);
    if(settings::Get().engine == 0) speech::StopListen(); else bridge::SttStop();
}

void CaptureDown(std::function<void(const std::string&)> sink)
{
    if(g_listening || g_waitFinal || g_captureActive) return;
    g_capture = std::move(sink); g_captureActive = true;
    if(!BeginListen()) { g_captureActive = false; g_capture = nullptr; }
}
void CaptureUp() { if(g_captureActive) EndListen(); }
bool Capturing() { return g_captureActive; }
bool Recognizing() { return g_waitFinal; }

void PttDown()
{
    TRACE("КНОПКА", "«Говорить» нажата");
    if(g_captureActive) return;
    if(g_waitFinal) { Notify("Секунду, распознаю прошлую фразу…"); return; }
    if(g_sink) { BeginListen(); return; }
    if(!g_c.phone)
    {
        // уже говорим с человеком рядом — продолжаем с ним (а не с прохожим, который оказался впереди)
        uintptr_t cur = CurrentPed(), aim = game::PlayerTarget();
        uintptr_t ped = (cur && game::Alive(cur) && game::Dist(game::Pos(cur), game::Pos(game::Player())) < 12 && (!aim || aim == cur)) ? cur : PickTarget();
        if(!ped) { Notify("Рядом никого — подойди к человеку ближе"); return; }
        StartWith(ped);
    }
    if(g_c.reqId) { claude::Cancel(g_c.reqId); g_c.reqId = 0; }
    if(Character* c = Current())
    {
        if(!g_c.phone && !g_c.remote) ScanListeners(world::PedOf(*c));
        c = Current();                                                  // массив персонажей мог сдвинуться
        if(c && !g_c.phone) ListenPose(c, world::PedOf(*c), 20000);    // слушает в своей позе (если не занят своим)
    }
    for(int id : g_c.group) if(Character* m = world::Find(id)) ListenPose(m, world::PedOf(*m), 12000);
    BeginListen();
}

void PttUp() { TRACE("КНОПКА", "«Говорить» отпущена"); if(!g_captureActive) EndListen(); }

static void SendToHost(Character& c, const std::string& text, bool phone)
{
    json::Value m = json::Value::Object();
    m["t"] = "talk"; m["text"] = text; m["situation"] = phone ? std::string("[Телефонный разговор]") : Situation(c, world::WHO_FRIEND, phone);
    m["pedType"] = c.pedType; m["model"] = c.model; m["phone"] = phone; m["hostCharId"] = c.hostId; m["localId"] = c.id;
    m["nick"] = world::W().playerNick;
    if(!coopnet::Send(m)) { Notify("Нет связи с хостом"); return; }
    if(g_c.active && g_c.charId == c.id) { g_c.reqId = -1; g_c.sentAt = Now(); }
}

static void OnPlayerSaid(const std::string& text)
{
    g_heardAt = Now();
    if(g_captureActive) { auto fn = std::move(g_capture); g_capture = nullptr; g_captureActive = false; if(fn) fn(text); return; }
    if(g_sink) { if(!text.empty()) { Say(world::W().playerNick, text, true, 0, 1, 1, false); g_sink(text); } return; }
    g_c.lastAt = Now();
    Character* c = Current();
    if(!c) return;
    if(text.empty()) { Notify("Не расслышал — попробуй ещё раз"); return; }
    Say(world::W().playerNick, text, true, 0, 1, 1, false);
    if(text.size() > 12)
        for(int id : g_c.group)
            if(Character* m = world::Find(id)) world::Remember(*m, Fmt("Слышал, как %s говорил %s: «%s»", world::W().playerNick.c_str(), c->name.c_str(), text.substr(0, 160).c_str()));
    c = Current(); if(!c) return;
    std::string line = Fmt("%s говорит: «%s»", world::W().playerNick.c_str(), text.c_str());
    if(g_c.offer) line += Fmt("\n(Он протягивает тебе $%d.)", g_c.offer);
    if(g_c.remote) { SendToHost(*c, line, g_c.phone); return; }
    SendTurn(*c, world::WHO_CJ, line, g_c.offer > 0, false);
}

void OfferMoney(int amount)
{
    Character* c = Current();
    if(!c || amount <= 0) return;
    if(game::Money() < amount) { Notify("У тебя нет столько денег"); return; }
    g_c.offer = amount;
    std::string line = Fmt("(%s молча протягивает тебе $%d.)", world::W().playerNick.c_str(), amount);
    Say(world::W().playerNick, Fmt("[протягивает $%d]", amount), true, 0, 1, 1, false);
    if(g_c.remote) { SendToHost(*c, line, g_c.phone); return; }
    SendTurn(*c, world::WHO_CJ, line, true, false);
}

void PayDemand()
{
    Character* c = Current();
    if(!c || g_c.demand <= 0) return;
    int amt = g_c.demand;
    if(game::Money() < amt) { Notify("Не хватает денег"); return; }
    game::AddMoney(-amt); c->money += amt;
    world::Relation& r = c->rel[world::WHO_CJ];
    if(r.payroll && g_c.demandWhy == "payroll") r.paidDay = world::Day();
    world::Remember(*c, Fmt("Получил от игрока $%d (%s)", amt, g_c.demandWhy.c_str()));
    world::ChangeRelation(*c, world::WHO_CJ, 4, 2, 3, 0, "заплатил");
    g_c.demand = 0;
    Say(world::W().playerNick, Fmt("[платит $%d]", amt), true, 0, 1, 1, false);
    std::string paid = Fmt("(%s заплатил тебе $%d, как ты просил.)", world::W().playerNick.c_str(), amt);
    if(g_c.remote) SendToHost(*c, paid, g_c.phone); else SendTurn(*c, world::WHO_CJ, paid, false, false);
}

bool React(Character& c, int who, const std::string& eventText, bool important)
{
    if(coopnet::GetRole() == coopnet::CLIENT) { SendToHost(c, "[Событие: " + eventText + "]", false); return true; }   // решает хост, исполняем у себя
    if(g_c.active && g_c.reqId) return false;
    if(g_c.active && g_c.charId != c.id) return false;
    if(!g_c.active && who == world::WHO_CJ)
    {
        uintptr_t ped = world::PedOf(c);
        if(!ped) return false;
        g_c = Conv(); g_c.active = true; g_c.charId = c.id; g_c.who = who;
    }
    return SendTurn(c, who, "[Событие: " + eventText + "]", important, who != world::WHO_CJ);
}

// ---------- физические действия ----------
std::string ExecutePhysical(Character* c, uintptr_t ped, int who, const std::string& n, const json::Value& in)
{
    uintptr_t pl = game::Player();
    bool phone = g_c.active && g_c.phone && c && g_c.charId == c->id;
    auto needPed = [&]() { return ped && !phone && game::Alive(ped) && !game::Vehicle(ped) && !game::IsScriptPed(ped); };   // водителю нельзя: сломаем езду

    if(n == "end_conversation") { g_c.endAfterSpeech = true; return "ок"; }
    if(n == "gesture")
    {
        if(!needPed()) return "ты не рядом";
        if(crew::Defending(ped)) return "ты сейчас в драке за него — не до жестов";
        struct G { const char* k; const char* block; const char* anim; const char* fbBlock; const char* fbAnim; bool loop; int ms; };
        static const G kG[] = {
            { "talk", "ped", "IDLE_chat", nullptr, nullptr, false, 0 }, { "agree", "COP_AMBIENT", "Coplook_nod", "ped", "IDLE_chat", false, 0 },
            { "disagree", "MISC", "plyr_shkhead", "ped", "endchat_02", false, 0 }, { "angry", "RIOT", "RIOT_ANGRY", "ped", "IDLE_chat", false, 0 },
            { "threaten", "RIOT", "RIOT_challenge", "ped", "fucku", false, 0 }, { "scared", "ped", "handscower", nullptr, nullptr, true, 3000 },
            { "laugh", "ped", "endchat_01", nullptr, nullptr, false, 0 }, { "wave", "ON_LOOKERS", "wave_loop", "ped", "endchat_03", true, 2500 },
            { "shrug", "ped", "XPRESSscratch", nullptr, nullptr, false, 0 }, { "point", "ON_LOOKERS", "point_loop", "ped", "IDLE_chat", true, 2500 },
            { "cross_arms", "COP_AMBIENT", "Coplook_loop", "ped", "IDLE_stance", true, 5000 }, { "fuck_you", "ped", "fucku", nullptr, nullptr, false, 0 },
            { "hands_up", "ped", "handsup", nullptr, nullptr, false, 0 }, { "cower", "ped", "cower", nullptr, nullptr, true, 4000 },
            { "think", "COP_AMBIENT", "Coplook_think", "ped", "IDLE_tired", false, 0 }, { "celebrate", "ON_LOOKERS", "shout_01", "ped", "IDLE_chat", false, 0 },
            { "smoke", "ped", "IDLE_stance", nullptr, nullptr, false, 0 }, { "sit", "ped", "IDLE_tired", nullptr, nullptr, false, 0 },   // prtial_* — анимации только для верха тела (ноги «ломались»); сидеть на улице не на чем
            { "dance", "DANCING", "dance_loop", "ped", "IDLE_chat", true, 8000 },
        };
        const std::string& k = in["kind"].str();
        for(auto& g : kG)
            if(k == g.k)
            {
                if(game::TaskAnim(ped, g.anim, g.block, g.loop, g.ms, k == "hands_up")) return "ок";
                if(g.fbAnim && game::TaskAnim(ped, g.fbAnim, g.fbBlock, g.loop, g.ms)) return "ок";
                return "не вышло";
            }
        return "нет такого жеста";
    }
    if(n == "move" && ped && !phone && game::Alive(ped) && game::Vehicle(ped) && !game::IsScriptPed(ped))
    {   // пед в машине: выйти (и, если звали, потом пойти за CJ); уйти/убежать — просто уехать (машина продолжит ехать)
        const std::string& m = in["mode"].str();
        if(m == "get_out" || m == "follow_player" || m == "come_closer")
        {
            game::TaskLeaveCar(ped);
            if(m != "get_out")
            {
                int ref = game::Ref(ped);
                Later(2.5, [ref, pl]() { uintptr_t p = game::FromRef(ref); if(p && game::Alive(p) && !game::Vehicle(p)) game::TaskFollow(p, pl); });
            }
            return m == "get_out" ? "выходишь из машины" : "выходишь из машины и идёшь за ним";
        }
        if(m == "go_away" || m == "flee") { if(!g_memberAct) g_c.endAfterSpeech = true; return "уезжаешь"; }
        return "ты в машине — сначала выйди (get_out)";
    }
    if(n == "move")
    {
        if(!needPed()) return "ты не рядом";
        if(crew::Defending(ped) && in["mode"].str() != "flee" && in["mode"].str() != "go_away") return "ты сейчас в драке за него";
        const std::string& m = in["mode"].str(); bool run = in["run"].b();
        int ms = run ? 6 : 4;
        if(m == "get_out") return "ты и так не в машине";
        if(m == "follow_player") return game::TaskFollow(ped, pl) ? "идёшь за ним" : "не вышло";
        if(m == "come_closer") return game::TaskGoTo(ped, game::Pos(pl), ms, 1.2f) ? "подходишь" : "не вышло";
        if(m == "flee") { if(c) world::ChangeRelation(*c, who, 0, 0, 0, 5, ""); if(!g_memberAct) EndConversation(); return game::TaskFlee(ped, pl) ? "убегаешь" : "не вышло"; }
        if(m == "wait_here") return game::TaskStand(ped, 60000) ? "ждёшь" : "не вышло";
        if(m == "go_home" && c) { game::Vec h = c->home; if(!g_memberAct) g_c.endAfterSpeech = true; return game::TaskGoTo(ped, h, ms, 2.0f) ? "идёшь домой" : "не вышло"; }
        if(m == "go_away" || m == "wander") { if(!g_memberAct) g_c.endAfterSpeech = true; return game::TaskWander(ped) ? "уходишь" : "не вышло"; }
        return "?";
    }
    static const struct { const char* n; int w; int ammo; } kW[] = { { "fists", 0, 0 }, { "knife", 4, 1 }, { "bat", 5, 1 }, { "pistol", 22, 60 },
        { "uzi", 28, 200 }, { "shotgun", 25, 30 }, { "ak47", 30, 150 } };
    auto weaponOf = [&](const std::string& s) { for(auto& w : kW) if(s == w.n) return w; return kW[0]; };
    // Цель: him — сам собеседник; his_attacker — тот, кто сейчас нападает на собеседника (вступиться за него)
    auto attackerOf = [&]() -> uintptr_t {
        std::vector<uintptr_t> att = crew::AttackersOf(pl);
        for(uintptr_t a : att) if(a != ped) return a;
        return 0;
    };
    // Свой из банды на CJ за слова оружие не поднимает (тест 0.2.10: обиделся на «индюка» — и стрелять).
    // Оружие против CJ — только если CJ сам недавно ранил своих оружием (кулак — кулаком).
    auto ownVsCJ = [&]() { return c && c->faction == world::W().playerGang && c->faction != world::F_CIVIL && !crew::PlayerArmedVsOwn(ped); };
    if(n == "attack")
    {
        if(!needPed()) return "ты не рядом";
        auto w = weaponOf(in["weapon"].str());
        if(in["target"].str() == "his_attacker")
        {
            uintptr_t a = attackerOf();
            if(!a) return "на него сейчас никто не нападает";
            if(crew::Defending(ped) && game::FightTarget(ped) == a) return "ты уже дерёшься с ним";
            if(w.w) { game::GiveWeapon(ped, w.w, w.ammo); game::SetCurrentWeapon(ped, w.w); }
            if(!crew::Defend(ped, a, w.w, "ИИ: вступился за собеседника")) return "не вышло";
            if(c) { world::Remember(*c, "Вступился за " + std::string(WhoName(who)) + ", когда на него напали"); world::ChangeRelation(*c, who, 3, 2, 3, 0, ""); }
            return "бросился на того, кто на него напал";
        }
        if(ownVsCJ() && w.w)
        {
            TRACE("СВОИ", "#%d хотел напасть на CJ с оружием %d — свой за слова только кулаками", c->id, w.w);
            w = kW[0];
        }
        if(w.w) { game::GiveWeapon(ped, w.w, w.ammo); game::SetCurrentWeapon(ped, w.w); }
        else game::SetCurrentWeapon(ped, 0);
        if(c) { world::Remember(*c, "Я напал на " + std::string(WhoName(who))); world::ChangeRelation(*c, who, -10, 0, -10, 0, ""); }
        if(!g_memberAct) EndConversation();
        return game::TaskAttack(ped, pl) ? (w.w ? "напал" : "полез драться на кулаках") : "не вышло";
    }
    if(n == "aim_weapon")
    {
        if(!needPed()) return "ты не рядом";
        auto w = weaponOf(in["weapon"].str());
        uintptr_t target = pl;
        if(in["target"].str() == "his_attacker") { target = attackerOf(); if(!target) return "на него сейчас никто не нападает"; }
        else if(ownVsCJ())
        {   // на брата ствол не наводят — злость показывают телом
            TRACE("СВОИ", "#%d хотел навести ствол на CJ — свой: только жест", c->id);
            game::TaskAnim(ped, "RIOT_challenge", "RIOT", false, 0);
            return "на своего ствол не наводишь — показал, что злой";
        }
        if(w.w) { game::GiveWeapon(ped, w.w, w.ammo); game::SetCurrentWeapon(ped, w.w); }
        return game::TaskAimAt(ped, target, 8000) ? (target == pl ? "держишь на прицеле" : "держишь на прицеле того, кто на него напал") : "не вышло";
    }
    if(n == "holster") { if(needPed()) { game::SetCurrentWeapon(ped, 0); game::TaskStand(ped, 4000); } return "ок"; }
    if(n == "give_money")
    {
        if(!c) return "?";
        int a = world::Clamp(in["amount"].i(), 0, c->money);
        if(a <= 0) return "у тебя нет денег";
        if(!needPed() && !phone) return "ты не рядом";
        if(phone) return "по телефону деньги не передашь — договорись о встрече";
        c->money -= a; game::AddMoney(a);
        Say(c->nick.empty() ? c->name : c->nick, Fmt("[отдаёт $%d]", a), false, c->id, 1, 1, false);
        return Fmt("отдал $%d", a);
    }
    if(n == "answer_offer")
    {
        if(!c) return "?";
        int a = g_c.offer; g_c.offer = 0;
        if(a <= 0) return "тебе ничего не предлагали";
        if(in["accept"].b())
        {
            if(game::Money() < a) return "у него уже нет этих денег";
            game::AddMoney(-a); c->money += a;
            world::Relation& r = c->rel[who];
            if(r.payroll) r.paidDay = world::Day();
            // свидетели взятки
            if(c->faction == world::F_POLICE)
            {
                world::Event& e = world::AddEvent("bribe", Fmt("дал взятку $%d копу %s", a, c->name.c_str()), game::Pos(pl), 2, who);
                e.victimChar = c->id;
                if(ped) game::ForEachPed([&](uintptr_t p) {
                    if(p == ped || game::IsPlayer(p) || !game::Alive(p) || game::Dist(game::Pos(p), game::Pos(ped)) > 15) return;
                    if(world::Character* wc = world::FindByPed(p)) world::AddWitness(e, wc->id); else e.anonWitnesses++;
                });
                if(e.anonWitnesses + (int)e.witnesses.size() > 0 && world::Rand(0, 100) < 20) world::W().pcase[who].iaInvestigation = true;
            }
            world::Remember(*c, Fmt("Взял у %s $%d (%s)", WhoName(who), a, in["reason"].str().c_str()));
            Say(c->nick.empty() ? c->name : c->nick, Fmt("[берёт $%d]", a), false, c->id, 1, 1, false);
            return Fmt("взял $%d", a);
        }
        world::Remember(*c, Fmt("Отказался от $%d от %s (%s)", a, WhoName(who), in["reason"].str().c_str()));
        return "отказался, деньги остались у игрока";
    }
    if(n == "demand_money") { g_c.demand = world::Clamp(in["amount"].i(), 1, 10000000); g_c.demandWhy = in["reason"].str(); return "игрок видит требование и решает сам"; }
    if(n == "call")
    {
        std::string who2 = in["who"].str();
        if(!needPed() && !phone) return "ты не можешь позвонить";
        bool radio = c && c->faction == world::F_POLICE;            // коп — по рации: быстрее
        if(needPed()) { if(radio) game::TaskAnim(ped, "phone_talk", "ped", true, 3500); else game::TaskPhone(ped, 6000); }
        int cid = c ? c->id : 0;
        float hp0 = ped ? game::Health(ped) : 0;
        if(who2 == "police")
        {
            TRACE("ЗВОНОК", "#%d %s звонит в полицию (%s)", cid, c ? c->name.c_str() : "?", radio ? "рация" : "телефон");
            Later(radio ? 3.5 : 6.5, [cid, who, hp0, radio]() {
                world::Character* ch = world::Find(cid);
                uintptr_t p = ch ? world::PedOf(*ch) : 0;
                if(ch && p && !game::Alive(p)) { TRACE("ЗВОНОК", "#%d не дозвонился: погиб", cid); return; }
                if(ch && p && game::Health(p) < hp0 - 1.0f)
                {   // ударили / ранили во время звонка — звонок сорвался
                    TRACE("ЗВОНОК", "#%d не дозвонился: его ударили во время звонка", cid);
                    world::Remember(*ch, std::string("Пытался вызвать полицию, но ") + world::W().playerNick + " не дал — ударил");
                    Notify(radio ? "Коп не успел договорить по рации" : "Он не успел дозвониться — ты помешал");
                    return;
                }
                if(who == world::WHO_CJ) { int w = game::Wanted(); game::SetWanted(w < 2 ? w + 1 : w); }
                world::W().pcase[who].heat = world::Clamp(world::W().pcase[who].heat + 10, 0, 100);
                Notify(radio ? "Коп вызвал подмогу по рации" : "Кто-то сообщил о тебе в полицию");
                TRACE("ЗВОНОК", "#%d дозвонился: розыск %d", cid, game::Wanted());
            });
            return radio ? "вызываешь подмогу по рации (3 секунды; если тебя ударят — не успеешь)" : "звонишь в полицию (6 секунд; если тебя ударят — не дозвонишься)";
        }
        if(who2 == "gang")
        {
            int fac = c ? c->faction : world::F_NONE;
            bool hostile = c && (c->rel[who].liking < 0 || world::W().fac[fac].hostility[who] > 40);
            Later(9.0, [fac, hostile]() { life::SpawnHelpers(fac, world::Rand(2, 3), hostile); });
            return "свои едут (около 10 секунд)";
        }
        if(who2 == "boss" && c) { auto& f = world::W().fac[c->faction]; f.hostility[who] = world::Clamp(f.hostility[who] + 5, 0, 100); return "доложил"; }
        return "позвонил";
    }
    if(n == "police_action")
    {
        if(!c || c->faction != world::F_POLICE) return "ты не полицейский";
        std::string a = in["action"].str(); int amt = in["amount"].i();
        if(a == "order_stop") { Notify("Полиция: «Стоять! Руки!» — кнопка «Сдаться» на экране"); if(needPed()) game::TaskAimAt(ped, pl, 6000); return "приказал"; }
        if(a == "arrest") { if(!needPed()) return "ты не рядом"; police::StartArrest(ped); return "задерживаешь"; }
        if(a == "let_go") { police::Release("коп отпустил"); if(who == world::WHO_CJ && c->rank >= 1) game::SetWanted(0); return "отпустил"; }
        if(a == "fine") { g_c.demand = world::Clamp(amt, 10, 100000); g_c.demandWhy = "штраф"; return "выписал штраф"; }
        if(a == "reduce_wanted" || a == "clear_wanted")
        {
            int maxDrop = c->rank + 1;            // патрульный — 1 звезда, капитан — 4
            int w = game::Wanted();
            int drop = a == "clear_wanted" ? w : world::Clamp(amt, 1, 6);
            if(drop > maxDrop) drop = maxDrop;
            game::SetWanted(w - drop);
            c->rel[who].favors++;
            return Fmt("розыск снижен на %d", drop);
        }
        if(a == "return_weapons") { if(c->rank < 1) return "у патрульного нет доступа к складу улик"; if(phone) { world::News("Оружие ждёт тебя у участка — забери при встрече"); return "договорился"; } game::GiveWeapon(pl, 22, 68); return "вернул пистолет"; }
        return "?";
    }
    return "неизвестное действие";
}

// ---------- кооп ----------
void HandleFriendTalk(const json::Value& req)
{
    if(!settings::Get().shareKeyWithFriend || !claude::HasKey())
    {
        json::Value e = json::Value::Object(); e["t"] = "reply"; e["localId"] = req["localId"].i();
        e["error"] = claude::HasKey() ? "Хост выключил разговоры через свой ключ" : "У хоста нет ключа ИИ";
        coopnet::Send(e); return;
    }
    auto& W = world::W();
    if(req["nick"].isString()) W.friendNick = req["nick"].str();
    Character* c = nullptr;
    int id = req["hostCharId"].i();
    if(id) c = world::Find(id);
    if(!c)
    {
        // персонаж с телефона друга: создаём у себя без педа
        world::Character nc;
        nc.id = W.nextId++; nc.pedType = req["pedType"].i(4); nc.model = req["model"].i(); nc.female = game::IsFemaleType(nc.pedType);
        nc.faction = world::FactionOfPedType(nc.pedType); nc.age = world::Rand(18, 60);
        nc.name = Fmt("%s №%d", game::PedTypeName(nc.pedType), nc.id); nc.dialog = json::Value::Array();
        nc.pitch = nc.female ? 1.15f : 0.85f; nc.rate = 1.0f;
        W.chars.push_back(nc);
        c = &W.chars.back();
        life::RequestProfile(*c);
    }
    std::string text = req["situation"].str() + "\n" + req["text"].str();
    int cid = c->id;
    int localId = req["localId"].i();
    SendTurn(*c, world::WHO_FRIEND, text, false, true, [cid, localId](json::Value rep) {
        rep["t"] = "reply"; rep["charId"] = cid; rep["localId"] = localId;
        coopnet::Send(rep);
    });
}

void HandleHostReply(const json::Value& rep)
{
    Character* c = world::Find(rep["localId"].i());
    if(g_c.active && c && g_c.charId == c->id) g_c.reqId = 0;
    if(rep.has("error")) { Notify("ИИ (через хоста): " + rep["error"].str()); return; }
    if(!c) return;
    c->hostId = rep["charId"].i();
    std::string name = rep["name"].str();
    if(!name.empty()) c->nick = name;
    c->pitch = (float)rep["pitch"].num(c->pitch); c->rate = (float)rep["rate"].num(c->rate);
    std::string text = rep["text"].str();
    if(!text.empty()) Say(name.empty() ? c->name : name, text, false, c->id, c->pitch, c->rate, true);
    uintptr_t ped = world::PedOf(*c);
    for(auto& a : rep["actions"].arr()) ExecutePhysical(c, ped, world::WHO_CJ, a["name"].str(), a["input"]);
    if(g_c.active && g_c.charId == c->id) g_c.lastAt = Now();
}

// ---------- кадр ----------
// Удар во время разговора: оборвать ответ и голос, снять нашу позу (чтобы сработала настоящая реакция игры —
// дать сдачи / убежать), запомнить, а через 0,8 с ИИ решает, что дальше. Раньше человек «договаривал» и стоял.
static std::map<int, float> g_hpSeen; static std::map<int, double> g_hitCool; static int g_hpConv = 0;
static void HitFrame()
{
    if(!g_c.active || g_c.phone) { g_hpSeen.clear(); g_hpConv = 0; return; }
    if(g_hpConv != g_c.charId) { g_hpSeen.clear(); g_hitCool.clear(); g_hpConv = g_c.charId; }
    uintptr_t pl = game::Player(), p2 = game::Player2();
    std::vector<int> ids{ g_c.charId }; for(int id : g_c.group) ids.push_back(id);
    for(int id : ids)
    {
        Character* m = world::Find(id); uintptr_t p = m ? world::PedOf(*m) : 0;
        if(!p) continue;
        // Пед занят ответом игры (драка, бегство, падение) — наша поза снимается сразу: две анимации вперемешку
        // давали кривые позы (тест 0.2.10, скриншот) и вылет в FrameUpdateCallBackWithVelocityExtractionSkinned.
        {
            int at = game::ActiveTaskType(p), st = game::SimplestTaskType(p);
            bool busy = (st >= 1000 && st < 1100)                 // бьют / дерётся / стреляет
                     || (st >= 700 && st < 800)                    // садится в машину / едет
                     || st == 205 || st == 206                     // падает / встаёт
                     || ((at == 401 || at == 203) && st == 900);   // наша поза «стоять», а он куда-то идёт (убегает)
            if((at == 401 || at == 203 || at == 920) && busy)
            { TRACE("РАЗГОВОР", "#%d занят своим (тело %d) — снимаю нашу позу %d", id, st, at); game::ClearTask(p); }
        }
        float hp = game::Health(p);
        auto it = g_hpSeen.find(id);
        if(it != g_hpSeen.end() && hp < it->second - 0.5f)
        {
            int at = game::ActiveTaskType(p);
            if(at == 401 || at == 203 || at == 920) game::ClearTask(p);         // ранили кто угодно — поза долой
            int wpn = 0; uint32_t tm = 0; uintptr_t by = game::LastDamager(p, wpn, tm);
            uint32_t now = game::TimeMs();
            bool fresh = tm && now >= tm && now - tm < 600;                    // удар именно сейчас, а не старый «последний обидчик»
            bool byCJ = fresh && by && (by == pl || (p2 && by == p2) || (game::Vehicle(pl) && by == game::Vehicle(pl)));
            double tnow = Now();
            if(byCJ && tnow >= g_hitCool[id])                                   // одна реакция на серию ударов (раньше — сотни за секунду)
            {
                g_hitCool[id] = tnow + 4.0;
                bool main = id == g_c.charId;
                TRACE("РАЗГОВОР", "удар по #%d %s (%s) во время разговора: %.0f → %.0f", id, m->name.c_str(), main ? "собеседник" : "слушатель", it->second, hp);
                world::Remember(*m, world::W().playerNick + (wpn >= 22 ? " ранил меня" : " ударил меня") + " посреди разговора");
                world::ChangeRelation(*m, g_c.who, -10, -2, -15, 12, "ударил посреди разговора");
                if(g_c.reqId > 0) { claude::Cancel(g_c.reqId); g_c.reqId = 0; }
                StopVoice();
                int cid = g_c.charId, who = g_c.who; std::string victim = main ? std::string("тебя") : m->name;
                std::string ev = world::W().playerNick + (wpn >= 22 ? " ранил " : " ударил ") + victim + " (здоровье " + std::to_string((int)hp) + "). Реагируй делом сразу: дать сдачи, убежать, позвать своих или копов";
                Later(0.8, [cid, who, ev]() { if(Character* cc = world::Find(cid)) if(g_c.active && g_c.charId == cid) React(*cc, who, ev, true); });
            }
        }
        g_hpSeen[id] = hp;
    }
}

// На CJ напал кто-то другой посреди разговора: собеседник не «договаривает», а реагирует делом.
// Свой из Grove к этому моменту уже вступился сам (crew), ИИ добавляет слова; чужой решает: убежать, звонить, стоять в стороне.
static double g_attackedCool = 0;
void OnPlayerAttacked(uintptr_t attacker, const std::string& who, int weapon)
{
    if(!g_c.active || g_c.phone || g_c.who != world::WHO_CJ) return;
    Character* c = Current(); uintptr_t ped = CurrentPed();
    if(!c || !ped || attacker == ped || !game::Alive(ped)) return;      // бьёт сам собеседник — это его ход, а не событие
    if(game::Dist(game::Pos(ped), game::Pos(game::Player())) > 25) return;
    double now = Now(); if(now < g_attackedCool) return;
    g_attackedCool = now + 8;
    if(g_c.reqId > 0) { claude::Cancel(g_c.reqId); g_c.reqId = 0; }
    StopVoice();
    std::string ev = "На " + world::W().playerNick + " прямо сейчас напал " + who + (weapon >= 22 ? " со стволом" : weapon > 0 ? " с оружием" : " с кулаками") +
                     ". Реагируй делом сразу: свой — вступись (attack target his_attacker), чужой — как сам решишь (убежать, звонить копам, стоять в стороне)";
    TRACE("РАЗГОВОР", "на CJ напал %s посреди разговора с #%d — событие собеседнику", who.c_str(), c->id);
    int cid = c->id;
    Later(0.3, [cid, ev]() { if(Character* cc = world::Find(cid)) if(g_c.active && g_c.charId == cid) React(*cc, world::WHO_CJ, ev, true); });
}

// Прогрев кэша правил педов: первый ответ нового разговора без него шёл 7 с (запись 6–7 тыс. токенов в кэш),
// с ним — как остальные (1–2 с). Раз в 4 минуты, если разговоров не было; цена — около цента в час.
static void WarmFrame(double now)
{
    if(now < g_nextWarm) return;
    g_nextWarm = now + 30;                                       // повторная проверка через 30 с
    if(!claude::HasKey() || claude::LimitReached() || game::GamePaused() || !game::Player()) return;
    if(coopnet::GetRole() == coopnet::CLIENT) return;            // у друга разговоры идут через хоста
    if(now - g_lastTalkReq < 240) return;                        // недавно говорили — кэш и так тёплый
    claude::Request req;
    json::Value rules = json::Value::Object(); rules["type"] = "text"; rules["text"] = WorldRules();
    json::Value cc = json::Value::Object(); cc["type"] = "ephemeral"; rules["cache_control"] = cc;
    req.system = json::Value::Array(); req.system.push(rules);
    req.tools = Tools();
    json::Value m = json::Value::Object(); m["role"] = "user"; m["content"] = "warmup";
    req.messages = json::Value::Array(); req.messages.push(m);
    req.effort = "low"; req.noThinking = false; req.cache = false; req.warmOnly = true; req.tag = "прогрев кэша";
    if(claude::Send(req, nullptr, nullptr)) { g_lastTalkReq = now; g_nextWarm = now + 240; }
}

// Игрок навёл ствол на собеседника: пед замечает сразу (рефлекс без ИИ), а ИИ потом решает, что сказать
static double g_aimCheck = 0; static bool g_aimed = false;
static void AimFrame(double now)
{
    if(now < g_aimCheck) return;
    g_aimCheck = now + 0.2;
    bool aimed = false;
    uintptr_t ped = (g_c.active && !g_c.phone) ? CurrentPed() : 0, pl = game::Player();
    int wt = 0, wa = 0; if(pl) game::CurWeapon(pl, wt, wa);
    bool gun = wt >= 22 && wt <= 38;                       // пистолеты…миниган; кулаки/бита/нож — это драка, а не «ствол»
    if(gun && ped && pl && script::Ready() && game::Alive(ped) && !game::Vehicle(ped) && !game::IsScriptPed(ped))
    {
        std::vector<script::Arg> a{ script::Arg::I(0), script::Arg::I(game::Ref(ped)) };
        bool f = false;
        if(script::Run(0x0457, a, &f)) aimed = f;          // IS_PLAYER_TARGETTING_CHAR
    }
    if(aimed && !g_aimed)
    {
        Character* c = Current();
        int courage = c ? c->tr.courage : 50;
        bool gang = game::IsGang(game::PedType(ped)) || game::PedType(ped) == game::PT_COP;
        bool own = c && c->faction == world::W().playerGang && c->rel[world::WHO_CJ].liking > -30;
        const char* what;
        if(own)                   { game::TaskAnim(ped, "handsup", "ped", false, 2500, true); what = "свой: поднял руки — «эй, это я!»"; }
        else if(gang && courage >= 55) { game::GiveWeapon(ped, 22, 30); game::SetCurrentWeapon(ped, 22); game::TaskAimAt(ped, pl, 8000); what = "навёл ствол в ответ"; }
        else if(courage < 35)     { game::TaskFlee(ped, pl, 60.0f, 20000); what = "бросился бежать"; }
        else                      { game::TaskAnim(ped, "handsup", "ped", false, 6000, true); what = "поднял руки"; }
        LOG_INFO("Ствол на #%d: %s", c ? c->id : 0, what);
        if(c)
        {
            if(g_c.reqId) { claude::Cancel(g_c.reqId); g_c.reqId = 0; }
            world::ChangeRelation(*c, g_c.who, 0, 0, 0, 8, "");
            React(*c, g_c.who, std::string("CJ навёл на тебя оружие. Ты уже ") + what + ". Скажи коротко, как реагируешь", true);
        }
    }
    g_aimed = aimed;
}

void Frame()
{
    // распознавание
    bridge::SttEvent se;
    while(bridge::PollStt(se))
    {
        if(se.kind == bridge::STT_PARTIAL) g_partial = se.text;
        else if(se.kind == bridge::STT_FINAL) { g_partial = se.text; if(g_waitFinal || g_listening) { g_waitFinal = false; g_listening = false; OnPlayerSaid(se.text); } }
        else if(se.kind == bridge::STT_ERROR)
        {
            g_listening = false; g_waitFinal = false;
            if(g_captureActive) { auto fn = std::move(g_capture); g_capture = nullptr; g_captureActive = false; if(fn) fn(""); }
            int code = atoi(se.text.c_str());
            if(code == 7 || code == 6) Notify("Не расслышал — скажи ещё раз");
            else if(code == 9) Notify("Нет разрешения на микрофон");
            else Notify("Распознавание речи: ошибка " + se.text);
        }
    }
    speech::Heard hd;
    while(speech::PollHeard(hd))
    {
        if(!g_waitFinal && !g_listening) continue;                 // уже не ждём (отменили)
        g_waitFinal = false; g_listening = false;
        if(!hd.ok) { Notify("Распознавание: " + hd.error); if(g_captureActive) { auto fn = std::move(g_capture); g_capture = nullptr; g_captureActive = false; if(fn) fn(""); } continue; }
        g_partial = hd.text;
        OnPlayerSaid(hd.text);
    }
    if(g_waitFinal && Now() > g_sttUntil)
    {
        g_waitFinal = false;
        if(g_captureActive && g_partial.empty()) { auto fn = std::move(g_capture); g_capture = nullptr; g_captureActive = false; if(fn) fn(""); }
        else if(!g_partial.empty()) OnPlayerSaid(g_partial);
    }

    AimFrame(Now());
    HitFrame();
    WarmFrame(Now());
    if(g_c.active && g_c.reqId == -1 && Now() - g_c.sentAt > 30) { g_c.reqId = 0; Notify("Хост не ответил"); }

    // голос: отзвучавшие фразы (конец речи → можно завершить разговор)
    std::string spokenId;
    while(speech::PollSpoken(spokenId)) {}

    // отложенные действия
    double now = Now();
    for(size_t i = 0; i < g_later.size();)
    {
        if(now >= g_later[i].at) { auto fn = std::move(g_later[i].fn); g_later.erase(g_later.begin() + i); fn(); }
        else ++i;
    }

    // разговор: собеседник ушёл далеко / умер — конец
    if(g_c.active && !g_c.phone)
    {
        Character* c = Current();
        uintptr_t ped = c ? world::PedOf(*c) : 0;
        uintptr_t pl = game::Player();
        bool far = !ped || !pl || game::Dist(game::Pos(ped), game::Pos(pl)) > 25;
        if(far && !g_c.reqId) EndConversation();
        else if(!g_c.reqId && !g_listening && now - g_c.lastAt > 45) EndConversation();
    }
    if(g_c.active && g_c.endAfterSpeech && !g_c.reqId && !speech::Speaking()) { g_c.endAfterSpeech = false; if(g_c.phone) Notify("Разговор окончен"); EndConversation(); }

    while(!g_subs.empty() && g_subs.front().until < now) g_subs.pop_front();
}

} // namespace talk
