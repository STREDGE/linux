#pragma once
// Идёт ли кооп SA Coop (интерфейс AML «SACoopNet»): «Новая игра» из лобби — всегда обычная GTA.
#include <mod/amlmod.h>
#include <mod/interface.h>
#include <stdint.h>

namespace sacoop_role
{
    struct Api { uint32_t version; int (*GetRole)(); bool (*Connected)(); };   // начало SACoopNetApi (sa-living/src/net/sacoop_api.h)
    inline bool CoopActive()
    {
        Api* a = (Api*)GetInterface("SACoopNet");
        if(!a || !a->GetRole) return false;
        int r = a->GetRole();
        return r == 2 || r == 3;
    }
}
