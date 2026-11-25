// SA Living — живые педы с ИИ для GTA SA Android 2.10 arm64 (AML).
// Одиночная игра: ИИ со своим ключом. Кооп (если стоит SA Coop и друг подключён):
// общий мир у хоста, запросы друга идут через ключ хоста — всё автоматически.

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/flogger.h>
#include <mod/interface.h>
#include <sautils/isautils.h>

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "ai/claude.h"
#include "game/game.h"
#include "net/link.h"
#include "platform/bridge.h"
#include "settings.h"
#include "sim/life.h"
#include "sim/police.h"
#include "talk/talk.h"
#include "ui/hud.h"
#include "util/log.h"
#include "world/world.h"

MYMODCFG(net.saliving.mod, SA Living, 0.1.0, SA Coop Team)

static FLogger g_fileLog("saliving.txt");

void LogInfo(const char* fmt, ...)
{
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    g_fileLog.Info("%s", buf); logger->Info("%s", buf);
}
void LogError(const char* fmt, ...)
{
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    g_fileLog.Error("%s", buf); logger->Error("%s", buf);
}

static bool   g_ok = false;
static double g_nextSave = 0, g_nextBridgeTry = 0, g_nextSync = 0;
static bool   g_ttsAsked = false;

// ---------- кооп: сообщения ----------
namespace coopnet
{
void OnMessage(const json::Value& m)
{
    const std::string& t = m["t"].str();
    Role r = GetRole();
    if(r == HOST)
    {
        if(t == "talk") talk::HandleFriendTalk(m);
        else if(t == "ai") claude::HandleProxyRequest(m);
        else if(t == "event")
        {
            world::Event& e = world::AddEvent(m["kind"].str(), m["text"].str(), game::Vec{}, m["sev"].i(1), world::WHO_FRIEND);
            e.zone = m["zone"].str(); e.victimFaction = m["vfac"].i(); e.reported = m["reported"].b(); e.anonWitnesses = m["anon"].i();
            if(e.kind == "kill") world::GangReactToKill(e.victimFaction, world::WHO_FRIEND, e.severity >= 4 ? 2 : 1);
            if(e.reported) { auto& p = world::W().pcase[world::WHO_FRIEND]; p.heat = world::Clamp(p.heat + 5 * e.severity, 0, 100); }
        }
    }
    else if(r == CLIENT)
    {
        if(t == "reply") talk::HandleHostReply(m);
        else if(t == "ai_res") claude::HandleProxyResult(m);
        else if(t == "news") { for(auto& n : m["items"].arr()) world::News(n.str()); talk::Notify("Новости города (телефон)"); }
        else if(t == "sync")
        {
            // контакты друга из общего мира: держим у себя «копии» без педа, чтобы звонить
            for(auto& c : m["contacts"].arr())
            {
                world::Character* local = nullptr;
                for(auto& x : world::W().chars) if(x.hostId == c["id"].i()) local = &x;
                if(!local)
                {
                    world::Character nc; nc.id = world::W().nextId++; nc.hostId = c["id"].i(); nc.dialog = json::Value::Array();
                    world::W().chars.push_back(nc); local = &world::W().chars.back();
                }
                local->name = c["name"].str(); local->nick = c["nick"].str(); local->faction = c["faction"].i();
                local->pedType = c["pedType"].i(4); local->alive = c["alive"].b(true);
                local->rel[0].contact = true; local->rel[0].met = true;
                local->rel[0].payroll = c["payroll"].b(); local->rel[0].informant = c["informant"].b();
                local->rel[0].trust = c["trust"].i(); local->rel[0].liking = c["liking"].i(); local->rel[0].respect = c["respect"].i(); local->rel[0].fear = c["fear"].i();
            }
            auto& p = world::W().pcase[0];
            p.heat = m["heat"].i(); p.evidence = m["evidence"].i();
        }
    }
}
}

static void ForwardEvent(const world::Event& e)
{
    if(coopnet::GetRole() != coopnet::CLIENT) return;
    json::Value m = json::Value::Object();
    m["t"] = "event"; m["kind"] = e.kind; m["text"] = e.text; m["sev"] = e.severity; m["zone"] = e.zone;
    m["vfac"] = e.victimFaction; m["reported"] = e.reported; m["anon"] = e.anonWitnesses;
    coopnet::Send(m);
}

static void HostSync()
{
    json::Value m = json::Value::Object(); m["t"] = "sync";
    json::Value list = json::Value::Array();
    for(auto& c : world::W().chars)
    {
        const world::Relation& r = c.rel[world::WHO_FRIEND];
        if(!r.contact) continue;
        json::Value v = json::Value::Object();
        v["id"] = c.id; v["name"] = c.name; v["nick"] = c.nick; v["faction"] = c.faction; v["pedType"] = c.pedType; v["alive"] = c.alive;
        v["payroll"] = r.payroll; v["informant"] = r.informant; v["trust"] = r.trust; v["liking"] = r.liking; v["respect"] = r.respect; v["fear"] = r.fear;
        list.push(v);
    }
    m["contacts"] = list;
    m["heat"] = world::W().pcase[world::WHO_FRIEND].heat; m["evidence"] = world::W().pcase[world::WHO_FRIEND].evidence;
    coopnet::Send(m);
}

// ---------- игровой кадр ----------
static void Frame()
{
    if(!g_ok || !settings::Get().enabled) return;
    double now = talk::Now();
    if(!bridge::Ready() && now >= g_nextBridgeTry) { g_nextBridgeTry = now + 10; bridge::Init(); }
    if(bridge::Ready() && !g_ttsAsked) { g_ttsAsked = true; bridge::TtsInit(settings::Get().lang.c_str()); }
    claude::Pump();
    coopnet::Frame();
    if(!game::Player()) return;
    world::Tick();
    talk::Frame();
    police::Frame();
    life::Frame();
    if(coopnet::GetRole() == coopnet::HOST && now >= g_nextSync) { g_nextSync = now + 15; HostSync(); }
    if(now >= g_nextSave)
    {
        if(g_nextSave > 0) { world::W().usage = claude::SaveUsage(); world::Save(world::DefaultPath()); }
        g_nextSave = now + 60;
    }
}

SDECL_HOOKv(GameProcess)
{
    GameProcess();
    Frame();
}

// Сохранение игры — заодно сохраняем мир (тот же поток игры, ничего не удаляем)
SDECL_HOOKb(GenericSave, int slot)
{
    bool r = GenericSave(slot);
    if(g_ok) { world::W().usage = claude::SaveUsage(); world::Save(world::DefaultPath()); }
    return r;
}

ON_MOD_LOAD()
{
    logger->SetTag("SALiving");
    LogInfo("SA Living %s загружается", modinfo->VersionString());
    void* h = aml->GetLibHandle("libGTASA.so");
    if(!h) { LogError("libGTASA.so не найдена — мод выключен"); return; }

    settings::Load();
    const settings::Data& s = settings::Get();
    if(!s.enabled) { LogInfo("Мод выключен в настройках (Enabled = 0)"); return; }

    claude::Settings cs; cs.key = s.key; cs.model = s.model; cs.effort = s.effort;
    claude::Configure(cs);
    claude::SetDailyLimit(s.dailyLimit);
    LogInfo("ИИ: ключ %s, модель %s, режим %s", claude::HasKey() ? "есть" : "нет (вставь в Настройки > AI)", s.model.c_str(), s.effort.c_str());

    world::Load(world::DefaultPath());
    claude::LoadUsage(world::W().usage);
    world::g_forwardEvent = ForwardEvent;

    bool gameOk = game::Init(h);
    police::Install(h);
    talk::Init();
    bool hudOk = hud::Init(h);
    bridge::Init();

    void* gp = (void*)aml->GetSym(h, "_ZN5CGame7ProcessEv");
    void* gs = (void*)aml->GetSym(h, "_ZN19CGenericGameStorage11GenericSaveEi");
    if(gp) aml->Hook(gp, (void*)HookOf_GameProcess, (void**)&GameProcess);
    if(gs) aml->Hook(gs, (void*)HookOf_GenericSave, (void**)&GenericSave);
    g_ok = gameOk && gp;
    LogInfo("SA Living: %s (интерфейс %s, мост Android %s)", g_ok ? "готов" : "НЕ все функции игры найдены",
            hudOk ? "есть" : "нет", bridge::Ready() ? "есть" : "позже");
}

ON_ALL_MODS_LOAD()
{
    ISAUtils* sau = (ISAUtils*)GetInterface("SAUtils");
    if(sau) hud::AttachSAUtils(sau);
    else LogError("SAUtils не найден — плитки «AI» в настройках не будет (ключ можно вписать в net.saliving.mod.ini)");
    coopnet::Init();
}

ON_GAME_CRASH()
{
    uintptr_t pc = mcontext ? (uintptr_t)mcontext->pc : 0;
    g_fileLog.Error("ВЫЛЕТ: сигнал %d, код %d, библиотека %s, адрес +0x%lX", sig, code, library ? library : "?", (unsigned long)(pc - libaddr));
}
