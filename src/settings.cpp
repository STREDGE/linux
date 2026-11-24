#include "settings.h"

#include <mod/amlmod.h>
#include <mod/config.h>

#include <string.h>
#include <strings.h>

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

    char role[16];
    cfg->GetString(role, sizeof(role), "Role", "off", "Network"); // off | echo | host | client
    d.net.role = RoleFromString(role);
    cfg->GetString(d.net.hostAddress, sizeof(d.net.hostAddress), "HostAddress", "100.64.0.1", "Network");
    d.net.port = (uint16_t)cfg->GetInt("Port", 7777, "Network");
    char model[net::kNickLen] = "Player";
    platform::DeviceModel(model, sizeof(model));
    cfg->GetString(d.net.nick, sizeof(d.net.nick), "Nick", model, "Network");
    cfg->Save();
    return d;
}

void Save(const Data& d)
{
    ConfigEntry* e;
    e = cfg->Bind("Enabled", true, "General");          e->SetBool(d.enabled);                 delete e;
    e = cfg->Bind("FriendlyFire", false, "General");    e->SetBool(d.friendlyFire);            delete e;
    e = cfg->Bind("Role", "off", "Network");            e->SetString(RoleToString(d.net.role)); delete e;
    e = cfg->Bind("HostAddress", "100.64.0.1", "Network"); e->SetString(d.net.hostAddress);    delete e;
    e = cfg->Bind("Port", 7777, "Network");             e->SetInt(d.net.port);                 delete e;
    e = cfg->Bind("Nick", "Player", "Network");         e->SetString(d.net.nick);              delete e;
    cfg->Save();
}

} // namespace settings
