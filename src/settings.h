#pragma once
// Настройки мода в конфиге AML:
// Android/data/com.rockstargames.gtasa/configs/net.sacoop.mod.ini

#include "net/session.h"

namespace settings
{
    struct Data
    {
        bool        enabled = true;
        bool        friendlyFire = false;
        net::Config net;
    };

    Data Load();
    void Save(const Data& d);

    const char* RoleToString(net::Role r);   // off | echo | host | client
}
