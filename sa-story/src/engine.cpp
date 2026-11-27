#include "engine.h"

#include <mod/amlmod.h>
#include <mod/config.h>
#include <stdio.h>

#include "log.h"
#include "script.h"
#include "story.h"
#include "ui.h"

using script::Arg;

namespace engine
{
namespace
{
    std::vector<Mission> g_missions;
    bool g_testMission = true;          // [Story] TestMission

    struct Running
    {
        const Mission* m = nullptr;
        std::vector<Step> steps;
        size_t cur = 0;
        bool entered = false;
        uint32_t stepStart = 0;
        std::map<std::string, int> cars, peds, entBlips;
        std::vector<std::string> enemies;
        std::vector<int> stepBlips;                          // убираются при переходе к следующему шагу
        std::vector<std::pair<std::string, std::string>> protect;
        uint32_t timerEnd = 0; bool timerOn = false;
        uint32_t nextCheck = 0;
        std::string failText;
        // GoTo «только в этой машине»: показана ли цель (или «вернись в машину»)
        int gotoBlip = -1; bool gotoInCar = true;
    };
    Running g_run;
    bool g_active = false;

    struct Contact { int blip = -1; };
    std::map<const Mission*, Contact> g_contacts;

    uint32_t Now() { return game::TimeMs(); }
    float Since(uint32_t t) { uint32_t n = Now(); return n >= t ? (n - t) / 1000.0f : 0.0f; }

    // ---------- обёртки команд ----------
    int  CmdOut(uint16_t op, std::vector<Arg> a) { return script::OutInt(op, std::move(a)); }
    bool Q(uint16_t op, std::vector<Arg> a) { return script::Check(op, std::move(a)); }
    void D(uint16_t op, std::vector<Arg> a) { script::Do(op, std::move(a)); }

    int  BlipCoord(const game::Vec& v) { return CmdOut(0x018A, { Arg::F(v.x), Arg::F(v.y), Arg::F(v.z), Arg::OI() }); }   // ADD_BLIP_FOR_COORD
    int  BlipCar(int car) { return CmdOut(0x0186, { Arg::I(car), Arg::OI() }); }                                         // ADD_BLIP_FOR_CAR
    int  BlipChar(int ped) { return CmdOut(0x0187, { Arg::I(ped), Arg::OI() }); }                                        // ADD_BLIP_FOR_CHAR
    void Friendly(int blip) { if(blip > 0) D(0x07E0, { Arg::I(blip), Arg::I(1) }); }                                     // SET_BLIP_AS_FRIENDLY
    void RemoveBlip(int& blip) { if(blip > 0) D(0x0164, { Arg::I(blip) }); blip = -1; }                                  // REMOVE_BLIP
    bool CarDead(int car) { return car <= 0 || Q(0x0119, { Arg::I(car) }); }                                            // IS_CAR_DEAD
    bool CharDead(int ped) { return ped <= 0 || Q(0x0118, { Arg::I(ped) }); }                                           // IS_CHAR_DEAD
    bool PlayerPlaying() { return Q(0x0256, { Arg::I(0) }); }                                                           // IS_PLAYER_PLAYING
    void SetOnMission(int v) { if(int32_t* p = game::Global(game::OnMissionVar())) *p = v; }

    void ClearStepBlips() { for(int& b : g_run.stepBlips) RemoveBlip(b); g_run.stepBlips.clear(); g_run.gotoBlip = -1; }

    void RemoveContactBlips() { for(auto& c : g_contacts) RemoveBlip(c.second.blip); }

    // ---------- конец миссии ----------
    void Cleanup()
    {
        WHERE("миссия: уборка");
        ClearStepBlips();
        for(auto& b : g_run.entBlips) RemoveBlip(b.second);
        for(auto& c : g_run.cars) if(c.second > 0) D(0x01C3, { Arg::I(c.second) });     // MARK_CAR_AS_NO_LONGER_NEEDED
        for(auto& p : g_run.peds) if(p.second > 0) D(0x01C2, { Arg::I(p.second) });     // MARK_CHAR_AS_NO_LONGER_NEEDED
        if(g_run.m) for(int md : g_run.m->models) D(0x0249, { Arg::I(md) });             // MARK_MODEL_AS_NO_LONGER_NEEDED
        ui::Timer(-1); ui::Counter("", -1); ui::ClearSubtitle();
        SetOnMission(0);
        g_run = Running();
        g_active = false;
    }

    void Fail(const std::string& why)
    {
        LOG("МИССИЯ", "ПРОВАЛ «%s»: %s (шаг %zu «%s»)", g_run.m ? g_run.m->key : "?", why.c_str(), g_run.cur,
            g_run.cur < g_run.steps.size() ? g_run.steps[g_run.cur].what.c_str() : "-");
        ui::Clear();
        ui::Result(false, "МИССИЯ ПРОВАЛЕНА", why);
        Cleanup();
    }

    void Pass()
    {
        const Mission* m = g_run.m;
        LOG("МИССИЯ", "ПРОЙДЕНА «%s», награда $%d", m->key, m->reward);
        D(0x0394, { Arg::I(1) });                                        // PLAY_MISSION_PASSED_TUNE
        if(m->reward > 0) D(0x0109, { Arg::I(0), Arg::I(m->reward) });  // ADD_SCORE
        ui::Clear();
        char b[32]; snprintf(b, sizeof(b), "+$%d", m->reward);
        ui::Result(true, "МИССИЯ ПРОЙДЕНА", m->reward > 0 ? b : "");
        if(m->id >= 0 && story::Get().progress <= m->id) story::SetProgress(m->id + 1);
        Cleanup();
    }

    // ---------- начало ----------
    void Start(const Mission* m)
    {
        WHERE("миссия: начало");
        RemoveContactBlips();
        g_run = Running();
        g_run.m = m;
        // шаг 0: загрузить модели (как родные миссии: REQUEST_MODEL → ждать HAS_MODEL_LOADED)
        g_run.steps.push_back({ "загрузка моделей",
            [m] { for(int md : m->models) D(0x0247, { Arg::I(md) }); },
            [m]() -> R {
                bool all = true;
                for(int md : m->models) if(!Q(0x0248, { Arg::I(md) })) { all = false; break; }
                if(all) return R::Next;
                if(Since(g_run.stepStart) > 6.0f) { D(0x038B, {}); return R::Next; }   // LOAD_ALL_MODELS_NOW — не ждать дольше
                return R::Run;
            } });
        Builder b(g_run.steps);
        m->build(b);
        g_active = true;
        SetOnMission(1);                      // игра считает, что идёт миссия: другие миссии и сохранение недоступны
        ui::Title(m->title);
        LOG("МИССИЯ", "НАЧАЛО «%s» (%s), шагов %zu", m->key, m->title, g_run.steps.size());
    }

    // ---------- общие проверки ----------
    bool GlobalChecks()
    {
        uint32_t now = Now();
        if(now < g_run.nextCheck && g_run.nextCheck - now < 1000) return true;
        g_run.nextCheck = now + 100;
        if(!PlayerPlaying())
        {
            Fail(game::Health(game::Player()) <= 0 ? "Ты погиб" : "Тебя задержали");
            return false;
        }
        for(auto& p : g_run.protect)
        {
            auto c = g_run.cars.find(p.first);
            if(c != g_run.cars.end() && CarDead(c->second)) { Fail(p.second); return false; }
            auto e = g_run.peds.find(p.first);
            if(e != g_run.peds.end() && CharDead(e->second)) { Fail(p.second); return false; }
        }
        if(g_run.timerOn)
        {
            int left = now >= g_run.timerEnd ? 0 : (int)((g_run.timerEnd - now + 999) / 1000);
            ui::Timer(left);
            if(left <= 0) { Fail("Время вышло"); return false; }
        }
        return true;
    }

    void MissionFrame()
    {
        if(!GlobalChecks()) return;
        if(g_run.cur >= g_run.steps.size()) { Pass(); return; }
        Step& s = g_run.steps[g_run.cur];
        if(!g_run.entered)
        {
            g_run.entered = true; g_run.stepStart = Now();
            LOG("МИССИЯ", "шаг %zu: %s", g_run.cur, s.what.c_str());
            WHERE("миссия: вход в шаг");
            if(s.enter) s.enter();
            if(!g_active) return;
        }
        WHERE("миссия: шаг");
        R r = s.tick ? s.tick() : R::Next;
        if(!g_active) return;
        if(r == R::Fail) { Fail(g_run.failText.empty() ? "Миссия сорвалась" : g_run.failText); return; }
        if(r == R::Next) { ClearStepBlips(); g_run.cur++; g_run.entered = false; }
    }

    // ---------- маркеры миссий (пешком в жёлтый круг) ----------
    void ContactsFrame()
    {
        uintptr_t pl = game::Player();
        if(!pl) return;
        bool otherMission = game::OnMission() || game::CutsceneRunning();
        game::Vec p = game::Pos(pl);
        const story::State& st = story::Get();
        for(const Mission& m : g_missions)
        {
            bool avail = (m.id < 0 && g_testMission) || (m.id >= 0 && m.id == st.progress);
            Contact& c = g_contacts[&m];
            if(!avail || otherMission) { RemoveBlip(c.blip); continue; }
            if(c.blip <= 0)
            {
                c.blip = BlipCoord(m.start);
                if(c.blip > 0) D(0x0165, { Arg::I(c.blip), Arg::I(4) });   // CHANGE_BLIP_COLOUR — жёлтый
            }
            if(game::Dist2D(p, m.start) > 120.0f) continue;
            // LOCATE_CHAR_ON_FOOT_3D со «сферой» = 1: игра рисует жёлтый круг маркера (каждый кадр)
            bool in = Q(0x00FF, { Arg::I(game::PedRef(pl)), Arg::F(m.start.x), Arg::F(m.start.y), Arg::F(m.start.z),
                                  Arg::F(1.2f), Arg::F(1.2f), Arg::F(2.0f), Arg::I(1) });
            if(in && PlayerPlaying()) { Start(&m); return; }
        }
    }
}

// ---------- сборщик шагов ----------
void Builder::Say(const std::string& who, const std::string& text, float sec, unsigned rgb, const std::string& voice)
{
    m_s.push_back({ "реплика " + who,
        [who, text, sec, rgb, voice] { ui::Subtitle(who, text, sec, rgb); (void)voice; /* голос — позже: файл voice */ },
        [sec]() -> R { return Since(g_run.stepStart) >= sec ? R::Next : R::Run; } });
}

void Builder::Wait(float sec)
{
    m_s.push_back({ "пауза", nullptr, [sec]() -> R { return Since(g_run.stepStart) >= sec ? R::Next : R::Run; } });
}

void Builder::Objective(const std::string& text) { m_s.push_back({ "цель", [text] { ui::Objective(text); }, nullptr }); }

void Builder::Do(const std::string& what, std::function<void()> fn) { m_s.push_back({ what, fn, nullptr }); }

void Builder::Car(const std::string& name, int model, game::Vec at, float heading, bool locked)
{
    m_s.push_back({ "машина " + name, [name, model, at, heading, locked] {
        D(0x0395, { Arg::F(at.x), Arg::F(at.y), Arg::F(at.z), Arg::F(4.0f), Arg::I(1) });       // CLEAR_AREA
        int h = CmdOut(0x00A5, { Arg::I(model), Arg::F(at.x), Arg::F(at.y), Arg::F(at.z), Arg::OI() });   // CREATE_CAR
        if(h <= 0) { LOG("ОШИБКА", "машина %s (модель %d) не создана", name.c_str(), model); return; }
        D(0x0175, { Arg::I(h), Arg::F(heading) });                                             // SET_CAR_HEADING
        if(locked) D(0x020A, { Arg::I(h), Arg::I(2) });                                        // LOCK_CAR_DOORS
        g_run.cars[name] = h;
    }, nullptr });
}

static int SpawnChar(const std::string& name, int pedType, int model, game::Vec at, float heading, int weapon, int ammo)
{
    int h = CmdOut(0x009A, { Arg::I(pedType), Arg::I(model), Arg::F(at.x), Arg::F(at.y), Arg::F(at.z), Arg::OI() });   // CREATE_CHAR
    if(h <= 0) { LOG("ОШИБКА", "пед %s (модель %d) не создан", name.c_str(), model); return -1; }
    D(0x0173, { Arg::I(h), Arg::F(heading) });                                                  // SET_CHAR_HEADING
    if(weapon > 0)
    {
        D(0x01B2, { Arg::I(h), Arg::I(weapon), Arg::I(ammo) });                                 // GIVE_WEAPON_TO_CHAR
        D(0x01B9, { Arg::I(h), Arg::I(weapon) });                                               // SET_CURRENT_CHAR_WEAPON
    }
    g_run.peds[name] = h;
    return h;
}

void Builder::Ally(const std::string& name, int pedType, int model, game::Vec at, float heading, int weapon, int ammo)
{
    m_s.push_back({ "союзник " + name, [=] {
        int h = SpawnChar(name, pedType, model, at, heading, weapon, ammo);
        if(h <= 0) return;
        int b = BlipChar(h); Friendly(b); if(b > 0) g_run.entBlips[name] = b;
    }, nullptr });
}

void Builder::Enemy(const std::string& name, int pedType, int model, game::Vec at, float heading, int weapon, int ammo, int accuracy)
{
    m_s.push_back({ "враг " + name, [=] {
        int h = SpawnChar(name, pedType, model, at, heading, weapon, ammo);
        if(h <= 0) return;
        D(0x02E2, { Arg::I(h), Arg::I(accuracy) });                                             // SET_CHAR_ACCURACY
        D(0x077A, { Arg::I(h), Arg::I(4), Arg::I(0) });                                         // SET_CHAR_RELATIONSHIP — ненавидит игрока
        D(0x05E2, { Arg::I(h), Arg::I(PlayerChar()) });                                         // TASK_KILL_CHAR_ON_FOOT
        int b = BlipChar(h); if(b > 0) g_run.entBlips[name] = b;                                // красная метка
        g_run.enemies.push_back(name);
    }, nullptr });
}

void Builder::GivePlayerWeapon(int weapon, int ammo)
{
    m_s.push_back({ "оружие игроку", [weapon, ammo] {
        D(0x01B2, { Arg::I(PlayerChar()), Arg::I(weapon), Arg::I(ammo) });                     // GIVE_WEAPON_TO_CHAR
    }, nullptr });
}

void Builder::Protect(const std::string& name, const std::string& failText)
{
    m_s.push_back({ "охранять " + name, [name, failText] { g_run.protect.push_back({ name, failText }); }, nullptr });
}

void Builder::EnterCar(const std::string& car, const std::string& objective)
{
    m_s.push_back({ "сесть в " + car,
        [car, objective] {
            ui::Objective(objective);
            int h = engine::Car(car);
            if(h > 0) { int b = BlipCar(h); Friendly(b); g_run.stepBlips.push_back(b); }
        },
        [car]() -> R {
            int h = engine::Car(car);
            if(CarDead(h)) { g_run.failText = "Машина уничтожена"; return R::Fail; }
            return Q(0x00DB, { Arg::I(PlayerChar()), Arg::I(h) }) ? R::Next : R::Run;            // IS_CHAR_IN_CAR
        } });
}

void Builder::GoTo(game::Vec at, float radius, const std::string& objective, const std::string& inCar)
{
    m_s.push_back({ "ехать к точке",
        [at, objective] {
            ui::Objective(objective);
            g_run.gotoBlip = BlipCoord(at); g_run.stepBlips.push_back(g_run.gotoBlip); g_run.gotoInCar = true;
        },
        [at, radius, objective, inCar]() -> R {
            int pc = PlayerChar();
            if(!inCar.empty())
            {
                int h = engine::Car(inCar);
                if(CarDead(h)) { g_run.failText = "Машина уничтожена"; return R::Fail; }
                bool inside = Q(0x00DB, { Arg::I(pc), Arg::I(h) });
                if(inside != g_run.gotoInCar)
                {   // вышел из машины — метка на машину и «вернись»; сел обратно — снова цель
                    g_run.gotoInCar = inside;
                    ClearStepBlips();
                    if(inside) { ui::Objective(objective); g_run.gotoBlip = BlipCoord(at); g_run.stepBlips.push_back(g_run.gotoBlip); }
                    else { ui::Objective("Вернись в машину"); int b = BlipCar(h); Friendly(b); g_run.stepBlips.push_back(b); }
                }
                if(!inside) return R::Run;
                return Q(0x0100, { Arg::I(pc), Arg::F(at.x), Arg::F(at.y), Arg::F(at.z), Arg::F(radius), Arg::F(radius), Arg::F(3.0f), Arg::I(1) })
                       ? R::Next : R::Run;                                                       // LOCATE_CHAR_IN_CAR_3D
            }
            return Q(0x00FE, { Arg::I(pc), Arg::F(at.x), Arg::F(at.y), Arg::F(at.z), Arg::F(radius), Arg::F(radius), Arg::F(3.0f), Arg::I(1) })
                   ? R::Next : R::Run;                                                           // LOCATE_CHAR_ANY_MEANS_3D
        } });
}

void Builder::KillEnemies(const std::string& objective)
{
    m_s.push_back({ "убить врагов",
        [objective] { ui::Objective(objective); },
        []() -> R {
            int alive = 0;
            for(const std::string& n : g_run.enemies)
            {
                int h = engine::Ped(n);
                if(h <= 0) continue;
                if(CharDead(h))
                {
                    auto b = g_run.entBlips.find(n);
                    if(b != g_run.entBlips.end() && b->second > 0) { RemoveBlip(b->second); LOG("МИССИЯ", "враг %s убит", n.c_str()); }
                }
                else alive++;
            }
            ui::Counter("Врагов", alive);
            if(alive == 0) { ui::Counter("", -1); g_run.enemies.clear(); return R::Next; }
            return R::Run;
        } });
}

void Builder::Wanted(int stars)
{
    m_s.push_back({ "розыск", [stars] { D(0x010D, { Arg::I(0), Arg::I(stars) }); }, nullptr });   // ALTER_WANTED_LEVEL
}

void Builder::LoseWanted(const std::string& objective)
{
    m_s.push_back({ "уйти от полиции", [objective] { ui::Objective(objective); },
        []() -> R { return game::Wanted() == 0 ? R::Next : R::Run; } });
}

void Builder::TimeLimit(int sec)
{
    m_s.push_back({ "таймер", [sec] {
        if(sec > 0) { g_run.timerOn = true; g_run.timerEnd = Now() + (uint32_t)sec * 1000u; }
        else { g_run.timerOn = false; ui::Timer(-1); }
    }, nullptr });
}

// ---------- общее ----------
void Register(const Mission& m) { g_missions.push_back(m); }
int Car(const std::string& n) { auto i = g_run.cars.find(n); return i == g_run.cars.end() ? -1 : i->second; }
int Ped(const std::string& n) { auto i = g_run.peds.find(n); return i == g_run.peds.end() ? -1 : i->second; }
int PlayerChar() { return game::PedRef(game::Player()); }

void Init()
{
    g_testMission = cfg->GetBool("TestMission", true, "Story");
    LOG("МИССИЯ", "движок готов: миссий %zu, пробная %s", g_missions.size(), g_testMission ? "есть" : "выключена");
}

void Frame()
{
    if(!story::On() || game::Paused() || !game::Player()) return;
    if(g_active) MissionFrame();
    else ContactsFrame();
}

void Abort(const char* why)
{
    if(g_active) LOG("МИССИЯ", "прервана «%s»: %s", g_run.m ? g_run.m->key : "?", why);
    g_run = Running();
    g_active = false;
    g_contacts.clear();          // метки принадлежали старому миру
    ui::Clear();
}

bool Busy() { return g_active; }

void OnNewStory()
{
    ui::Title("СВОЙ СЮЖЕТ", 4.0f);
    ui::Notice(g_testMission ? "Родные миссии выключены. Жёлтая метка на карте — пробная миссия" : "Родные миссии выключены", 8.0f);
    LOG("СЮЖЕТ", "новая игра со своим сюжетом началась");
}

void BeforeSave() { RemoveContactBlips(); }
}
