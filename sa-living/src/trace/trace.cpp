#include "trace.h"
#include <onelog/onelog.h>

#include <mod/amlmod.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ai/claude.h"
#include "game/game.h"
#include "jarvis/script.h"
#include "settings.h"
#include "sim/police.h"
#include "speech/speech.h"
#include "talk/talk.h"
#include "util/log.h"
#include "world/world.h"

namespace trace
{
namespace
{
    std::mutex g_mx;
    std::condition_variable g_cv;
    std::string g_pending;               // ещё не записано в файл
    int g_fd = -1;
    std::string g_path;
    std::atomic<bool> g_started{ false };

    // ---------- запись ----------
    void Writer()
    {
        std::string chunk;
        for(;;)
        {
            {
                std::unique_lock<std::mutex> l(g_mx);
                g_cv.wait_for(l, std::chrono::milliseconds(250));
                chunk.swap(g_pending);
            }
            if(!chunk.empty()) onelog::Write(g_fd, chunk.data(), chunk.size());
            chunk.clear();
        }
    }

    void Stamp(char* buf, size_t n)
    {
        struct timeval tv; gettimeofday(&tv, nullptr);
        struct tm t; localtime_r(&tv.tv_sec, &t);
        snprintf(buf, n, "%02d:%02d:%02d.%03d", t.tm_hour, t.tm_min, t.tm_sec, (int)(tv.tv_usec / 1000));
    }

    void Push(const std::string& s)
    {
        if(!g_started) return;
        std::lock_guard<std::mutex> l(g_mx);
        g_pending += s;
        if(g_pending.size() > (8u << 20)) g_pending.erase(0, g_pending.size() - (4u << 20));   // запись отстала — не растём бесконечно
    }


    // оставить последние 10 файлов сессий (без сжатия — место занимают)

    std::string Prop(const char* k) { char v[PROP_VALUE_MAX] = ""; __system_property_get(k, v); return v; }

    // ---------- состояние для слежения ----------
    struct PedSeen { game::Vec pos; int st = -1, task = -2, car = 0; int hp = -1; };
    struct CarSeen { game::Vec pos; int hp = -1, drv = 0, pass = -1; };
    std::map<int, PedSeen> g_peds;
    std::map<int, CarSeen> g_cars;
    double g_nextPlayer = 0, g_nextWorld = 0, g_nextFull = 0, g_nextPerf = 0;
    double g_lastFrame = 0, g_maxDt = 0; int g_frames = 0;
    // прошлое состояние игрока (события)
    int  g_wanted = -1, g_state = -1, g_area = -1, g_money = INT32_MIN, g_wType = -1, g_task = -2;
    int  g_carRef = -1; float g_hp = -1, g_arm = -1;
    bool g_mission = false, g_cut = false, g_pause = false, g_lock = false, g_fade = false, g_alive = true, g_arrest = false;
    game::Vec g_lastPos; bool g_havePos = false;

    double Now() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

    std::string PedLine(uintptr_t p)
    {
        game::Vec v = game::Pos(p);
        int car = game::Vehicle(p) ? game::VehRef(game::Vehicle(p)) : 0;
        std::string s = world::Fmt("P#%d m=%d t=%d st=%d hp=%.0f task=%d", game::Ref(p), game::Model(p), game::PedType(p), game::PedState(p),
                                   game::Health(p), game::ActiveTaskType(p));
        if(car) s += world::Fmt(" car=V#%d", car);
        s += world::Fmt(" (%.1f,%.1f,%.1f) h=%.0f v=%.1f", v.x, v.y, v.z, game::Heading(p) * 57.29578f, game::Speed(p));
        int wt, wa; if(game::CurWeapon(p, wt, wa) && wt) s += world::Fmt(" w=%d/%d", wt, wa);
        s += world::Fmt(" тело=%d", game::SimplestTaskType(p));
        if(uintptr_t lo = game::LockOn(p)) s += " целится=" + game::EntName(lo);
        else if(uintptr_t ft = game::FightTarget(p)) s += " цель=" + game::EntName(ft);     // у педов цель — в их задачах (драка, прицел)
        if(world::Character* c = world::FindByPed(p)) s += world::Fmt(" [наш#%d %s]", c->id, c->name.c_str());
        if(game::IsScriptPed(p)) s += " [скрипт]";
        if(game::IsPlayer(p)) s += p == game::Player() ? " [ИГРОК]" : " [ИГРОК2]";
        if(game::AreaCode(p)) s += world::Fmt(" int=%d", game::AreaCode(p));
        return s;
    }
    std::string CarLine(uintptr_t v)
    {
        game::Vec p = game::Pos(v);
        uintptr_t d = game::VehDriver(v);
        int maxp = game::VehMaxPassengers(v);
        std::string s = world::Fmt("V#%d m=%d (%.1f,%.1f,%.1f) h=%.0f v=%.1f hp=%.0f", game::VehRef(v), game::Model(v), p.x, p.y, p.z,
                                   game::Heading(v) * 57.29578f, game::Speed(v), game::VehHealth(v));
        if(d) s += world::Fmt(" drv=P#%d", game::Ref(d));
        s += world::Fmt(" пас=%d/%d", maxp - game::VehFreeSeats(v), maxp);
        if(v == game::PlayerVehicle()) s += " [игрок]";
        return s;
    }

    void Snapshot(bool full, const char* why)
    {
        uintptr_t pl = game::Player(); if(!pl) return;
        int pu, pm, cu, cm; game::PoolUse(pu, pm, cu, cm);
        if(full) Line("МИР", "=== полный снимок (%s): педов %d/%d, машин %d/%d ===", why, pu, pm, cu, cm);
        std::map<int, PedSeen> peds; std::map<int, CarSeen> cars;
        game::ForEachPed([&](uintptr_t p) {
            int ref = game::Ref(p); if(!ref) return;
            PedSeen n; n.pos = game::Pos(p); n.st = game::PedState(p); n.task = game::ActiveTaskType(p) * 10000 + game::SimplestTaskType(p);
            n.car = game::Vehicle(p) ? game::VehRef(game::Vehicle(p)) : 0; n.hp = (int)game::Health(p);
            auto it = g_peds.find(ref);
            bool changed = it == g_peds.end() || game::Dist(it->second.pos, n.pos) > 0.5f || it->second.st != n.st ||
                           it->second.task != n.task || it->second.car != n.car || it->second.hp != n.hp;
            if(full || changed) Line(it == g_peds.end() ? "ПЕД+" : "ПЕД", "%s", PedLine(p).c_str());
            peds[ref] = n;
        });
        for(auto& kv : g_peds) if(!peds.count(kv.first)) Line("ПЕД-", "P#%d исчез (был в %.1f,%.1f,%.1f)", kv.first, kv.second.pos.x, kv.second.pos.y, kv.second.pos.z);
        game::ForEachVehicle([&](uintptr_t v) {
            int ref = game::VehRef(v); if(!ref) return;
            CarSeen n; n.pos = game::Pos(v); n.hp = (int)game::VehHealth(v);
            uintptr_t d = game::VehDriver(v); n.drv = d ? game::Ref(d) : 0; n.pass = game::VehMaxPassengers(v) - game::VehFreeSeats(v);
            auto it = g_cars.find(ref);
            bool changed = it == g_cars.end() || game::Dist(it->second.pos, n.pos) > 1.5f || it->second.hp != n.hp ||
                           it->second.drv != n.drv || it->second.pass != n.pass;
            if(full || changed) Line(it == g_cars.end() ? "МАШ+" : "МАШ", "%s", CarLine(v).c_str());
            cars[ref] = n;
        });
        for(auto& kv : g_cars) if(!cars.count(kv.first)) Line("МАШ-", "V#%d исчезла (была в %.1f,%.1f,%.1f)", kv.first, kv.second.pos.x, kv.second.pos.y, kv.second.pos.z);
        g_peds.swap(peds); g_cars.swap(cars);
    }

    void PlayerLine()
    {
        uintptr_t pl = game::Player(); if(!pl) return;
        game::Vec p = game::Pos(pl);
        uintptr_t v = game::PlayerVehicle();
        int wt = 0, wa = 0; game::CurWeapon(pl, wt, wa);
        game::Vec cp, cf; bool cam = game::Camera(cp, cf);
        uintptr_t lo = game::LockOn(pl);
        Line("ИГРОК", "(%.2f,%.2f,%.2f) h=%.0f v=%.1f hp=%.0f/%.0f бр=%.0f зв=%d $%d w=%d/%d st=%d task=%d тело=%d целится=%s камера=(%.1f,%.1f,%.1f)→(%.2f,%.2f,%.2f)%s%s int=%d | %02d:%02d %s%s%s%s%s",
             p.x, p.y, p.z, game::Heading(pl) * 57.29578f, game::Speed(v ? v : pl), game::Health(pl), game::MaxHealth(pl), game::Armour(pl),
             game::Wanted(), game::Money(), wt, wa, game::PedState(pl), game::ActiveTaskType(pl), game::SimplestTaskType(pl), lo ? game::EntName(lo).c_str() : "-",
             cam ? cp.x : 0, cam ? cp.y : 0, cam ? cp.z : 0, cam ? cf.x : 0, cam ? cf.y : 0, cam ? cf.z : 0,
             v ? world::Fmt(" car=V#%d m=%d", game::VehRef(v), game::Model(v)).c_str() : "", game::PadLocked() ? " УПРАВЛЕНИЕ_ЗАБЛОКИРОВАНО" : "",
             game::AreaCode(pl), game::Hour(), game::Minute(), game::ZoneName().c_str(),
             game::OnMission() ? " МИССИЯ" : "", game::CutsceneRunning() ? " КАТСЦЕНА" : "", game::CamFading() ? " ЗАТЕМНЕНИЕ" : "",
             police::Arresting() ? " АРЕСТ" : "");
    }

    // бой — каждый кадр для всех педов в 60 м (и игрока): урон с виновником, выстрелы, смена прицела, смерть
    struct Fight { float hp = -1; int ammo = -1, wtype = -1, shots = 0; double shotsAt = 0; uintptr_t lock = 0; bool alive = true; };
    std::map<int, Fight> g_fight;
    void Combat(double now)
    {
        uintptr_t pl = game::Player(); if(!pl) return;
        game::Vec me = game::Pos(pl);
        std::map<int, Fight> next;
        game::ForEachPed([&](uintptr_t p) {
            if(game::Dist(game::Pos(p), me) > 60) return;
            int ref = game::Ref(p); if(!ref) return;
            Fight f; auto it = g_fight.find(ref); if(it != g_fight.end()) f = it->second;
            float hp = game::Health(p);
            const std::string who = p == pl ? std::string("игрок") : game::EntName(p);
            if(f.hp >= 0 && hp < f.hp - 0.5f)
            {
                int wpn = 0; uint32_t t = 0; uintptr_t by = game::LastDamager(p, wpn, t);
                Line("УДАР", "%s: здоровье %.0f → %.0f, кто: %s, оружие %d", who.c_str(), f.hp, hp, game::EntName(by).c_str(), wpn);
            }
            bool alive = game::Alive(p);
            if(f.hp >= 0 && f.alive && !alive) { int wpn = 0; uint32_t t = 0; uintptr_t by = game::LastDamager(p, wpn, t); Line("ГИБЕЛЬ", "%s погиб, кто: %s, оружие %d", who.c_str(), game::EntName(by).c_str(), wpn); }
            int wt = 0, wa = 0; game::CurWeapon(p, wt, wa);
            if(f.ammo >= 0 && wt == f.wtype && wa < f.ammo && wt >= 22) { f.shots += f.ammo - wa; if(f.shotsAt == 0) f.shotsAt = now; }
            if(f.shots && now - f.shotsAt > 0.5)
            {
                uintptr_t lo = game::LockOn(p); if(!lo) lo = game::FightTarget(p);
                Line("ВЫСТРЕЛЫ", "%s: %d выстр. из оружия %d, целится в %s", who.c_str(), f.shots, wt, game::EntName(lo).c_str());
                f.shots = 0; f.shotsAt = 0;
            }
            uintptr_t lo = game::LockOn(p); if(!lo && p != pl) lo = game::FightTarget(p);   // у педов — цель из задач драки/прицела
            if(lo != f.lock && f.hp >= 0) Line("ПРИЦЕЛ", "%s → %s", who.c_str(), game::EntName(lo).c_str());
            f.hp = hp; f.ammo = wa; f.wtype = wt; f.lock = lo; f.alive = alive;
            next[ref] = f;
        });
        g_fight.swap(next);
    }

    // события игрока — проверяем каждый кадр
    void PlayerEvents()
    {
        uintptr_t pl = game::Player(); if(!pl) return;
        game::Vec p = game::Pos(pl);
        if(g_havePos && game::Dist(p, g_lastPos) > 30.0f)
            Line("ПЕРЕНОС", "игрок перенесён (%.1f,%.1f,%.1f) → (%.1f,%.1f,%.1f) [мод: %s]", g_lastPos.x, g_lastPos.y, g_lastPos.z, p.x, p.y, p.z, g_where ? g_where : "?");
        g_lastPos = p; g_havePos = true;
        int w = game::Wanted(); if(w != g_wanted) { if(g_wanted >= 0) Line("РОЗЫСК", "%d → %d [мод: %s]", g_wanted, w, g_where ? g_where : "?"); g_wanted = w; }
        int st = game::PedState(pl); if(st != g_state) { if(g_state >= 0) Line("СОСТОЯНИЕ", "игрок: %d → %d", g_state, st); g_state = st; }
        int tk = game::ActiveTaskType(pl); if(tk != g_task) { if(g_task != -2) Line("ЗАДАЧА-ИГРОКА", "%d → %d", g_task, tk); g_task = tk; }
        int ar = game::AreaCode(pl); if(ar != g_area) { if(g_area >= 0) Line("ИНТЕРЬЕР", "%d → %d", g_area, ar); g_area = ar; }
        uintptr_t v = game::PlayerVehicle(); int cr = v ? game::VehRef(v) : 0;
        if(cr != g_carRef)
        {
            if(g_carRef >= 0) { if(cr) Line("СЕЛ", "в машину %s", CarLine(v).c_str()); else Line("ВЫШЕЛ", "из машины V#%d", g_carRef); }
            g_carRef = cr;
        }
        float hp = game::Health(pl), arm = game::Armour(pl);
        if(g_hp >= 0 && (hp < g_hp - 0.5f || arm < g_arm - 0.5f)) Line("УРОН", "игрок: здоровье %.0f → %.0f, броня %.0f → %.0f", g_hp, hp, g_arm, arm);
        else if(g_hp >= 0 && (hp > g_hp + 0.5f || arm > g_arm + 0.5f)) Line("ЛЕЧЕНИЕ", "игрок: здоровье %.0f → %.0f, броня %.0f → %.0f", g_hp, hp, g_arm, arm);
        g_hp = hp; g_arm = arm;
        bool alive = game::Alive(pl); if(alive != g_alive) { Line(alive ? "ЖИВ" : "СМЕРТЬ", alive ? "игрок снова жив" : "игрок погиб (здоровье %.0f)", hp); g_alive = alive; }
        int m = game::Money(); if(g_money != INT32_MIN && m != g_money) Line("ДЕНЬГИ", "$%d → $%d", g_money, m); g_money = m;
        int wt = 0, wa = 0; game::CurWeapon(pl, wt, wa); if(wt != g_wType) { if(g_wType >= 0) Line("ОРУЖИЕ", "в руках: %d → %d (патроны %d)", g_wType, wt, wa); g_wType = wt; }
        auto flag = [](bool now, bool& was, const char* tag, const char* on, const char* off) { if(now != was) { Line(tag, "%s", now ? on : off); was = now; } };
        flag(game::OnMission(), g_mission, "МИССИЯ", "началась", "закончилась");
        flag(game::CutsceneRunning(), g_cut, "КАТСЦЕНА", "началась", "закончилась");
        flag(game::GamePaused(), g_pause, "ПАУЗА", "игра на паузе", "пауза снята");
        flag(game::PadLocked(), g_lock, "УПРАВЛЕНИЕ", "заблокировано", "разблокировано");
        flag(game::CamFading(), g_fade, "ЭКРАН", "затемнение/проявление началось", "затемнение закончилось");
        flag(police::Arresting(), g_arrest, "АРЕСТ", "сценарий ареста начался", "сценарий ареста закончился");
    }
}

std::string Path() { return g_path; }

void LineV(const char* tag, const char* fmt, va_list ap)
{
    if(!g_started) return;
    char ts[24]; Stamp(ts, sizeof(ts));
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    std::string s; s.reserve(strlen(buf) + 40);
    s += "И "; s += ts; s += " ["; s += tag; s += "] "; s += buf; s += '\n';
    Push(s);
}

void Line(const char* tag, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); LineV(tag, fmt, ap); va_end(ap);
}

void Init()
{
    if(g_started) return;
    const char* root = aml->GetInternalPath();
    onelog::RemoveOld(root, modinfo->GUID(), "saliving");    // старые saliving.txt, saliving_prev.txt, trace/
    g_fd = onelog::Open(root, &g_path);                       // общий с SA Coop: Documents/SA_LOG.txt
    if(g_fd < 0)
    {   // папка Documents недоступна — личная папка игры
        const char* data = aml->GetDataPath();
        g_path = std::string(data ? data : ".") + "/SA_LOG.txt";
        g_fd = open(g_path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    }
    if(g_fd < 0) { LogError("Лог: не удалось открыть файл (%s)", strerror(errno)); return; }
    g_started = true;
    std::thread(Writer).detach();
    time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
    LogInfo("Лог: %s (строки «И» — SA Living)", g_path.c_str());

    // ---- шапка ----
    Line("СЕССИЯ", "SA Living %s, начало %04d-%02d-%02d %02d:%02d:%02d", modinfo->VersionString(), tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    Line("СЕССИЯ", "телефон: %s %s, Android %s (API %s), процессор %s", Prop("ro.product.manufacturer").c_str(), Prop("ro.product.model").c_str(),
         Prop("ro.build.version.release").c_str(), Prop("ro.build.version.sdk").c_str(), Prop("ro.product.cpu.abi").c_str());
    Dl_info di;
    if(dladdr((void*)&Init, &di) && di.dli_fname)
    {
        std::string self = di.dli_fname, mods = self.substr(0, self.find_last_of('/'));
        Line("СЕССИЯ", "мод: %s", self.c_str());
        if(DIR* d = opendir(mods.c_str()))
        {
            std::string list;
            while(dirent* e = readdir(d))
            {
                std::string n = e->d_name; if(n.size() < 4 || n.compare(n.size() - 3, 3, ".so")) continue;
                struct stat st; long sz = stat((mods + "/" + n).c_str(), &st) == 0 ? (long)st.st_size : -1;
                list += world::Fmt(" %s(%ld КБ)", n.c_str(), sz / 1024);
            }
            closedir(d);
            Line("СЕССИЯ", "библиотеки в папке мода:%s", list.c_str());
        }
    }
    const settings::Data& s = settings::Get();
    Line("СЕССИЯ", "настройки: включён=%d ключ=%s модель=%s режим=%s речь=%s голоса=%d субтитры=%d инициатива=%d лимит=%d арест=%d свидетели=%d режиссёр=%d забывать=%d дн.",
         s.enabled, s.key.empty() ? "нет" : "есть", s.model.c_str(), s.effort.c_str(), s.engine ? "Google" : "своя", s.voices, s.subtitles, s.initiative,
         s.dailyLimit, s.realArrest, s.witnesses, s.director, s.forgetDays);
    Line("СЕССИЯ", "Джарвис: включён=%d кнопка=%d голос=%s обращение=%s подтверждение=%d другу=%d",
         s.jarvis, s.jarvisButton, s.jarvisVoice.empty() ? "авто" : s.jarvisVoice.c_str(), s.jarvisAddress.c_str(), s.jarvisConfirm, s.jarvisFriend);
}

void Frame()
{
    if(!g_started) return;
    double now = Now();
    // производительность
    if(g_lastFrame > 0)
    {
        double dt = now - g_lastFrame;
        if(dt > g_maxDt) g_maxDt = dt;
        if(dt > 0.15) Line("ПОДВИСАНИЕ", "кадр %.0f мс [мод перед этим: %s]", dt * 1000, g_where ? g_where : "?");
    }
    g_lastFrame = now; ++g_frames;
    if(now >= g_nextPerf)
    {
        if(g_nextPerf > 0)
        {
            int pu, pm, cu, cm; game::PoolUse(pu, pm, cu, cm);
            speech::Status ss = speech::Get();
            Line("FPS", "%.0f кадр/с (худший кадр %.0f мс), педов %d/%d, машин %d/%d, ИИ ждём %d, речь: %s%s",
                 g_frames / (now - (g_nextPerf - 5.0)), g_maxDt * 1000, pu, pm, cu, cm, claude::InFlight(),
                 ss.busy ? "загрузка " : "", speech::Speaking() ? "говорит" : "тихо");
        }
        g_nextPerf = now + 5.0; g_frames = 0; g_maxDt = 0;
    }
    if(!game::Player()) return;
    PlayerEvents();
    Combat(now);
    if(now >= g_nextPlayer) { g_nextPlayer = now + 0.25; PlayerLine(); }
    if(now >= g_nextWorld)
    {
        bool full = now >= g_nextFull;
        if(full) g_nextFull = now + 15.0;
        g_nextWorld = now + 1.0;
        Snapshot(full, "раз в 15 с");
    }
}

void Mark(const std::string& text)
{
    Line("ОТМЕТКА", "================ ОТМЕТКА ИГРОКА: %s ================", text.empty() ? "(без слов)" : text.c_str());
    PlayerLine();
    Snapshot(true, "отметка");
    Line("ОТМЕТКА", "================ конец снимка ================");
}

void Crash(int sig, unsigned long pc, unsigned long lr, const char* lib, unsigned long off, const char* where)
{
    if(!g_started || g_fd < 0) return;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "И XX:XX:XX.XXX [ВЫЛЕТ] сигнал %d, библиотека %s, адрес +0x%lX, pc=0x%lX, lr=0x%lX, мод: %s\n", sig, lib ? lib : "?", off, pc, lr, where ? where : "?");
    // в обработчике вылета: дописать хвост очереди без ожидания потока записи
    if(g_mx.try_lock()) { std::string rest; rest.swap(g_pending); g_mx.unlock(); onelog::Write(g_fd, rest.data(), rest.size()); }
    onelog::Write(g_fd, buf, n > 0 ? (size_t)n : 0);
}
}
