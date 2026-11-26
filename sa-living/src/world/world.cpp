#include "world.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <ctype.h>

#include <algorithm>

#include "util/log.h"

namespace world
{
namespace
{
    World g_w;

    const char* kMaleNames[] = { "Дешон", "Тайрон", "Маркус", "Джамал", "Андре", "Кевин", "Дэррил", "Терренс", "Лерой", "Рикки",
        "Карлос", "Мигель", "Хорхе", "Луис", "Эдди", "Тони", "Винни", "Сэл", "Фрэнк", "Джимми", "Майк", "Дэнни", "Рэй", "Стив",
        "Хуан", "Педро", "Рауль", "Хэнк", "Билли", "Джо", "Ли", "Чен", "Хо", "Нгуен", "Бобби", "Уэйн", "Дуэйн", "Клиф", "Отис", "Гэри" };
    const char* kFemaleNames[] = { "Кэнди", "Тиша", "Латойя", "Мария", "Кармен", "Роза", "Дженни", "Эшли", "Кейша", "Бренда",
        "Моника", "Лиза", "Сара", "Тина", "Кэти", "Анджела", "Ванесса", "Ирма", "Долорес", "Шерил" };
    const char* kSurnames[] = { "Джонсон", "Уильямс", "Браун", "Дэвис", "Миллер", "Харрис", "Кларк", "Льюис", "Уокер", "Янг",
        "Гарсия", "Мартинес", "Лопес", "Эрнандес", "Гонсалес", "Перес", "Санчес", "Ривера", "Торрес", "Флорес",
        "Риччи", "Марино", "Конти", "Ли", "Ван", "Чжан", "Нгуен", "Тран", "Смит", "Тейлор" };
    const char* kStreetNicks[] = { "Малой", "Бугай", "Тихий", "Шустрый", "Борода", "Кривой", "Счастливчик", "Псих", "Доктор", "Шрам",
        "Профессор", "Призрак", "Кузен", "Лысый", "Крот", "Скорый", "Дымок", "Пятница", "Козырь", "Бульдог" };

    template<size_t N> const char* Pick(const char* (&a)[N]) { return a[rand() % N]; }

    std::string g_path;
}

World& W() { return g_w; }
int g_forgetDays = 7;
void (*g_forwardEvent)(const Event& e) = nullptr;

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int Rand(int lo, int hi) { return hi <= lo ? lo : lo + rand() % (hi - lo + 1); }

std::string Fmt(const char* fmt, ...)
{
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return buf;
}

const char* FactionName(int f)
{
    static const char* n[F_COUNT] = { "нет", "полиция", "Grove Street", "Ballas", "Vagos", "Aztecas", "Rifa", "Da Nang Boys", "Триады", "Мафия", "горожане" };
    return f >= 0 && f < F_COUNT ? n[f] : "?";
}

int FactionOfPedType(int t)
{
    switch(t)
    {
        case game::PT_COP: return F_POLICE; case game::PT_GROVE: return F_GROVE; case game::PT_BALLAS: return F_BALLAS;
        case game::PT_VAGOS: return F_VAGOS; case game::PT_AZTECA: return F_AZTECAS; case game::PT_RIFA: return F_RIFA;
        case game::PT_DANANG: return F_DANANG; case game::PT_TRIAD: return F_TRIADS; case game::PT_MAFIA: return F_MAFIA;
        default: return F_CIVIL;
    }
}

void Reset()
{
    std::string p = g_path;
    g_w = World();
    g_path = p;
    // стартовые отношения банд к CJ (он из Grove)
    for(int f = 0; f < F_COUNT; ++f) { g_w.fac[f].respect[0] = 0; g_w.fac[f].hostility[0] = 0; }
    g_w.fac[F_GROVE].respect[WHO_CJ] = 40;
    g_w.fac[F_BALLAS].hostility[WHO_CJ] = 55; g_w.fac[F_BALLAS].tension = 60;
    g_w.fac[F_VAGOS].hostility[WHO_CJ] = 45;  g_w.fac[F_VAGOS].tension = 45;
    g_w.fac[F_AZTECAS].hostility[WHO_CJ] = 10;
    g_w.fac[F_POLICE].hostility[WHO_CJ] = 20;
}

std::string BaseDir()
{
    const char* base = aml->GetAndroidDataPath();
    std::string dir = std::string(base ? base : "/sdcard") + "/SALiving";
    mkdir(dir.c_str(), 0777);
    return dir;
}
std::string DefaultPath() { return BaseDir() + "/world.json"; }

// ---------------- память по сохранениям ----------------
static std::string g_key;
static std::string MemoryPath(const std::string& key)
{
    std::string dir = BaseDir() + "/memory";
    mkdir(dir.c_str(), 0777);
    std::string safe;
    for(char ch : key) safe += (isalnum((unsigned char)ch) || ch == '_' || ch == '-') ? ch : '_';
    return dir + "/" + safe + ".json";
}
static std::string KeyOf(const std::string& saveName)
{
    std::string k = saveName;
    size_t sl = k.find_last_of("/\\"); if(sl != std::string::npos) k = k.substr(sl + 1);
    size_t dot = k.rfind('.'); if(dot != std::string::npos) k = k.substr(0, dot);
    return k.empty() ? "save" : k;
}
const std::string& MemoryKey() { return g_key; }

void SwitchTo(const std::string& saveName)
{
    g_key = KeyOf(saveName);
    json::Value usage = g_w.usage;
    std::string p = MemoryPath(g_key);
    if(!Load(p)) { Reset(); g_path = p; }
    g_w.usage = usage;
    LOG_INFO("Память: сохранение «%s» — %s (персонажей %d, день %d)", g_key.c_str(), g_w.chars.empty() ? "новая" : "загружена", (int)g_w.chars.size(), g_w.day);
}

void SaveAs(const std::string& saveName)
{
    g_key = KeyOf(saveName);
    if(Save(MemoryPath(g_key))) LOG_INFO("Память: записана для сохранения «%s» (персонажей %d)", g_key.c_str(), (int)g_w.chars.size());
    else LOG_ERROR("Память: не удалось записать для «%s»", g_key.c_str());
}

void NewGame()
{
    json::Value usage = g_w.usage;
    Reset();
    g_w.usage = usage;
    g_key = "new";
    g_path = MemoryPath(g_key);
    LOG_INFO("Память: новая игра — пустая");
}

// ---------------- сюжетные персонажи ----------------
static const struct { const char* key; const char* name; bool female; int faction; } kStory[] = {
    { "sweet", "Свит Джонсон", false, F_GROVE }, { "ryder", "Райдер", false, F_GROVE }, { "smoke", "Биг Смоук", false, F_GROVE },
    { "tenpenny", "офицер Тенпенни", false, F_POLICE }, { "pulaski", "офицер Пуласки", false, F_POLICE }, { "hernandez", "офицер Эрнандес", false, F_POLICE },
    { "dwaine", "Дуэйн", false, F_CIVIL }, { "forelli", "Форелли", false, F_MAFIA },
    { "cesar", "Цезарь Виальпандо", false, F_AZTECAS }, { "kendl", "Кендл Джонсон", true, F_GROVE }, { "ogloc", "Оу-Джи Лок", false, F_GROVE },
    { "wuzimu", "Ву Зи Му", false, F_TRIADS }, { "truth", "Правда", false, F_CIVIL }, { "cat", "Каталина", true, F_CIVIL },
    { "torino", "Майк Торено", false, F_CIVIL }, { "zero", "Зеро", false, F_CIVIL }, { "jethro", "Джетро", false, F_CIVIL },
    { "maccer", "Маккер", false, F_CIVIL }, { "paul", "Кент Пол", false, F_CIVIL }, { "rose", "Кен Розенберг", false, F_CIVIL },
    { "emmet", "Эммет", false, F_GROVE }, { "bbthin", "Би Дап", false, F_GROVE }, { "bb", "Барбара", true, F_POLICE },
    { "maddogg", "Мэдд Догг", false, F_CIVIL }, { "jizzy", "Джиззи Би", false, F_CIVIL }, { "tbone", "Ти-Боун Мендес", false, F_RIFA },
    { "ryder1", "Райдер", false, F_GROVE }, { "ryder2", "Райдер", false, F_GROVE }, { "ryder3", "Райдер", false, F_GROVE },
    { "smokev", "Биг Смоук", false, F_GROVE }, { "sweet2", "Свит Джонсон", false, F_GROVE }, { "csblue2", "Виллис", false, F_CIVIL },
    { "salvatore", "Сальваторе Леоне", false, F_MAFIA }, { "suzie", "Сьюзи", true, F_TRIADS }, { "guppy", "Гуппи", false, F_TRIADS },
    { "claude", "Клод", false, F_CIVIL }, { "janitor", "уборщик", false, F_CIVIL }, { "ogloc2", "Оу-Джи Лок", false, F_GROVE },
};
const char* StoryName(const std::string& story)
{
    for(auto& s : kStory) if(story == s.key) return s.name;
    return nullptr;
}

static json::Value g_storyVoices;      // { "sweet": {voice,pitch,rate}, … } — общий файл для всех сохранений
static bool g_storyLoaded = false;
static void LoadStoryVoices()
{
    if(g_storyLoaded) return;
    g_storyLoaded = true; g_storyVoices = json::Value::Object();
    FILE* f = fopen((BaseDir() + "/story_voices.json").c_str(), "rb");
    if(!f) return;
    std::string s; char buf[8192]; size_t n;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    json::Value v; if(json::Parse(s, v) && v.isObject()) g_storyVoices = v;
}
static void SaveStoryVoices()
{
    FILE* f = fopen((BaseDir() + "/story_voices.json").c_str(), "wb");
    if(!f) return;
    std::string s = g_storyVoices.dump(); fwrite(s.data(), 1, s.size(), f); fclose(f);
}
bool GetStoryVoice(const std::string& story, StoryVoice& out)
{
    LoadStoryVoices();
    if(story.empty() || !g_storyVoices.has(story)) return false;
    const json::Value& v = g_storyVoices[story];
    out.voice = v["voice"].str(); out.pitch = (float)v["pitch"].num(1); out.rate = (float)v["rate"].num(1);
    return !out.voice.empty();
}
void SetStoryVoice(const std::string& story, const StoryVoice& sv)
{
    LoadStoryVoices();
    if(story.empty()) return;
    json::Value v = json::Value::Object(); v["voice"] = sv.voice; v["pitch"] = sv.pitch; v["rate"] = sv.rate;
    g_storyVoices[story] = v;
    SaveStoryVoices();
}
void ClearStoryVoice(const std::string& story) { LoadStoryVoices(); if(g_storyVoices.has(story)) { g_storyVoices.erase(story); SaveStoryVoices(); } }

// ---------------- забывание неважных ----------------
int ForgetUnimportant(int keepDays)
{
    int forgotten = 0;
    for(size_t i = 0; i < g_w.chars.size();)
    {
        Character& c = g_w.chars[i];
        bool keep = !c.story.empty() || c.jailUntilDay > g_w.day || !c.goal.empty() || c.hostId != 0 || c.exposed;
        for(int w = 0; w < WHO_COUNT && !keep; ++w)
        {
            const Relation& r = c.rel[w];
            if(r.contact || r.payroll || r.informant || r.debt != 0 || r.favors > 0) keep = true;
            if(abs(r.trust) >= 25 || abs(r.respect) >= 25 || abs(r.liking) >= 25 || r.fear >= 40) keep = true;   // сильные отношения помнятся
        }
        if(!keep) for(auto& e : g_w.events)
        {   // свидетель или жертва незакрытого дела — помним
            if(e.day < g_w.day - 30) continue;
            if(e.victimChar == c.id) { keep = true; break; }
            for(int wid : e.witnesses) if(wid == c.id) { keep = true; break; }
            if(keep) break;
        }
        int last = c.lastTalkDay >= 0 ? c.lastTalkDay : 0;
        if(!keep && PedOf(c)) keep = true;                           // он сейчас рядом
        if(!keep && g_w.day - last >= keepDays)
        {
            LOG_INFO("Память: забыт #%d %s (не виделись %d дн., ничего важного)", c.id, c.name.c_str(), g_w.day - last);
            g_w.chars.erase(g_w.chars.begin() + i); ++forgotten;
            continue;
        }
        ++i;
    }
    return forgotten;
}

// ---------------- сохранение ----------------
static json::Value RelToJ(const Relation& r)
{
    json::Value v = json::Value::Object();
    v["trust"] = r.trust; v["respect"] = r.respect; v["liking"] = r.liking; v["fear"] = r.fear; v["debt"] = r.debt;
    v["met"] = r.met; v["contact"] = r.contact; v["payroll"] = r.payroll; v["weekly"] = r.weekly; v["paidDay"] = r.paidDay;
    v["informant"] = r.informant; v["favors"] = r.favors;
    return v;
}
static Relation RelFromJ(const json::Value& v)
{
    Relation r; r.trust = v["trust"].i(); r.respect = v["respect"].i(); r.liking = v["liking"].i(); r.fear = v["fear"].i();
    r.debt = v["debt"].i(); r.met = v["met"].b(); r.contact = v["contact"].b(); r.payroll = v["payroll"].b();
    r.weekly = v["weekly"].i(); r.paidDay = v["paidDay"].i(); r.informant = v["informant"].b(); r.favors = v["favors"].i();
    return r;
}
static json::Value VecToJ(const game::Vec& p) { json::Value a = json::Value::Array(); a.push(p.x); a.push(p.y); a.push(p.z); return a; }
static game::Vec VecFromJ(const json::Value& a) { game::Vec p; p.x = (float)a[0].num(); p.y = (float)a[1].num(); p.z = (float)a[2].num(); return p; }

bool Save(const std::string& path)
{
    g_path = path;
    json::Value root = json::Value::Object();
    root["version"] = 1;
    root["nextId"] = g_w.nextId; root["day"] = g_w.day; root["playerGang"] = g_w.playerGang;
    root["playerNick"] = g_w.playerNick;
    json::Value& cs = root["chars"]; cs = json::Value::Array();
    for(auto& c : g_w.chars)
    {
        json::Value v = json::Value::Object();
        v["id"] = c.id; v["name"] = c.name; v["nick"] = c.nick; v["pedType"] = c.pedType; v["model"] = c.model; v["female"] = c.female;
        v["age"] = c.age; v["faction"] = c.faction; v["rank"] = c.rank;
        json::Value t = json::Value::Array();
        for(int x : { c.tr.loyalty, c.tr.greed, c.tr.courage, c.tr.aggression, c.tr.honor, c.tr.cunning, c.tr.talkative, c.tr.ambition, c.tr.suspicion }) t.push(x);
        v["traits"] = t;
        json::Value rels = json::Value::Array(); for(int w = 0; w < WHO_COUNT; ++w) rels.push(RelToJ(c.rel[w])); v["rel"] = rels;
        json::Value mem = json::Value::Array();
        for(auto& n : c.mem) { json::Value m = json::Value::Object(); m["d"] = n.day; m["t"] = n.text; mem.push(m); }
        v["mem"] = mem; v["summary"] = c.summary;
        v["backstory"] = c.backstory; v["dream"] = c.dream; v["fear"] = c.fearOf; v["speech"] = c.speech; v["role"] = c.role; v["aiProfile"] = c.aiProfile;
        v["home"] = VecToJ(c.home); v["homeZone"] = c.homeZone; v["money"] = c.money; v["alive"] = c.alive; v["jail"] = c.jailUntilDay;
        v["secret"] = c.secretlyLoyal; v["exposed"] = c.exposed; v["goal"] = c.goal; v["goalUntil"] = c.goalUntilMin; v["goalTarget"] = c.goalTarget;
        v["voice"] = c.voice; v["vpitch"] = c.vpitch; v["vrate"] = c.vrate; v["story"] = c.story;
        v["pitch"] = c.pitch; v["rate"] = c.rate; v["dialog"] = c.dialog; v["lastTalkDay"] = c.lastTalkDay; v["hostId"] = c.hostId;
        cs.push(v);
    }
    json::Value& es = root["events"]; es = json::Value::Array();
    size_t from = g_w.events.size() > 300 ? g_w.events.size() - 300 : 0;
    for(size_t i = from; i < g_w.events.size(); ++i)
    {
        const Event& e = g_w.events[i];
        json::Value v = json::Value::Object();
        v["id"] = e.id; v["day"] = e.day; v["min"] = e.minute; v["kind"] = e.kind; v["text"] = e.text; v["zone"] = e.zone;
        v["pos"] = VecToJ(e.pos); v["sev"] = e.severity; v["who"] = e.who; v["victim"] = e.victimChar; v["vfac"] = e.victimFaction;
        json::Value wi = json::Value::Array(); for(int w : e.witnesses) wi.push(w); v["wit"] = wi; v["anon"] = e.anonWitnesses; v["rep"] = e.reported;
        es.push(v);
    }
    json::Value& rs = root["rumors"]; rs = json::Value::Array();
    for(auto& r : g_w.rumors) { json::Value v = json::Value::Object(); v["e"] = r.eventId; v["t"] = r.text; v["z"] = r.zone; v["d"] = r.day; v["r"] = r.reach; rs.push(v); }
    json::Value& fs = root["factions"]; fs = json::Value::Array();
    for(int f = 0; f < F_COUNT; ++f)
    {
        const FactionState& s = g_w.fac[f];
        json::Value v = json::Value::Object();
        v["resp0"] = s.respect[0]; v["resp1"] = s.respect[1]; v["host0"] = s.hostility[0]; v["host1"] = s.hostility[1];
        v["tension"] = s.tension; v["truce"] = s.truce; v["truceUntil"] = s.truceUntil; v["plan"] = s.plan;
        fs.push(v);
    }
    json::Value& pc = root["police"]; pc = json::Value::Array();
    for(int w = 0; w < WHO_COUNT; ++w)
    {
        const PoliceCase& p = g_w.pcase[w];
        json::Value v = json::Value::Object();
        v["heat"] = p.heat; v["evidence"] = p.evidence; v["ia"] = p.iaInvestigation; v["jailDays"] = p.jailDays;
        json::Value ch = json::Value::Array(); for(auto& s : p.charges) ch.push(s); v["charges"] = ch;
        pc.push(v);
    }
    json::Value news = json::Value::Array(); for(auto& n : g_w.news) news.push(n); root["news"] = news;
    root["usage"] = g_w.usage;
    root["batch"] = g_w.pendingBatch;

    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if(!f) { LOG_ERROR("Мир: не записать %s", tmp.c_str()); return false; }
    std::string s = root.dump();
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    fclose(f);
    if(ok) { remove(path.c_str()); ok = rename(tmp.c_str(), path.c_str()) == 0; }
    return ok;
}

bool Load(const std::string& path)
{
    g_path = path;
    FILE* f = fopen(path.c_str(), "rb");
    if(!f) { Reset(); LOG_INFO("Мир: новый (файла нет)"); return false; }
    std::string s; char buf[65536]; size_t n;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    json::Value root; std::string err;
    if(!json::Parse(s, root, &err)) { LOG_ERROR("Мир: файл повреждён (%s) — начинаю заново", err.c_str()); Reset(); return false; }
    Reset();
    g_w.nextId = root["nextId"].i(1); g_w.day = root["day"].i(1); g_w.playerGang = root["playerGang"].i(F_GROVE);
    g_w.playerNick = root["playerNick"].s("CJ");
    for(auto& v : root["chars"].arr())
    {
        Character c;
        c.id = v["id"].i(); c.name = v["name"].str(); c.nick = v["nick"].str(); c.pedType = v["pedType"].i(4); c.model = v["model"].i();
        c.female = v["female"].b(); c.age = v["age"].i(30); c.faction = v["faction"].i(F_CIVIL); c.rank = v["rank"].i();
        const json::Value& t = v["traits"];
        c.tr.loyalty = t[0].i(50); c.tr.greed = t[1].i(50); c.tr.courage = t[2].i(50); c.tr.aggression = t[3].i(50); c.tr.honor = t[4].i(50);
        c.tr.cunning = t[5].i(50); c.tr.talkative = t[6].i(50); c.tr.ambition = t[7].i(50); c.tr.suspicion = t[8].i(50);
        for(int w = 0; w < WHO_COUNT; ++w) c.rel[w] = RelFromJ(v["rel"][w]);
        for(auto& m : v["mem"].arr()) { Note no; no.day = m["d"].i(); no.text = m["t"].str(); c.mem.push_back(no); }
        c.summary = v["summary"].str(); c.backstory = v["backstory"].str(); c.dream = v["dream"].str(); c.fearOf = v["fear"].str();
        c.speech = v["speech"].str(); c.role = v["role"].str(); c.aiProfile = v["aiProfile"].b();
        c.home = VecFromJ(v["home"]); c.homeZone = v["homeZone"].str(); c.money = v["money"].i(100); c.alive = v["alive"].b(true);
        c.jailUntilDay = v["jail"].i(); c.secretlyLoyal = v["secret"].b(); c.exposed = v["exposed"].b();
        c.goal = v["goal"].str(); c.goalUntilMin = v["goalUntil"].i(); c.goalTarget = v["goalTarget"].i();
        c.voice = v["voice"].str(); c.vpitch = (float)v["vpitch"].num(1); c.vrate = (float)v["vrate"].num(1); c.story = v["story"].str();
        c.pitch = (float)v["pitch"].num(1); c.rate = (float)v["rate"].num(1); c.dialog = v["dialog"]; c.lastTalkDay = v["lastTalkDay"].i(-1); c.hostId = v["hostId"].i();
        g_w.chars.push_back(std::move(c));
    }
    for(auto& v : root["events"].arr())
    {
        Event e; e.id = v["id"].i(); e.day = v["day"].i(); e.minute = v["min"].i(); e.kind = v["kind"].str(); e.text = v["text"].str();
        e.zone = v["zone"].str(); e.pos = VecFromJ(v["pos"]); e.severity = v["sev"].i(1); e.who = v["who"].i(); e.victimChar = v["victim"].i();
        e.victimFaction = v["vfac"].i(); for(auto& w : v["wit"].arr()) e.witnesses.push_back(w.i()); e.anonWitnesses = v["anon"].i(); e.reported = v["rep"].b();
        g_w.events.push_back(e);
    }
    for(auto& v : root["rumors"].arr()) { Rumor r; r.eventId = v["e"].i(); r.text = v["t"].str(); r.zone = v["z"].str(); r.day = v["d"].i(); r.reach = v["r"].i(1); g_w.rumors.push_back(r); }
    for(int f = 0; f < F_COUNT && f < (int)root["factions"].size(); ++f)
    {
        const json::Value& v = root["factions"][f]; FactionState& s = g_w.fac[f];
        s.respect[0] = v["resp0"].i(); s.respect[1] = v["resp1"].i(); s.hostility[0] = v["host0"].i(); s.hostility[1] = v["host1"].i();
        s.tension = v["tension"].i(); s.truce = v["truce"].b(); s.truceUntil = v["truceUntil"].i(); s.plan = v["plan"].str();
    }
    for(int w = 0; w < WHO_COUNT && w < (int)root["police"].size(); ++w)
    {
        const json::Value& v = root["police"][w]; PoliceCase& p = g_w.pcase[w];
        p.heat = v["heat"].i(); p.evidence = v["evidence"].i(); p.iaInvestigation = v["ia"].b(); p.jailDays = v["jailDays"].i();
        for(auto& c : v["charges"].arr()) p.charges.push_back(c.str());
    }
    for(auto& n : root["news"].arr()) g_w.news.push_back(n.str());
    g_w.usage = root["usage"]; g_w.pendingBatch = root["batch"];
    LOG_INFO("Мир загружен: персонажей %d, событий %d, слухов %d, день %d",
             (int)g_w.chars.size(), (int)g_w.events.size(), (int)g_w.rumors.size(), g_w.day);
    return true;
}

// ---------------- персонажи ----------------
Character* Find(int id) { for(auto& c : g_w.chars) if(c.id == id) return &c; return nullptr; }

Character* FindByPed(uintptr_t ped)
{
    if(!ped) return nullptr;
    int ref = game::Ref(ped);
    for(auto& c : g_w.chars) if(c.pedRef && c.pedRef == ref && game::FromRef(ref) == ped) return &c;
    return nullptr;
}

uintptr_t PedOf(const Character& c)
{
    if(!c.pedRef) return 0;
    uintptr_t p = game::FromRef(c.pedRef);
    return p && game::Alive(p) ? p : 0;
}

void Bind(Character& c, uintptr_t ped) { c.pedRef = game::Ref(ped); c.seenMs = game::TimeMs(); }

static void RollTraits(Character& c)
{
    Traits& t = c.tr;
    auto r = [](int lo, int hi) { return Rand(lo, hi); };
    t = Traits{ r(20, 90), r(10, 90), r(10, 90), r(10, 80), r(10, 90), r(10, 90), r(10, 90), r(10, 90), r(10, 90) };
    switch(c.faction)
    {
        case F_POLICE:  t.loyalty = r(30, 95); t.honor = r(20, 95); t.greed = r(10, 85); t.courage = r(40, 95); t.suspicion = r(40, 95); break;
        case F_GROVE: case F_BALLAS: case F_VAGOS: case F_AZTECAS: case F_RIFA: case F_DANANG: case F_TRIADS: case F_MAFIA:
            t.loyalty = r(35, 98); t.aggression = r(40, 95); t.courage = r(35, 95); t.suspicion = r(40, 95); break;
        default: t.aggression = r(5, 60); t.courage = r(5, 70); break;
    }
    if(c.pedType == game::PT_BUM) { t.greed = r(50, 95); t.talkative = r(50, 95); }
    if(c.pedType == game::PT_DEALER) { t.greed = r(60, 95); t.cunning = r(50, 95); }
}

Character& Promote(uintptr_t ped)
{
    if(Character* ex = FindByPed(ped)) return *ex;
    Character c;
    c.id = g_w.nextId++;
    c.pedType = game::PedType(ped);
    c.model = game::Model(ped);
    c.female = game::IsFemaleType(c.pedType);
    c.faction = FactionOfPedType(c.pedType);
    c.age = Rand(18, 60);
    c.name = std::string(c.female ? Pick(kFemaleNames) : Pick(kMaleNames)) + " " + Pick(kSurnames);
    if(game::IsGang(c.pedType) || c.pedType == game::PT_DEALER || c.pedType == game::PT_CRIMINAL) c.nick = Pick(kStreetNicks);
    if(c.faction == F_POLICE) { int r = Rand(0, 99); c.rank = r < 65 ? 0 : (r < 88 ? 1 : (r < 97 ? 2 : 3)); }
    else if(c.faction != F_CIVIL) { int r = Rand(0, 99); c.rank = r < 35 ? 0 : (r < 85 ? 1 : (r < 98 ? 2 : 3)); }
    RollTraits(c);
    c.story = game::StoryKey(ped);
    if(!c.story.empty())
    {   // сюжетный персонаж: настоящее имя, а не случайное
        for(auto& st : kStory) if(c.story == st.key) { c.name = st.name; c.nick.clear(); c.female = st.female; c.faction = st.faction; break; }
        if(!StoryName(c.story)) { c.name = c.story; c.name[0] = (char)toupper((unsigned char)c.name[0]); }
        c.rank = 2;
    }
    c.home = game::Pos(ped); c.homeZone = game::ZoneName();
    c.money = c.faction == F_POLICE ? Rand(80, 400) : (c.pedType == game::PT_BUM ? Rand(0, 8) : (c.pedType == game::PT_DEALER ? Rand(500, 3000) : Rand(20, 300)));
    if(game::IsGang(c.pedType)) c.money = Rand(50, 800);
    // двойная игра заложена в характер: хитрый и преданный своим может играть «в друга»
    c.secretlyLoyal = c.faction != F_CIVIL && c.faction != F_POLICE && c.faction != g_w.playerGang && c.tr.loyalty > 70 && c.tr.cunning > 60;
    c.pitch = c.female ? 1.05f + Rand(0, 25) / 100.0f : 0.75f + Rand(0, 30) / 100.0f;
    c.rate  = 0.9f + Rand(0, 25) / 100.0f;
    c.dialog = json::Value::Array();
    Bind(c, ped);
    game::KeepPed(ped, true);
    g_w.chars.push_back(std::move(c));
    Character& ref = g_w.chars.back();
    LOG_INFO("Новый персонаж #%d %s (%s), черты: преданность %d, жадность %d, хитрость %d",
             ref.id, ref.name.c_str(), game::PedTypeName(ref.pedType), ref.tr.loyalty, ref.tr.greed, ref.tr.cunning);
    return ref;
}

void Remember(Character& c, const std::string& text)
{
    c.mem.push_back(Note{ g_w.day, text });
    if(c.mem.size() > 24) c.mem.erase(c.mem.begin(), c.mem.begin() + (c.mem.size() - 24));
}

void ChangeRelation(Character& c, int who, int dT, int dR, int dL, int dF, const char* why)
{
    Relation& r = c.rel[who];
    auto lim = [](int d) { return Clamp(d, -20, 20); };   // за раз — не больше 20
    r.trust = Clamp(r.trust + lim(dT), -100, 100);
    r.respect = Clamp(r.respect + lim(dR), -100, 100);
    r.liking = Clamp(r.liking + lim(dL), -100, 100);
    r.fear = Clamp(r.fear + lim(dF), 0, 100);
    r.met = true;
    if(why && *why) LOG_INFO("Отношения #%d %s: доверие %d, уважение %d, симпатия %d, страх %d (%s)",
                             c.id, c.name.c_str(), r.trust, r.respect, r.liking, r.fear, why);
}

static const char* Level(int v)
{
    if(v >= 90) return "очень высокая"; if(v >= 70) return "высокая"; if(v >= 45) return "средняя"; if(v >= 25) return "низкая"; return "очень низкая";
}

std::string Card(const Character& c, int who)
{
    std::string s;
    s += Fmt("Тебя зовут %s%s%s%s. %s, %d лет. ", c.name.c_str(), c.nick.empty() ? "" : " по кличке «", c.nick.c_str(), c.nick.empty() ? "" : "»",
             c.female ? "Женщина" : "Мужчина", c.age);
    s += Fmt("Ты — %s", game::PedTypeName(c.pedType));
    if(c.faction == F_POLICE) { static const char* r[] = { "патрульный", "сержант", "детектив", "капитан участка" }; s += Fmt(", звание: %s", r[Clamp(c.rank, 0, 3)]); }
    else if(c.faction != F_CIVIL) { static const char* r[] = { "молодой", "боец", "смотрящий за районом", "один из главных" }; s += Fmt(" (%s), положение в банде: %s", FactionName(c.faction), r[Clamp(c.rank, 0, 3)]); }
    s += Fmt(". Живёшь/бываешь в районе %s. Денег при себе около $%d.\n", c.homeZone.empty() ? "неизвестно" : c.homeZone.c_str(), c.money);
    if(!c.role.empty()) s += "Чем занят: " + c.role + "\n";
    if(!c.backstory.empty()) s += "Прошлое: " + c.backstory + "\n";
    if(!c.dream.empty()) s += "Мечта/цель в жизни: " + c.dream + "\n";
    if(!c.fearOf.empty()) s += "Боится: " + c.fearOf + "\n";
    if(!c.speech.empty()) s += "Манера речи: " + c.speech + "\n";
    const Traits& t = c.tr;
    s += Fmt("Характер: преданность своим — %s; жадность — %s; смелость — %s; агрессия — %s; честь/принципы — %s; хитрость — %s; "
             "болтливость — %s; амбиции — %s; мнительность — %s.\n",
             Level(t.loyalty), Level(t.greed), Level(t.courage), Level(t.aggression), Level(t.honor), Level(t.cunning),
             Level(t.talkative), Level(t.ambition), Level(t.suspicion));
    if(c.secretlyLoyal && c.faction != F_POLICE) s += "ТАЙНА (игрок не знает): ты на самом деле предан своей банде. Если игрок набивается в друзья — можешь подыграть, чтобы выведать его планы и сдать своим или заманить в засаду.\n";
    if(c.exposed) s += "Твоя банда узнала о твоих связях с игроком — у тебя проблемы со своими.\n";
    if(!c.goal.empty()) s += "Твоя текущая цель: " + c.goal + "\n";
    const Relation& r = c.rel[who];
    s += Fmt("Отношение к собеседнику (%s): доверие %d, уважение %d, симпатия %d, страх %d (шкалы −100…100).",
             who == WHO_CJ ? g_w.playerNick.c_str() : (g_w.friendNick.empty() ? "друг CJ" : g_w.friendNick.c_str()),
             r.trust, r.respect, r.liking, r.fear);
    if(!r.met) s += " Вы раньше не встречались.";
    if(r.debt > 0) s += Fmt(" Ты должен ему $%d.", r.debt);
    if(r.debt < 0) s += Fmt(" Он должен тебе $%d.", -r.debt);
    if(r.payroll) s += Fmt(" Ты у него на зарплате: $%d в неделю.", r.weekly);
    if(r.informant) s += " Ты тайно сливаешь ему сведения.";
    if(r.contact) s += " У него есть твой номер телефона.";
    s += "\n";
    if(!c.summary.empty()) s += "Ваша история: " + c.summary + "\n";
    if(!c.mem.empty())
    {
        s += "Что ты помнишь:\n";
        size_t from = c.mem.size() > 10 ? c.mem.size() - 10 : 0;
        for(size_t i = from; i < c.mem.size(); ++i) s += Fmt("- (день %d) %s\n", c.mem[i].day, c.mem[i].text.c_str());
    }
    return s;
}

std::string KnownFacts(const Character& c, int who)
{
    std::string s;
    int n = 0;
    for(auto it = g_w.events.rbegin(); it != g_w.events.rend() && n < 6; ++it)
    {
        const Event& e = *it;
        if(e.who != who) continue;
        bool saw = std::find(e.witnesses.begin(), e.witnesses.end(), c.id) != e.witnesses.end();
        if(saw) { s += Fmt("- видел сам (день %d, %s): %s\n", e.day, e.zone.c_str(), e.text.c_str()); ++n; }
    }
    for(auto& r : RumorsFor(c, 4)) { s += "- слышал: " + r + "\n"; }
    const FactionState& f = g_w.fac[c.faction];
    if(c.faction != F_CIVIL && c.faction != F_NONE)
        s += Fmt("- отношение твоих (%s) к нему: уважение %d, вражда %d%s\n", FactionName(c.faction), f.respect[who], f.hostility[who],
                 f.plan.empty() ? "" : (std::string(", у твоих сейчас план: ") + f.plan).c_str());
    if(c.faction == F_POLICE)
    {
        const PoliceCase& p = g_w.pcase[who];
        s += Fmt("- по базе полиции на него: розыск сейчас %d зв., «копают» %d/100, улик %d/100", game::Wanted(), p.heat, p.evidence);
        if(!p.charges.empty()) { s += ", статьи: "; for(size_t i = 0; i < p.charges.size() && i < 5; ++i) s += (i ? ", " : "") + p.charges[i]; }
        s += "\n";
        if(p.iaInvestigation) s += "- служба собственной безопасности сейчас проверяет продажных копов — опасно брать деньги\n";
    }
    return s.empty() ? "- ничего особенного\n" : s;
}

// ---------------- события ----------------
int Day() { return g_w.day; }
int NowMin() { return g_w.day * 1440 + game::Hour() * 60 + game::Minute(); }

Event& AddEvent(const std::string& kind, const std::string& text, const game::Vec& pos, int sev, int who)
{
    Event e; e.id = g_w.nextId++; e.day = g_w.day; e.minute = game::Hour() * 60 + game::Minute(); e.kind = kind; e.text = text;
    e.pos = pos; e.zone = game::ZoneName(); e.severity = sev; e.who = who;
    g_w.events.push_back(e);
    if(g_w.events.size() > 400) g_w.events.erase(g_w.events.begin(), g_w.events.begin() + 100);
    LOG_INFO("Событие #%d [%s] %s (тяжесть %d)", e.id, kind.c_str(), text.c_str(), sev);
    if(g_forwardEvent) g_forwardEvent(g_w.events.back());
    return g_w.events.back();
}

void AddWitness(Event& e, int charId)
{
    if(std::find(e.witnesses.begin(), e.witnesses.end(), charId) == e.witnesses.end()) e.witnesses.push_back(charId);
}

void SpreadRumors()
{
    // Каждое событие со свидетелями превращается в слух; болтливые разносят дальше
    for(auto& e : g_w.events)
    {
        if(e.day < g_w.day - 7 || e.severity < 2) continue;
        int talk = 0;
        for(int id : e.witnesses) if(Character* c = Find(id)) talk += c->tr.talkative;
        talk += e.anonWitnesses * 40;
        if(!talk) continue;
        Rumor* r = nullptr;
        for(auto& x : g_w.rumors) if(x.eventId == e.id) { r = &x; break; }
        if(!r)
        {
            if(Rand(0, 150) > talk + e.severity * 20) continue;
            Rumor nr; nr.eventId = e.id; nr.zone = e.zone; nr.day = g_w.day; nr.reach = 1;
            nr.text = Fmt("говорят, в %s %s", e.zone.empty() ? "городе" : e.zone.c_str(), e.text.c_str());
            g_w.rumors.push_back(nr);
        }
        else if(r->reach < 3 && Rand(0, 300) < talk + e.severity * 25) r->reach++;
    }
    if(g_w.rumors.size() > 120) g_w.rumors.erase(g_w.rumors.begin(), g_w.rumors.begin() + 40);
}

std::vector<std::string> RumorsFor(const Character& c, int max)
{
    std::vector<std::string> out;
    for(auto it = g_w.rumors.rbegin(); it != g_w.rumors.rend() && (int)out.size() < max; ++it)
    {
        bool reach = it->reach >= 2 || it->zone == c.homeZone || (it->reach >= 3);
        if(reach) out.push_back(it->text);
    }
    return out;
}

// ---------------- время ----------------
void Tick()
{
    int h = game::Hour();
    if(g_w.lastHour < 0) { g_w.lastHour = h; return; }
    if(h == g_w.lastHour) return;
    if(h < g_w.lastHour) // новые сутки
    {
        g_w.day++;
        // зарплаты «своим людям»: раз в 7 дней игрок должен платить (UI напоминает), иначе обида
        for(auto& c : g_w.chars)
            for(int w = 0; w < WHO_COUNT; ++w)
            {
                Relation& r = c.rel[w];
                if(r.payroll && g_w.day - r.paidDay > 7)
                {
                    ChangeRelation(c, w, -10, -5, -8, 0, "не заплатил вовремя");
                    if(g_w.day - r.paidDay > 14) { r.payroll = false; Remember(c, "Игрок перестал платить — договор разорван"); News(Fmt("%s больше на тебя не работает: ты не платил.", c.name.c_str())); }
                }
            }
        // перемирия истекают, напряжение спадает
        for(int f = 0; f < F_COUNT; ++f)
        {
            FactionState& s = g_w.fac[f];
            if(s.truce && g_w.day > s.truceUntil) { s.truce = false; News(Fmt("Перемирие с %s закончилось.", FactionName(f))); }
            if(s.tension > 0) s.tension = Clamp(s.tension - 2, 0, 100);
            for(int w = 0; w < WHO_COUNT; ++w) s.hostility[w] = Clamp(s.hostility[w] - 1, 0, 100);
        }
        for(int w = 0; w < WHO_COUNT; ++w) { PoliceCase& p = g_w.pcase[w]; p.heat = Clamp(p.heat - 3, 0, 100); }
        if(g_forgetDays > 0) { int n = ForgetUnimportant(g_forgetDays); if(n) LOG_INFO("Память: за день забыто персонажей: %d (осталось %d)", n, (int)g_w.chars.size()); }
    }
    g_w.lastHour = h;
    SpreadRumors();
}

void GangReactToKill(int faction, int who, int severity)
{
    if(faction <= F_POLICE || faction >= F_COUNT) return;
    FactionState& s = g_w.fac[faction];
    s.hostility[who] = Clamp(s.hostility[who] + 8 * severity, 0, 100);
    s.respect[who] = Clamp(s.respect[who] + 2, -100, 100);   // боятся/уважают силу
    if(faction != g_w.playerGang) s.tension = Clamp(s.tension + 5 * severity, 0, 100);
    else s.respect[who] = Clamp(s.respect[who] - 15 * severity, -100, 100);
}

std::string FactionSummary(int who)
{
    std::string s;
    for(int f = F_POLICE; f < F_COUNT; ++f)
    {
        const FactionState& x = g_w.fac[f];
        s += Fmt("%s: уважение %d, вражда %d%s%s; ", FactionName(f), x.respect[who], x.hostility[who],
                 x.truce ? ", перемирие" : "", x.tension > 70 ? ", ВОЙНА" : "");
    }
    return s;
}

void News(const std::string& s)
{
    g_w.news.push_back(s);
    if(g_w.news.size() > 40) g_w.news.erase(g_w.news.begin());
    LOG_INFO("Новость: %s", s.c_str());
}

} // namespace world
