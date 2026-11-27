#include "world.h"
#include "trace/ctrace.h"
#include "coop/mission.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>

namespace coop { namespace world {
namespace
{
    constexpr const char* kHostFile   = "SACoopWorld.b";   // хост: сюда сохраняет мир для друга
    constexpr const char* kClientFile = "SACoopHost.b";    // друг: сюда кладёт мир хоста
    constexpr int      kWindow      = 32;                  // кусков в пути
    constexpr uint64_t kResendUs    = 250000;
    constexpr uint64_t kReqEveryUs  = 1000000;
    constexpr int      kMaxFile     = 2 * 1024 * 1024;
    constexpr int      kLoadSlot    = 9;                   // номер ячейки для загрузки (имя файла подменяем)

    FLogger*      g_log = nullptr;
    net::Session* g_session = nullptr;
    bool          g_ok = false;

    void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool error, const char* fmt, ...)
    {
        char buf[512];
        va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        if(g_log) { if(error) g_log->Error("%s", buf); else g_log->Info("%s", buf); }   // → SA_LOG.txt (FLogger::Sink)
        else ctrace::FromLog(error, buf);
    }

    // --- функции игры ---
    char*  g_validName = nullptr;           // CGenericGameStorage::ms_ValidSaveName
    int*   g_lastResume = nullptr;          // lastSaveForResume
    void*  g_pcSave = nullptr;              // PcSaveHelper
    int  (*SaveSlot)(void* self, int8_t slot, bool noExtras) = nullptr;
    bool (*CheckSlotDataValid)(int slot, bool) = nullptr;
    void (*MenuLoadSlot)(int slot) = nullptr;
    bool (*MenuIsPlayingGame)(bool) = nullptr;
    void (*MainMenuOnResume)(void* self) = nullptr;
    void (*NewGameCheck)(void* self) = nullptr;     // StartGameScreen::OnNewGameCheck — как «Новая игра» в меню игры
    int  (*OSFileOpen)(int area, void** handle, const char* name, int access) = nullptr;
    int  (*OSFileRead)(void* h, void* buf, int n) = nullptr;
    int  (*OSFileWrite)(void* h, const void* buf, int n) = nullptr;
    int  (*OSFileSize)(void* h) = nullptr;
    int  (*OSFileClose)(void* h) = nullptr;

    // --- подмена имени файла сохранения ---
    bool g_hostSaving = false;              // хост сохраняет мир для друга
    bool g_loadRedirect = false;            // грузим мир коопа (ячейка kLoadSlot → g_loadName)
    const char* g_loadName = kClientFile;
    bool g_coopWorld = false;               // идёт кооп-мир: все ячейки — кооп-файлы SACoop_N.b
    int  g_origResume = -1;
    net::Role g_role = net::Role::Off;

    // --- видимая папка: SA/SACoop/Сохранения ---
    char g_folder[256] = "";
    char g_savesDir[320] = "";

    // --- лобби ---
    uint32_t g_launchId = 0;                // хост: номер «Запуска»; друг: что прислал хост
    int      g_launchSource = 0;
    char     g_launchName[64] = "";
    std::vector<uint8_t> g_launchFile;      // хост: файл, который получит друг (вместо свежего сохранения)
    bool     g_hostWantLoad = false;        // хост: загрузить выбранный файл
    bool     g_hostLoading = false;
    uint64_t g_nextLobbyUs = 0;
    uint32_t g_seenLaunch = 0;              // друг: на какой «Запуск» уже загрузились
    bool     g_lobbyHostInGame = false, g_lobbyHostLoading = false;
    bool     g_inGame = false;
    bool     g_wantNewGame = false;         // начать новую игру в безопасный момент (хост — после рассылки, друг — по команде хоста)
    uint64_t g_newGameAtUs = 0;             // хост: не раньше (лобби успевает уйти другу)
    uint64_t g_newGameSinceUs = 0;          // хост: когда снова в игре после «Новой игры»

    std::vector<std::string> g_notices;
    void Notice(const char* t) { if(g_notices.size() < 8) g_notices.push_back(t); }

    // --- хост: отправка ---
    uint32_t g_sendNonce = 0;
    bool     g_needSave = false;
    std::vector<uint8_t>  g_out;
    std::vector<uint8_t>  g_outAcked;
    std::vector<uint64_t> g_outSentUs;
    int      g_outAckedCount = 0;
    bool     g_outDoneLogged = false;
    int      g_lastPctNotice = -1;

    // --- друг: приём ---
    enum class CState { Idle, Requesting, Receiving, LoadRequested, Loaded, Done, Failed };
    CState   g_cs = CState::Idle;
    uint32_t g_myNonce = 0;
    uint64_t g_nextReqUs = 0;
    std::vector<uint8_t> g_in;
    std::vector<uint8_t> g_inGot;
    int      g_inGotCount = 0, g_inChunks = 0;
    uint32_t g_inTotal = 0;
    bool     g_teleport = false;
    bool     g_wantLoad = false;            // мир получен — загрузить в безопасный момент
    bool     g_usedHostWorld = false;       // мир хоста уже загружен в этом режиме
    char     g_failText[160] = "";
    bool     g_copyReq = false;             // друг попросил «сохранить мир хоста себе»
    bool     g_copyMode = false;            // идёт такая передача: файл — в ячейку, без загрузки
    constexpr const char* kCopySlotFile = "GTASAsf8.b";   // 8-я ячейка
    uint64_t g_startUs = 0;

    int Chunks(uint32_t total) { return (int)((total + net::kWorldChunkMax - 1) / net::kWorldChunkMax); }

    bool ReadGameFile(const char* name, std::vector<uint8_t>& out)
    {
        void* h = nullptr;
        if(OSFileOpen(1, &h, name, 0) != 0 || !h) return false;
        int n = OSFileSize(h);
        if(n <= 0 || n > kMaxFile) { OSFileClose(h); return false; }
        out.resize(n);
        int r = OSFileRead(h, out.data(), n);
        OSFileClose(h);
        return r >= 0;
    }

    bool WriteGameFile(const char* name, const std::vector<uint8_t>& data)
    {
        void* h = nullptr;
        if(OSFileOpen(1, &h, name, 1) != 0 || !h) return false;
        OSFileWrite(h, data.data(), (int)data.size());
        OSFileClose(h);
        std::vector<uint8_t> check;
        return ReadGameFile(name, check) && check.size() == data.size() && !memcmp(check.data(), data.data(), data.size());
    }

    void SendGame(const void* p, int len) { if(g_session) g_session->SendGame(p, len); }

    bool MakeFolders()
    {
        if(g_savesDir[0]) return true;
        const char* root = aml->GetInternalPath();
        if(!root || !root[0]) return false;
        snprintf(g_folder, sizeof(g_folder), "%s/SA/SACoop", root);
        char sa[300]; snprintf(sa, sizeof(sa), "%s/SA", root); mkdir(sa, 0777);
        mkdir(g_folder, 0777);
        snprintf(g_savesDir, sizeof(g_savesDir), "%s/Сохранения", g_folder);
        if(mkdir(g_savesDir, 0777) != 0 && errno != EEXIST)
        {
            Log(true, "[ЛОББИ] Не удалось создать %s: %s", g_savesDir, strerror(errno));
            g_savesDir[0] = 0;
            return false;
        }
        char readme[400]; snprintf(readme, sizeof(readme), "%s/ПРОЧТИ.txt", g_folder);
        if(FILE* f = fopen(readme, "wb"))
        {
            fputs("SA Coop — папка мини-сервера (кооп)\n\n"
                  "Сохранения/ — кооп-сохранения. Сюда попадает каждое сохранение, сделанное в кооп-игре\n"
                  "  (у точки сохранения), и «Мир хоста …» — мир хоста, сохранённый другом себе.\n"
                  "  Можно класть свои файлы сохранений GTA SA (.b) — хост увидит их в лобби.\n"
                  "Одиночные сохранения игры (GTASAsf1…10.b) кооп не трогает.\n", f);
            fclose(f);
        }
        return true;
    }

    bool ExportBytes(const char* name, const std::vector<uint8_t>& data)
    {
        if(!MakeFolders()) return false;
        char path[512]; snprintf(path, sizeof(path), "%s/%s", g_savesDir, name);
        FILE* f = fopen(path, "wb");
        if(!f) { Log(true, "[ЛОББИ] Не могу записать %s: %s", path, strerror(errno)); return false; }
        bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
        fclose(f);
        Log(ok ? false : true, "[ЛОББИ] %s %s (%u КБ)", ok ? "Сохранено в папку:" : "Не удалось записать", path, (unsigned)(data.size() / 1024));
        return ok;
    }

    bool ReadPath(const char* path, std::vector<uint8_t>& out)
    {
        FILE* f = fopen(path, "rb");
        if(!f) return false;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if(n <= 0 || n > kMaxFile) { fclose(f); return false; }
        out.resize(n);
        bool ok = fread(out.data(), 1, n, f) == (size_t)n;
        fclose(f);
        return ok;
    }

    void CoopSlotName(int slot, char* out, size_t len) { snprintf(out, len, "SACoop_%d.b", slot + 1); }

    // ===== хост =====
    void HostTick(bool connected, bool inGame, uint64_t now)
    {
        if(!connected) return;
        if(g_needSave && !g_launchFile.empty())
        {   // «Запуск» файла: друг получает выбранный файл как есть
            g_needSave = false;
            g_out = g_launchFile;
            int n = Chunks((uint32_t)g_out.size());
            g_outAcked.assign(n, 0); g_outSentUs.assign(n, 0);
            g_outAckedCount = 0; g_outDoneLogged = false; g_lastPctNotice = -1;
            Log(false, "[ЛОББИ] Отправляю другу «%s»: %u байт, %d кусков", g_launchName, (unsigned)g_out.size(), n);
        }
        if(g_needSave && inGame && !g_hostLoading)
        {
            g_needSave = false;
            uint64_t t0 = net::NowUs();
            g_hostSaving = true;
            int r = SaveSlot(g_pcSave, 0, false);
            g_hostSaving = false;
            std::vector<uint8_t> file;
            if(r != 0 || !ReadGameFile(kHostFile, file))
            {
                Log(true, "[МИР] Не удалось сохранить мир для друга (код %d) — друг загрузит свою игру", r);
                return;
            }
            g_out.swap(file);
            int n = Chunks((uint32_t)g_out.size());
            g_outAcked.assign(n, 0); g_outSentUs.assign(n, 0);
            g_outAckedCount = 0; g_outDoneLogged = false; g_lastPctNotice = -1;
            Log(false, "[МИР] Мир сохранён для друга: %u байт, %d кусков, за %.0f мс", (unsigned)g_out.size(), n,
                net::Since(net::NowUs(), t0) / 1000.0f);
        }
        if(g_out.empty() || g_outAckedCount >= (int)g_outAcked.size()) return;

        int inFlight = 0;
        for(size_t i = 0; i < g_outAcked.size(); ++i)
            if(!g_outAcked[i] && g_outSentUs[i] && net::Since(now, g_outSentUs[i]) < kResendUs) ++inFlight;
        for(size_t i = 0; i < g_outAcked.size() && inFlight < kWindow; ++i)
        {
            if(g_outAcked[i]) continue;
            if(g_outSentUs[i] && net::Since(now, g_outSentUs[i]) < kResendUs) continue;
            net::WorldChunkPacket p;
            p.hdr = net::MakeHeader(net::PKT_WORLD_CHUNK);
            p.nonce = g_sendNonce; p.total = (uint32_t)g_out.size();
            p.offset = (uint32_t)(i * net::kWorldChunkMax);
            uint32_t len = p.total - p.offset; if(len > (uint32_t)net::kWorldChunkMax) len = net::kWorldChunkMax;
            p.len = (uint16_t)len; p.pad = 0;
            memcpy(p.data, g_out.data() + p.offset, len);
            SendGame(&p, net::kWorldChunkHeaderSize + (int)len);
            g_outSentUs[i] = now;
            ++inFlight;
        }
    }

    // ===== друг =====
    // Загрузить файл коопа (у друга — мир хоста, у хоста — выбранное в лобби) через ячейку kLoadSlot
    bool StartLoadFile(const char* file, const char* what)
    {
        g_origResume = g_lastResume ? *g_lastResume : -1;
        g_loadName = file;
        g_loadRedirect = true;
        if(!CheckSlotDataValid(kLoadSlot, true))
        {
            g_loadRedirect = false;
            Log(true, "[МИР] Игра не приняла файл «%s» (%s) — загрузка отменена", what, file);
            return false;
        }
        if(g_lastResume) *g_lastResume = kLoadSlot;
        bool playing = MenuIsPlayingGame(true);
        Log(false, "[МИР] Загружаю %s (%s)", what, playing ? "из игры" : "из главного меню");
        if(playing) MenuLoadSlot(kLoadSlot);
        MainMenuOnResume(nullptr);   // из меню: проверить, загрузить, закрыть меню; из игры: закрыть меню
        return true;
    }

    void StartLoad()
    {
        if(!StartLoadFile(kClientFile, "мир хоста"))
        {
            g_cs = CState::Failed;
            snprintf(g_failText, sizeof(g_failText), "Мир хоста не прошёл проверку игры");
            return;
        }
        g_cs = CState::LoadRequested;
    }

    void ClientTick(bool connected, uint64_t now)
    {
        if(connected && g_copyReq && (g_cs == CState::Idle || g_cs == CState::Done || g_cs == CState::Failed))
        {
            g_copyReq = false; g_copyMode = true;
            g_cs = CState::Requesting; g_myNonce = (uint32_t)(now ^ (now >> 32)) | 1; g_nextReqUs = 0; g_startUs = now;
        }
        if(!connected) return;
        // хост нажал «Запуск» (новый) — загружаемся в его мир
        if(g_launchId && g_launchId != g_seenLaunch && !g_copyMode &&
           (g_cs == CState::Idle || g_cs == CState::Done || g_cs == CState::Failed))
        {
            g_seenLaunch = g_launchId;
            if(g_launchSource == net::LOBBY_NEW)
            {   // хост начал новую игру — начинаем свою новую одновременно (мир не передаём: он одинаковый)
                g_wantNewGame = true; g_newGameAtUs = 0;
                g_usedHostWorld = true;
                Log(false, "[ЛОББИ] Хост начал новую игру — начинаю новую игру вместе с ним");
                Notice("Хост начал новую игру — начинаем вместе");
                return;
            }
            g_usedHostWorld = false;
            g_cs = CState::Requesting; g_myNonce = (uint32_t)(now ^ (now >> 32)) | 1; g_nextReqUs = 0; g_startUs = now;
            Log(false, "[ЛОББИ] Хост запустил «%s» — загружаюсь в его мир", g_launchName);
        }
        if(g_cs == CState::Requesting && now >= g_nextReqUs)
        {
            net::WorldReqPacket r{ net::MakeHeader(net::PKT_WORLD_REQ), g_myNonce };
            SendGame(&r, sizeof(r));
            g_nextReqUs = now + kReqEveryUs;
            static uint32_t logged = 0;
            if(logged != g_myNonce) { logged = g_myNonce; Log(false, "[МИР] Прошу у хоста его мир (передача %08X)", g_myNonce); }
        }
        if(g_cs == CState::Receiving && g_inChunks && g_inGotCount >= g_inChunks && g_copyMode)
        {   // «Сохранить мир хоста себе»: в 8-ю ячейку (старое — в .bak), без загрузки
            std::vector<uint8_t> old;
            if(ReadGameFile(kCopySlotFile, old)) WriteGameFile("GTASAsf8.b.bak", old);
            bool ok = WriteGameFile(kCopySlotFile, g_in);
            {
                time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
                char name[96]; strftime(name, sizeof(name), "Мир хоста %Y-%m-%d %H-%M.b", &tmv);
                ExportBytes(name, g_in);
            }
            g_copyMode = false;
            g_cs = CState::Done;
            Log(ok ? false : true, ok ? "[МИР] Мир хоста сохранён тебе в ячейку 8 (прежнее — GTASAsf8.b.bak)" : "[МИР] Не удалось записать мир хоста в ячейку 8");
            Notice(ok ? "Мир хоста сохранён в ячейку 8 — можно продолжить одному" : "Не удалось сохранить мир хоста");
            return;
        }
        if(g_cs == CState::Receiving && g_inChunks && g_inGotCount >= g_inChunks)
        {
            if(!WriteGameFile(kClientFile, g_in))
            {
                g_cs = CState::Failed;
                snprintf(g_failText, sizeof(g_failText), "Не удалось записать мир хоста в память телефона");
                Log(true, "[МИР] Не удалось записать %s — проверь свободное место", kClientFile);
                return;
            }
            Log(false, "[МИР] Мир хоста получен: %u байт за %.1f с", (unsigned)g_inTotal, net::Since(now, g_startUs) / 1e6f);
            g_wantLoad = true;
            g_cs = CState::LoadRequested;
        }
    }
}

// Подмена имени файла сохранения
DECL_HOOKv(MakeValidSaveName, int slot)
{
    MakeValidSaveName(slot);
    if(!g_validName) return;
    if(g_hostSaving)        strcpy(g_validName, kHostFile);
    else if(g_loadRedirect && slot == kLoadSlot) strcpy(g_validName, g_loadName);
    else if(g_coopWorld && slot >= 0 && slot < 16)
    {   // кооп-мир: свои кооп-ячейки, одиночные сохранения не трогаем
        static int logged = -1;
        if(logged != slot) { logged = slot; Log(false, "[ЛОББИ] Кооп-мир: ячейка %d → кооп-файл (одиночные сохранения целы)", slot + 1); }
        CoopSlotName(slot, g_validName, 32);
    }
}

// Сохранение в кооп-мире: копия — в папку SA/SACoop/Сохранения
DECL_HOOK(int, SaveSlotHook, void* self, int8_t slot, bool noExtras)
{
    int r = SaveSlotHook(self, slot, noExtras);
    if(r == 0 && g_coopWorld && !g_hostSaving && slot >= 0)
    {
        char file[32]; CoopSlotName(slot, file, sizeof(file));
        std::vector<uint8_t> data;
        if(ReadGameFile(file, data))
        {
            char name[96]; snprintf(name, sizeof(name), "Кооп ячейка %d.b", slot + 1);
            ExportBytes(name, data);
        }
    }
    return r;
}

bool Install(void* h, FLogger* log)
{
    g_log = log;
    auto sym = [&](const char* s) { return (void*)aml->GetSym(h, s); };
    g_validName  = (char*)sym("_ZN19CGenericGameStorage16ms_ValidSaveNameE");
    g_lastResume = (int*)sym("lastSaveForResume");
    g_pcSave     = sym("PcSaveHelper");
    SaveSlot     = (int(*)(void*, int8_t, bool))sym("_ZN8C_PcSave8SaveSlotEab");
    CheckSlotDataValid = (bool(*)(int, bool))sym("_ZN19CGenericGameStorage18CheckSlotDataValidEib");
    MenuLoadSlot = (void(*)(int))sym("_Z13Menu_LoadSloti");
    MenuIsPlayingGame = (bool(*)(bool))sym("_Z18Menu_IsPlayingGameb");
    MainMenuOnResume = (void(*)(void*))sym("_ZN14MainMenuScreen8OnResumeEv");
    NewGameCheck = (void(*)(void*))sym("_ZN15StartGameScreen14OnNewGameCheckEv");
    OSFileOpen  = (int(*)(int, void**, const char*, int))sym("_Z11OS_FileOpen14OSFileDataAreaPPvPKc16OSFileAccessType");
    OSFileRead  = (int(*)(void*, void*, int))sym("_Z11OS_FileReadPvS_i");
    OSFileWrite = (int(*)(void*, const void*, int))sym("_Z12OS_FileWritePvPKvi");
    OSFileSize  = (int(*)(void*))sym("_Z11OS_FileSizePv");
    OSFileClose = (int(*)(void*))sym("_Z12OS_FileClosePv");
    void* mk = sym("_ZN19CGenericGameStorage17MakeValidSaveNameEi");
    g_ok = g_validName && SaveSlot && CheckSlotDataValid && MenuLoadSlot && MenuIsPlayingGame && MainMenuOnResume &&
           OSFileOpen && OSFileRead && OSFileWrite && OSFileSize && OSFileClose && mk;
    if(!g_ok) { Log(true, "[МИР] Не все функции сохранения найдены — друг будет загружать свою игру"); return false; }
    aml->Hook(mk, (void*)HookOf_MakeValidSaveName, (void**)&MakeValidSaveName);
    aml->Hook((void*)SaveSlot, (void*)HookOf_SaveSlotHook, (void**)&SaveSlotHook);
    SaveSlot = SaveSlotHook;   // свои сохранения мира для друга — без копии в папку (g_hostSaving)
    MakeFolders();
    if(!NewGameCheck) Log(true, "[ЛОББИ] Нет StartGameScreen::OnNewGameCheck — «Новая игра» в лобби недоступна");
    Log(false, "[ЛОББИ] Папка кооп-сохранений: %s", g_savesDir[0] ? g_savesDir : "(не создалась)");
    Log(false, "[МИР] Готово: друг сможет заходить сразу в мир хоста");
    return true;
}

void SetSession(net::Session* s) { g_session = s; }

bool RunDeferredLoad(bool fromMenuUpdate)
{
    if(!g_ok) return false;
    if(g_wantNewGame)
    {   // «Новая игра»: как кнопка игры (из главного меню — в обновлении меню, из игры — в игровом кадре)
        if(!NewGameCheck) { g_wantNewGame = false; return false; }
        if(g_newGameAtUs && net::NowUs() < g_newGameAtUs) return false;
        bool playing = MenuIsPlayingGame(true);
        if(playing == fromMenuUpdate) return false;
        g_wantNewGame = false;
        g_coopWorld = true;
        g_newGameSinceUs = 0;
        if(g_role == net::Role::Client) mission::ForceClientBlock(true);   // вступление и миссии — только у хоста
        Log(false, "[ЛОББИ] Начинаю новую игру (%s)", playing ? "из игры" : "из главного меню");
        NewGameCheck(nullptr);
        return true;
    }
    if(g_hostWantLoad)
    {
        bool playing = MenuIsPlayingGame(true);
        if(playing == fromMenuUpdate) return false;
        g_hostWantLoad = false;
        if(StartLoadFile(kHostFile, "выбранное сохранение")) { g_hostLoading = true; return true; }
        Notice("Игра не приняла это сохранение — выбери другое");
        g_launchFile.clear();
        return false;
    }
    if(!g_wantLoad) return false;
    bool playing = MenuIsPlayingGame(true);
    // Из главного меню грузим так же, как кнопка «Продолжить» (в обновлении меню);
    // из игры — в игровом кадре, как «Загрузить игру» из паузы.
    if(!playing && !fromMenuUpdate) return false;
    if(playing && fromMenuUpdate) return false;
    g_wantLoad = false;
    StartLoad();
    return g_cs == CState::LoadRequested;
}

void OnPacket(uint8_t type, const uint8_t* data, int len)
{
    if(!g_ok) return;
    uint64_t now = net::NowUs();
    if(type == net::PKT_LOBBY && len == sizeof(net::LobbyPacket))
    {
        if(g_role != net::Role::Client) return;
        const auto* l = (const net::LobbyPacket*)data;
        if(l->launchId != g_launchId || strncmp(l->saveName, g_launchName, sizeof(g_launchName)))
            Log(false, "[ЛОББИ] Хост: %s «%.63s»", l->launchId ? "запуск" : "выбирает", l->saveName);
        g_launchId = l->launchId; g_launchSource = l->source;
        memcpy(g_launchName, l->saveName, sizeof(g_launchName)); g_launchName[sizeof(g_launchName) - 1] = 0;
        g_lobbyHostInGame = l->hostInGame; g_lobbyHostLoading = l->hostLoading;
        return;
    }
    if(type == net::PKT_WORLD_REQ && len == sizeof(net::WorldReqPacket))
    {
        uint32_t nonce = ((const net::WorldReqPacket*)data)->nonce;
        if(nonce != g_sendNonce)
        {
            g_sendNonce = nonce; g_needSave = true; g_out.clear();
            Log(false, "[МИР] Друг просит мой мир (передача %08X) — сохраняю и отправляю", nonce);
            Notice("Друг загружается в твой мир…");
        }
    }
    else if(type == net::PKT_WORLD_ACK && len == sizeof(net::WorldAckPacket))
    {
        const auto* a = (const net::WorldAckPacket*)data;
        if(a->nonce != g_sendNonce || g_out.empty()) return;
        int i = (int)(a->offset / net::kWorldChunkMax);
        if(i >= 0 && i < (int)g_outAcked.size() && !g_outAcked[i]) { g_outAcked[i] = 1; ++g_outAckedCount; }
        int pct = g_outAcked.empty() ? 0 : g_outAckedCount * 100 / (int)g_outAcked.size();
        if(pct >= 100 && !g_outDoneLogged)
        {
            g_outDoneLogged = true;
            g_launchFile.clear();   // дальше друг (при переподключении) получит свежее сохранение
            Log(false, "[МИР] Мир отправлен другу целиком");
            Notice("Мир отправлен другу — он загружается");
        }
        (void)now;
    }
    else if(type == net::PKT_WORLD_CHUNK && len >= net::kWorldChunkHeaderSize)
    {
        const auto* p = (const net::WorldChunkPacket*)data;
        if(p->len > net::kWorldChunkMax || net::kWorldChunkHeaderSize + p->len != len) return;
        net::WorldAckPacket ack{ net::MakeHeader(net::PKT_WORLD_ACK), p->nonce, p->offset };
        SendGame(&ack, sizeof(ack));                          // подтверждаем всегда (вдруг ACK потерялся)
        if(p->nonce != g_myNonce) return;
        if(g_cs == CState::Requesting)
        {
            if(p->total == 0 || p->total > (uint32_t)kMaxFile) return;
            g_inTotal = p->total; g_inChunks = Chunks(p->total);
            g_in.assign(p->total, 0); g_inGot.assign(g_inChunks, 0); g_inGotCount = 0;
            g_cs = CState::Receiving;
            Log(false, "[МИР] Хост шлёт мир: %u байт", (unsigned)p->total);
        }
        if(g_cs != CState::Receiving || p->total != g_inTotal) return;
        int i = (int)(p->offset / net::kWorldChunkMax);
        if(i < 0 || i >= g_inChunks || p->offset + p->len > g_inTotal || g_inGot[i]) return;
        memcpy(g_in.data() + p->offset, p->data, p->len);
        g_inGot[i] = 1; ++g_inGotCount;
    }
}

void Tick(net::Role role, bool connected, bool inGame, uint64_t now)
{
    if(!g_ok) return;
    g_role = role; g_inGame = inGame;
    if(role == net::Role::Off && !inGame && g_coopWorld && !g_loadRedirect)
    {
        g_coopWorld = false;
        Log(false, "[ЛОББИ] Кооп выключен — сохранения снова одиночные (твои ячейки)");
    }
    if(role == net::Role::Host && g_launchSource == net::LOBBY_NEW && !g_wantNewGame && inGame)
    {   // новая игра идёт: через 30 с переподключившийся друг должен получать мир хоста, а не начинать заново
        if(!g_newGameSinceUs) g_newGameSinceUs = now;
        else if(net::Since(now, g_newGameSinceUs) > 30000000)
        {
            g_launchSource = net::LOBBY_CURRENT;
            Log(false, "[ЛОББИ] Новая игра идёт — переподключившийся друг загрузится в мой мир");
        }
    }
    if(role == net::Role::Host && connected && now >= g_nextLobbyUs)
    {
        g_nextLobbyUs = now + (g_wantNewGame ? 200000 : 1000000);   // перед «Новой игрой» — чаще, чтобы друг точно получил
        net::LobbyPacket l{};
        l.hdr = net::MakeHeader(net::PKT_LOBBY);
        l.launchId = g_launchId; l.source = (uint8_t)g_launchSource;
        l.hostInGame = inGame ? 1 : 0; l.hostLoading = (g_hostLoading || g_hostWantLoad || g_wantNewGame) ? 1 : 0;
        snprintf(l.saveName, sizeof(l.saveName), "%s", g_launchName);
        SendGame(&l, sizeof(l));
    }
    if(role == net::Role::Host) HostTick(connected, inGame, now);
    else if(role == net::Role::Client) ClientTick(connected, now);
}

void OnSessionReset()
{
    g_sendNonce = 0; g_needSave = false; g_out.clear(); g_outAcked.clear(); g_outSentUs.clear(); g_outAckedCount = 0;
    if(g_cs != CState::LoadRequested) { g_cs = CState::Idle; g_in.clear(); g_inGot.clear(); g_inGotCount = g_inChunks = 0; }
    g_usedHostWorld = false; g_teleport = false;
    g_seenLaunch = 0;
    if(g_role == net::Role::Client) { g_launchId = 0; g_launchName[0] = 0; g_launchSource = 0; }
}

void AfterGameLoaded()
{
    if(g_hostLoading)
    {
        g_hostLoading = false;
        g_loadRedirect = false;
        if(g_lastResume && g_origResume >= 0) *g_lastResume = g_origResume;
        g_coopWorld = true;
        Log(false, "[ЛОББИ] Хост загрузил «%s» — кооп-мир, сохранения в кооп-ячейки", g_launchName);
        Notice("Мир загружен — друг загружается следом");
        return;
    }
    if(g_cs != CState::LoadRequested)
    {   // загрузка не от коопа: из меню игры. В кооп-мире ячейки и так кооп-файлы — мир остаётся кооп.
        return;
    }
    g_loadRedirect = false;
    if(g_lastResume && g_origResume >= 0) *g_lastResume = g_origResume;
    g_coopWorld = true;
    g_usedHostWorld = true;
    g_teleport = true;
    g_cs = CState::Loaded;
    Log(false, "[МИР] Мир хоста загружен — переношу к хосту");
}

bool TeleportPending() { return g_teleport; }
void TeleportDone()
{
    g_teleport = false;
    if(g_cs == CState::Loaded) g_cs = CState::Done;
    Notice("Ты в мире хоста");
}

void RequestSaveCopy() { g_copyReq = true; Log(false, "[МИР] Прошу у хоста свежий мир — сохраню в ячейку 8"); }

Progress GetProgress(net::Link link)
{
    Progress p;
    if(!g_ok || g_copyMode) return p;
    switch(g_cs)
    {
        case CState::Done: case CState::Idle:
            if(g_cs == CState::Idle && !g_usedHostWorld && (link == net::Link::Requesting || link == net::Link::Denied))
            {
                p.active = true; p.percent = 5;
                snprintf(p.title, sizeof(p.title), "Подключение к хосту");
                snprintf(p.detail, sizeof(p.detail), link == net::Link::Denied ? "Хост отклонил запрос"
                                                     : "Стучусь к хосту… Хост должен нажать «Принять»");
            }
            return p;
        case CState::Requesting:
            p.active = true; p.percent = 15;
            snprintf(p.title, sizeof(p.title), "Подключение к миру хоста");
            snprintf(p.detail, sizeof(p.detail), "Хост сохраняет свой мир…");
            return p;
        case CState::Receiving:
            p.active = true;
            p.percent = 15 + (g_inChunks ? g_inGotCount * 65 / g_inChunks : 0);
            snprintf(p.title, sizeof(p.title), "Подключение к миру хоста");
            snprintf(p.detail, sizeof(p.detail), "Получаю мир хоста: %d из %d КБ", g_inGotCount * net::kWorldChunkMax / 1024, (int)(g_inTotal / 1024));
            return p;
        case CState::LoadRequested:
            p.active = true; p.percent = 85;
            snprintf(p.title, sizeof(p.title), "Подключение к миру хоста");
            snprintf(p.detail, sizeof(p.detail), "Загружаю мир хоста…");
            return p;
        case CState::Loaded:
            p.active = true; p.percent = 95;
            snprintf(p.title, sizeof(p.title), "Подключение к миру хоста");
            snprintf(p.detail, sizeof(p.detail), "Переношу к хосту…");
            return p;
        case CState::Failed:
            p.active = true; p.percent = 0;
            snprintf(p.title, sizeof(p.title), "Не удалось зайти в мир хоста");
            snprintf(p.detail, sizeof(p.detail), "%s", g_failText);
            return p;
    }
    return p;
}

std::vector<SaveEntry> ListSaves()
{
    std::vector<SaveEntry> v;
    if(!MakeFolders()) return v;
    DIR* d = opendir(g_savesDir);
    if(!d) return v;
    while(dirent* e = readdir(d))
    {
        size_t n = strlen(e->d_name);
        if(e->d_name[0] == '.' || n < 3 || strcasecmp(e->d_name + n - 2, ".b")) continue;
        SaveEntry s{};
        snprintf(s.path, sizeof(s.path), "%s/%s", g_savesDir, e->d_name);
        struct stat st;
        if(stat(s.path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(s.name, sizeof(s.name), "%.*s", (int)(n - 2), e->d_name);
        s.size = (long)st.st_size; s.mtime = st.st_mtime;
        v.push_back(s);
    }
    closedir(d);
    std::sort(v.begin(), v.end(), [](const SaveEntry& a, const SaveEntry& b) { return a.mtime > b.mtime; });
    return v;
}

const char* SavesFolder() { MakeFolders(); return g_savesDir; }

bool HostLaunch(const char* path, const char* name, char* why, size_t whyLen)
{
    auto fail = [&](const char* t) { snprintf(why, whyLen, "%s", t); Log(true, "[ЛОББИ] Запуск не удался: %s", t); return false; };
    if(!g_ok) return fail("сохранения игры недоступны моду");
    if(g_role != net::Role::Host) return fail("запускает только хост");
    if(g_hostLoading || g_hostWantLoad) return fail("уже загружается");
    if(!path)
    {
        if(!g_inGame) return fail("ты ещё не в игре — выбери сохранение");
        g_launchFile.clear();
        g_launchSource = net::LOBBY_CURRENT;
        snprintf(g_launchName, sizeof(g_launchName), "%s", name && name[0] ? name : "Текущая игра хоста");
        g_coopWorld = true;
    }
    else
    {
        std::vector<uint8_t> data;
        if(!ReadPath(path, data)) return fail("файл не читается (или больше 2 МБ)");
        if(data.size() < 50 * 1024) return fail("это не сохранение GTA SA (слишком маленький файл)");
        if(!WriteGameFile(kHostFile, data)) return fail("не удалось записать в память игры");
        g_launchFile.swap(data);
        g_launchSource = net::LOBBY_FILE;
        snprintf(g_launchName, sizeof(g_launchName), "%s", name ? name : "сохранение");
        g_hostWantLoad = true;
    }
    g_launchId = (uint32_t)(net::NowUs() & 0x7FFFFFFF) | 1;
    g_sendNonce = 0; g_out.clear();             // новая передача
    g_nextLobbyUs = 0;
    Log(false, "[ЛОББИ] Запуск: «%s» (%s)", g_launchName, path ? path : "текущая игра");
    return true;
}

bool NewGameAvailable() { return g_ok && NewGameCheck; }

bool HostNewGame(char* why, size_t whyLen)
{
    auto fail = [&](const char* t) { snprintf(why, whyLen, "%s", t); Log(true, "[ЛОББИ] Новая игра не началась: %s", t); return false; };
    if(!g_ok || !NewGameCheck) return fail("игра не даёт моду начать новую игру");
    if(g_role != net::Role::Host) return fail("запускает только хост");
    if(g_hostLoading || g_hostWantLoad || g_wantNewGame) return fail("уже загружается");
    g_launchFile.clear();
    g_launchSource = net::LOBBY_NEW;
    snprintf(g_launchName, sizeof(g_launchName), "Новая игра");
    g_launchId = (uint32_t)(net::NowUs() & 0x7FFFFFFF) | 1;
    g_sendNonce = 0; g_out.clear();
    g_nextLobbyUs = 0;
    g_coopWorld = true;
    g_wantNewGame = true; g_newGameAtUs = net::NowUs() + 800000; g_newGameSinceUs = 0;   // 0,8 с — друг успевает получить команду
    Log(false, "[ЛОББИ] Запуск: «Новая игра» — начинаем вместе с другом");
    return true;
}

Lobby GetLobby()
{
    Lobby l;
    l.launchId = g_launchId; l.source = g_launchSource;
    snprintf(l.saveName, sizeof(l.saveName), "%s", g_launchName);
    if(g_role == net::Role::Host) { l.hostInGame = g_inGame; l.hostLoading = g_hostLoading || g_hostWantLoad || g_wantNewGame; }
    else { l.hostInGame = g_lobbyHostInGame; l.hostLoading = g_lobbyHostLoading; }
    return l;
}

bool CoopWorld() { return g_coopWorld; }

bool PopNotice(char* out, size_t len)
{
    if(g_notices.empty()) return false;
    snprintf(out, len, "%s", g_notices.front().c_str());
    g_notices.erase(g_notices.begin());
    return true;
}

}}
