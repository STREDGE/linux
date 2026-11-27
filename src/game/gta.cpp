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
    uintptr_t (*GetPedFromRef)(int)                                   = nullptr;
    int       (*SetEntityBlip)(int, int, unsigned int, int, char*)    = nullptr;
    void      (*ChangeBlipColour)(int, unsigned int)                  = nullptr;
    void      (*ChangeBlipScale)(int, int)                            = nullptr;
    void      (*ClearBlip)(int)                                       = nullptr;
    bool      (*PedIsAlive)(uintptr_t)                                = nullptr;
    void      (*PedSetCharCreatedBy)(uintptr_t, uint8_t)              = nullptr;

    uintptr_t (*CreateCarForScript)(int, CVector, bool)                = nullptr;
    void      (*RequestModel)(int, int)                                = nullptr;
    void      (*LoadAllRequestedModels)(bool)                          = nullptr;
    int       (*GetVehicleRef)(uintptr_t)                              = nullptr;
    uintptr_t (*GetVehicleFromRef)(int)                                = nullptr;
    void*     (*TaskNew)(unsigned long)                                = nullptr;
    void      (*TaskMgrSetTask)(uintptr_t, void*, int, bool)           = nullptr;
    uintptr_t (*TaskMgrGetActive)(uintptr_t)                           = nullptr;
    void      (*TaskEnterAsDriver)(void*, uintptr_t)                   = nullptr;
    void      (*TaskEnterAsPassenger)(void*, uintptr_t, int, bool)     = nullptr;
    void      (*TaskLeaveCar)(void*, uintptr_t, int, int, bool, bool)  = nullptr;
    void      (*TaskSetPedOutCtor)(void*, uintptr_t, int, bool)        = nullptr;
    void      (*TaskSetPedOutDtor)(void*)                              = nullptr;
    bool      (*TaskSetPedOutProcess)(void*, uintptr_t)                = nullptr;
    bool      (*SetPedInCarDirect)(uintptr_t, uintptr_t, int, bool)    = nullptr;
    int       (*DoorForPassengerSeat)(uintptr_t, int)                  = nullptr;
    void      (*GiveWeapon)(uintptr_t, int, unsigned, bool)            = nullptr;
    void      (*SetCurrentWeaponType)(uintptr_t, int)                  = nullptr;
    int       (*GetWeaponSlot)(uintptr_t, int)                         = nullptr;
    bool      (*WeaponFire)(uintptr_t, uintptr_t, CVector*, CVector*, uintptr_t, CVector*, CVector*) = nullptr;
    bool      (*GenerateDamageEvent)(uintptr_t, uintptr_t, int, int, int, int) = nullptr;
    void      (*PedSetAimFlag)(uintptr_t, float)                       = nullptr;
    void      (*PedClearAimFlag)(uintptr_t)                            = nullptr;
    bool      (*PointGunAtPosition)(uintptr_t, const CVector&, float)  = nullptr;
    uintptr_t (*FindPlayerWanted)(int)                                 = nullptr;
    void      (*WantedSetLevel)(uintptr_t, int)                        = nullptr;
    void      (*WantedCheatLevel)(uintptr_t, int)                      = nullptr;
    void      (*PedTeleport)(uintptr_t, CVector, bool)                 = nullptr;
    uintptr_t VehicleAnimGroups = 0, AnimAssocGroupsVar = 0;
    int       (*VehAnimGroupGetGroup)(uintptr_t, int)                  = nullptr;
    const char* (*AnimBlockName)(int)                                  = nullptr;
    int       (*AnimBlockIndex)(const char*)                           = nullptr;
    void      (*GangDriveByCtor)(void*, uintptr_t, const CVector*, float, int8_t, int8_t, bool) = nullptr;
    bool      (*VehCanPedLeanOut)(uintptr_t, uintptr_t)                = nullptr;
    uintptr_t (*GetWeaponInfo)(int, int8_t)                            = nullptr;
    void      (*PedAddWeaponModel)(uintptr_t, int)                     = nullptr;
    void      (*EventScriptCommandCtor)(void*, int, void*, bool)       = nullptr;
    void      (*EventScriptCommandDtor)(void*)                         = nullptr;
    void      (*EventGroupAdd)(uintptr_t, void*, bool)                 = nullptr;
    uintptr_t AnimBlocksVar = 0, DriveByLoopStart = 0, DriveByLoopEnd = 0;
    uintptr_t (*BlendAnimation)(uintptr_t, int, int, float)            = nullptr;
    uintptr_t (*ClumpGetAssociation)(uintptr_t, unsigned)              = nullptr;
    void      (*AddAnimBlockRef)(int)                                  = nullptr;
    void      (*VehSetWindowOpen)(uintptr_t, uint8_t)                  = nullptr;
    void      (*RequestSpecialModel)(int, const char*, int)            = nullptr;
    bool      (*DirFindItem)(uintptr_t, const char*, unsigned*, unsigned*) = nullptr;
    uintptr_t ModelInfoPtrs = 0, ExtraObjectsDir = 0;
    void      (*AnimSetDeleteCallback)(uintptr_t, void*, void*)        = nullptr;
    void      (*AnimSetCurrentTime)(uintptr_t, float)                  = nullptr;
    void*     DefaultAnimCB = nullptr;
    uintptr_t StreamingInfo = 0;
    uint8_t*  CurrArea = nullptr;

    uintptr_t Players = 0;
    uintptr_t TheCamera = 0;

    static bool g_vehOk = false, g_combatOk = false;

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
        ok &= Resolve(h, (void**)&GetPedFromRef,        "_ZN6CPools6GetPedEi");
        ok &= Resolve(h, (void**)&SetEntityBlip,        "_ZN6CRadar13SetEntityBlipE9eBlipTypeij12eBlipDisplayPc");
        ok &= Resolve(h, (void**)&ChangeBlipColour,     "_ZN6CRadar16ChangeBlipColourEij");
        ok &= Resolve(h, (void**)&ChangeBlipScale,      "_ZN6CRadar15ChangeBlipScaleEii");
        ok &= Resolve(h, (void**)&ClearBlip,            "_ZN6CRadar9ClearBlipEi");
        ok &= Resolve(h, (void**)&PedIsAlive,           "_ZNK4CPed7IsAliveEv");
        ok &= Resolve(h, (void**)&PedSetCharCreatedBy,  "_ZN4CPed16SetCharCreatedByEh");
        ok &= Resolve(h, (void**)&Players,              "_ZN6CWorld7PlayersE");
        ok &= Resolve(h, (void**)&TheCamera,            "TheCamera");

        // Машины, оружие, урон — необязательные: без них кооп работает как раньше (пешком)
        bool v = true, c = true;
        v &= Resolve(h, (void**)&CreateCarForScript,    "_ZN8CCarCtrl18CreateCarForScriptEi7CVectorh");
        v &= Resolve(h, (void**)&RequestModel,          "_ZN10CStreaming12RequestModelEii");
        v &= Resolve(h, (void**)&LoadAllRequestedModels,"_ZN10CStreaming22LoadAllRequestedModelsEb");
        v &= Resolve(h, (void**)&GetVehicleRef,         "_ZN6CPools13GetVehicleRefEP8CVehicle");
        v &= Resolve(h, (void**)&GetVehicleFromRef,     "_ZN6CPools10GetVehicleEi");
        v &= Resolve(h, (void**)&TaskNew,               "_ZN5CTasknwEm");
        v &= Resolve(h, (void**)&TaskMgrSetTask,        "_ZN12CTaskManager7SetTaskEP5CTaskib");
        v &= Resolve(h, (void**)&TaskMgrGetActive,      "_ZNK12CTaskManager13GetActiveTaskEv");
        v &= Resolve(h, (void**)&TaskEnterAsDriver,     "_ZN28CTaskComplexEnterCarAsDriverC2EP8CVehicle");
        v &= Resolve(h, (void**)&TaskEnterAsPassenger,  "_ZN31CTaskComplexEnterCarAsPassengerC2EP8CVehicleib");
        v &= Resolve(h, (void**)&TaskLeaveCar,          "_ZN20CTaskComplexLeaveCarC2EP8CVehicleiibb");
        v &= Resolve(h, (void**)&TaskSetPedOutCtor,     "_ZN23CTaskSimpleCarSetPedOutC2EP8CVehicleib");
        v &= Resolve(h, (void**)&TaskSetPedOutDtor,     "_ZN23CTaskSimpleCarSetPedOutD2Ev");
        v &= Resolve(h, (void**)&TaskSetPedOutProcess,  "_ZN23CTaskSimpleCarSetPedOut10ProcessPedEP4CPed");
        v &= Resolve(h, (void**)&SetPedInCarDirect,     "_ZN13CCarEnterExit17SetPedInCarDirectEP4CPedP8CVehicleib");
        v &= Resolve(h, (void**)&DoorForPassengerSeat,  "_ZN13CCarEnterExit35ComputeTargetDoorToEnterAsPassengerERK8CVehiclei");
        v &= Resolve(h, (void**)&StreamingInfo,         "_ZN10CStreaming16ms_aInfoForModelE");
        c &= Resolve(h, (void**)&GiveWeapon,            "_ZN4CPed10GiveWeaponE11eWeaponTypejb");
        c &= Resolve(h, (void**)&SetCurrentWeaponType,  "_ZN4CPed16SetCurrentWeaponE11eWeaponType");
        c &= Resolve(h, (void**)&GetWeaponSlot,         "_ZN4CPed13GetWeaponSlotE11eWeaponType");
        c &= Resolve(h, (void**)&WeaponFire,            "_ZN7CWeapon4FireEP7CEntityP7CVectorS3_S1_S3_S3_");
        c &= Resolve(h, (void**)&GenerateDamageEvent,   "_ZN7CWeapon19GenerateDamageEventEP4CPedP7CEntity11eWeaponTypei14ePedPieceTypesi");
        c &= Resolve(h, (void**)&PedSetAimFlag,         "_ZN4CPed10SetAimFlagEf");
        c &= Resolve(h, (void**)&PedClearAimFlag,       "_ZN4CPed12ClearAimFlagEv");
        c &= Resolve(h, (void**)&PointGunAtPosition,    "_ZN6CPedIK18PointGunAtPositionERK7CVectorf");
        c &= Resolve(h, (void**)&FindPlayerWanted,      "_Z16FindPlayerWantedi");
        c &= Resolve(h, (void**)&WantedSetLevel,        "_ZN7CWanted14SetWantedLevelEi");
        c &= Resolve(h, (void**)&WantedCheatLevel,      "_ZN7CWanted16CheatWantedLevelEi");
        Resolve(h, (void**)&CurrArea, "_ZN5CGame8currAreaE");
        Resolve(h, (void**)&PedTeleport, "_ZN4CPed8TeleportE7CVectorh");
        // анимации дверей машин (проверка перед посадкой второго CJ) и стрельба пассажира из окна — необязательные
        Resolve(h, (void**)&VehicleAnimGroups,      "_ZN21CVehicleAnimGroupData19m_vehicleAnimGroupsE");
        Resolve(h, (void**)&AnimAssocGroupsVar,     "_ZN12CAnimManager19ms_aAnimAssocGroupsE");
        Resolve(h, (void**)&VehAnimGroupGetGroup,   "_ZN17CVehicleAnimGroup8GetGroupEi");
        Resolve(h, (void**)&AnimBlockName,          "_ZN12CAnimManager16GetAnimBlockNameE12AssocGroupId");
        Resolve(h, (void**)&AnimBlockIndex,         "_ZN12CAnimManager22GetAnimationBlockIndexEPKc");
        Resolve(h, (void**)&GangDriveByCtor,        "_ZN22CTaskSimpleGangDriveByC2EP7CEntityPK7CVectorfaab");
        Resolve(h, (void**)&VehCanPedLeanOut,       "_ZNK8CVehicle13CanPedLeanOutEP4CPed");
        Resolve(h, (void**)&GetWeaponInfo,          "_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponTypea");
        Resolve(h, (void**)&PedAddWeaponModel,      "_ZN4CPed14AddWeaponModelEi");
        Resolve(h, (void**)&EventScriptCommandCtor, "_ZN19CEventScriptCommandC2EiP5CTaskb");
        Resolve(h, (void**)&EventScriptCommandDtor, "_ZN19CEventScriptCommandD2Ev");
        Resolve(h, (void**)&EventGroupAdd,          "_ZN11CEventGroup3AddER6CEventb");
        // вид стрельбы друга из машины (анимация «высунулся из окна») — необязательные
        Resolve(h, (void**)&AnimBlocksVar,          "_ZN12CAnimManager14ms_aAnimBlocksE");
        Resolve(h, (void**)&DriveByLoopStart,       "_ZN22CTaskSimpleGangDriveBy15ANIM_LOOP_STARTE");
        Resolve(h, (void**)&DriveByLoopEnd,         "_ZN22CTaskSimpleGangDriveBy13ANIM_LOOP_ENDE");
        Resolve(h, (void**)&BlendAnimation,         "_ZN12CAnimManager14BlendAnimationEP7RpClump12AssocGroupId11AnimationIdf");
        Resolve(h, (void**)&ClumpGetAssociation,    "_Z30RpAnimBlendClumpGetAssociationP7RpClumpj");
        Resolve(h, (void**)&AddAnimBlockRef,        "_ZN12CAnimManager15AddAnimBlockRefEi");
        Resolve(h, (void**)&VehSetWindowOpen,       "_ZN8CVehicle17SetWindowOpenFlagEh");
        // особые модели педов миссий (ячейки 290..299) — необязательные
        Resolve(h, (void**)&RequestSpecialModel,    "_ZN10CStreaming19RequestSpecialModelEiPKci");
        Resolve(h, (void**)&DirFindItem,            "_ZN10CDirectory8FindItemEPKcRjS2_");
        Resolve(h, (void**)&ModelInfoPtrs,          "_ZN10CModelInfo16ms_modelInfoPtrsE");
        Resolve(h, (void**)&ExtraObjectsDir,        "_ZN10CStreaming19ms_pExtraObjectsDirE");
        Resolve(h, (void**)&AnimSetDeleteCallback,  "_ZN21CAnimBlendAssociation17SetDeleteCallbackEPFvPS_PvES1_");
        Resolve(h, (void**)&AnimSetCurrentTime,     "_ZN21CAnimBlendAssociation14SetCurrentTimeEf");
        Resolve(h, (void**)&DefaultAnimCB,          "_ZN20CDefaultAnimCallback13DefaultAnimCBEP21CAnimBlendAssociationPv");
        g_vehOk = v; g_combatOk = c && v;   // урону нужны и машины (TaskNew, модели)
        return ok;
    }

    bool VehiclesReady() { return g_vehOk; }
    bool CombatReady()   { return g_combatOk; }

    int ActiveTaskType(uintptr_t ped, uintptr_t* taskOut)
    {
        if(taskOut) *taskOut = 0;
        uintptr_t mgr = ped ? TaskManager(ped) : 0;
        if(!mgr || !TaskMgrGetActive) return -1;
        uintptr_t t = TaskMgrGetActive(mgr);
        if(!t) return -1;
        uintptr_t vt = Field<uintptr_t>(t, 0);
        if(!vt) return -1;
        if(taskOut) *taskOut = t;
        return ((int(*)(uintptr_t))Field<uintptr_t>(vt, off::VtTaskType))(t);
    }

    bool ModelLoaded(int model)
    {
        if(!StreamingInfo || model < 0 || model >= 20000) return false;
        return Field<uint8_t>(StreamingInfo + (uintptr_t)model * off::StreamInfoSize, off::StreamStatus) == 1;
    }

    // Файл модели есть на диске (у «особой» ячейки без имени размер 0 — заказывать такую нельзя: вылет в потоке загрузки)
    bool ModelOnDisk(int model)
    {
        if(!StreamingInfo || model < 0 || model >= 20000) return false;
        uintptr_t si = StreamingInfo + (uintptr_t)model * off::StreamInfoSize;
        return Field<uint8_t>(si, off::StreamStatus) == 1 || Field<uint32_t>(si, 0xC) != 0;
    }

    // Имя модели (CBaseModelInfo+0xC, до 20 букв) — "" если нет
    const char* ModelName(int model)
    {
        if(!ModelInfoPtrs || model < 0 || model >= 20000) return "";
        uintptr_t mi = Field<uintptr_t>(ModelInfoPtrs, (uintptr_t)model * 8);
        if(!mi) return "";
        const char* n = (const char*)(mi + 0xC);
        for(int i = 0; i < 21; ++i)
        {
            unsigned char ch = (unsigned char)n[i];
            if(!ch) return i ? n : "";
            if(!(ch == '_' || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))) return "";
        }
        return "";
    }

    // Особый персонаж с таким именем есть в файлах игры
    bool SpecialOnDisk(const char* name)
    {
        if(!DirFindItem || !ExtraObjectsDir || !name || !name[0]) return false;
        uintptr_t dir = Field<uintptr_t>(ExtraObjectsDir, 0);
        if(!dir) return false;
        unsigned pos = 0, size = 0;
        return DirFindItem(dir, name, &pos, &size) && size != 0;
    }

    uintptr_t CurrentWeapon(uintptr_t ped)
    {
        int slot = Field<int8_t>(ped, off::PedCurWeaponSlot);
        if(slot < 0 || slot > 12) return 0;
        return ped + off::PedWeapons + (uintptr_t)slot * off::WeaponSize;
    }
    int CurrentWeaponType(uintptr_t ped)
    {
        uintptr_t w = CurrentWeapon(ped);
        return w ? Field<int>(w, off::WeapType) : 0;
    }

    int WeaponModel(int w)
    {
        // eWeaponType -> модель (как в weapon.dat; 0 — кулак / без модели)
        static const int kModels[] = { 0, 331, 333, 334, 335, 336, 337, 338, 339, 341, 321, 322, 323, 324, 325, 326, 342, 343, 344, 0, 0, 0,
                                       346, 347, 348, 349, 350, 351, 352, 353, 355, 356, 372, 357, 358, 359, 360, 361, 362, 363, 364, 365, 366, 367, 368, 369, 371 };
        return (w > 0 && w < (int)(sizeof(kModels) / sizeof(kModels[0]))) ? kModels[w] : 0;
    }

    int SeatOf(uintptr_t ped, uintptr_t veh)
    {
        if(!ped || !veh) return -1;
        if(Field<uintptr_t>(veh, off::VehDriver) == ped) return 0;
        for(int i = 0; i < 8; ++i)
            if(Field<uintptr_t>(veh, off::VehPassengers + (uintptr_t)i * 8) == ped) return i + 1;
        return -1;
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
