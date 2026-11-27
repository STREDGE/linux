// SA Coop — кооператив для GTA San Andreas Android 2.10 (arm64) через AML.
// Второй CJ на карте + вкладка «Co-op» в настройках игры (через SAUtils). Проверяет, что мод загружается,
// находит libGTASA.so и нужные символы игры, создаёт конфиг.

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/flogger.h>

#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

#include "coop/coop.h"
#include "settings.h"
#include "platform/android.h"
#include "ui/menu.h"
#include "ui/overlay.h"
#include "trace/ctrace.h"
#include "coop/modsync.h"
#include <sautils/isautils.h>

MYMODCFG(net.sacoop.mod, SA Coop, 1.6.2, SA Coop Team)

// Строки этого логгера идут в общий Documents/SA_LOG.txt (FLogger::Sink → ctrace)
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
    (void)code;
    ctrace::Crash(sig, (unsigned long)pc, (unsigned long)lr, library, (unsigned long)(pc - libaddr), coop::Where(),
                  (unsigned long)(pGTASA ? lr - pGTASA : 0));
}

static bool g_installed = false;
static bool g_overlayOk = false;

ON_MOD_LOAD()
{
    FLogger::Sink = [](bool error, const char* text) { ctrace::FromLog(error, text); };   // всё — в один файл SA_LOG.txt
    ctrace::Init();          // лог (всегда включён): Documents/SA_LOG.txt, общий с SA Living
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
    CTRACE("СЕССИЯ", "libGTASA.so base = 0x%lX, символов найдено %d из %d", (unsigned long)pGTASA, found, total);

    // Папка SA/cleo в корне памяти (рядом с Download, Documents): CLEO игрока читает скрипты оттуда.
    // Создаём, если её нет (на новой установке). Нужен «Доступ ко всем файлам».
    {
        const char* root = aml->GetInternalPath();   // обычно /storage/emulated/0
        if(root && root[0])
        {
            char path[512];
            snprintf(path, sizeof(path), "%s/SA", root);      mkdir(path, 0777);
            snprintf(path, sizeof(path), "%s/SA/cleo", root); bool ok = mkdir(path, 0777) == 0 || errno == EEXIST;
            g_fileLog.Info(ok ? "Папка SA/cleo на месте: %s" : "Не удалось создать %s (нет доступа ко всем файлам?)", path);
        }
    }

    {
        int mic = platform::HasPermission("android.permission.RECORD_AUDIO");
        g_fileLog.Info(mic == 1 ? "Микрофон: разрешение ЕСТЬ — голос сможет работать"
                     : mic == 0 ? "Микрофон: разрешения НЕТ (нет в APK или не включено в настройках Android)"
                                : "Микрофон: не удалось проверить разрешение");
        CTRACE("ГОЛОС", "разрешение микрофона: %s", mic == 1 ? "есть" : mic == 0 ? "НЕТ" : "не проверить");
    }

    settings::Data st = settings::Load();
    CTRACE("СЕССИЯ", "настройки: включён=%d режим=%s хост=%s порт=%u ник=«%s» мой ID=%s посредник=%s огонь по своим=%d общий розыск=%d",
           st.enabled, settings::RoleToString(st.net.role), st.net.hostAddress, (unsigned)st.net.port, st.net.nick,
           st.net.myId[0] ? st.net.myId : "-", st.net.relay[0] ? st.net.relay : "нет", st.friendlyFire, st.sharedWanted);

    g_fileLog.Info("SA Coop %s, символов найдено %d из %d", modinfo->VersionString(), found, total);
    g_fileLog.Info("Режим из настроек: %s, адрес хоста: %s, порт: %u",
                   settings::RoleToString(st.net.role), st.net.hostAddress, (unsigned)st.net.port);

    modsync::Init(st.enabled ? st.net.role : net::Role::Off);   // не «Подключиться» — вернуть моды, выключенные для коопа

    if(!st.enabled)
    {
        logger->Info("Мод выключен в настройках (Enabled = 0)");
        g_fileLog.Info("Мод выключен в настройках");
        return;
    }

    g_installed = coop::Install(hGTASA, &g_fileLog);
    if(g_installed)
    {
        coop::SetConfig(st.net);
        coop::SetFriendlyFire(st.friendlyFire);
        coop::SetSharedWanted(st.sharedWanted);
        g_overlayOk = overlay::Init(hGTASA, &g_fileLog);
        g_fileLog.Info(g_overlayOk ? "Своё меню Co-op готово" : "Своё меню не запустилось — будет простая вкладка SAUtils");
    }

    logger->Info("SA Coop: загрузка завершена");
}

// Меню — когда все моды загружены (SAUtils грузится после нас по алфавиту)
ON_ALL_MODS_LOAD()
{
    if(!g_installed) return;
    ISAUtils* sautils = (ISAUtils*)GetInterface("SAUtils");
    if(!sautils)
    {
        logger->Error("SAUtils не найден — вкладки Co-op не будет, режим берётся из net.sacoop.mod.ini");
        g_fileLog.Error("SAUtils не найден — вкладки Co-op в настройках не будет");
        return;
    }
    if(g_overlayOk)
    {
        overlay::AttachSAUtils(sautils);
        g_fileLog.Info("Кнопка CO-OP добавлена в главное меню и в паузу");
    }
    else
    {
        menu::Init(sautils);
        g_fileLog.Info("Вкладка Co-op добавлена в настройки игры");
    }
}
