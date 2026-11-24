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
        constexpr uintptr_t PedHealth        = 0x6AC; // float
        constexpr uintptr_t PedRotCurrent    = 0x6C4; // m_fCurrentRotation
        constexpr uintptr_t PedRotAiming     = 0x6C8; // m_fAimingRotation
        constexpr uintptr_t PedVehicle       = 0x708; // CVehicle*
        constexpr uintptr_t PedType          = 0x720; // 0 = PLAYER1, 1 = PLAYER2

        constexpr uintptr_t PadSize          = 0x158; // sizeof(CPad)
        constexpr uintptr_t PadDisableCtrl   = 0x110; // uint16 DisablePlayerControls
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

    extern uintptr_t Players;      // CWorld::Players (массив CPlayerInfo)

    // Ищет всё нужное. Возвращает false и пишет в лог, если чего-то нет.
    bool Init(void* libHandle, uintptr_t libBase);

    // --- Помощники ---
    template<typename T> inline T& Field(uintptr_t obj, uintptr_t offset) { return *(T*)(obj + offset); }

    inline uintptr_t& SlotPed(int index) { return Field<uintptr_t>(Players, index * off::PlayerInfoSize); }

    inline bool InVehicle(uintptr_t ped)
    {
        return (Field<uint8_t>(ped, off::PedFlags585) & 1) && Field<uintptr_t>(ped, off::PedVehicle);
    }

    CVector GetPosition(uintptr_t entity);
    void    SetPosition(uintptr_t entity, const CVector& pos);
    float   GetMatrixHeading(uintptr_t entity); // по направлению «вперёд» матрицы
}
