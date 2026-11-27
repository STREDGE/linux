#pragma once
// Доступ к libGTASA.so 2.10 arm64: адреса функций и смещения полей.
// Смещения получены дизассемблированием именно этой версии
// (см. docs/libgtasa-2.10-arm64.md). Для других версий они НЕ подходят.

#include <stdint.h>

struct CVector { float x, y, z; };

namespace gta
{
    // --- Смещения полей (arm64, 2.10) ---
    namespace off
    {
        constexpr uintptr_t PlayerInfoSize   = 0x1D8; // sizeof(CPlayerInfo), m_pPed по смещению 0

        constexpr uintptr_t EntPlacementPos  = 0x08;  // CPlaceable::m_placement.pos (если матрицы нет)
        constexpr uintptr_t EntPlacementHead = 0x14;  // CPlaceable::m_placement.heading
        constexpr uintptr_t EntMatrix        = 0x18;  // CPlaceable::m_matrix (CMatrix*)
        constexpr uintptr_t EntRwObject      = 0x20;  // CEntity::m_pRwObject
        constexpr uintptr_t MatForward       = 0x10;  // CMatrix::forward
        constexpr uintptr_t MatPos           = 0x30;  // CMatrix::pos

        constexpr uintptr_t PhysMoveSpeed    = 0x68;  // CPhysical::m_vecMoveSpeed (ед./кадр при 50 FPS)
        constexpr uintptr_t PhysTurnSpeed    = 0x74;  // CPhysical::m_vecTurnSpeed

        constexpr uintptr_t PedFlags585      = 0x585; // бит 0 = bInVehicle
        constexpr uintptr_t PedCreatedBy     = 0x548; // uint8: 1 = игра (можно удалить), 2 = миссия
        constexpr uintptr_t PedState         = 0x54C; // ePedState
        constexpr uintptr_t PedMoveState     = 0x550; // eMoveState (CPed::SetMoveState)
        constexpr uintptr_t PedPlayerData    = 0x540; // CPlayerPedData* (только у игроков)
        constexpr uintptr_t PlayerDataMoveBlendRatio = 0x20; // float, из PlayerControlZelda
        constexpr uintptr_t PedHealth        = 0x6AC; // float
        constexpr uintptr_t PedArmour        = 0x6B4; // float (CCheat::MoneyArmourHealthCheat)
        constexpr uintptr_t PedRotCurrent    = 0x6C4; // m_fCurrentRotation
        constexpr uintptr_t PedRotAiming     = 0x6C8; // m_fAimingRotation
        constexpr uintptr_t PedVehicle       = 0x708; // CVehicle*
        constexpr uintptr_t PedType          = 0x720; // 0 = PLAYER1, 1 = PLAYER2

        constexpr uintptr_t PadSize          = 0x158; // sizeof(CPad)
        constexpr uintptr_t PadDisableCtrl   = 0x110; // uint16 DisablePlayerControls

        constexpr uintptr_t CamOrientation   = 0x154; // float TheCamera.Orientation (PlayerControlZelda)

        // --- из отладочной информации libGTASA (DWARF), см. docs/libgtasa-2.10-arm64.md ---
        constexpr uintptr_t EntFlags         = 0x28;  // CEntity::m_nFlags (uint32)
        constexpr uintptr_t EntModel         = 0x32;  // int16 m_nModelIndex
        constexpr uintptr_t EntArea          = 0x4B;  // uint8 m_areaCode (интерьер; 0 — улица)
        constexpr uintptr_t EntInfo          = 0x5A;  // тип (&7: 2 машина, 3 пед) и статус (>>3: 5 — разбита)
        constexpr uintptr_t MatRight         = 0x00;
        constexpr uintptr_t MatUp            = 0x20;

        constexpr uintptr_t PedIntel         = 0x538; // CPedIntelligence*; +8 — CTaskManager
        constexpr uintptr_t PedFlagsWord     = 0x584; // CPedFlags: байт 0x584 бит 4 — целится
        constexpr uintptr_t PedFlags587      = 0x587; // бит 2 — присел, бит 5 — не вытаскивать из машины
        constexpr uintptr_t PedIK            = 0x678; // CPedIK
        constexpr uintptr_t PedWeapons       = 0x730; // CWeapon[13], по 0x20
        constexpr uintptr_t WeaponSize       = 0x20;
        constexpr uintptr_t PedCurWeaponSlot = 0x8DC; // int8
        constexpr uintptr_t PedLockOnTarget  = 0x8E0; // CEntity*

        constexpr uintptr_t WeapType         = 0x00;
        constexpr uintptr_t WeapState        = 0x04;
        constexpr uintptr_t WeapClip         = 0x08;
        constexpr uintptr_t WeapTotal        = 0x0C;
        constexpr uintptr_t WeapTimer        = 0x10;

        constexpr uintptr_t VehFlags         = 0x568; // CVehicleFlags (8 байт)
        constexpr uintptr_t VehColours       = 0x574; // uint8[4]
        constexpr uintptr_t VehDriver        = 0x5A0;
        constexpr uintptr_t VehPassengers    = 0x5A8; // CPed*[8]
        constexpr uintptr_t VehMaxPassengers = 0x5EC; // uint8
        constexpr uintptr_t VehSteer         = 0x600;
        constexpr uintptr_t VehSteerInput    = 0x730; // fSteer: руль −1..1 (из пульта, сглаженный)
        constexpr uintptr_t VehGas           = 0x608;
        constexpr uintptr_t VehBrake         = 0x60C;
        constexpr uintptr_t VehCreatedBy     = 0x610; // 1 — случайная, 2 — миссии (не удаляется)
        constexpr uintptr_t VehHealth        = 0x634;
        constexpr uintptr_t VehDoorLock      = 0x684;
        constexpr uintptr_t VehHorn          = 0x6A4;
        constexpr uintptr_t VehBaseType      = 0x734; // 0 машина … 5 лодка, 6 поезд, 9 мотоцикл, 10 велосипед
        constexpr uintptr_t AutoWheels       = 0x75D; // CAutomobile: CDamageManager(0x758)::m_Wheel[4]
        constexpr uintptr_t VtBlowUpCar      = 0x150; // vtable CVehicle::BlowUpCar(CEntity*, bool)
        constexpr uintptr_t VtDeletingDtor   = 0x08;  // vtable: удалить объект (как CCarCtrl)
        constexpr uintptr_t VtTaskType       = 0x28;  // vtable CTask::GetTaskType

        constexpr uintptr_t TaskEnterVehicle = 0x18;  // CTaskComplexEnterCar::m_pTargetVehicle
        constexpr uintptr_t TaskEnterSeat    = 0x2C;  // m_iTargetSeat
        constexpr uintptr_t WantedStars      = 0x2C;  // CWanted::m_WantedLevel
        constexpr uintptr_t StreamInfoSize   = 0x14;
        constexpr uintptr_t StreamStatus     = 0x10;  // 1 — модель загружена

        // CPedDamageResponseCalculator / CPedDamageResponse
        constexpr uintptr_t CalcInflictor    = 0x00;
        constexpr uintptr_t CalcDamage       = 0x08;
        constexpr uintptr_t CalcPiece        = 0x0C;
        constexpr uintptr_t CalcWeapon       = 0x10;
        constexpr float     StickScale       = 60.0f; // джойстик / 60 = сила движения (константа игры)
    }

    enum eBlipType    { BLIP_CAR = 1, BLIP_CHAR = 2 };
    enum eBlipDisplay { BLIP_DISPLAY_NEITHER = 0, BLIP_DISPLAY_MARKER = 1, BLIP_DISPLAY_BLIP = 2, BLIP_DISPLAY_BOTH = 3 };

    // --- Функции игры (заполняются в Init) ---
    extern void      (*SetupPlayerPed)(int index);
    extern void      (*RemovePlayerPed)(int index);
    extern uintptr_t (*GetPad)(int index);
    extern void      (*WorldAdd)(uintptr_t entity);
    extern void      (*WorldRemove)(uintptr_t entity);
    extern void      (*PhysicalRemoveAndAdd)(uintptr_t physical);
    extern void      (*MatrixSetRotateZOnly)(uintptr_t matrix, float angle);
    extern void      (*MatrixUpdateRW)(uintptr_t matrix);
    extern void      (*EntityUpdateRwFrame)(uintptr_t entity);
    extern int       (*GetPedRef)(uintptr_t ped);
    extern uintptr_t (*GetPedFromRef)(int handle); // nullptr, если персонаж удалён
    extern int       (*SetEntityBlip)(int type, int handle, unsigned int colour, int display, char* name);
    extern void      (*ChangeBlipColour)(int blip, unsigned int colour);
    extern void      (*ChangeBlipScale)(int blip, int scale);
    extern void      (*ClearBlip)(int blip);
    extern bool      (*PedIsAlive)(uintptr_t ped);
    extern void      (*PedSetCharCreatedBy)(uintptr_t ped, uint8_t createdBy); // 2 = персонаж миссии

    // --- машины, задачи, оружие, урон (необязательные: нет — эти части коопа выключены) ---
    extern uintptr_t (*CreateCarForScript)(int model, CVector pos, bool missionCleanup);
    extern void      (*RequestModel)(int model, int flags);
    extern void      (*LoadAllRequestedModels)(bool priorityOnly);
    extern int       (*GetVehicleRef)(uintptr_t veh);
    extern uintptr_t (*GetVehicleFromRef)(int ref);
    extern void*     (*TaskNew)(unsigned long size);
    extern void      (*TaskMgrSetTask)(uintptr_t mgr, void* task, int slot, bool);
    extern uintptr_t (*TaskMgrGetActive)(uintptr_t mgr);
    extern void      (*TaskEnterAsDriver)(void* task, uintptr_t veh);
    extern void      (*TaskEnterAsPassenger)(void* task, uintptr_t veh, int seat, bool carryOn);
    extern void      (*TaskLeaveCar)(void* task, uintptr_t veh, int door, int delay, bool sensible, bool force);
    extern void      (*TaskSetPedOutCtor)(void* task, uintptr_t veh, int door, bool engineOff);
    extern void      (*TaskSetPedOutDtor)(void* task);
    extern bool      (*TaskSetPedOutProcess)(void* task, uintptr_t ped);
    extern bool      (*SetPedInCarDirect)(uintptr_t ped, uintptr_t veh, int door, bool driver);
    extern int       (*DoorForPassengerSeat)(uintptr_t veh, int seat);
    extern void      (*GiveWeapon)(uintptr_t ped, int type, unsigned ammo, bool pickup);
    extern void      (*SetCurrentWeaponType)(uintptr_t ped, int type);
    extern int       (*GetWeaponSlot)(uintptr_t ped, int type);
    extern bool      (*WeaponFire)(uintptr_t weapon, uintptr_t owner, CVector* start, CVector* barrel, uintptr_t targetEnt, CVector* target, CVector* alt);
    extern bool      (*GenerateDamageEvent)(uintptr_t victim, uintptr_t attacker, int weapon, int damage, int piece, int dir);
    extern void      (*PedSetAimFlag)(uintptr_t ped, float heading);
    extern void      (*PedClearAimFlag)(uintptr_t ped);
    extern bool      (*PointGunAtPosition)(uintptr_t ik, const CVector& pos, float blend);
    extern uintptr_t (*FindPlayerWanted)(int player);
    extern void      (*WantedSetLevel)(uintptr_t wanted, int level);
    extern void      (*WantedCheatLevel)(uintptr_t wanted, int level);
    extern void      (*PedTeleport)(uintptr_t ped, CVector to, bool clearOrientation);
    // анимации дверей машин: CVehicleAnimGroupData::m_vehicleAnimGroups (шаг 0x94), CAnimManager::ms_aAnimAssocGroups (указатель)
    extern uintptr_t VehicleAnimGroups, AnimAssocGroupsVar;
    extern int       (*VehAnimGroupGetGroup)(uintptr_t vehAnimGroup, int animId);
    extern const char* (*AnimBlockName)(int assocGroup);
    extern int       (*AnimBlockIndex)(const char* name);
    // стрельба пассажира из окна (как в миссиях: CTaskSimpleGangDriveBy через событие «команда сценария»)
    extern void      (*GangDriveByCtor)(void* task, uintptr_t target, const CVector* pos, float range, int8_t freq, int8_t style, bool seatRight);
    extern bool      (*VehCanPedLeanOut)(uintptr_t veh, uintptr_t ped);
    extern uintptr_t (*GetWeaponInfo)(int type, int8_t skill);
    extern void      (*PedAddWeaponModel)(uintptr_t ped, int model);
    extern void      (*EventScriptCommandCtor)(void* ev, int primaryTask, void* task, bool affectsDeadPeds);
    extern void      (*EventScriptCommandDtor)(void* ev);
    extern void      (*EventGroupAdd)(uintptr_t group, void* ev, bool valid);
    extern uintptr_t StreamingInfo; // CStreaming::ms_aInfoForModel
    extern uint8_t*  CurrArea;      // CGame::currArea

    extern uintptr_t Players;      // CWorld::Players (массив CPlayerInfo)
    extern uintptr_t TheCamera;    // CCamera

    // Ищет всё нужное. Возвращает false и пишет в лог, если чего-то нет.
    bool Init(void* libHandle, uintptr_t libBase);

    // --- Помощники ---
    template<typename T> inline T& Field(uintptr_t obj, uintptr_t offset) { return *(T*)(obj + offset); }

    inline uintptr_t& SlotPed(int index) { return Field<uintptr_t>(Players, index * off::PlayerInfoSize); }

    inline bool InVehicle(uintptr_t ped)
    {
        return (Field<uint8_t>(ped, off::PedFlags585) & 1) && Field<uintptr_t>(ped, off::PedVehicle);
    }

    bool    VehiclesReady();   // всё для машин найдено
    bool    CombatReady();     // всё для оружия и урона найдено

    inline int  EntType(uintptr_t e)   { return Field<uint8_t>(e, off::EntInfo) & 7; }
    inline int  EntStatus(uintptr_t e) { return Field<uint8_t>(e, off::EntInfo) >> 3; }
    inline int  ModelOf(uintptr_t e)   { return Field<int16_t>(e, off::EntModel); }
    inline uintptr_t TaskManager(uintptr_t ped)
    {
        uintptr_t intel = Field<uintptr_t>(ped, off::PedIntel);
        return intel ? intel + 8 : 0;
    }
    int       ActiveTaskType(uintptr_t ped, uintptr_t* taskOut = nullptr);
    bool      ModelLoaded(int model);
    uintptr_t CurrentWeapon(uintptr_t ped);          // CWeapon* в руках
    int       CurrentWeaponType(uintptr_t ped);
    int       WeaponModel(int weaponType);           // модель оружия (0 — без модели)
    int       SeatOf(uintptr_t ped, uintptr_t veh);  // -1 нет, 0 водитель, 1.. пассажир+1

    CVector GetPosition(uintptr_t entity);
    void    SetPosition(uintptr_t entity, const CVector& pos);
    float   GetMatrixHeading(uintptr_t entity); // по направлению «вперёд» матрицы
}
