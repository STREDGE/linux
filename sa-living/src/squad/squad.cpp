#include "squad/squad.h"

#include <mod/amlmod.h>
#include <mod/interface.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <algorithm>

#include <swat/sgame.h>
#include <swat/swat_api.h>
#include <swat/tactics.h>

#include "ai/claude.h"
#include "game/game.h"
#include "settings.h"
#include "talk/talk.h"
#include "trace/trace.h"
#include "util/json.h"
#include "util/log.h"
#include "world/world.h"

namespace squad
{
namespace
{
    SAStorySwatApi* g_api = nullptr;
    swat::Team* g_team = nullptr;
    uint32_t g_apiTry = 0;

    struct Member
    {
        std::string name;
        int staffIdx = -1, model = 280, role = swat::R_NONE, charId = 0;
        int ref = 0;
        bool dead = false, traveling = false, leaving = false;
        uint32_t deadAt = 0, arriveAt = 0, spawnTry = 0;
        std::string character;                 // характер (для головы)
        int weapons[13] = {}, ammo[13] = {};
    };
    std::vector<Member> g_m;
    bool g_loadPending = false; std::string g_loadKey;

    // голова
    int g_req = 0; uint32_t g_lastAsk = 0; bool g_askPending = false; std::string g_pendingWhy, g_pendingOrder;
    json::Value g_history = json::Value::Array();
    std::string g_lastOrderText;

    uint32_t Now() { return swat::NowMs(); }
    bool Ago(uint32_t t, uint32_t ms) { uint32_t n = Now(); return n < t || n - t >= ms; }
    std::string Lower(const std::string& s)
    {   // кириллица в UTF-8: А–Я → а–я (2 байта), латиница — tolower
        std::string r; r.reserve(s.size());
        for(size_t i = 0; i < s.size(); ++i)
        {
            unsigned char c = (unsigned char)s[i];
            if(c == 0xD0 && i + 1 < s.size())
            {
                unsigned char d = (unsigned char)s[i + 1];
                if(d >= 0x90 && d <= 0x9F) { r += (char)0xD0; r += (char)(d + 0x20); i++; continue; }
                if(d >= 0xA0 && d <= 0xAF) { r += (char)0xD1; r += (char)(d - 0x20); i++; continue; }
                if(d == 0x81) { r += (char)0xD1; r += (char)0x91; i++; continue; }   // Ё → ё
            }
            r += (char)tolower(c);
        }
        return r;
    }
    bool Has(const std::string& low, const char* w) { return low.find(w) != std::string::npos; }

    bool Api()
    {
        if(g_api) return true;
        if(!Ago(g_apiTry, 3000)) return false;
        g_apiTry = Now();
        g_api = (SAStorySwatApi*)GetInterface(SASTORY_SWAT_INTERFACE);
        if(g_api) LOG_INFO("Отряд: SA Story найден (режим S.W.A.T.)");
        return g_api != nullptr;
    }
    swat::V3 Place(int id)
    {
        SwatPlace p = {};
        if(g_api && g_api->GetPlace(id, &p)) return swat::Mk(p.x, p.y, p.z);
        return swat::V3();
    }

    // ---------------- речь ----------------
    world::Character* CharOf(Member& m)
    {
        if(m.charId) if(world::Character* c = world::Find(m.charId)) return c;
        uintptr_t p = m.ref ? game::FromRef(m.ref) : 0;
        if(!p) return nullptr;
        world::Character& c = world::Promote(p);
        c.name = m.name; c.faction = world::F_POLICE; c.role = "сотрудник базы S.W.A.T., в отряде Артёма";
        m.charId = c.id;
        talk::EnsureVoice(c);
        return &c;
    }
    void Speak(Member& m, const std::string& text)
    {
        world::Character* c = CharOf(m);
        talk::Say(m.name, text, false, c ? c->id : 0, 1, 1, true);
        TRACE("ОТРЯД", "%s: %s", m.name.c_str(), text.c_str());
    }
    Member* ByRef(int ref) { for(auto& m : g_m) if(m.ref == ref && ref) return &m; return nullptr; }
    Member* ByName(const std::string& low)
    {
        for(auto& m : g_m)
        {
            std::string n = Lower(m.name);
            size_t sp = n.rfind(' ');
            std::string last = sp == std::string::npos ? n : n.substr(sp + 1);
            if(last.size() >= 4 && Has(low, last.substr(0, last.size() - 2).c_str())) return &m;   // «Томми», «Томми,», «Томми!»
        }
        return nullptr;
    }
    void TeamSay(int ref, const std::string& who, const std::string& text, bool urgent)
    {
        (void)who; (void)urgent;
        if(Member* m = ByRef(ref)) Speak(*m, text);
    }
    void TeamBackup(int kind, const swat::V3& at) { if(g_api) g_api->CallBackup(kind, at.x, at.y, at.z); }
    void TeamArrested(int ref) { if(g_api) g_api->Arrested(ref); }
    void TeamDelivered(int ref) { if(g_api) g_api->Delivered(ref); }
    int  TeamStars(int ref) { return g_api ? g_api->NpcStars(ref) : 0; }

    void EnsureTeam()
    {
        if(g_team) return;
        g_team = new swat::Team("отряд Артёма");
        g_team->SetSay(TeamSay);
        g_team->SetBackup(TeamBackup);
        g_team->SetArrest(TeamArrested, TeamDelivered);
        g_team->SetStars(TeamStars);
        g_team->Auto(true);
    }
    void Places()
    {
        if(!g_team || !g_api) return;
        g_team->SetPlace(0, Place(SWAT_PLACE_MEDBAY));
        g_team->SetPlace(1, Place(SWAT_PLACE_ARMORY));
        g_team->SetPlace(2, Place(SWAT_PLACE_CELL));
        g_team->SetPlace(3, Place(SWAT_PLACE_HELIPAD));
        g_team->SetPlace(4, Place(SWAT_PLACE_GATE));
        g_team->SetPlace(5, Place(SWAT_PLACE_GARAGE));
        g_team->SetPlace(6, Place(SWAT_PLACE_REST));
    }
    int RoleFor(int idx)
    {   // роли заранее (Т131): первый — ведущий, второй — стрелок, третий — медик/замыкающий
        static const int r[3] = { swat::R_LEAD, swat::R_MARKSMAN, swat::R_MEDIC };
        return r[idx % 3];
    }
    void AttachToTeam(Member& m)
    {
        EnsureTeam();
        if(m.ref <= 0) return;
        g_team->Add(m.ref, m.name, m.role);
        g_team->SetLeader(swat::PlayerRef());
        Places();
        swat::Do(0x0961, { swat::Ai(m.ref), swat::Ai(1) });          // SET_CHAR_KEEP_TASK
        swat::Do(0x077A, { swat::Ai(m.ref), swat::Ai(0), swat::Ai(0) });   // SET_CHAR_RELATIONSHIP: уважает игрока
    }

    // ---------------- набор / отпуск ----------------
    bool Recruit(uintptr_t ped)
    {
        if(!g_api || !g_api->ModeOn()) return false;
        int ref = game::Ref(ped);
        int idx = g_api->StaffIndexOf(ref);
        if(idx < 0) return false;
        world::Character& c = world::Promote(ped);
        if(g_api->StaffKind(idx) == SWAT_FRANK)
        {
            talk::Say(g_api->StaffName(idx), "Я с базы не ухожу, Артём. Бери ребят.", false, c.id, 1, 1, true);
            return true;
        }
        if(ByRef(ref)) { talk::Say(g_api->StaffName(idx), "Я уже с тобой, Артём.", false, c.id, 1, 1, true); return true; }
        if(g_m.size() >= 3) { talk::Say(g_api->StaffName(idx), "Отряд полный, Артём.", false, c.id, 1, 1, true); return true; }
        Member m; m.name = g_api->StaffName(idx); m.staffIdx = idx; m.model = g_api->StaffModel(idx); m.ref = ref;
        m.role = RoleFor((int)g_m.size());
        m.charId = c.id; c.name = m.name; c.faction = world::F_POLICE; talk::EnsureVoice(c);
        m.character = g_api->StaffKind(idx) == SWAT_TOMMY ? "раздолбай и шутник, но в деле надёжный" :
                      (g_api->StaffKind(idx) == SWAT_GUARD ? "сдержанный, внимательный, бывший охранник" : "собранный, немного ворчит, профессионал");
        g_api->ReleaseStaff(ref);
        // в отряд — со стволом и бронёй (как взял бы в оружейной)
        swat::GiveWeapon(ref, 31, 300);
        swat::GiveWeapon(ref, 22, 120);
        swat::Do(0x035F, { swat::Ai(ref), swat::Ai(50) });
        g_m.push_back(m);
        AttachToTeam(g_m.back());
        static const char* yes[] = { "Так точно, Артём! Я с тобой.", "Наконец-то дело. Так точно, Артём!", "Есть! Так точно, Артём!" };
        Speak(g_m.back(), yes[g_m.size() % 3]);
        LOG_INFO("Отряд: + %s (%s), всего %d", m.name.c_str(), swat::RoleName(m.role), (int)g_m.size());
        return true;
    }
    void Release(Member& m, bool quiet)
    {
        if(!quiet) Speak(m, "Так точно, Артём! Ухожу на базу.");
        if(g_team && m.ref) g_team->Remove(m.ref);
        swat::V3 home = Place(SWAT_PLACE_REST);
        if(m.ref && swat::RefAlive(m.ref) && swat::Dist2(swat::RefPos(m.ref), home) < 160)
            swat::Do(0x0603, { swat::Ai(m.ref), swat::Af(home.x), swat::Af(home.y), swat::Af(home.z), swat::Ai(6), swat::Ai(-1) });   // TASK_GO_TO_COORD_ANY_MEANS
        else if(m.ref && swat::RefAlive(m.ref)) swat::Do(0x009B, { swat::Ai(m.ref) });     // далеко — «уехал сам»
        if(g_api && m.ref) g_api->ReturnStaff(m.ref);
        LOG_INFO("Отряд: %s свободен — ушёл на базу", m.name.c_str());
    }

    // ---------------- голова (Haiku) ----------------
    const char* kSchema = R"({"type":"json_schema","schema":{"type":"object","additionalProperties":false,
      "required":["tactic","target","side","point","roles","lines","release","note"],
      "properties":{
        "tactic":{"type":"integer","description":"номер тактики из каталога, 0 — не менять"},
        "target":{"type":"string","description":"цель: #номер угрозы из сводки, 'artem_target' — в кого целится Артём, 'none'"},
        "side":{"type":"string","enum":["left","right","auto"]},
        "point":{"type":"string","enum":["none","artem_aim","waypoint","artem","base","medbay","armory","cell","helipad"]},
        "roles":{"type":"array","items":{"type":"object","additionalProperties":false,"required":["name","role"],
          "properties":{"name":{"type":"string"},"role":{"type":"string","enum":["ведущий","замыкающий","стрелок","водитель","пилот","медик"]}}}},
        "lines":{"type":"array","items":{"type":"object","additionalProperties":false,"required":["name","text"],
          "properties":{"name":{"type":"string"},"text":{"type":"string"}}}},
        "release":{"type":"array","items":{"type":"string"},"description":"имена тех, кого Артём отпустил на базу"},
        "note":{"type":"string"}}}})";

    std::string SystemText()
    {
        std::string s =
            "Ты — «голова» отряда спецназа S.W.A.T. в GTA San Andreas (мод). Командир — Артём (игрок). В отряде до трёх бойцов — "
            "сотрудников базы. Ты думаешь и решаешь за ВЕСЬ отряд одним ответом: выбираешь тактику из каталога (номер), цель, сторону, "
            "место, роли, реплики. Исполняет наш код: укрытия, очереди, свой водитель и пилот, — тебе не нужно описывать движения.\n"
            "Правила: профессиональный опытный спецназ, как в жизни. Приказ Артёма выполняется ВСЕГДА. Бойцы могут спорить в своём "
            "характере (коротко), но приказ Артёма заканчивают уставным «Так точно, Артём!». В бою — короткие фразы (2–5 слов). "
            "В спокойное время можно шутить. Не стрелять, если на линии мирные. Сдавшихся не трогать — задерживать. "
            "Без читов: машины и оружие — только те, что есть. Говорят по очереди — 0–2 реплики за ответ, по-русски.\n"
            "Если Артём ничего не приказал, а случилось событие — выбери разумную тактику сам (контакт — укрытия и ответный огонь, "
            "ранен свой — медик, бегущий подозреваемый — погоня/перехват, тишина — ромб вокруг Артёма). tactic=0 — оставить как есть.\n"
            "Каталог тактик (номер и смысл):";
        s += swat::CatalogText();
        s += "\nРоли: ведущий (идёт первым), замыкающий (смотрит назад), стрелок (M4, одиночные, дальние цели), водитель, пилот, медик.";
        return s;
    }

    void ApplyHead(const json::Value& v)
    {
        if(!g_team) return;
        // роли
        for(auto& r : v["roles"].arr())
        {
            std::string low = Lower(r["name"].str());
            Member* m = ByName(low);
            if(!m) for(auto& x : g_m) if(Lower(x.name) == low) m = &x;
            if(!m) continue;
            std::string role = r["role"].str();
            int ri = role == "ведущий" ? swat::R_LEAD : role == "замыкающий" ? swat::R_REAR : role == "стрелок" ? swat::R_MARKSMAN :
                     role == "водитель" ? swat::R_DRIVER : role == "пилот" ? swat::R_PILOT : role == "медик" ? swat::R_MEDIC : swat::R_NONE;
            if(ri && ri != m->role) { m->role = ri; if(m->ref) g_team->SetRole(m->ref, ri); }
        }
        // отпуск
        for(auto& n : v["release"].arr())
        {
            std::string low = Lower(n.str());
            for(size_t i = 0; i < g_m.size(); ++i)
                if(Lower(g_m[i].name).find(low) != std::string::npos || (ByName(low) == &g_m[i])) { Release(g_m[i], true); g_m.erase(g_m.begin() + i); break; }
        }
        // реплики — по очереди
        int k = 0;
        for(auto& l : v["lines"].arr())
        {
            if(k++ >= 3) break;
            std::string low = Lower(l["name"].str());
            Member* m = ByName(low);
            if(!m) for(auto& x : g_m) if(Lower(x.name).find(low) != std::string::npos) m = &x;
            if(!m && !g_m.empty()) m = &g_m[0];
            if(m && !l["text"].str().empty()) Speak(*m, l["text"].str());
        }
        int t = v["tactic"].i(0);
        if(t <= 0 || t > 140) return;
        swat::Order o; o.tactic = t; o.why = "голова: " + v["note"].str();
        std::string tg = v["target"].str();
        if(tg.size() > 1 && tg[0] == '#') o.target = atoi(tg.c_str() + 1);
        else if(tg == "artem_target") { uintptr_t pt = swat::PlayerTarget(); o.target = pt ? swat::Ref(pt) : 0; }
        std::string side = v["side"].str(); o.side = side == "left" ? -1 : side == "right" ? 1 : 0;
        std::string pt = v["point"].str();
        swat::V3 p; bool has = false;
        if(pt == "artem_aim") { uintptr_t a = swat::PlayerTarget(); if(a) { p = swat::Pos(a); has = true; } }
        else if(pt == "waypoint") has = swat::PlayerWaypoint(p);
        else if(pt == "artem") { p = swat::Pos(swat::Player()); has = true; }
        else if(pt == "base") { p = Place(SWAT_PLACE_CENTER); has = true; }
        else if(pt == "medbay") { p = Place(SWAT_PLACE_MEDBAY); has = true; }
        else if(pt == "armory") { p = Place(SWAT_PLACE_ARMORY); has = true; }
        else if(pt == "cell") { p = Place(SWAT_PLACE_CELL); has = true; }
        else if(pt == "helipad") { p = Place(SWAT_PLACE_HELIPAD); has = true; }
        o.point = p; o.hasPoint = has;
        g_team->Command(o);
        // по приказу Артёма голова ведёт; без приказа правила снова включатся через минуту
        g_team->Auto(false);
    }

    void AskHead(const std::string& why, const std::string& order)
    {
        if(!claude::Available() || !g_team || g_m.empty()) return;
        if(g_req) { g_askPending = true; g_pendingWhy = why; if(!order.empty()) g_pendingOrder = order; return; }
        if(order.empty() && !Ago(g_lastAsk, 5000)) { g_askPending = true; g_pendingWhy = why; return; }
        g_lastAsk = Now();
        claude::Request r;
        json::Value sys = json::Value::Object(); sys["type"] = "text"; sys["text"] = SystemText();
        r.system = json::Value::Array(); r.system.push(sys);
        std::string u = "Бойцы отряда:\n";
        for(auto& m : g_m) u += "- " + m.name + " (" + swat::RoleName(m.role) + "): " + m.character + (m.dead ? " — погиб, едет из медпункта" : (m.traveling ? " — в пути" : "")) + "\n";
        u += "\nСводка обстановки:\n" + g_team->Situation(1600);
        u += "\nСобытие: " + why;
        if(!order.empty()) u += "\nПРИКАЗ АРТЁМА (голосом): «" + order + "»";
        json::Value msg = json::Value::Object(); msg["role"] = "user"; msg["content"] = u;
        json::Value msgs = json::Value::Array();
        for(auto& h : g_history.arr()) msgs.push(h);
        msgs.push(msg);
        r.messages = msgs;
        json::Value fmt; json::Parse(kSchema, fmt); r.format = fmt;
        r.effort = settings::Get().squadEffort.empty() ? "xhigh" : settings::Get().squadEffort;
        r.maxTokens = 12000; r.timeoutMs = 60000; r.tag = "отряд"; r.role = claude::ROLE_REACT; r.cache = true;
        std::string orderCopy = order;
        g_req = claude::Send(r, nullptr, [u, orderCopy](claude::Result& res) {
            g_req = 0;
            json::Value v;
            if(!res.ok || !json::Parse(res.text, v)) { TRACE("ОТРЯД", "голова: нет ответа (%s)", res.error.c_str()); return; }
            TRACE("ОТРЯД", "голова: Т%03d, цель %s, точка %s — %s", v["tactic"].i(0), v["target"].str().c_str(), v["point"].str().c_str(), v["note"].str().c_str());
            ApplyHead(v);
            // короткая память разговора (последние 3 хода)
            json::Value um = json::Value::Object(); um["role"] = "user"; um["content"] = u.size() > 900 ? u.substr(0, 900) : u;
            json::Value am = json::Value::Object(); am["role"] = "assistant"; am["content"] = res.text;
            g_history.push(um); g_history.push(am);
            while(g_history.size() > 6) g_history.arr().erase(g_history.arr().begin());
        });
        if(!g_req) TRACE("ОТРЯД", "голова: запрос не отправлен (нет ключа/моста)");
        else TRACE("ОТРЯД", "голова думает: %s%s", why.c_str(), order.empty() ? "" : (" / приказ: " + order).c_str());
    }

    // ---------------- простые приказы без ИИ (мгновенная реакция, §9 ТЗ-SQUAD) ----------------
    int ParseOrder(const std::string& low, swat::Order& o)
    {
        struct K { const char* w; int t; };
        static const K keys[] = {
            { "свобод", -1 }, { "на базу", -1 }, { "иди домой", -1 },
            { "за мной", 15 }, { "ко мне", 15 }, { "собер", 15 }, { "ромб", 4 }, { "колонн", 1 }, { "клин", 2 }, { "шеренг", 3 },
            { "жди", 37 }, { "стой", 37 }, { "держи вход", 122 }, { "держите вход", 122 }, { "держи позиц", 37 },
            { "прикрой спин", 113 }, { "прикрой меня", 113 }, { "прикрыв", 113 }, { "тенью", 114 },
            { "займи позиц", 115 }, { "займите позиц", 115 }, { "на крышу", 116 }, { "займи крыш", 116 },
            { "огонь", 35 }, { "стреля", 35 }, { "огонь на подавл", 38 }, { "подав", 18 }, { "фланг слева", 19 }, { "слева", 19 }, { "фланг справа", 20 }, { "справа", 20 }, { "клещи", 21 },
            { "отход", 22 }, { "отступ", 22 }, { "в укрыт", 16 }, { "рассредоточ", 14 }, { "не стрел", 28 }, { "прекратить огонь", 29 },
            { "задерж", 56 }, { "руки вверх", 57 }, { "арест", 56 }, { "в камеру", 64 }, { "обыщ", 60 },
            { "догон", 67 }, { "перехват", 77 }, { "отрежь", 68 }, { "пит", 78 }, { "заслон", 80 }, { "по колёсам", 83 }, { "по колесам", 83 },
            { "в машину", 139 }, { "по машинам", 139 }, { "за руль", 124 }, { "отвези", 124 }, { "жди в машине", 125 }, { "подгони машину", 119 },
            { "вертолёт", 126 }, { "вертолет", 126 }, { "с вертол", 126 }, { "наблюдение с воздуха", 101 }, { "на базу на вертол", 112 },
            { "подмог", 120 }, { "вызови", 120 }, { "найди", 123 }, { "ищи", 123 }, { "прочес", 123 }, { "следи", 121 },
            { "проверь машину", 117 }, { "броню", 127 }, { "принеси", 118 }, { "оружейн", 133 },
            { "зачист", 45 }, { "у двери", 42 }, { "вход", 43 }, { "задний выход", 50 }, { "периметр", 31 }, { "чисто", 130 },
            { "перекличк", 129 }, { "доклад", 128 }, { "отдых", 140 }, { "медик", 24 }, { "перевяж", 24 }, { "патрон", 33 },
        };
        int best = 0; size_t bestLen = 0;
        for(auto& k : keys) { size_t l = strlen(k.w); if(Has(low, k.w) && l > bestLen) { best = k.t; bestLen = l; } }
        if(best <= 0) return best;
        o.tactic = best; o.why = "приказ голосом";
        if(best == 37 || best == 122) { o.point = swat::Pos(swat::Player()); o.hasPoint = true; }
        uintptr_t pt = swat::PlayerTarget(); if(pt) o.target = swat::Ref(pt);
        return best;
    }

    // ---------------- смерть, «в пути», появление ----------------
    int SpawnMember(Member& m, const swat::V3& at)
    {
        if(!swat::Is(0x0248, { swat::Ai(m.model) })) { swat::Do(0x0247, { swat::Ai(m.model) }); return 0; }
        int h = swat::OutI(0x009A, { swat::Ai(6), swat::Ai(m.model), swat::Af(at.x), swat::Af(at.y), swat::Af(at.z + 0.5f), swat::Aoi() });   // CREATE_CHAR
        if(h <= 0) return 0;
        swat::Do(0x0249, { swat::Ai(m.model) });
        bool any = false;
        for(int i = 0; i < 13; ++i) if(m.weapons[i] > 0 && m.ammo[i] > 0) { swat::GiveWeapon(h, m.weapons[i], m.ammo[i]); any = true; }
        if(!any) swat::GiveWeapon(h, 31, 300);
        m.ref = h;
        if(world::Character* c = world::Find(m.charId)) world::Bind(*c, swat::Ped(h));
        AttachToTeam(m);
        return h;
    }
    swat::V3 AroundCorner()
    {   // появиться рядом с Артёмом вне поля зрения
        swat::V3 me = swat::Pos(swat::Player());
        for(int i = 0; i < 10; ++i)
        {
            swat::V3 probe = swat::Add(me, swat::Mul(swat::Dir(swat::Rnd(0, 360)), swat::Rnd(25, 45))), n;
            if(!swat::ClosestPedNode(probe, n)) continue;
            if(swat::OnScreen(swat::Add(n, swat::Mk(0, 0, 1)), 1.5f)) continue;
            if(fabsf(n.z - me.z) > 8) continue;
            return n;
        }
        return swat::Add(me, swat::Mk(0, -3, 0));
    }
    void Remember(Member& m)
    {
        uintptr_t p = swat::Ped(m.ref); if(!p) return;
        for(int i = 0; i < 13; ++i) { uintptr_t w = p + 0x730 + (uintptr_t)i * 0x20; m.weapons[i] = *(int*)w; m.ammo[i] = *(int*)(w + 12); }
    }
    void LifeFrame()
    {
        swat::V3 me = swat::Pos(swat::Player());
        for(auto& m : g_m)
        {
            if(m.ref && !m.dead && !swat::RefAlive(m.ref))
            {
                m.dead = true; m.deadAt = Now();
                if(g_team) g_team->Remove(m.ref);
                m.ref = 0;
                LOG_INFO("Отряд: %s погиб — через ~30 с появится в медпункте базы", m.name.c_str());
                AskHead(m.name + " погиб", "");
                continue;
            }
            if(m.dead && Ago(m.deadAt, 30000))
            {   // ~30 с → медпункт базы, «Я на базе, подлатали», дальше едет к Артёму
                swat::V3 med = Place(SWAT_PLACE_MEDBAY);
                if(swat::Dist2(me, med) < 140)
                {
                    if(SpawnMember(m, med)) { m.dead = false; Speak(m, "Я на базе, подлатали. Иду к тебе."); }
                }
                else
                {
                    m.dead = false; m.traveling = true;
                    m.arriveAt = Now() + (uint32_t)(swat::Dist2(me, med) / 14.0f * 1000.0f);
                    talk::Say(m.name, "Рация: я на базе, подлатали. Еду к тебе.", false, m.charId, 1, 1, true);
                }
                continue;
            }
            if(m.ref && !m.dead && !m.traveling)
            {
                float d = swat::Dist2(swat::RefPos(m.ref), me);
                if(Ago(m.spawnTry, 4000)) { m.spawnTry = Now(); Remember(m); }
                if(d > 230 && !swat::PedVehicle(swat::Ped(m.ref)))
                {   // далеко (телефон держит в памяти только то, что рядом) — «в пути», время = дорога на машине
                    Remember(m);
                    if(g_team) g_team->Remove(m.ref);
                    swat::Do(0x009B, { swat::Ai(m.ref) });
                    m.ref = 0; m.traveling = true; m.arriveAt = Now() + (uint32_t)(d / 15.0f * 1000.0f);
                    LOG_INFO("Отряд: %s далеко (%.0f м) — «в пути», будет через %u с", m.name.c_str(), d, (m.arriveAt - Now()) / 1000);
                }
            }
            if(m.traveling && Now() >= m.arriveAt && Ago(m.spawnTry, 1500))
            {
                m.spawnTry = Now();
                if(SpawnMember(m, AroundCorner())) { m.traveling = false; Speak(m, "Я на месте, Артём."); }
            }
        }
    }

    // ---------------- сохранение ----------------
    std::string FileOf(const std::string& key)
    {
        std::string d = world::BaseDir() + "/squad"; mkdir(d.c_str(), 0777);
        std::string k; for(char c : key) k += (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') ? c : '_';
        return d + "/" + (k.empty() ? "default" : k) + ".json";
    }
    void SaveTo(const std::string& key)
    {
        json::Value root = json::Value::Object(); json::Value arr = json::Value::Array();
        for(auto& m : g_m)
        {
            Remember(m);
            json::Value j = json::Value::Object();
            j["name"] = m.name; j["staff"] = m.staffIdx; j["model"] = m.model; j["role"] = m.role; j["char"] = m.charId; j["character"] = m.character;
            json::Value w = json::Value::Array(); for(int i = 0; i < 13; ++i) { json::Value x = json::Value::Array(); x.push(m.weapons[i]); x.push(m.ammo[i]); w.push(x); }
            j["weapons"] = w;
            arr.push(j);
        }
        root["members"] = arr;
        std::string p = FileOf(key), s = root.dump();
        FILE* f = fopen(p.c_str(), "wb"); if(!f) return; fwrite(s.data(), 1, s.size(), f); fclose(f);
        LOG_INFO("Отряд: сохранён (%d бойцов) — %s", (int)g_m.size(), p.c_str());
    }
    void LoadFrom(const std::string& key)
    {
        g_m.clear();
        FILE* f = fopen(FileOf(key).c_str(), "rb");
        if(!f) return;
        std::string s; char b[4096]; size_t n; while((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n); fclose(f);
        json::Value v; if(!json::Parse(s, v)) return;
        for(auto& j : v["members"].arr())
        {
            Member m; m.name = j["name"].str(); m.staffIdx = j["staff"].i(-1); m.model = j["model"].i(280); m.role = j["role"].i(swat::R_LEAD);
            m.charId = j["char"].i(0); m.character = j["character"].str();
            int i = 0; for(auto& w : j["weapons"].arr()) { if(i < 13) { m.weapons[i] = w[(size_t)0].i(0); m.ammo[i] = w[(size_t)1].i(0); } i++; }
            m.traveling = true; m.arriveAt = Now() + 2500;          // загрузка → появляется рядом (из-за угла)
            g_m.push_back(m);
        }
        LOG_INFO("Отряд: загружен (%d бойцов) — появятся рядом", (int)g_m.size());
    }
}

// ======================================================================================================
void Init(void* h)
{
    swat::Init(h);
    swat::InstallDriverHooks(h);
}

void Reset()
{
    if(g_team) g_team->Clear();
    g_m.clear(); g_history = json::Value::Array();
    if(g_req) { claude::Cancel(g_req); g_req = 0; }
}
void OnSave(const std::string& key) { if(ModeOn()) SaveTo(key); }
void OnLoad(const std::string& key) { Reset(); g_loadPending = true; g_loadKey = key; }

bool ModeOn() { return Api() && g_api->ModeOn(); }
bool Active() { return ModeOn() && !g_m.empty(); }
bool IsMember(uintptr_t ped) { int r = game::Ref(ped); return ped && ByRef(r) != nullptr; }
bool HeadThinking() { return g_req != 0; }

bool ListenForSquad(uintptr_t picked)
{
    if(!Active()) return false;
    return !picked || IsMember(picked);
}

bool OnSpeech(const std::string& text, uintptr_t talkPed)
{
    if(!ModeOn() || text.empty()) return false;
    std::string low = Lower(text);
    // набор: «пойдём со мной» сотруднику базы
    bool recruitWords = Has(low, "пойд") || Has(low, "пошли со мной") || Has(low, "со мной") || Has(low, "в отряд") || Has(low, "за мной");
    if(talkPed && !IsMember(talkPed) && recruitWords && g_api->StaffIndexOf(game::Ref(talkPed)) >= 0)
        return Recruit(talkPed);
    if(g_m.empty()) return false;
    // фраза отряду: обращение «отряд/парни/ребята», имя бойца или разговор не с человеком
    Member* who = ByName(low);
    bool all = Has(low, "отряд") || Has(low, "парни") || Has(low, "ребят") || Has(low, "все ");
    bool forSquad = all || who || !talkPed || IsMember(talkPed);
    if(!forSquad) return false;
    EnsureTeam();
    swat::Order o;
    int t = ParseOrder(low, o);
    if(t == -1)
    {   // «свободен» — одному по имени или всем
        if(who) { Release(*who, false); g_m.erase(g_m.begin() + (who - &g_m[0])); }
        else { for(auto& m : g_m) Release(m, false); g_m.clear(); }
        return true;
    }
    if(t > 0)
    {
        g_team->Command(o);
        g_team->Auto(false);
        Member* m = who ? who : (g_m.empty() ? nullptr : &g_m[0]);
        if(m && !claude::Available()) { static const char* ok[] = { "Так точно, Артём!", "Есть!", "Принял, Артём." }; Speak(*m, ok[Now() % 3]); }
        const swat::TacticInfo* ti = swat::FindTactic(t);
        LOG_INFO("Отряд: приказ «%s» → Т%03d %s", text.c_str(), t, ti ? ti->name : "?");
    }
    g_lastOrderText = text;
    AskHead("приказ Артёма", text);                         // голова уточняет и отвечает в характере
    return true;
}

std::vector<Card> Cards()
{
    std::vector<Card> r;
    std::vector<swat::UnitPublic> units = g_team ? g_team->Units() : std::vector<swat::UnitPublic>();
    for(auto& m : g_m)
    {
        Card c; c.name = m.name; c.role = swat::RoleName(m.role);
        c.alive = !m.dead; c.traveling = m.traveling;
        if(m.traveling) { c.eta = (int)((m.arriveAt > Now() ? m.arriveAt - Now() : 0) / 1000); char b[32]; snprintf(b, sizeof(b), "в пути %d:%02d", c.eta / 60, c.eta % 60); c.doing = b; }
        else if(m.dead) c.doing = "погиб — медпункт";
        for(auto& u : units) if(u.ref == m.ref && m.ref) { c.hp = u.hp; c.armour = u.armour; if(!m.traveling && !m.dead) c.doing = u.doing; }
        r.push_back(c);
    }
    return r;
}

void Frame()
{
    if(!ModeOn()) { if(!g_m.empty()) Reset(); return; }
    if(!swat::Player()) return;
    if(g_loadPending) { g_loadPending = false; LoadFrom(g_loadKey); }
    swat::NewFrame();
    WHERE("отряд: жизнь"); LifeFrame();
    if(g_team && g_team->Count())
    {
        g_team->SetLeader(swat::PlayerRef());
        WHERE("отряд: тактика"); g_team->Tick();
        swat::Event e;
        while(g_team->PopEvent(e))
        {
            // события → голове (контакт, ранен, погиб, цель ушла, чисто, задержан…)
            if(e.kind == "contact" || e.kind == "wounded" || e.kind == "lost" || e.kind == "clear" || e.kind == "arrest" || e.kind == "found" || e.kind == "target_down")
                AskHead(e.text, "");
            if(e.kind == "clear" || e.kind == "contact") g_team->Auto(true);   // новый бой/тишина — правила, пока голова думает
        }
    }
    if(g_askPending && !g_req && Ago(g_lastAsk, 4000)) { g_askPending = false; std::string w = g_pendingWhy, o = g_pendingOrder; g_pendingOrder.clear(); AskHead(w, o); }
    WHERE("отряд: водитель"); swat::DriverFrame();
}
}
