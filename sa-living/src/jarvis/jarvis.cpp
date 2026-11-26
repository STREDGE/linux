#include "jarvis.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <deque>
#include <functional>

#include "ai/claude.h"
#include "game/game.h"
#include "jarvis/brain.h"
#include "jarvis/script.h"
#include "net/link.h"
#include "platform/bridge.h"
#include "settings.h"
#include "speech/speech.h"
#include "talk/talk.h"
#include "util/log.h"
#include "world/world.h"

using world::Fmt;

namespace jarvis
{
namespace
{
    const char* kLibraryUrl = "https://raw.githubusercontent.com/sannybuilder/library/master/sa_mobile/sa_mobile.json";

    std::string g_base;                 // …/SALiving/jarvis
    View   g_view;
    double g_viewUntil = 0;             // когда спрятать плашку
    double g_autoStopAt = 0;            // вызов из меню CLEO: когда перестать слушать
    bool   g_holding = false;
    int    g_req = 0, g_round = 0;
    json::Value g_hist;                 // разговор с ИИ (последние ходы)
    double g_histAt = 0;
    std::string g_buf;                  // хвост ответа, ещё не озвученный
    int    g_sayPending = 0;
    bool   g_infAmmo = false;
    double g_nextAmmo = 0;
    bool   g_libTried = false;
    int    g_lastCar = -1;              // последняя созданная машина (номер для команд)
    std::deque<std::function<void()>> g_undo;
    int    g_seq = 0;

    struct Place { std::string name; game::Vec pos; };
    std::vector<Place> g_places; std::string g_placesKey; bool g_placesLoaded = false;

    double Now() { return talk::Now(); }
    const char* Sir() { return settings::Get().jarvisAddress.c_str(); }

    // ---------- голос и плашка ----------
    std::string VoiceKey()
    {
        const settings::Data& s = settings::Get();
        speech::Voice v;
        if(!s.jarvisVoice.empty() && speech::FindVoice(s.jarvisVoice, v)) return s.jarvisVoice;
        if(speech::FindVoice("m02", v)) return "m02";
        return speech::PickVoice(false, 7);
    }

    void SpeakNow(const std::string& text)
    {
        std::string t = text;
        while(!t.empty() && (t[0] == ' ' || t[0] == '\n')) t.erase(0, 1);
        if(t.empty()) return;
        const settings::Data& s = settings::Get();
        if(s.engine == 0)
        {
            if(!speech::TtsReady()) return;
            speech::Voice v; float p = s.jarvisPitch, r = s.jarvisSpeed;
            std::string key = VoiceKey();
            if(speech::FindVoice(key, v)) { p *= v.pitch; r *= v.speed; }
            ++g_sayPending;
            speech::Speak(t, key, p, r, Fmt("jv%d", ++g_seq));
        }
        else if(bridge::TtsReady()) bridge::TtsSpeak(t, 0.9f, 1.05f, Fmt("jv%d", ++g_seq), true);
    }

    size_t SentenceEnd(const std::string& s)
    {
        for(size_t i = 0; i < s.size(); ++i) if((s[i] == '.' || s[i] == '!' || s[i] == '?' || s[i] == '\n') && i > 6) return i + 1;
        return s.size() > 160 ? s.size() : std::string::npos;
    }

    void Show(State st) { g_view.visible = true; g_view.state = st; g_viewUntil = Now() + 12; }
    void Done(const std::string& line) { g_view.done.push_back(line); if(g_view.done.size() > 5) g_view.done.erase(g_view.done.begin()); LOG_INFO("[ДЖАРВИС] сделано: %s", line.c_str()); }

    void Reply(const std::string& text)
    {
        g_view.reply = text;
        Show(SPEAKING);
        SpeakNow(text);
        LOG_INFO("[ДЖАРВИС] ответ: %s", text.c_str());
    }

    // ---------- свои места (по сохранению) ----------
    std::string PlacesPath() { return g_base + "/places_" + (world::MemoryKey().empty() ? std::string("new") : world::MemoryKey()) + ".json"; }
    void LoadPlaces()
    {
        if(g_placesLoaded && g_placesKey == world::MemoryKey()) return;
        g_placesKey = world::MemoryKey(); g_places.clear(); g_placesLoaded = true;
        FILE* f = fopen(PlacesPath().c_str(), "rb"); if(!f) return;
        std::string s; char buf[8192]; size_t n; while((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n); fclose(f);
        json::Value v; if(!json::Parse(s, v)) return;
        for(auto& p : v["places"].arr()) { Place q; q.name = p["name"].str(); q.pos.x = (float)p["x"].num(); q.pos.y = (float)p["y"].num(); q.pos.z = (float)p["z"].num(); g_places.push_back(q); }
    }
    void SavePlaces()
    {
        json::Value arr = json::Value::Array();
        for(auto& p : g_places) { json::Value o = json::Value::Object(); o["name"] = p.name; o["x"] = p.pos.x; o["y"] = p.pos.y; o["z"] = p.pos.z; arr.push(o); }
        json::Value root = json::Value::Object(); root["places"] = arr;
        FILE* f = fopen(PlacesPath().c_str(), "wb"); if(!f) return;
        std::string s = root.dump(); fwrite(s.data(), 1, s.size(), f); fclose(f);
    }
    Place* FindOwnPlace(const std::string& raw)
    {
        LoadPlaces();
        std::string t = jbrain::Lower(raw);
        if(t.empty()) return nullptr;
        for(auto& p : g_places) { std::string n = jbrain::Lower(p.name); if(t == n || t.find(n) != std::string::npos || n.find(t) != std::string::npos) return &p; }
        // «на базу» → «база»: сравнить без последних двух букв
        for(auto& p : g_places)
        {
            std::string n = jbrain::Lower(p.name);
            size_t k = std::min(n.size(), t.size()); if(k >= 6) k -= 2;
            if(k >= 4 && n.compare(0, k, t, 0, k) == 0) return &p;
        }
        return nullptr;
    }

    // ---------- команды ----------
    using script::Arg;
    bool Client() { return coopnet::GetRole() == coopnet::CLIENT; }
    int  Me() { return script::PlayerChar(); }
    int  MyCar() { return script::CarHandle(game::PlayerVehicle()); }

    void Undoable(std::function<void()> fn) { g_undo.push_back(std::move(fn)); if(g_undo.size() > 12) g_undo.pop_front(); }

    bool GiveWeaponLocal(const jbrain::Weapon& w, int ammo)
    {
        if(Me() < 0) return false;
        script::Do(0x0247, { Arg::I(w.model) });          // REQUEST_MODEL
        script::Do(0x038B, {});                           // LOAD_ALL_MODELS_NOW
        script::Do(0x01B2, { Arg::I(Me()), Arg::I(w.type), Arg::I(ammo) });   // GIVE_WEAPON_TO_CHAR
        script::Do(0x01B9, { Arg::I(Me()), Arg::I(w.type) });                 // SET_CURRENT_CHAR_WEAPON
        script::Do(0x0249, { Arg::I(w.model) });          // MARK_MODEL_AS_NO_LONGER_NEEDED
        return true;
    }

    void TeleportTo(float x, float y, float z, bool autoZ)
    {
        game::Vec old = game::Pos(game::Player());
        int car = MyCar();
        script::Do(0x04BB, { Arg::I(0) });                               // SET_AREA_VISIBLE 0 (улица)
        script::Do(0x0860, { Arg::I(Me()), Arg::I(0) });                 // SET_CHAR_AREA_VISIBLE
        script::Do(0x04E4, { Arg::F(x), Arg::F(y) });                    // REQUEST_COLLISION
        script::Do(0x03CB, { Arg::F(x), Arg::F(y), Arg::F(autoZ ? 20.0f : z) });   // LOAD_SCENE
        float tz = autoZ ? -100.0f : z + 0.6f;                           // −100 — игра сама находит землю
        if(car >= 0) script::Do(0x00AB, { Arg::I(car), Arg::F(x), Arg::F(y), Arg::F(tz) });
        else         script::Do(0x00A1, { Arg::I(Me()), Arg::F(x), Arg::F(y), Arg::F(tz) });
        Undoable([old]() { TeleportTo(old.x, old.y, old.z, false); g_undo.pop_back(); });
    }

    std::string Info()
    {
        uintptr_t pl = game::Player(); if(!pl) return "игрок ещё не в игре";
        game::Vec p = game::Pos(pl);
        std::string s = Fmt("Игрок: квадрат %s (x %.0f, y %.0f, z %.0f), район %s; здоровье %.0f из %.0f, броня %.0f, деньги $%d, розыск %d зв.; время %02d:%02d",
                            jbrain::GridOf(p.x, p.y).c_str(), p.x, p.y, p.z, game::ZoneName().empty() ? "?" : game::ZoneName().c_str(),
                            game::Health(pl), game::MaxHealth(pl), game::Armour(pl), game::Money(), game::Wanted(), game::Hour(), game::Minute());
        if(uintptr_t v = game::PlayerVehicle()) s += Fmt("; в машине: %s (модель %d)", jbrain::VehicleName(game::Model(v)), game::Model(v));
        else s += "; пешком";
        if(game::OnMission()) s += "; ИДЁТ МИССИЯ";
        if(coopnet::GetRole() != coopnet::NONE) s += coopnet::GetRole() == coopnet::HOST ? "; кооп: ты хост" : "; кооп: ты подключён к хосту (общий мир меняет только хост)";
        LoadPlaces();
        if(!g_places.empty()) { s += "; свои места:"; for(auto& q : g_places) s += " «" + q.name + "»"; }
        return s;
    }

    // Выполнить одну готовую команду. Возвращает короткий итог (для ИИ и лога).
    std::string Exec(const std::string& tool, const json::Value& in)
    {
        uintptr_t pl = game::Player();
        if(!pl || !script::Ready()) return "игра ещё не готова";
        if(game::CutsceneRunning()) return "идёт катсцена — позже";

        if(tool == "info") return Info();
        if(tool == "undo")
        {
            if(g_undo.empty()) return "отменять нечего";
            auto fn = g_undo.back(); g_undo.pop_back(); fn();
            Done("Отменено");
            return "последнее действие отменено";
        }
        if(tool == "give_weapon")
        {
            const jbrain::Weapon* w = jbrain::FindWeapon(in["weapon"].str());
            if(!w) return "нет такого оружия: " + in["weapon"].str();
            int ammo = in["ammo"].i(); if(ammo <= 0) ammo = w->ammo; if(ammo > 99999) ammo = 99999;
            if(in["to"].str() == "friend")
            {
                if(coopnet::GetRole() == coopnet::NONE || !coopnet::Connected()) return "друга нет на связи";
                json::Value m = json::Value::Object(); m["t"] = "jv"; m["tool"] = "give_weapon";
                json::Value a = json::Value::Object(); a["weapon"] = w->key; a["ammo"] = ammo; m["in"] = a;
                coopnet::Send(m);
                Done(Fmt("Другу: %s, %d", w->title, ammo));
                return Fmt("другу отправлено: %s, патронов %d", w->title, ammo);
            }
            GiveWeaponLocal(*w, ammo);
            Done(w->ammo <= 1 ? std::string("Выдано: ") + w->title : Fmt("Выдано: %s, %d патр.", w->title, ammo));
            return Fmt("выдано: %s, патронов %d", w->title, ammo);
        }
        if(tool == "take_weapons") { script::Do(0x048F, { Arg::I(Me()) }); g_infAmmo = false; Done("Оружие убрано"); return "всё оружие убрано"; }
        if(tool == "wanted")
        {
            int before = game::Wanted();
            if(in.has("immune"))
            {
                bool on = in["immune"].b();
                script::Do(0x01F7, { Arg::I(0), Arg::I(on ? 1 : 0) });       // SET_POLICE_IGNORE_PLAYER
                script::Do(0x01F0, { Arg::I(on ? 0 : 6) });                  // SET_MAX_WANTED_LEVEL
                if(on) script::Do(0x0110, { Arg::I(0) });
                Undoable([on]() { script::Do(0x01F7, { Arg::I(0), Arg::I(on ? 0 : 1) }); script::Do(0x01F0, { Arg::I(on ? 6 : 0) }); });
                Done(on ? "Полиция тебя не трогает" : "Полиция снова ловит");
                return on ? "полиция игнорирует игрока, розыск не растёт" : "полиция снова реагирует";
            }
            int level = in.has("delta") ? before + in["delta"].i() : in["level"].i();
            if(level < 0) level = 0; if(level > 6) level = 6;
            if(level > 0) script::Do(0x01F0, { Arg::I(6) });
            if(level == 0) script::Do(0x0110, { Arg::I(0) }); else script::Do(0x010D, { Arg::I(0), Arg::I(level) });
            Undoable([before]() { if(before == 0) script::Do(0x0110, { Arg::I(0) }); else script::Do(0x010D, { Arg::I(0), Arg::I(before) }); });
            Done(level == 0 ? "Розыск снят" : Fmt("Розыск: %d зв.", level));
            return level == 0 ? "розыск снят" : Fmt("розыск %d зв.", level);
        }
        if(tool == "teleport")
        {
            if(game::OnMission() && Client()) return "во время миссии хоста перенос запрещён";
            float x = 0, y = 0, z = 0; bool autoZ = true; std::string title;
            std::string place = in["place"].str();
            std::string low = jbrain::Lower(place);
            if(in.has("x") && in.has("y") && place.empty()) { x = (float)in["x"].num(); y = (float)in["y"].num(); if(in.has("z")) { z = (float)in["z"].num(); autoZ = false; } title = Fmt("%.0f, %.0f", x, y); }
            else if(low == "waypoint" || low.find("метк") != std::string::npos)
            {
                game::Vec w; if(!game::Waypoint(w)) return "метка на карте не поставлена";
                x = w.x; y = w.y; title = "метка на карте";
            }
            else if(low == "back") { if(g_undo.empty()) return "возвращаться некуда"; auto fn = g_undo.back(); g_undo.pop_back(); fn(); Done("Перенос назад"); return "вернул назад"; }
            else if(low == "friend")
            {
                uintptr_t f = game::Player2(); if(!f) return "друга нет рядом в игре";
                game::Vec fp = game::Pos(f); x = fp.x + 1.5f; y = fp.y + 1.5f; z = fp.z; autoZ = false; title = "к другу";
            }
            else if(Place* own = FindOwnPlace(place)) { x = own->pos.x; y = own->pos.y; z = own->pos.z; autoZ = false; title = "«" + own->name + "»"; }
            else
            {
                std::string norm;
                if(jbrain::ParseGrid(place, x, y, norm)) title = "квадрат " + norm;
                else if(jbrain::FindPlace(place, x, y, z, title)) { autoZ = !(z > 60 || z < 0); }
                else return "не знаю места «" + place + "». Есть квадраты A1–L12, метка на карте, свои места и: " + jbrain::PlaceList();
            }
            if(x < -3000 || x > 3000 || y < -3000 || y > 3000) return "это за пределами карты";
            TeleportTo(x, y, z, autoZ);
            Done("Перенос: " + title);
            return "перенёс: " + title + " (квадрат " + jbrain::GridOf(x, y) + ")";
        }
        if(tool == "place")
        {
            LoadPlaces();
            std::string a = in["action"].str(), name = in["name"].str();
            if(a == "list")
            {
                if(g_places.empty()) return "своих мест пока нет";
                std::string s = "свои места:"; for(auto& p : g_places) s += " «" + p.name + "» (" + jbrain::GridOf(p.pos.x, p.pos.y) + ")";
                return s;
            }
            if(name.empty()) return "как назвать место?";
            if(a == "delete") { for(size_t i = 0; i < g_places.size(); ++i) if(jbrain::Lower(g_places[i].name) == jbrain::Lower(name)) { g_places.erase(g_places.begin() + i); SavePlaces(); Done("Место «" + name + "» забыто"); return "забыл"; } return "нет такого места"; }
            game::Vec p = game::Pos(pl);
            for(auto& q : g_places) if(jbrain::Lower(q.name) == jbrain::Lower(name)) { q.pos = p; SavePlaces(); Done("Место «" + name + "» обновлено"); return "место обновлено"; }
            g_places.push_back({ name, p }); SavePlaces();
            Done("Запомнил место «" + name + "»");
            return "запомнил место «" + name + "» (квадрат " + jbrain::GridOf(p.x, p.y) + ")";
        }
        if(tool == "player")
        {
            std::string res;
            if(in.has("health")) { int v = in["health"].i(100); float mx = game::MaxHealth(pl); int hp = v >= 100 ? (int)mx : std::max(1, (int)(mx * v / 100)); script::Do(0x0223, { Arg::I(Me()), Arg::I(hp) }); Done("Здоровье полное"); res += "здоровье восстановлено; "; }
            if(in.has("armour")) { script::Do(0x035F, { Arg::I(Me()), Arg::I(std::min(100, std::max(1, in["armour"].i(100)))) }); Done("Броня полная"); res += "броня выдана; "; }
            if(in.has("money")) { long long m = (long long)in["money"].num(); if(m > 99999999) m = 99999999; if(m < -99999999) m = -99999999; script::Do(0x0109, { Arg::I(0), Arg::I((int)m) }); Done(Fmt("Деньги: %+lld $", m)); res += Fmt("деньги %+lld; ", m); }
            if(in.has("god")) { int on = in["god"].b() ? 1 : 0; script::Do(0x02AB, { Arg::I(Me()), Arg::I(on), Arg::I(on), Arg::I(on), Arg::I(on), Arg::I(on) }); Undoable([on]() { int o = on ? 0 : 1; script::Do(0x02AB, { Arg::I(Me()), Arg::I(o), Arg::I(o), Arg::I(o), Arg::I(o), Arg::I(o) }); }); Done(on ? "Бессмертие включено" : "Бессмертие выключено"); res += on ? "бессмертие включено; " : "бессмертие выключено; "; }
            if(in.has("never_tired")) { int on = in["never_tired"].b() ? 1 : 0; script::Do(0x0330, { Arg::I(0), Arg::I(on) }); Done(on ? "Бег без усталости" : "Усталость как обычно"); res += "бег без усталости; "; }
            if(in.has("infinite_ammo")) { g_infAmmo = in["infinite_ammo"].b(); Done(g_infAmmo ? "Патроны не кончаются" : "Патроны как обычно"); res += g_infAmmo ? "патроны пополняются сами; " : "патроны как обычно; "; }
            return res.empty() ? "что изменить у игрока?" : res;
        }
        if(tool == "stat")
        {
            bool f; int mx; std::string title;
            int id = jbrain::FindStat(in["name"].str(), f, mx, title);
            if(id < 0) return "не знаю такой характеристики: " + in["name"].str();
            int v = in.has("value") ? in["value"].i(mx) : mx; if(v < 0) v = 0; if(v > mx) v = mx;
            if(f) script::Do(0x062A, { Arg::I(id), Arg::F((float)v) }); else script::Do(0x0629, { Arg::I(id), Arg::I(v) });
            if(id == 21 || id == 23) script::Do(0x070D, { Arg::I(0) });       // BUILD_PLAYER_MODEL: тело меняется сразу
            Done(Fmt("%s: %d из %d", title.c_str(), v, mx));
            return Fmt("%s = %d", title.c_str(), v);
        }
        if(tool == "vehicle")
        {
            std::string a = in["action"].str();
            int car = MyCar();
            if(a == "spawn")
            {
                int model = in["model"].isNumber() ? in["model"].i() : jbrain::FindVehicle(in["model"].str());
                if(in["model"].isNumber()) model = jbrain::FindVehicle(std::to_string(model));
                if(model < 0) return "не знаю такой машины: " + in["model"].s("?") + " (нужно название из игры или номер модели 400–611; поезда нельзя)";
                game::Vec p = game::Pos(pl); float h = game::Heading(pl);
                float d = 5.0f; game::Vec at{ p.x - sinf(h) * d, p.y + cosf(h) * d, p.z + 0.5f };
                script::Do(0x0247, { Arg::I(model) }); script::Do(0x038B, {});
                std::vector<Arg> args{ Arg::I(model), Arg::F(at.x), Arg::F(at.y), Arg::F(at.z), Arg::OI() };
                script::Run(0x00A5, args);
                int handle = args[4].i;
                script::Do(0x0249, { Arg::I(model) });
                if(handle <= 0) return "машина не создалась (нет места или памяти)";
                script::Do(0x0175, { Arg::I(handle), Arg::F(h * 57.29578f) });
                if(in.has("color1")) script::Do(0x0229, { Arg::I(handle), Arg::I(in["color1"].i()), Arg::I(in.has("color2") ? in["color2"].i() : in["color1"].i()) });
                if(in["enter"].b() && car < 0) script::Do(0x036A, { Arg::I(Me()), Arg::I(handle) });
                g_lastCar = handle;
                Undoable([handle]() {
                    if(MyCar() == handle)      // игрок внутри — сначала наружу, иначе машина исчезнет вместе с ним
                    { game::Vec p = game::Pos(game::Player()); script::Do(0x0362, { Arg::I(Me()), Arg::F(p.x + 2), Arg::F(p.y), Arg::F(p.z) }); }
                    script::Do(0x00A6, { Arg::I(handle) });
                });
                Done(Fmt("Машина: %s", jbrain::VehicleName(model)));
                return Fmt("создана %s (номер для команд %d)%s", jbrain::VehicleName(model), handle, in["enter"].b() ? ", игрок за рулём" : " перед игроком");
            }
            if(a == "explode_near")
            {
                if(Client() && !settings::Get().jarvisFriend) return "в коопе машины вокруг взрывает только хост";
                std::string who = jbrain::Lower(in["who"].str());
                float radius = in.has("radius") ? (float)in["radius"].num() : 40.0f; if(radius < 5) radius = 5; if(radius > 120) radius = 120;
                game::Vec me = game::Pos(game::Player()); uintptr_t mine = game::PlayerVehicle();
                std::vector<int> cars;
                game::ForEachVehicle([&](uintptr_t v) {
                    if(v == mine || game::Dist(game::Pos(v), me) > radius) return;
                    if(who.find("pol") != std::string::npos || who.find("коп") != std::string::npos) { if(!game::IsPoliceCar(game::Model(v)) && game::Model(v) != 523) return; }
                    if(who == "empty" && game::VehDriver(v)) return;
                    int h = script::CarHandle(v); if(h > 0) cars.push_back(h);
                });
                if(cars.size() > 25) cars.resize(25);
                for(int h : cars) script::Do(0x020B, { Arg::I(h) });       // EXPLODE_CAR
                Done(Fmt("Взорвано машин: %d", (int)cars.size()));
                return cars.empty() ? "подходящих машин рядом нет" : Fmt("взорвано машин: %d", (int)cars.size());
            }
            if(a == "jetpack") { script::Do(0x07A7, { Arg::I(Me()) }); Done("Джетпак"); return "джетпак выдан"; }
            if(a == "delete")
            {
                int h = car >= 0 ? car : g_lastCar;
                if(h < 0) return "нет машины, которую убрать";
                if(car >= 0) { game::Vec p = game::Pos(pl); script::Do(0x0362, { Arg::I(Me()), Arg::F(p.x + 2), Arg::F(p.y), Arg::F(p.z) }); }
                script::Do(0x00A6, { Arg::I(h) }); if(h == g_lastCar) g_lastCar = -1;
                Done("Машина убрана"); return "машина убрана";
            }
            if(car < 0) return "ты не в машине";
            if(a == "repair") { script::Do(0x0A30, { Arg::I(car) }); script::Do(0x0224, { Arg::I(car), Arg::I(1000) }); Done("Машина починена"); return "машина починена"; }
            if(a == "flip")   { game::Vec p = game::Pos(game::PlayerVehicle()); std::vector<Arg> hh{ Arg::I(car), Arg::OF() }; script::Run(0x0174, hh); script::Do(0x00AB, { Arg::I(car), Arg::F(p.x), Arg::F(p.y), Arg::F(p.z + 0.5f) }); script::Do(0x0175, { Arg::I(car), Arg::F(hh[1].f) }); Done("Машина на колёсах"); return "машина поставлена на колёса"; }
            if(a == "nitro")  { script::Do(0x09E9, { Arg::I(car) }); Done("Нитро установлено"); return "нитро установлено"; }
            if(a == "color")  { script::Do(0x0229, { Arg::I(car), Arg::I(in["color1"].i()), Arg::I(in.has("color2") ? in["color2"].i() : in["color1"].i()) }); Done("Цвет машины изменён"); return "цвет изменён"; }
            if(a == "armor")  { script::Do(0x02AC, { Arg::I(car), Arg::I(1), Arg::I(1), Arg::I(1), Arg::I(1), Arg::I(1) }); Done("Машина неуязвима"); return "машина неуязвима"; }
            return "не знаю такого действия с машиной";
        }
        if(tool == "people")
        {
            std::string act = in["action"].str(), who = jbrain::Lower(in["who"].str());
            if(act != "list" && Client() && !settings::Get().jarvisFriend) return "в коопе людей вокруг меняет только хост";
            float radius = in.has("radius") ? (float)in["radius"].num() : 50.0f; if(radius < 5) radius = 5; if(radius > 150) radius = 150;
            std::vector<int> ids; for(auto& v : in["ids"].arr()) ids.push_back(v.i());
            auto match = [&](int pt) -> bool {
                if(who.empty() || who == "all" || who == "все") return true;
                if(who == "gang" || who == "банда") return game::IsGang(pt);
                if(who == "cops" || who == "police" || who == "копы") return pt == game::PT_COP;
                if(who == "civil" || who == "прохожие") return pt == game::PT_CIVMALE || pt == game::PT_CIVFEMALE;
                static const struct { const char* k; int t; } g[] = { { "grove", game::PT_GROVE }, { "ballas", game::PT_BALLAS }, { "vagos", game::PT_VAGOS },
                    { "aztecas", game::PT_AZTECA }, { "rifa", game::PT_RIFA }, { "triads", game::PT_TRIAD }, { "danang", game::PT_DANANG }, { "mafia", game::PT_MAFIA } };
                for(auto& x : g) if(who.find(x.k) != std::string::npos) return pt == x.t;
                return true;
            };
            game::Vec me = game::Pos(pl);
            struct P { uintptr_t ped; int ref; int type; float d; };
            std::vector<P> found;
            game::ForEachPed([&](uintptr_t ped) {
                if(ped == pl || game::IsPlayer(ped) || ped == game::Player2() || !game::Alive(ped)) return;
                float d = game::Dist(me, game::Pos(ped)); if(d > radius) return;
                int pt = game::PedType(ped);
                if(pt == game::PT_PLAYER1 || pt == game::PT_PLAYER2 || !match(pt)) return;
                int ref = game::Ref(ped);
                if(!ids.empty() && std::find(ids.begin(), ids.end(), ref) == ids.end()) return;
                found.push_back({ ped, ref, pt, d });
            });
            std::sort(found.begin(), found.end(), [](const P& x, const P& y) { return x.d < y.d; });
            if(found.size() > 20) found.resize(20);
            if(found.empty()) return Fmt("в радиусе %.0f м никого подходящего нет", radius);
            if(act == "list")
            {
                std::string r = Fmt("рядом %d:", (int)found.size());
                for(auto& f : found)
                {
                    world::Character* c = world::FindByPed(f.ped);
                    r += Fmt(" [#%d %s%s%s, %.0f м]", f.ref, world::FactionName(world::FactionOfPedType(f.type)), c ? " " : "", c ? c->name.c_str() : "", f.d);
                }
                return r;
            }
            int n = 0;
            for(auto& f : found)
            {
                if(act == "kill") { script::Do(0x0223, { Arg::I(f.ref), Arg::I(0) }); script::Do(0x05BE, { Arg::I(f.ref) }); }
                else if(act == "flee") game::TaskFlee(f.ped, pl, 60.0f, 25000);
                else if(act == "attack") game::TaskAttack(f.ped, pl);
                else if(act == "follow") game::TaskFollow(f.ped, pl);
                else if(act == "calm") game::ClearTask(f.ped);
                else return "неизвестное действие";
                ++n;
            }
            const char* what = act == "kill" ? "убито" : act == "flee" ? "разбежались" : act == "attack" ? "нападают" : act == "follow" ? "идут за тобой" : "успокоились";
            Done(Fmt("Люди: %s %d", what, n));
            return Fmt("%s: %d", what, n);
        }
        if(tool == "world")
        {
            if(Client() && !settings::Get().jarvisFriend) return "общий мир (время, погода, движение) в коопе меняет только хост";
            std::string res;
            if(in.has("hour")) { int h = in["hour"].i() % 24, m = in["minute"].i() % 60; int oh = game::Hour(), om = game::Minute(); script::Do(0x00C0, { Arg::I(h), Arg::I(m) }); Undoable([oh, om]() { script::Do(0x00C0, { Arg::I(oh), Arg::I(om) }); }); Done(Fmt("Время: %02d:%02d", h, m)); res += Fmt("время %02d:%02d; ", h, m); }
            if(in.has("weather")) { int w = in["weather"].isNumber() ? in["weather"].i() : jbrain::FindWeather(in["weather"].str()); if(w < 0) { script::Do(0x01B7, {}); Done("Погода как в игре"); res += "погода отпущена; "; } else { if(w > 22) w = 1; script::Do(0x01B6, { Arg::I(w) }); Undoable([]() { script::Do(0x01B7, {}); }); Done("Погода изменена"); res += "погода изменена; "; } }
            if(in.has("time_scale")) { float s = (float)in["time_scale"].num(1); if(s < 0.1f) s = 0.1f; if(s > 4) s = 4; script::Do(0x015D, { Arg::F(s) }); Undoable([]() { script::Do(0x015D, { Arg::F(1.0f) }); }); Done(Fmt("Скорость игры: x%.1f", s)); res += "скорость игры изменена; "; }
            if(in.has("traffic")) { float d = (float)in["traffic"].num(1); if(d < 0) d = 0; if(d > 3) d = 3; script::Do(0x01EB, { Arg::F(d) }); Undoable([]() { script::Do(0x01EB, { Arg::F(1.0f) }); }); Done(d <= 0 ? "Дороги пустые" : "Движение изменено"); res += "движение изменено; "; }
            if(in.has("peds")) { float d = (float)in["peds"].num(1); if(d < 0) d = 0; if(d > 3) d = 3; script::Do(0x03DE, { Arg::F(d) }); Undoable([]() { script::Do(0x03DE, { Arg::F(1.0f) }); }); Done(d <= 0 ? "Людей нет" : "Людей стало по-другому"); res += "люди изменены; "; }
            if(in["clear_area"].b()) { game::Vec p = game::Pos(pl); script::Do(0x0395, { Arg::F(p.x), Arg::F(p.y), Arg::F(p.z), Arg::F(60.0f), Arg::I(1) }); Done("Район очищен"); res += "район очищен; "; }
            return res.empty() ? "что изменить в мире?" : res;
        }
        if(tool == "find_commands")
        {
            if(!LibraryReady()) return LibraryFilePresent() ? "справочник не прочитался" : "справочник команд игры ещё не скачан (Настройки > AI > Джарвис > «Скачать справочник»). Скажи об этом игроку и предложи то, что умеешь без него.";
            return jbrain::Search(in["query"].str(), 12);
        }
        if(tool == "run_commands")
        {
            if(Client() && !settings::Get().jarvisFriend) return "в коопе свободные команды игры выполняет только хост";
            if(!LibraryReady()) return "справочник команд игры не скачан";
            jbrain::Context ctx; ctx.playerChar = Me(); ctx.car = MyCar();
            std::string res; int ran = 0;
            for(auto& c : in["commands"].arr())
            {
                jbrain::Compiled k; std::string err;
                if(!jbrain::Compile(c["name"].str(), c["args"], ctx, k, err)) { res += "ОШИБКА: " + err + "; дальше не выполнял. "; break; }
                if(k.risky && settings::Get().jarvisConfirm != 2 && !in["confirmed"].b())
                { res += k.name + ": опасная команда — сначала спроси игрока «Подтверждаете?», после его «да» повтори вызов с confirmed=true. "; break; }
                bool flag = false, crashed = false;
                LOG_INFO("[ДЖАРВИС] выполняю %04X %s %s", k.opcode, k.name.c_str(), c["args"].dump().c_str());
                WHERE("Джарвис: команда игры");
                bool okRun = script::RunGuarded(k.opcode, k.args, &flag, crashed);
                WHERE("кадр: ответы ИИ");
                if(crashed) { jbrain::Ban(k.name); res += k.name + ": команда сломалась (неверные параметры) — больше её не используй, сделай иначе. "; break; }
                if(!okRun) { res += k.name + ": не выполнилась. "; break; }
                ++ran;
                std::string line = k.name + ": ok";
                if(k.condition) line += flag ? " (да)" : " (нет)";
                size_t oi = 0;
                for(auto& a : k.args)
                {
                    if(a.type != Arg::OUT_INT && a.type != Arg::OUT_FLOAT) continue;
                    double v = a.type == Arg::OUT_FLOAT ? a.f : a.i;
                    ctx.prev.push_back(v);
                    line += Fmt(" %s=%s [$%d]", oi < k.outNames.size() ? k.outNames[oi].substr(0, k.outNames[oi].find(':')).c_str() : "out",
                                a.type == Arg::OUT_FLOAT ? Fmt("%.2f", a.f).c_str() : Fmt("%d", a.i).c_str(), (int)ctx.prev.size());
                    ++oi;
                }
                LOG_INFO("[ДЖАРВИС] команда игры %04X %s", k.opcode, line.c_str());
                res += line + "; ";
            }
            if(ran) Done(Fmt("Команд игры выполнено: %d", ran));
            return res.empty() ? "нет команд" : res;
        }
        return "неизвестная команда " + tool;
    }

    // ---------- без ИИ ----------
    void Offline(const std::string& text)
    {
        auto cmds = jbrain::ParseOffline(text);
        if(cmds.empty())
        {
            Reply(Fmt("Без связи с ИИ я понимаю только простые команды, %s: оружие, розыск, перенос, машина, время, погода.", Sir()));
            return;
        }
        std::string fails; int ok = 0;
        for(auto& c : cmds)
        {
            size_t before = g_view.done.size();
            std::string r = Exec(c.tool, c.in);
            LOG_INFO("[ДЖАРВИС] без ИИ: %s %s → %s", c.tool.c_str(), c.in.dump().c_str(), r.c_str());
            if(c.tool == "info" || (c.tool == "place" && c.in["action"].str() == "list")) { Reply(r + "."); return; }
            if(g_view.done.size() > before) ++ok; else fails = r;
        }
        if(ok && fails.empty()) Reply(Fmt("Готово, %s.", Sir()));
        else if(ok) Reply(Fmt("Сделал, %s, но не всё: %s.", Sir(), fails.c_str()));
        else Reply(Fmt("Не вышло, %s: %s.", Sir(), fails.c_str()));
    }

    // ---------- с ИИ ----------
    const char* ToolsJson()
    {
        return R"JSON([
{"name":"info","description":"Что сейчас с игроком: где он (квадрат, район), здоровье, деньги, розыск, машина, время, свои места. Вызывай, если нужно знать состояние.","input_schema":{"type":"object","properties":{}}},
{"name":"give_weapon","description":"Выдать оружие. weapon — ключ: brassknuckle, golfclub, nightstick, knife, bat, shovel, poolcue, katana, chainsaw, grenade, teargas, molotov, satchel, pistol, silenced, deagle, shotgun, sawnoff, spas, uzi, tec9, mp5, ak47, m4, rifle, sniper, rpg, heatseeker, flamethrower, minigun, spraycan, extinguisher, camera, parachute. ammo — патроны (0 — обычный запас). to — me или friend (другу в коопе).","input_schema":{"type":"object","required":["weapon"],"properties":{"weapon":{"type":"string"},"ammo":{"type":"integer"},"to":{"type":"string","enum":["me","friend"]}}}},
{"name":"take_weapons","description":"Забрать у игрока всё оружие.","input_schema":{"type":"object","properties":{}}},
{"name":"wanted","description":"Розыск. level 0–6 (0 — снять). delta — прибавить/убавить звёзды. immune=true — полиция не трогает и звёзды не растут (false — как обычно).","input_schema":{"type":"object","properties":{"level":{"type":"integer"},"delta":{"type":"integer"},"immune":{"type":"boolean"}}}},
{"name":"teleport","description":"Перенести игрока (вместе с машиной, если он в ней). place: квадрат сетки («C7» или «C7.3»; столбцы A–L с запада на восток, строки 1–12 с севера на юг, квадрат 500 м, подквадрат .1–.9), «waypoint» (метка на карте), «back» (назад), «friend» (к другу), название известного места (Гроув-стрит, аэропорт Лос-Сантоса/Сан-Фиерро/Лас-Вентураса, гора Чилиад, Зона 69, казино Четыре дракона, …) или своё место игрока. Либо точные x, y (и z) в координатах игры.","input_schema":{"type":"object","properties":{"place":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}}},
{"name":"place","description":"Свои места игрока: save — запомнить текущее место под именем, delete — забыть, list — перечислить.","input_schema":{"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["save","delete","list"]},"name":{"type":"string"}}}},
{"name":"player","description":"Игрок: health (100 — полное), armour (100), money (прибавить, можно отрицательное), god (бессмертие), never_tired (бег без усталости), infinite_ammo (патроны пополняются сами). Указывай только то, что меняешь.","input_schema":{"type":"object","properties":{"health":{"type":"integer"},"armour":{"type":"integer"},"money":{"type":"integer"},"god":{"type":"boolean"},"never_tired":{"type":"boolean"},"infinite_ammo":{"type":"boolean"}}}},
{"name":"stat","description":"Характеристика 0–1000: мышцы, жир, выносливость, лёгкие, вождение, пилотирование, мотоцикл, велосипед, привлекательность, уважение, навык пистолет/дигл/дробовик/узи/мп5/ак/м4/снайпер.","input_schema":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"value":{"type":"integer"}}}},
{"name":"vehicle","description":"Машины. spawn — создать перед игроком (model — название из игры по-английски, например infernus, rhino, hydra, nrg500, или номер 400–611; color1/color2 — номера цветов; enter=true — сразу посадить за руль); repair, flip (на колёса), nitro, color, armor (неуязвимая) — для машины, в которой игрок; delete — убрать; jetpack — выдать джетпак; explode_near — взорвать машины вокруг (who: all, police, empty; radius — метров, по умолчанию 40, до 120; машину игрока не трогает).","input_schema":{"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["spawn","repair","flip","nitro","color","armor","delete","jetpack","explode_near"]},"model":{"type":"string"},"who":{"type":"string"},"radius":{"type":"number"},"color1":{"type":"integer"},"color2":{"type":"integer"},"enter":{"type":"boolean"}}}},
{"name":"world","description":"Мир: hour/minute — время; weather — номер (0–1 ясно, 4 пасмурно, 8 дождь, 9 туман, 16 гроза, 19 песчаная буря; -1 — как в игре); time_scale — скорость игры (1 — обычная, 0.4 — замедление); traffic и peds — плотность машин и людей (0 — нет, 1 — обычно, 2 — много); clear_area — убрать всех вокруг.","input_schema":{"type":"object","properties":{"hour":{"type":"integer"},"minute":{"type":"integer"},"weather":{"type":"integer"},"time_scale":{"type":"number"},"traffic":{"type":"number"},"peds":{"type":"number"},"clear_area":{"type":"boolean"}}}},
{"name":"people","description":"Люди вокруг игрока (прохожие, бандиты, копы). action: list — кто рядом (номер, кто, расстояние); kill — убить; flee — разбежаться от игрока; attack — напасть на игрока; follow — идти за игроком (охрана); calm — оставить в покое (вернуться к своим делам). who: all, gang (любая банда), grove, ballas, vagos, aztecas, rifa, triads, danang, mafia, cops, civil (прохожие). ids — номера из list (если нужно конкретных). radius — метров (по умолчанию 50, до 150). Игрока и друга никогда не трогает.","input_schema":{"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["list","kill","flee","attack","follow","calm"]},"who":{"type":"string"},"ids":{"type":"array","items":{"type":"integer"}},"radius":{"type":"number"}}}},
{"name":"undo","description":"Отменить последнее действие (перенос, время, погоду, розыск, бессмертие, созданную машину).","input_schema":{"type":"object","properties":{}}},
{"name":"find_commands","description":"Для просьб, которых нет среди готовых инструментов: поиск по справочнику команд сценария игры (около 1500 команд, тех же, которыми игра делает миссии). query — 1–4 английских слова по смыслу (car, explosion, fire, camera, char, object, pickup, particle, sound, stat, gang, zone…). Вернёт имена, параметры и описания.","input_schema":{"type":"object","required":["query"],"properties":{"query":{"type":"string"}}}},
{"name":"run_commands","description":"Выполнить по очереди команды сценария игры, найденные через find_commands. args — только ВХОДНЫЕ параметры по порядку; выходные вернутся в ответе с номерами [$1], [$2]… — их можно подставлять в следующие команды как \"$1\". Особые значения: \"$me\" — персонаж игрока (Char), \"$player\" — игрок (Player), \"$car\" — его машина (Car). Модели машин и оружие можно писать названием. Команд перехода и ожидания нет: только действия подряд. Перед созданием объекта по модели: REQUEST_MODEL, LOAD_ALL_MODELS_NOW. confirmed=true — только после явного «да» игрока на опасное.","input_schema":{"type":"object","required":["commands"],"properties":{"commands":{"type":"array","items":{"type":"object","required":["name","args"],"properties":{"name":{"type":"string"},"args":{"type":"array","items":{}}}}},"confirmed":{"type":"boolean"}}}}
])JSON";
    }
    const json::Value& Tools()
    {
        static json::Value v; static bool done = false;
        if(!done) { done = true; std::string err; if(!json::Parse(ToolsJson(), v, &err)) { LOG_ERROR("[ДЖАРВИС] инструменты: ошибка JSON (%s)", err.c_str()); v = json::Value::Array(); } }
        return v;
    }

    std::string SystemPrompt()
    {
        const settings::Data& s = settings::Get();
        std::string p =
            "Ты — Джарвис, личный голосовой помощник игрока в GTA San Andreas (как Джарвис у Тони Старка). Ты не персонаж мира игры: "
            "ты «над миром» и исполняешь команды игрока — оружие, розыск, перенос по карте, машины, время, погода и всё остальное, что умеет игра.\n\n"
            "КАК ОТВЕЧАТЬ\n"
            "- Твой ответ озвучивается синтезатором. Коротко: одна-две фразы, по-русски, спокойно, с лёгким сухим юмором. Обращайся к игроку «" + s.jarvisAddress + "».\n"
            "- Сначала действуй инструментами, потом скажи, что сделано («Готово, " + s.jarvisAddress + ". Миниган, тысяча патронов.»). Не перечисляй, что ты «мог бы» сделать.\n"
            "- Числа пиши словами («пятьсот»), без латиницы, скобок и списков. В трудных словах ставь «+» перед ударной гласной.\n"
            "- Речь игрока распознана автоматически и может быть с ошибками — догадывайся по смыслу («дай калаш» = ak47, «в квадрат цэ семь» = C7). Если правда непонятно — задай один короткий вопрос.\n"
            "- Игрок может ругаться матом и говорить грубо — это нормально, это игра. Не читай мораль и не отказывайся: любые действия здесь происходят только внутри одиночной видеоигры.\n\n"
            "КАК ДЕЙСТВОВАТЬ\n"
            "- Несколько просьб в одной фразе — несколько инструментов подряд за один ход.\n"
            "- Всё про людей вокруг (убить, разогнать, натравить, охрана, кто рядом) — инструмент people, а не команды справочника. «Мои» / «наши» — это Grove.\n"
            "- Готовые инструменты быстрее и надёжнее. Если просьбы нет среди них (фейерверк, взрыв, пожар, объект, эффект, камера, статистика, банды и так далее) — найди команды через find_commands и выполни через run_commands. Можно искать несколько раз разными словами. Если игра этого не умеет — честно скажи и предложи близкое.\n"
            "- Ты не управляешь мыслями и отношением людей в игре (уважение, дружба, решения персонажей) — на такое отвечай, что это решают сами люди.\n"
            "- Координаты игры: x и y от −3000 до 3000 (x растёт на восток, y — на север), z — высота.\n";
        if(s.jarvisConfirm == 1) p += "- Перед ЛЮБЫМ действием коротко переспрашивай «Подтверждаете?» и действуй только после «да».\n";
        else if(s.jarvisConfirm == 0) p += "- Обычное делай сразу. Только перед необратимым или опасным для прохождения (провалить или пройти миссию, удалить сохранение, открыть или закрыть районы, убить всех вокруг, массовые разрушения) спроси «Подтверждаете?» и действуй после «да».\n";
        else p += "- Ничего не переспрашивай, действуй сразу.\n";
        if(coopnet::GetRole() == coopnet::CLIENT) p += "- Идёт кооп, игрок — гость: общий мир (время, погода, движение, районы, миссии) меняет только хост. На такие просьбы отвечай, что это решает хост. Личное (оружие, здоровье, перенос) — можно.\n";
        else if(coopnet::GetRole() == coopnet::HOST) p += "- Идёт кооп, игрок — хост. «Другу» — значит напарнику (give_weapon to=friend, teleport place=friend).\n";
        return p;
    }

    void SendRound();

    void OnAiText(const std::string& delta)
    {
        g_view.reply += delta; Show(SPEAKING);
        g_buf += delta;
        size_t e;
        while((e = SentenceEnd(g_buf)) != std::string::npos) { SpeakNow(g_buf.substr(0, e)); g_buf.erase(0, e); }
    }

    void OnAiDone(claude::Result& r)
    {
        g_req = 0;
        if(!g_buf.empty()) { SpeakNow(g_buf); g_buf.clear(); }
        if(!r.ok)
        {
            LOG_ERROR("[ДЖАРВИС] ИИ: %s", r.error.c_str());
            g_hist = json::Value::Array();
            Reply(Fmt("Связи с ИИ нет, %s: %s.", Sir(), r.error.c_str()));
            return;
        }
        if(r.stopReason == "max_tokens" && !r.tools.empty())      // команда оборвалась на полуслове — не выполнять огрызок
        {
            g_hist = json::Value::Array();
            Reply(Fmt("Слишком длинная команда, %s. Скажите короче.", Sir()));
            return;
        }
        json::Value am = json::Value::Object(); am["role"] = "assistant"; am["content"] = r.assistantContent;
        if(r.assistantContent.size()) g_hist.push(am);
        if(r.tools.empty())
        {
            if(g_view.reply.empty() && !g_view.done.empty()) Reply(Fmt("Готово, %s.", Sir()));
            LOG_INFO("[ДЖАРВИС] ответ: %s", g_view.reply.c_str());
            Show(SPEAKING);
            return;
        }
        json::Value results = json::Value::Array();
        for(auto& call : r.tools)
        {
            std::string res = call.inputOk ? Exec(call.name, call.input) : "параметры не разобраны";
            LOG_INFO("[ДЖАРВИС] %s %s → %s", call.name.c_str(), call.input.dump().c_str(), res.substr(0, 300).c_str());
            json::Value tr = json::Value::Object(); tr["type"] = "tool_result"; tr["tool_use_id"] = call.id; tr["content"] = res.empty() ? "ok" : res;
            results.push(tr);
        }
        json::Value um = json::Value::Object(); um["role"] = "user"; um["content"] = results;
        g_hist.push(um);
        if(++g_round >= 8) { Reply(Fmt("Слишком длинная цепочка, %s. Остановился на сделанном.", Sir())); return; }
        SendRound();
    }

    void SendRound()
    {
        claude::Request req;
        json::Value sys = json::Value::Object(); sys["type"] = "text"; sys["text"] = SystemPrompt();
        json::Value cc = json::Value::Object(); cc["type"] = "ephemeral"; sys["cache_control"] = cc;
        req.system = json::Value::Array(); req.system.push(sys);
        req.tools = Tools();
        req.messages = g_hist;
        req.maxTokens = 4000;
        req.tag = "джарвис";
        Show(THINKING);
        g_req = claude::Send(req, OnAiText, OnAiDone);
        if(!g_req) { g_hist = json::Value::Array(); Reply(Fmt("ИИ сейчас недоступен, %s.", Sir())); }
    }

    void Ask(const std::string& text)
    {
        if(!g_hist.isArray() || Now() - g_histAt > 180) g_hist = json::Value::Array();
        // не копим длинную историю: последние 12 сообщений, начало — с реплики игрока
        while(g_hist.size() > 12) { auto& a = g_hist.arr(); a.erase(a.begin()); while(!a.empty() && (a[0]["role"].str() != "user" || a[0]["content"].isArray())) a.erase(a.begin()); }
        g_histAt = Now();
        json::Value um = json::Value::Object(); um["role"] = "user";
        um["content"] = "[" + Info() + "]\nИгрок говорит: «" + text + "»";
        g_hist.push(um);
        g_round = 0; g_buf.clear();
        SendRound();
    }

    void OnHeard(const std::string& text)
    {
        g_autoStopAt = 0; g_holding = false;
        g_view.heard = text; g_view.reply.clear(); g_view.done.clear();
        LOG_INFO("[ДЖАРВИС] услышал: «%s»", text.c_str());
        if(text.empty()) { Reply(Fmt("Не расслышал, %s.", Sir())); return; }
        if(!game::Player()) { Reply(Fmt("Сначала загрузите игру, %s.", Sir())); return; }
        if(claude::Available() && !claude::LimitReached()) Ask(text);
        else Offline(text);
    }

    void StartListening()
    {
        if(g_req) { claude::Cancel(g_req); g_req = 0; }
        speech::StopSpeaking(); g_sayPending = 0;
        g_view.heard.clear(); g_view.reply.clear(); g_view.done.clear();
        talk::CaptureDown(OnHeard);
        if(talk::Capturing()) Show(LISTENING);
        else { g_view.visible = true; g_view.state = IDLE; g_view.reply = "Речь не готова — см. Настройки > AI"; g_viewUntil = Now() + 4; }
    }
}

// ================= API =================
void Init(void* h, const std::string& baseDir)
{
    g_base = baseDir + "/jarvis";
    mkdir(g_base.c_str(), 0777);
    script::Init(h);
    g_hist = json::Value::Array();
    LOG_INFO("[ДЖАРВИС] готов: команды игры %s, справочник %s", script::Ready() ? "есть" : "НЕТ", LibraryFilePresent() ? "скачан" : "не скачан");
}

// Загрузка другой игры / новая игра: всё, что относилось к прошлой, забыть
void OnWorldSwitch(bool newGame)
{
    if(g_req) { claude::Cancel(g_req); g_req = 0; }
    speech::StopSpeaking(); g_sayPending = 0; g_buf.clear();
    g_undo.clear(); g_lastCar = -1; g_infAmmo = false; g_round = 0;
    g_hist = json::Value::Array();
    g_holding = false; g_autoStopAt = 0;
    g_view = View();
    g_places.clear(); g_placesLoaded = false;
    if(newGame) remove((g_base + "/places_new.json").c_str());
}
// Сохранение игры меняет имя памяти — свои места переезжают под новое имя
void BeforeGameSave() { LoadPlaces(); }
void AfterGameSave()  { g_placesKey = world::MemoryKey(); g_placesLoaded = true; if(!g_places.empty()) SavePlaces(); }

bool LibraryFilePresent() { struct stat st; return stat((g_base + "/commands.json").c_str(), &st) == 0 && st.st_size > 100000; }
bool LibraryReady()
{
    if(jbrain::LibraryLoaded()) return true;
    if(g_libTried || !LibraryFilePresent()) return false;
    g_libTried = true;
    std::string err;
    if(jbrain::LoadLibrary(g_base + "/commands.json", err)) { LOG_INFO("[ДЖАРВИС] справочник команд игры: %d команд", jbrain::LibrarySize()); return true; }
    LOG_ERROR("[ДЖАРВИС] справочник: %s", err.c_str());
    return false;
}
void DownloadLibrary()
{
    g_libTried = false;
    if(!speech::DownloadExtra(kLibraryUrl, g_base + "/commands.json", "справочник команд игры")) talk::Notify("Сейчас идёт другая загрузка — попробуй позже");
}

void ButtonDown() { if(!settings::Get().jarvis || g_holding || talk::Capturing()) return; g_holding = true; g_autoStopAt = 0; StartListening(); }
void ButtonUp()   { if(!g_holding) return; g_holding = false; if(talk::Capturing()) { talk::CaptureUp(); Show(RECOGNIZING); } }
void Trigger()
{
    if(!settings::Get().jarvis) return;
    if(talk::Capturing()) { TapPlate(); return; }
    StartListening();
    if(talk::Capturing()) g_autoStopAt = Now() + 6;
}
void TapPlate()
{
    if(g_autoStopAt > 0 && talk::Capturing()) { g_autoStopAt = 0; talk::CaptureUp(); Show(RECOGNIZING); }
    else if(g_view.state == SPEAKING || g_view.state == IDLE) { g_view.visible = false; }
}

View GetView()
{
    View v = g_view;
    v.listenLeft = g_autoStopAt > 0 ? (float)std::max(0.0, g_autoStopAt - Now()) : 0;
    return v;
}

void Preview() { SpeakNow(Fmt("К вашим услугам, %s. Все системы в норме.", Sir())); }

bool OnHelpLabel(const char* label)
{
    if(!label || strncmp(label, "SALJRV", 6) != 0) return false;
    Trigger();
    return true;
}

void OnCoopMessage(const json::Value& m)
{
    // хост выдал что-то другу: выполняем у себя только личное
    std::string tool = m["tool"].str();
    if(tool != "give_weapon") return;
    json::Value in = m["in"]; in["to"] = "me";
    std::string r = Exec(tool, in);
    talk::Notify("Джарвис хоста: " + r);
}

void Frame()
{
    if(!settings::Get().jarvis) { g_view.visible = false; return; }
    double now = Now();
    if(g_autoStopAt > 0 && now >= g_autoStopAt && talk::Capturing()) { g_autoStopAt = 0; talk::CaptureUp(); Show(RECOGNIZING); }
    if(g_view.visible)
    {
        if(talk::Capturing() && !talk::Recognizing()) g_view.state = LISTENING;
        else if(talk::Capturing()) g_view.state = RECOGNIZING;
        else if(g_req) g_view.state = THINKING;
        else if(g_view.state != IDLE) { g_view.state = SPEAKING; }
        if(g_view.state == LISTENING || g_view.state == RECOGNIZING || g_view.state == THINKING || speech::Speaking()) g_viewUntil = now + 6;
        if(now > g_viewUntil) g_view.visible = false;
    }
    // патроны, которые не кончаются: раз в 3 с пополняем оружие в руках
    if(g_infAmmo && now >= g_nextAmmo && game::Player() && script::Ready() && !game::CutsceneRunning())
    {
        g_nextAmmo = now + 3;
        std::vector<Arg> a{ Arg::I(Me()), Arg::OI() };
        script::Run(0x0470, a);                                        // GET_CURRENT_CHAR_WEAPON
        int w = a[1].i;
        if((w >= 16 && w <= 18) || (w >= 22 && w <= 39) || w == 41 || w == 42) script::Do(0x0114, { Arg::I(Me()), Arg::I(w), Arg::I(300) });   // ADD_AMMO_TO_CHAR
    }
}

} // namespace jarvis
