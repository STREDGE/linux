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
        bool        sharedWanted = true;    // розыск общий на двоих (решает хост)
        net::Config net;
    };

    Data Load();
    void Save(const Data& d);

    // Расположение элементов поверх игры (доли экрана) и их масштаб
    struct LayoutItem { float x, y, s; };
    enum LayoutId { L_MIC, L_TEAM, L_FEED, L_TALK, L_COUNT };
    struct Layout { LayoutItem item[L_COUNT]; };
    Layout DefaultLayout();
    Layout LoadLayout();
    void   SaveLayout(const Layout& l);

    const char* RoleToString(net::Role r);   // off | echo | host | client
}
