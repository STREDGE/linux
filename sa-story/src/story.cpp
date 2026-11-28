#include <swat/sgame.h>
#include "story.h"

#include <mod/amlmod.h>
#include <mod/config.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <map>

#include "base.h"
#include "demolish.h"
#include "engine.h"
#include "game.h"
#include "intro.h"
#include "layout.h"
#include "log.h"
#include "script.h"
#include "ui.h"
#include "wanted.h"

namespace story
{
namespace
{
    State g_st;
    std::map<std::string, int> g_vals;
    bool g_installed = false;
    int  g_pendingChoice = -1;        // выбор в окне «Новая игра» (0 обычная, 1 S.W.A.T.)
    bool g_cfgDefaultSwat = false;    // [Story] NewGameIsStory — если окна выбора нет (нет функций меню)
    constexpr int kHeroModel = 285;   // S.W.A.T.

    char* g_validSaveName = nullptr;
    char* g_loadFileName = nullptr;
    int32_t* g_scriptParams = nullptr;
    void (*CollectParameters)(uintptr_t script, int16_t n) = nullptr;
    void (*UpdateCompareFlag)(uintptr_t script, uint8_t v) = nullptr;

    bool g_introSkipped = false, g_blockedLaunch = false;
    int  g_blockedMission = -1;
    uint32_t g_lastBlockLog = 0, g_heroTryMs = 0, g_nextContactScan = 0, g_nextImmunity = 0, g_nextKeep = 0;
    bool g_restartSet = false, g_dead = false, g_weatherSet = false;
    int  g_keepMoney = 0; int g_keepW[13] = {}, g_keepA[13] = {};

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
    bool StoryMission(int mi) { return mi == 2 || (mi >= 11 && mi <= 112 && mi != 35 && mi != 71 && mi != 83); }

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

    bool ReadState(const std::string& key, State& st, std::map<std::string, int>& vals)
    {
        FILE* f = fopen(PathOf(key).c_str(), "rb");
        if(!f) return false;
        char line[256]; st = State(); vals.clear();
        while(fgets(line, sizeof(line), f))
        {
            int v; char k[96];
            if(sscanf(line, "story=%d", &v) == 1) st.on = v != 0;
            else if(sscanf(line, "progress=%d", &v) == 1) st.progress = v;
            else if(sscanf(line, "hero=%d", &v) == 1) st.hero = v;
            else if(sscanf(line, "money=%d", &v) == 1) st.money = v;
            else if(sscanf(line, "x.%95[^=]=%d", k, &v) == 2) vals[k] = v;
        }
        fclose(f);
        return true;
    }
    void WriteState(const std::string& key, const State& st)
    {
        std::string p = PathOf(key), tmp = p + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if(!f) { LOG("ОШИБКА", "не записать %s", p.c_str()); return; }
        fprintf(f, "# SA Story: режим S.W.A.T. для сохранения %s\nstory=%d\nprogress=%d\nhero=%d\nmoney=%d\n",
                key.c_str(), st.on ? 1 : 0, st.progress, st.hero, st.money);
        for(auto& kv : g_vals) fprintf(f, "x.%s=%d\n", kv.first.c_str(), kv.second);
        fclose(f);
        rename(tmp.c_str(), p.c_str());
    }
    void SetOnMissionVar(int v) { if(int32_t* p = game::Global(game::OnMissionVar())) *p = v; }

    void ResetWorld(const char* why)
    {
        engine::Abort(why);
        base::Reset(); wanted::Reset(); demolish::Reset(); intro::Abort();
        g_introSkipped = g_blockedLaunch = false; g_restartSet = false; g_dead = false; g_weatherSet = false;
    }

    void ApplyStart()
    {   // однократно после новой игры/загрузки в режиме: точка возрождения — медпункт базы, вид базы
        if(g_restartSet || !game::Player()) return;
        g_restartSet = true;
        game::Vec m = layout::W(layout::kMedU, layout::kMedV + 3.0f, 0.5f);
        script::Do(0x08DF, { script::Arg::F(m.x), script::Arg::F(m.y), script::Arg::F(m.z), script::Arg::F(6000.0f), script::Arg::F(layout::H(0)) });   // SET_EXTRA_HOSPITAL_RESTART_POINT
        script::Do(0x08E0, { script::Arg::F(m.x), script::Arg::F(m.y), script::Arg::F(m.z), script::Arg::F(6000.0f), script::Arg::F(layout::H(0)) });   // ..._POLICE_STATION_ (на всякий)
        LOG("РЕЖИМ", "точка возрождения — медпункт базы");
    }
}

// ---------- перехваты ----------
SDECL_HOOKv(MenuNewGameH)
{
    WHERE("новая игра");
    ResetWorld("новая игра");
    g_st = State(); g_vals.clear();
    g_st.on = g_pendingChoice >= 0 ? g_pendingChoice == 1 : g_cfgDefaultSwat;
    g_st.hero = g_st.on ? kHeroModel : 0;
    if(g_st.on) g_vals["look"] = 1;                       // по умолчанию — «Готэм»
    g_pendingChoice = -1;
    LOG("РЕЖИМ", "Новая игра: %s", g_st.on ? "S.W.A.T. (родные миссии выключены, вступление — вертолёт)" : "обычная GTA (мод ничего не трогает)");
    MenuNewGameH();
    WHERE("игра (не мод)");
}

SDECL_HOOKb(GenericSaveH, int slot)
{
    WHERE("сохранение игры");
    if(g_st.on) { engine::BeforeSave(); base::BeforeSave(); wanted::BeforeSave(); }
    bool r = GenericSaveH(slot);
    if(g_st.on) wanted::AfterSave();
    if(r)
    {
        std::string key = KeyOf(g_validSaveName);
        if(g_st.on) { WriteState(key, g_st); LOG("РЕЖИМ", "Сохранение %d (%s): S.W.A.T., значений %d", slot, key.c_str(), (int)g_vals.size()); }
        else if(access(PathOf(key).c_str(), F_OK) == 0)
        {
            remove(PathOf(key).c_str());
            LOG("РЕЖИМ", "Сохранение %d (%s): обычная игра — старый файл режима удалён", slot, key.c_str());
        }
    }
    WHERE("игра (не мод)");
    return r;
}

// Игра (и родной скрипт — команда «пересобрать игрока») надевает одежду CJ на того, кто сейчас игрок.
// На скелете другой модели (S.W.A.T. 285) это даёт «CJ с вытянутыми руками». Пока герой в своём скине — не даём.
static bool SkipRebuild(uintptr_t ped)
{
    if(!g_st.on || g_st.hero <= 0 || !ped || game::Model(ped) != g_st.hero) return false;
    static uint32_t s_logAt = 0; uint32_t now = game::TimeMs();
    if(now - s_logAt > 10000 || now < s_logAt) { s_logAt = now; LOG("РЕЖИМ", "игра хотела надеть одежду CJ на героя (модель %d) — пропущено", g_st.hero); }
    return true;
}
// Страховка от вылета (лог 16:29): задача игры «идти к цели, целясь» (CTaskComplexSeekEntityAiming) не проверяет,
// жива ли цель. Если цель удалили (игра убрала прохожего), игра падала. Цели нет — задача просто заканчивается
// (пустой ответ игра обрабатывает штатно: снимает подзадачу).
SDECL_HOOK(uintptr_t, SeekAimingCtrlH, uintptr_t task, uintptr_t ped)
{
    if(task && *(uintptr_t*)(task + 0x18) == 0)
    {
        static uint32_t s_logAt = 0; uint32_t now = game::TimeMs();
        if(now - s_logAt > 10000 || now < s_logAt) { s_logAt = now; LOG("ЗАЩИТА", "задача «идти к цели целясь» без цели — завершена (вместо вылета)"); }
        return 0;
    }
    return SeekAimingCtrlH(task, ped);
}

SDECL_HOOKv(RebuildPlayerH, uintptr_t ped, bool ignoreFat)
{
    if(SkipRebuild(ped)) return;
    RebuildPlayerH(ped, ignoreFat);
}
SDECL_HOOKv(RebuildPlayerIfNeededH, uintptr_t ped)
{
    if(SkipRebuild(ped)) return;
    RebuildPlayerIfNeededH(ped);
}

SDECL_HOOK(bool, GenericLoadH, bool& ok)
{
    WHERE("загрузка игры");
    ResetWorld("загрузка игры");
    bool r = GenericLoadH(ok);
    std::string a = KeyOf(g_loadFileName), b = KeyOf(g_validSaveName);
    State st; std::map<std::string, int> vals;
    bool found = ReadState(a, st, vals) || (b != a && ReadState(b, st, vals));
    g_st = found ? st : State(); g_vals = found ? vals : std::map<std::string, int>();
    if(g_st.on) g_st.hero = kHeroModel;
    game::RestoreStoryContacts();
    LOG("РЕЖИМ", "Загрузка «%s»: %s", a.c_str(), g_st.on ? "S.W.A.T." : "обычная игра (файла режима нет — мод ничего не трогает)");
    WHERE("игра (не мод)");
    return r;
}

SDECL_HOOK(int8_t, Cmd1000H, uintptr_t script, int cmd)
{
    if(g_st.on && (cmd == 0x3EE || cmd == 0x417) && CollectParameters && g_scriptParams)
    {
        const char* name = (const char*)(script + 0x10);
        if(cmd == 0x3EE && UpdateCompareFlag && StoryThread(name))
        {
            CollectParameters(script, 1);
            UpdateCompareFlag(script, 0);
            return 0;
        }
        if(cmd == 0x417)
        {
            uintptr_t ip = *(uintptr_t*)(script + 0x20);
            CollectParameters(script, 1);
            int mi = g_scriptParams[0];
            if(mi == 2) { g_introSkipped = true; LOG("РЕЖИМ", "Родное вступление (миссия 2 INTRO) пропущено — вместо него вертолёт"); return 0; }
            if(StoryMission(mi))
            {
                g_blockedLaunch = true; g_blockedMission = mi;
                uint32_t now = game::TimeMs();
                if(now - g_lastBlockLog > 3000 || now < g_lastBlockLog) { g_lastBlockLog = now; LOG("РЕЖИМ", "Запуск родной миссии %d (поток %.8s) заблокирован", mi, name); }
                return 0;
            }
            *(uintptr_t*)(script + 0x20) = ip;
        }
    }
    return Cmd1000H(script, cmd);
}

bool Install(void* h)
{
    g_cfgDefaultSwat = cfg->GetBool("NewGameIsStory", false, "Story");
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
        LOG("ОШИБКА", "не все функции игры найдены (новая игра %p, сохранение %p, загрузка %p, команды %p) — режим выключен", ng, gs, gl, pc);
        return false;
    }
    aml->Hook(ng, (void*)HookOf_MenuNewGameH, (void**)&MenuNewGameH);
    aml->Hook(gs, (void*)HookOf_GenericSaveH, (void**)&GenericSaveH);
    aml->Hook(gl, (void*)HookOf_GenericLoadH, (void**)&GenericLoadH);
    aml->Hook(pc, (void*)HookOf_Cmd1000H, (void**)&Cmd1000H);
    void* rb  = (void*)aml->GetSym(h, "_ZN8CClothes13RebuildPlayerEP10CPlayerPedb");
    void* rbn = (void*)aml->GetSym(h, "_ZN8CClothes21RebuildPlayerIfNeededEP10CPlayerPed");
    if(rb)  aml->Hook(rb,  (void*)HookOf_RebuildPlayerH, (void**)&RebuildPlayerH);
    if(rbn) aml->Hook(rbn, (void*)HookOf_RebuildPlayerIfNeededH, (void**)&RebuildPlayerIfNeededH);
    if(!rb || !rbn) LOG("ОШИБКА", "нет функций одежды игрока (%p, %p) — скин героя может ломаться", rb, rbn);
    void* sa = (void*)aml->GetSym(h, "_ZN28CTaskComplexSeekEntityAiming14ControlSubTaskEP4CPed");
    if(sa) aml->Hook(sa, (void*)HookOf_SeekAimingCtrlH, (void**)&SeekAimingCtrlH);
    else LOG("ОШИБКА", "нет задачи игры SeekEntityAiming — страховка от вылета не стоит");
    ui::SetNewGameHandler([](int choice) { g_pendingChoice = choice; });
    g_installed = true;
    LOG("РЕЖИМ", "готов: при «Новой игре» — выбор «Обычная GTA / S.W.A.T.»");
    return true;
}

const State& Get() { return g_st; }
bool On() { return g_installed && g_st.on; }
void SetProgress(int p) { g_st.progress = p; }
int  GetInt(const std::string& k, int def) { auto it = g_vals.find(k); return it == g_vals.end() ? def : it->second; }
void SetInt(const std::string& k, int v) { g_vals[k] = v; }

void Frame()
{
    if(!On() || !game::Player()) return;
    uint32_t now = game::TimeMs();
    int pc = game::PedRef(game::Player());

    if(g_introSkipped)
    {   // вместо открытия экрана — вертолёт к базе
        g_introSkipped = false;
        WHERE("режим: начало новой игры");
        if(int32_t* v = game::Global(92)) *v = 1;
        intro::Start();
        WHERE("кадр: S.W.A.T.");
    }
    if(g_blockedLaunch)
    {
        g_blockedLaunch = false;
        WHERE("режим: возврат после блока миссии");
        SetOnMissionVar(0);
        script::Do(0x02A3, { script::Arg::I(0) });
        script::Do(0x01B4, { script::Arg::I(0), script::Arg::I(1) });
        script::Do(0x02EB, {});
        script::Do(0x016A, { script::Arg::I(500), script::Arg::I(1) });
        ui::Notice("Родные миссии выключены — режим S.W.A.T.");
        WHERE("кадр: S.W.A.T.");
    }
    if(now >= g_nextContactScan || now + 10000 < g_nextContactScan) { g_nextContactScan = now + 2000; game::HideStoryContacts(); }
    ApplyStart();
    if(!g_weatherSet) { g_weatherSet = true; base::SetLook(GetInt("look", 1)); }
    else base::KeepWeather();

    // неприкосновенность: героям звёзды не даются никогда, полиция дружественна
    if(now >= g_nextImmunity || now + 10000 < g_nextImmunity)
    {
        g_nextImmunity = now + 1000;
        std::vector<script::Arg> mw = { script::Arg::I(0) }; script::Run(0x01F0, mw, nullptr, true);   // SET_MAX_WANTED_LEVEL 0
        if(game::Wanted() > 0) { script::Do(0x0110, { script::Arg::I(0) }); LOG("РЕЖИМ", "розыск героя снят (герой на стороне полиции)"); }   // CLEAR_WANTED_LEVEL
        std::vector<script::Arg> a = { script::Arg::I(0), script::Arg::I(1) }; script::Run(0x01F7, a, nullptr, true);   // SET_POLICE_IGNORE_PLAYER
    }
    // смерть: возрождение в медпункте и без штрафов (деньги и оружие — вернуть)
    float hp = game::Health(game::Player());
    if(hp <= 0 && !g_dead)
    {
        g_dead = true;
        game::Vec m = layout::W(layout::kMedU, layout::kMedV + 3.0f, 0.5f);
        script::Do(0x016E, { script::Arg::F(m.x), script::Arg::F(m.y), script::Arg::F(m.z), script::Arg::F(layout::H(0)) });   // OVERRIDE_NEXT_RESTART
        LOG("РЕЖИМ", "Артём погиб — возрождение в медпункте базы");
    }
    else if(hp > 0 && g_dead && !game::CutsceneRunning())
    {
        g_dead = false;
        if(game::Money() < g_keepMoney) game::AddMoney(g_keepMoney - game::Money());
        for(int i = 0; i < 13; ++i) if(g_keepW[i] > 0 && g_keepA[i] > 0) swat::GiveWeapon(pc, g_keepW[i], g_keepA[i]);
        ui::Notice("Медпункт базы: подлатали. Деньги и оружие при тебе.", 5.0f);
        LOG("РЕЖИМ", "возрождение: деньги ($%d) и оружие возвращены", g_keepMoney);
    }
    else if(hp > 0 && (now >= g_nextKeep || now + 10000 < g_nextKeep))
    {   // запомнить деньги и оружие (вернуть после смерти)
        g_nextKeep = now + 3000;
        g_keepMoney = game::Money();
        uintptr_t p = game::Player();
        for(int i = 0; i < 13; ++i) { uintptr_t w = p + 0x730 + (uintptr_t)i * 0x20; g_keepW[i] = *(int*)w; g_keepA[i] = *(int*)(w + 12); }
    }
    // герой: модель S.W.A.T. (после смерти/загрузки/интерьера игра ставит CJ)
    if(g_st.hero > 0 && !intro::Attached() && !game::CutsceneRunning() && game::Model(game::Player()) != g_st.hero && hp > 0 && (now - g_heroTryMs > 1000 || now < g_heroTryMs))
    {
        g_heroTryMs = now;
        WHERE("режим: модель героя");
        if(!script::Check(0x0248, { script::Arg::I(g_st.hero) })) script::Do(0x0247, { script::Arg::I(g_st.hero) });
        else
        {
            script::Do(0x09C7, { script::Arg::I(0), script::Arg::I(g_st.hero) });
            script::Do(0x0249, { script::Arg::I(g_st.hero) });
            LOG("РЕЖИМ", "герой Артём: модель %d (S.W.A.T.)", g_st.hero);
        }
        WHERE("кадр: S.W.A.T.");
    }
}
}
