#include "gta.h"

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <math.h>

namespace gta
{
    void      (*SetupPlayerPed)(int)                                  = nullptr;
    void      (*RemovePlayerPed)(int)                                 = nullptr;
    uintptr_t (*GetPad)(int)                                          = nullptr;
    void      (*WorldAdd)(uintptr_t)                                  = nullptr;
    void      (*WorldRemove)(uintptr_t)                               = nullptr;
    void      (*PhysicalRemoveAndAdd)(uintptr_t)                      = nullptr;
    void      (*MatrixSetRotateZOnly)(uintptr_t, float)               = nullptr;
    void      (*MatrixUpdateRW)(uintptr_t)                            = nullptr;
    void      (*EntityUpdateRwFrame)(uintptr_t)                       = nullptr;
    int       (*GetPedRef)(uintptr_t)                                 = nullptr;
    int       (*SetEntityBlip)(int, int, unsigned int, int, char*)    = nullptr;
    void      (*ChangeBlipColour)(int, unsigned int)                  = nullptr;
    void      (*ChangeBlipScale)(int, int)                            = nullptr;
    void      (*ClearBlip)(int)                                       = nullptr;
    bool      (*PedIsAlive)(uintptr_t)                                = nullptr;

    uintptr_t Players = 0;

    namespace
    {
        bool Resolve(void* handle, void** out, const char* sym)
        {
            *out = (void*)aml->GetSym(handle, sym);
            if(!*out) logger->Error("Не найден символ %s", sym);
            return *out != nullptr;
        }
    }

    bool Init(void* h, uintptr_t /*base*/)
    {
        bool ok = true;
        ok &= Resolve(h, (void**)&SetupPlayerPed,       "_ZN10CPlayerPed14SetupPlayerPedEi");
        ok &= Resolve(h, (void**)&RemovePlayerPed,      "_ZN10CPlayerPed15RemovePlayerPedEi");
        ok &= Resolve(h, (void**)&GetPad,               "_ZN4CPad6GetPadEi");
        ok &= Resolve(h, (void**)&WorldAdd,             "_ZN6CWorld3AddEP7CEntity");
        ok &= Resolve(h, (void**)&WorldRemove,          "_ZN6CWorld6RemoveEP7CEntity");
        ok &= Resolve(h, (void**)&PhysicalRemoveAndAdd, "_ZN9CPhysical12RemoveAndAddEv");
        ok &= Resolve(h, (void**)&MatrixSetRotateZOnly, "_ZN7CMatrix14SetRotateZOnlyEf");
        ok &= Resolve(h, (void**)&MatrixUpdateRW,       "_ZNK7CMatrix8UpdateRWEv");
        ok &= Resolve(h, (void**)&EntityUpdateRwFrame,  "_ZN7CEntity13UpdateRwFrameEv");
        ok &= Resolve(h, (void**)&GetPedRef,            "_ZN6CPools9GetPedRefEP4CPed");
        ok &= Resolve(h, (void**)&SetEntityBlip,        "_ZN6CRadar13SetEntityBlipE9eBlipTypeij12eBlipDisplayPc");
        ok &= Resolve(h, (void**)&ChangeBlipColour,     "_ZN6CRadar16ChangeBlipColourEij");
        ok &= Resolve(h, (void**)&ChangeBlipScale,      "_ZN6CRadar15ChangeBlipScaleEii");
        ok &= Resolve(h, (void**)&ClearBlip,            "_ZN6CRadar9ClearBlipEi");
        ok &= Resolve(h, (void**)&PedIsAlive,           "_ZNK4CPed7IsAliveEv");
        ok &= Resolve(h, (void**)&Players,              "_ZN6CWorld7PlayersE");
        return ok;
    }

    CVector GetPosition(uintptr_t e)
    {
        uintptr_t m = Field<uintptr_t>(e, off::EntMatrix);
        return m ? Field<CVector>(m, off::MatPos) : Field<CVector>(e, off::EntPlacementPos);
    }

    void SetPosition(uintptr_t e, const CVector& p)
    {
        uintptr_t m = Field<uintptr_t>(e, off::EntMatrix);
        if(m) Field<CVector>(m, off::MatPos) = p;
        else  Field<CVector>(e, off::EntPlacementPos) = p;
    }

    float GetMatrixHeading(uintptr_t e)
    {
        uintptr_t m = Field<uintptr_t>(e, off::EntMatrix);
        if(!m) return Field<float>(e, off::EntPlacementHead);
        const CVector& fwd = Field<CVector>(m, off::MatForward);
        return atan2f(-fwd.x, fwd.y);
    }
}
