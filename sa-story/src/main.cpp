// SA Story — свой сюжет для GTA SA Android 2.10 arm64 (AML). Отдельный мод: только сюжет.
// «Новая игра» = свой сюжет (родные миссии выключены), миссии — src/missions.cpp, движок — src/engine.cpp.

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

#include <time.h>

#include "engine.h"
#include "game.h"
#include "log.h"
#include "script.h"
#include "story.h"
#include "ui.h"

MYMODCFG(net.sastory.mod, SA Story, 0.1.0, SA Coop Team)

namespace missions { void RegisterAll(); }

const char* volatile g_where = "загрузка";

ON_GAME_CRASH()
{
    uintptr_t pc = mcontext ? (uintptr_t)mcontext->pc : 0;
    uintptr_t lr = mcontext ? (uintptr_t)mcontext->regs[30] : 0;
    (void)code;
    slog::Crash(sig, (unsigned long)pc, (unsigned long)lr, library, (unsigned long)(pc - libaddr), g_where);
}

static bool g_ok = false;
static double g_modSum = 0, g_modMax = 0; static int g_frames = 0; static uint32_t g_nextFpsLog = 0;

SDECL_HOOKv(GameProcessH)
{
    WHERE("игра (не мод)");
    GameProcessH();
    if(!g_ok) return;
    timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    WHERE("кадр: сюжет");
    story::Frame();
    WHERE("кадр: миссии");
    engine::Frame();
    WHERE("игра (не мод)");
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
    g_modSum += s; if(s > g_modMax) g_modMax = s; g_frames++;
    uint32_t now = game::TimeMs();
    if(story::On() && (now >= g_nextFpsLog || now + 60000 < g_nextFpsLog))
    {
        if(g_nextFpsLog && g_frames) LOG("ВРЕМЯ", "SA Story в кадре: в среднем %.2f мс, худший %.1f мс (%d кадров)", g_modSum * 1000 / g_frames, g_modMax * 1000, g_frames);
        g_nextFpsLog = now + 30000; g_modSum = g_modMax = 0; g_frames = 0;
    }
}

ON_MOD_LOAD()
{
    logger->SetTag("SAStory");
    slog::Init();
    void* h = aml->GetLibHandle("libGTASA.so");
    if(!h) { LOG("ОШИБКА", "libGTASA.so не найдена — мод выключен"); return; }
    if(!cfg->GetBool("Enabled", true, "General")) { LOG("СЕССИЯ", "мод выключен в настройках (Enabled = 0)"); return; }
    bool gameOk = game::Init(h);
    bool scriptOk = script::Init(h);
    bool uiOk = ui::Init(h);
    missions::RegisterAll();
    engine::Init();
    bool storyOk = story::Install(h);
    void* gp = (void*)aml->GetSym(h, "_ZN5CGame7ProcessEv");
    if(gp) aml->Hook(gp, (void*)HookOf_GameProcessH, (void**)&GameProcessH);
    g_ok = gameOk && scriptOk && storyOk && gp;
    LOG("СЕССИЯ", "SA Story %s: %s (экран %s)", modinfo->VersionString(), g_ok ? "готов" : "НЕ все функции игры найдены — сюжет выключен", uiOk ? "есть" : "нет");
}
