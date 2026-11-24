// SA Coop — кооператив для GTA San Andreas Android 2.10 (arm64) через AML.
// Этап 0.5: диагностика + проверка связи. Проверяет, что мод загружается,
// находит libGTASA.so и нужные символы игры, создаёт конфиг.

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/flogger.h>

#include <string.h>

#include "net/nettest.h"

MYMODCFG(net.sacoop.mod, SA Coop, 0.0.2, SA Coop Team)

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

    net::TestConfig net{};
    char role[16];
    cfg->GetString(role, sizeof(role), "Role", "host", "Network");          // host | client
    cfg->GetString(net.hostAddress, sizeof(net.hostAddress), "HostAddress", "100.64.0.1", "Network");
    net.port = (uint16_t)cfg->GetInt("Port", 7777, "Network");
    net.role = (strcasecmp(role, "client") == 0) ? net::Role::Client : net::Role::Host;
    cfg->Save();

    g_fileLog.Info("SA Coop %s, символов найдено %d из %d", modinfo->VersionString(), found, total);
    g_fileLog.Info("Роль: %s, адрес хоста: %s, порт: %u", net.role == net::Role::Host ? "хост" : "клиент",
                   net.hostAddress, (unsigned)net.port);

    if(!enabled)
    {
        logger->Info("Мод выключен в настройках (Enabled = 0)");
        g_fileLog.Info("Мод выключен в настройках");
        return;
    }

    // Этап 0.5: проверка связи между телефонами
    net::StartConnectionTest(net, &g_fileLog);

    logger->Info("SA Coop: загружен, тест связи запущен");
}
