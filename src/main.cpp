// SA Coop — кооператив для GTA San Andreas Android 2.10 (arm64) через AML.
// Этап 1: второй CJ на карте (позиция, поворот, метка на радаре). Проверяет, что мод загружается,
// находит libGTASA.so и нужные символы игры, создаёт конфиг.

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/flogger.h>

#include <string.h>

#include "coop/coop.h"

MYMODCFG(net.sacoop.mod, SA Coop, 0.2.1, SA Coop Team)

// Файл: /sdcard/Documents/AMLModLogs/net.sacoop.mod/sacoop.txt
static FLogger   g_fileLog("sacoop.txt");
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

// Если игра упадёт — записать, где именно (адрес внутри библиотеки)
ON_GAME_CRASH()
{
    uintptr_t pc = mcontext ? (uintptr_t)mcontext->pc : 0;
    uintptr_t lr = mcontext ? (uintptr_t)mcontext->regs[30] : 0;
    g_fileLog.Error("ВЫЛЕТ: сигнал %d, код %d, библиотека %s, адрес +0x%lX, pc=0x%lX, lr=0x%lX (lr-libGTASA=+0x%lX)",
                    sig, code, library ? library : "?", (unsigned long)(pc - libaddr),
                    (unsigned long)pc, (unsigned long)lr, (unsigned long)(pGTASA ? lr - pGTASA : 0));
    g_fileLog.Error("Мод в момент вылета: %s", coop::Where());
}

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
    bool enabled = cfg->GetBool("Enabled", true, "General");
    cfg->GetBool("FriendlyFire", false, "General");

    net::Config net{};
    char role[16];
    cfg->GetString(role, sizeof(role), "Role", "echo", "Network");          // host | client | echo
    cfg->GetString(net.hostAddress, sizeof(net.hostAddress), "HostAddress", "100.64.0.1", "Network");
    net.port = (uint16_t)cfg->GetInt("Port", 7777, "Network");
    net.role = strcasecmp(role, "client") == 0 ? net::Role::Client
             : strcasecmp(role, "host")   == 0 ? net::Role::Host
                                               : net::Role::Echo;
#ifdef SACOOP_FORCE_ECHO
    net.role = net::Role::Echo; // тестовая сборка: настройки Role не нужны
#endif
    cfg->Save();

    g_fileLog.Info("SA Coop %s, символов найдено %d из %d", modinfo->VersionString(), found, total);
    g_fileLog.Info("Роль: %s, адрес хоста: %s, порт: %u", net.role == net::Role::Host ? "хост" : net.role == net::Role::Client ? "клиент" : "эхо (проверка в одиночку)",
                   net.hostAddress, (unsigned)net.port);

    if(!enabled)
    {
        logger->Info("Мод выключен в настройках (Enabled = 0)");
        g_fileLog.Info("Мод выключен в настройках");
        return;
    }

    coop::Install(hGTASA, net, &g_fileLog);

    logger->Info("SA Coop: загрузка завершена");
}
