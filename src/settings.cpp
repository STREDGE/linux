#include "settings.h"

#include <mod/amlmod.h>
#include <mod/config.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "platform/android.h"

namespace settings
{

const char* RoleToString(net::Role r)
{
    switch(r)
    {
        case net::Role::Echo:   return "echo";
        case net::Role::Host:   return "host";
        case net::Role::Client: return "client";
        default:                return "off";
    }
}

static net::Role RoleFromString(const char* s)
{
    if(!strcasecmp(s, "echo"))   return net::Role::Echo;
    if(!strcasecmp(s, "host"))   return net::Role::Host;
    if(!strcasecmp(s, "client")) return net::Role::Client;
    return net::Role::Off;
}

Data Load()
{
    Data d;
    d.enabled      = cfg->GetBool("Enabled", true, "General");
    d.friendlyFire = cfg->GetBool("FriendlyFire", false, "General");
    d.sharedWanted = cfg->GetBool("SharedWanted", true, "General");

    char role[16];
    cfg->GetString(role, sizeof(role), "Role", "off", "Network"); // off | echo | host | client
    d.net.role = RoleFromString(role);
    cfg->GetString(d.net.hostAddress, sizeof(d.net.hostAddress), "HostAddress", "100.64.0.1", "Network");
    d.net.port = (uint16_t)cfg->GetInt("Port", 7777, "Network");
    char model[net::kNickLen] = "Player";
    platform::DeviceModel(model, sizeof(model));
    cfg->GetString(d.net.nick, sizeof(d.net.nick), "Nick", model, "Network");
    cfg->GetString(d.net.relay, sizeof(d.net.relay), "Relay", "", "Network");   // сервер-посредник (адрес:порт), пусто — Tailscale
    // Свой ID точки: 6 цифр, создаётся один раз (друг вводит его, чтобы подключиться)
    cfg->GetString(d.net.myId, sizeof(d.net.myId), "MyId", "", "Network");
    bool idOk = strlen(d.net.myId) == 6;
    for(int i = 0; idOk && i < 6; ++i) if(d.net.myId[i] < '0' || d.net.myId[i] > '9') idOk = false;
    if(!idOk)
    {
        uint32_t r = arc4random_uniform(900000) + 100000;
        snprintf(d.net.myId, sizeof(d.net.myId), "%06u", r);
        ConfigEntry* e = cfg->Bind("MyId", "", "Network"); e->SetString(d.net.myId); delete e;
    }
    cfg->Save();
    return d;
}

void Save(const Data& d)
{
    ConfigEntry* e;
    e = cfg->Bind("Enabled", true, "General");          e->SetBool(d.enabled);                 delete e;
    e = cfg->Bind("FriendlyFire", false, "General");    e->SetBool(d.friendlyFire);            delete e;
    e = cfg->Bind("SharedWanted", true, "General");     e->SetBool(d.sharedWanted);            delete e;
    e = cfg->Bind("Role", "off", "Network");            e->SetString(RoleToString(d.net.role)); delete e;
    e = cfg->Bind("HostAddress", "100.64.0.1", "Network"); e->SetString(d.net.hostAddress);    delete e;
    e = cfg->Bind("Port", 7777, "Network");             e->SetInt(d.net.port);                 delete e;
    e = cfg->Bind("Nick", "Player", "Network");         e->SetString(d.net.nick);              delete e;
    e = cfg->Bind("Relay", "", "Network");              e->SetString(d.net.relay);             delete e;
    if(d.net.myId[0]) { e = cfg->Bind("MyId", "", "Network"); e->SetString(d.net.myId); delete e; }
    cfg->Save();
}

static const char* kLayoutNames[L_COUNT] = { "Mic", "Team", "Feed", "Talk" };

Layout DefaultLayout()
{
    Layout l;
    l.item[L_MIC]  = { 0.66f, 0.13f, 1.0f };  // центр кнопки
    l.item[L_TEAM] = { 0.012f, 0.30f, 1.0f }; // левый верхний угол панели
    l.item[L_FEED] = { 0.50f, 0.66f, 1.0f };  // низ ленты уведомлений, по центру
    l.item[L_TALK] = { 0.50f, 0.03f, 1.0f };  // верх полоски «говорит», по центру
    return l;
}

Layout LoadLayout()
{
    Layout d = DefaultLayout(), l = d;
    char key[32];
    for(int i = 0; i < L_COUNT; ++i)
    {
        snprintf(key, sizeof(key), "%sX", kLayoutNames[i]); l.item[i].x = cfg->GetInt(key, (int)(d.item[i].x * 10000), "Layout") / 10000.0f;
        snprintf(key, sizeof(key), "%sY", kLayoutNames[i]); l.item[i].y = cfg->GetInt(key, (int)(d.item[i].y * 10000), "Layout") / 10000.0f;
        snprintf(key, sizeof(key), "%sScale", kLayoutNames[i]); l.item[i].s = cfg->GetInt(key, (int)(d.item[i].s * 100), "Layout") / 100.0f;
        if(l.item[i].s < 0.5f || l.item[i].s > 2.5f) l.item[i].s = 1.0f;
    }
    cfg->Save();
    return l;
}

void SaveLayout(const Layout& l)
{
    char key[32];
    for(int i = 0; i < L_COUNT; ++i)
    {
        ConfigEntry* e;
        snprintf(key, sizeof(key), "%sX", kLayoutNames[i]);     e = cfg->Bind(key, 0, "Layout"); e->SetInt((int)(l.item[i].x * 10000)); delete e;
        snprintf(key, sizeof(key), "%sY", kLayoutNames[i]);     e = cfg->Bind(key, 0, "Layout"); e->SetInt((int)(l.item[i].y * 10000)); delete e;
        snprintf(key, sizeof(key), "%sScale", kLayoutNames[i]); e = cfg->Bind(key, 100, "Layout"); e->SetInt((int)(l.item[i].s * 100)); delete e;
    }
    cfg->Save();
}

} // namespace settings
