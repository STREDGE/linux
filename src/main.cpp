// SA Coop — кооператив для GTA San Andreas Android 2.10 (arm64) через AML.
// Этап 0: диагностический мод. Проверяет, что мод загружается,
// находит libGTASA.so и нужные символы игры, создаёт конфиг.

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

MYMODCFG(net.sacoop.mod, SA Coop, 0.0.1, SA Coop Team)

static uintptr_t pGTASA = 0;
static void*     hGTASA = nullptr;

struct SymCheck { const char* sym; const char* desc; };

// Символы, на которые опирается кооп. Если какого-то нет в 2.10 —
// узнаем сразу из лога, до того как писать сетевой код.
static const SymCheck g_symbols[] = {
    { "_ZN6CWorld7PlayersE",                 "CWorld::Players (слоты игроков)" },
    { "_Z13FindPlayerPedi",                  "FindPlayerPed(int)" },
    { "_ZN6CPools11ms_pPedPoolE",            "CPools::ms_pPedPool" },
    { "_ZN6CPools15ms_pVehiclePoolE",        "CPools::ms_pVehiclePool" },
    { "_ZN11CTheScripts7ProcessEv",          "CTheScripts::Process (скрипты миссий)" },
    { "_ZN14CRunningScript17ProcessOneCommandEv", "CRunningScript::ProcessOneCommand" },
    { "_ZN5CGame7ProcessEv",                 "CGame::Process (игровой кадр)" },
};

ON_MOD_LOAD()
{
    logger->SetTag("SACoop");
    logger->Info("SA Coop %s загружается", modinfo->VersionString());

    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    if(!pGTASA || !hGTASA)
    {
        logger->Error("libGTASA.so не найдена — мод отключён");
        return;
    }
    logger->Info("libGTASA.so base = 0x%lX", (unsigned long)pGTASA);

    int found = 0, total = sizeof(g_symbols) / sizeof(g_symbols[0]);
    for(const auto& s : g_symbols)
    {
        uintptr_t addr = aml->GetSym(hGTASA, s.sym);
        if(addr) { ++found; logger->Info("[OK]   %s = +0x%lX", s.desc, (unsigned long)(addr - pGTASA)); }
        else     {          logger->Error("[НЕТ]  %s (%s)", s.desc, s.sym); }
    }
    logger->Info("Символов найдено: %d из %d", found, total);

    // Настройки (файл конфига AML для мода net.sacoop.mod)
    delete cfg->Bind("Enabled",      true,          "General");
    delete cfg->Bind("FriendlyFire", false,         "General");
    delete cfg->Bind("Role",         "host",        "Network");   // host | client
    delete cfg->Bind("HostAddress",  "100.64.0.1",  "Network");
    delete cfg->Bind("Port",         7777,          "Network");
    cfg->Save();

    logger->Info("SA Coop: этап 0 завершён");
}
