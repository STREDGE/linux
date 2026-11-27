#include "story.h"

#include <mod/amlmod.h>
#include <mod/config.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "game.h"
#include "log.h"
#include "script.h"
#include "ui.h"

namespace story
{
namespace
{
    State g_st;
    bool g_installed = false;
    bool g_newGameStory = true;      // [Story] NewGameIsStory
    int  g_cfgHero = 0;              // [Story] HeroModel

    // перехваченные данные игры
    char* g_validSaveName = nullptr;   // CGenericGameStorage::ms_ValidSaveName
    char* g_loadFileName = nullptr;    // CGenericGameStorage::ms_LoadFileName
    int32_t* g_scriptParams = nullptr; // ScriptParams
    void (*CollectParameters)(uintptr_t script, int16_t n) = nullptr;
    void (*UpdateCompareFlag)(uintptr_t script, uint8_t v) = nullptr;

    // события из перехватов → обработка в кадре (внутри сценария игры команды не запускаем)
    bool g_introSkipped = false;      // родное вступление не запущено — открыть экран, дать управление
    bool g_blockedLaunch = false;     // заблокирован запуск родной миссии — вернуть управление/камеру
    int  g_blockedMission = -1;
    uint32_t g_lastBlockLog = 0;
    uint32_t g_heroTryMs = 0;
    uint32_t g_nextContactScan = 0;

    // Потоки main.scm, у которых маркер запускает миссию СЮЖЕТА (разбор mainV1.scm: поток → номера миссий после
    // CAN_PLAYER_START_MISSION). Подработки, гонки, школы, спортзалы, покупки — не трогаем.
    const char* const kStoryThreads[] = {
        "INT", "SWEET", "CRASH", "RYDER", "SMOKE", "STRAP", "LA1FIN", "CESAR", "BCESAR", "BCRASH", "CAT", "TRU",
        "GARAGE", "SCRASH", "WUZI", "SYND", "STEAL", "ZERO", "DESERT", "CASINO", "VCRASH", "DOC", "HEIST",
        "MANSION", "GROVE", "RIOT",
    };
    bool StoryThread(const char* name)
    {
        char n[9] = {}; memcpy(n, name, 8);
        for(const char* t : kStoryThreads) if(!strcmp(n, t)) return true;
        return false;
    }
    // Номера миссий сюжета (mainV1.scm): 2 — вступление, 11…112 — сюжет; 35 — уличные гонки, 71 — автошкола,
    // 83 — лётная школа: это занятия, их не трогаем.
    bool StoryMission(int mi) { return mi == 2 || (mi >= 11 && mi <= 112 && mi != 35 && mi != 71 && mi != 83); }

    // ---------- файл состояния рядом с сохранением ----------
    std::string Dir()
    {
        const char* base = aml->GetAndroidDataPath();
        std::string d = std::string(base ? base : "/sdcard") + "/SAStory";
        mkdir(d.c_str(), 0777);
        return d;
    }
    std::string KeyOf(const char* name)
    {
        std::string s = name ? name : "";
        size_t sl = s.find_last_of("/\\");
        if(sl != std::string::npos) s = s.substr(sl + 1);
        std::string k;
        for(char c : s) k += (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') ? c : '_';
        return k.empty() ? "default" : k;
    }
    std::string PathOf(const std::string& key) { return Dir() + "/" + key + ".txt"; }

    bool ReadState(const std::string& key, State& st)
    {
        FILE* f = fopen(PathOf(key).c_str(), "rb");
        if(!f) return false;
        char line[256]; st = State();
        while(fgets(line, sizeof(line), f))
        {
            int v;
            if(sscanf(line, "story=%d", &v) == 1) st.on = v != 0;
            else if(sscanf(line, "progress=%d", &v) == 1) st.progress = v;
            else if(sscanf(line, "hero=%d", &v) == 1) st.hero = v;
            else if(sscanf(line, "money=%d", &v) == 1) st.money = v;
        }
        fclose(f);
        return true;
    }
    void WriteState(const std::string& key, const State& st)
    {
        std::string p = PathOf(key), tmp = p + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if(!f) { LOG("ОШИБКА", "не записать %s", p.c_str()); return; }
        fprintf(f, "# SA Story: состояние своего сюжета для сохранения %s\nstory=%d\nprogress=%d\nhero=%d\nmoney=%d\n",
                key.c_str(), st.on ? 1 : 0, st.progress, st.hero, st.money);
        fclose(f);
        rename(tmp.c_str(), p.c_str());
    }

    void SetOnMissionVar(int v) { if(int32_t* p = game::Global(game::OnMissionVar())) *p = v; }
}

// ---------- перехваты ----------
SDECL_HOOKv(MenuNewGameH)
{
    WHERE("новая игра");
    engine::Abort("новая игра");
    g_st = State();
    g_st.on = g_newGameStory;
    g_st.hero = g_cfgHero;
    g_introSkipped = g_blockedLaunch = false;
    LOG("СЮЖЕТ", "Новая игра: %s", g_st.on ? "СВОЙ СЮЖЕТ (родные миссии выключены)" : "обычная игра (NewGameIsStory = 0)");
    MenuNewGameH();
    WHERE("игра (не мод)");
}

SDECL_HOOKb(GenericSaveH, int slot)
{
    WHERE("сохранение игры");
    if(g_st.on) engine::BeforeSave();      // метки маркеров не должны попасть в сохранение (после загрузки были бы двойные)
    bool r = GenericSaveH(slot);
    if(r)
    {
        std::string key = KeyOf(g_validSaveName);
        if(g_st.on) { WriteState(key, g_st); LOG("СЮЖЕТ", "Сохранение %d (%s): свой сюжет, пройдено миссий %d", slot, key.c_str(), g_st.progress); }
        else if(access(PathOf(key).c_str(), F_OK) == 0)
        {   // на место сюжетного сохранения записали обычную игру — файл сюжета больше не к нему
            remove(PathOf(key).c_str());
            LOG("СЮЖЕТ", "Сохранение %d (%s): обычная игра — старый файл сюжета удалён", slot, key.c_str());
        }
    }
    WHERE("игра (не мод)");
    return r;
}

SDECL_HOOK(bool, GenericLoadH, bool& ok)
{
    WHERE("загрузка игры");
    engine::Abort("загрузка игры");
    bool r = GenericLoadH(ok);
    std::string a = KeyOf(g_loadFileName), b = KeyOf(g_validSaveName);
    State st;
    bool found = ReadState(a, st) || (b != a && ReadState(b, st));
    g_st = found ? st : State();
    g_introSkipped = g_blockedLaunch = false;
    game::RestoreStoryContacts();
    LOG("СЮЖЕТ", "Загрузка «%s»: %s", a.c_str(), g_st.on ? "свой сюжет" : "обычная игра (файла сюжета нет — мод ничего не трогает)");
    if(g_st.on) LOG("СЮЖЕТ", "пройдено миссий сюжета: %d, герой: %s", g_st.progress, g_st.hero ? std::to_string(g_st.hero).c_str() : "CJ");
    WHERE("игра (не мод)");
    return r;
}

// Команды сценария 1000–1099: 03EE CAN_PLAYER_START_MISSION и 0417 LOAD_AND_LAUNCH_MISSION_INTERNAL
SDECL_HOOK(int8_t, Cmd1000H, uintptr_t script, int cmd)
{
    if(g_st.on && (cmd == 0x3EE || cmd == 0x417) && CollectParameters && g_scriptParams)
    {
        const char* name = (const char*)(script + 0x10);
        if(cmd == 0x3EE && UpdateCompareFlag && StoryThread(name))
        {
            CollectParameters(script, 1);
            UpdateCompareFlag(script, 0);            // «миссию начать нельзя» — маркер родной миссии не срабатывает
            return 0;
        }
        if(cmd == 0x417)
        {
            uintptr_t ip = *(uintptr_t*)(script + 0x20);   // CRunningScript::m_pCurrentIP
            CollectParameters(script, 1);
            int mi = g_scriptParams[0];
            if(mi == 2)
            {   // родное вступление (заставка, Тенпенни, велосипед) — не запускаем
                g_introSkipped = true;
                LOG("СЮЖЕТ", "Родное вступление (миссия 2 INTRO) пропущено — начинается свой сюжет");
                return 0;
            }
            if(StoryMission(mi))
            {
                g_blockedLaunch = true; g_blockedMission = mi;
                uint32_t now = game::TimeMs();
                if(now - g_lastBlockLog > 3000 || now < g_lastBlockLog) { g_lastBlockLog = now; LOG("СЮЖЕТ", "Запуск родной миссии %d (поток %.8s) заблокирован", mi, name); }
                return 0;
            }
            *(uintptr_t*)(script + 0x20) = ip;           // не наша — пусть игра прочитает номер сама
        }
    }
    return Cmd1000H(script, cmd);
}

bool Install(void* h)
{
    g_newGameStory = cfg->GetBool("NewGameIsStory", true, "Story");
    g_cfgHero = cfg->GetInt("HeroModel", 0, "Story");
    g_validSaveName = (char*)aml->GetSym(h, "_ZN19CGenericGameStorage16ms_ValidSaveNameE");
    g_loadFileName  = (char*)aml->GetSym(h, "_ZN19CGenericGameStorage15ms_LoadFileNameE");
    g_scriptParams  = (int32_t*)aml->GetSym(h, "ScriptParams");
    CollectParameters = (void(*)(uintptr_t, int16_t))aml->GetSym(h, "_ZN14CRunningScript17CollectParametersEs");
    UpdateCompareFlag = (void(*)(uintptr_t, uint8_t))aml->GetSym(h, "_ZN14CRunningScript17UpdateCompareFlagEh");
    void* ng = (void*)aml->GetSym(h, "_Z12Menu_NewGamev");
    void* gs = (void*)aml->GetSym(h, "_ZN19CGenericGameStorage11GenericSaveEi");
    void* gl = (void*)aml->GetSym(h, "_ZN19CGenericGameStorage11GenericLoadERb");
    void* pc = (void*)aml->GetSym(h, "_ZN14CRunningScript25ProcessCommands1000To1099Ei");
    if(!ng || !gs || !gl || !pc || !CollectParameters || !UpdateCompareFlag || !g_scriptParams || !g_validSaveName || !g_loadFileName)
    {
        LOG("ОШИБКА", "не все функции игры найдены (новая игра %p, сохранение %p, загрузка %p, команды %p) — сюжет выключен", ng, gs, gl, pc);
        return false;
    }
    aml->Hook(ng, (void*)HookOf_MenuNewGameH, (void**)&MenuNewGameH);
    aml->Hook(gs, (void*)HookOf_GenericSaveH, (void**)&GenericSaveH);
    aml->Hook(gl, (void*)HookOf_GenericLoadH, (void**)&GenericLoadH);
    aml->Hook(pc, (void*)HookOf_Cmd1000H, (void**)&Cmd1000H);
    g_installed = true;
    LOG("СЮЖЕТ", "готов: новая игра = %s, герой = %s", g_newGameStory ? "свой сюжет" : "обычная", g_cfgHero ? std::to_string(g_cfgHero).c_str() : "CJ");
    return true;
}

const State& Get() { return g_st; }
bool On() { return g_installed && g_st.on; }
void SetProgress(int p) { g_st.progress = p; LOG("СЮЖЕТ", "пройдено миссий сюжета: %d (запишется при сохранении игры)", p); }

void Frame()
{
    if(!On() || !game::Player()) return;
    uint32_t now = game::TimeMs();

    if(g_introSkipped)
    {   // main.scm после вступления сам даёт управление; экран остался затемнённым (DO_FADE 0 0) — открываем
        g_introSkipped = false;
        WHERE("сюжет: начало новой игры");
        if(int32_t* v = game::Global(92)) *v = 1;   // $92 = 1 — «вступление пройдено» (иначе не работают подсказки/дом)
        script::Do(0x01B4, { script::Arg::I(0), script::Arg::I(1) });   // SET_PLAYER_CONTROL
        script::Do(0x0373, {});                                         // SET_CAMERA_BEHIND_PLAYER
        script::Do(0x016A, { script::Arg::I(1500), script::Arg::I(1) }); // DO_FADE 1500 — открыть экран
        engine::OnNewStory();
        WHERE("игра (не мод)");
    }
    if(g_blockedLaunch)
    {   // маркер родной миссии всё-таки сработал: скрипт уже убрал управление, включил кино и затемнение — вернуть
        g_blockedLaunch = false;
        WHERE("сюжет: возврат после блока миссии");
        SetOnMissionVar(0);
        script::Do(0x02A3, { script::Arg::I(0) });                      // SWITCH_WIDESCREEN 0
        script::Do(0x01B4, { script::Arg::I(0), script::Arg::I(1) });   // SET_PLAYER_CONTROL 1
        script::Do(0x02EB, {});                                         // RESTORE_CAMERA_JUMPCUT
        script::Do(0x016A, { script::Arg::I(500), script::Arg::I(1) });  // DO_FADE 500 — открыть экран
        ui::Notice("Родные миссии выключены — играем свой сюжет");
        WHERE("игра (не мод)");
    }
    if(now >= g_nextContactScan || now + 10000 < g_nextContactScan)
    {
        g_nextContactScan = now + 2000;
        game::HideStoryContacts();
    }
    // герой: своя модель вместо CJ (после новой игры и после загрузки игра снова делает CJ)
    if(g_st.hero > 0 && !engine::Busy() && !game::CutsceneRunning() && game::Model(game::Player()) != g_st.hero
       && (now - g_heroTryMs > 1000 || now < g_heroTryMs))
    {
        g_heroTryMs = now;
        WHERE("сюжет: модель героя");
        if(!script::Check(0x0248, { script::Arg::I(g_st.hero) }))       // HAS_MODEL_LOADED
            script::Do(0x0247, { script::Arg::I(g_st.hero) });          // REQUEST_MODEL
        else
        {
            script::Do(0x09C7, { script::Arg::I(0), script::Arg::I(g_st.hero) });   // SET_PLAYER_MODEL
            script::Do(0x0249, { script::Arg::I(g_st.hero) });                      // MARK_MODEL_AS_NO_LONGER_NEEDED
            LOG("СЮЖЕТ", "герой: модель %d", g_st.hero);
        }
        WHERE("игра (не мод)");
    }
}
}
