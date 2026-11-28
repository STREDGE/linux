#pragma once
// Интерфейс SA Story (режим S.W.A.T.) для других модов (SA Living — отряд с ИИ). Регистрируется в AML под именем
// "SAStorySwat". Только C-функции. Общий файл обоих модов (third_party).

#include <stdint.h>

#define SASTORY_SWAT_INTERFACE "SAStorySwat"

struct SwatPlace { float x, y, z, heading; };

enum SwatPlaceId
{
    SWAT_PLACE_CENTER = 0, SWAT_PLACE_MEDBAY, SWAT_PLACE_ARMORY, SWAT_PLACE_CELL, SWAT_PLACE_HELIPAD, SWAT_PLACE_GATE,
    SWAT_PLACE_GARAGE, SWAT_PLACE_HQ_DOOR, SWAT_PLACE_PARKING, SWAT_PLACE_REST, SWAT_PLACE_COUNT
};

enum SwatStaffKind { SWAT_STAFF = 0, SWAT_GUARD = 1, SWAT_FRANK = 2, SWAT_TOMMY = 3 };

enum SwatBackup { SWAT_BK_POLICE = 0, SWAT_BK_SWAT = 1, SWAT_BK_FBI = 2, SWAT_BK_ARMY = 3, SWAT_BK_HELI = 4, SWAT_BK_MEDIC = 5 };

struct SAStorySwatApi
{
    uint32_t version;                                     // 1
    bool (*ModeOn)();                                     // в этой игре режим S.W.A.T.
    bool (*BaseLive)();                                   // база «живёт» (игрок ближе ~150 м)
    bool (*GetPlace)(int id, SwatPlace* out);
    int  (*StaffCount)();
    int  (*StaffRef)(int i);                              // handle педа (0 — сейчас не создан)
    const char* (*StaffName)(int i);
    int  (*StaffKind)(int i);
    int  (*StaffModel)(int i);
    int  (*StaffIndexOf)(int ref);                        // -1 — не сотрудник базы
    void (*ReleaseStaff)(int ref);                        // взят в отряд: база им больше не управляет
    void (*ReturnStaff)(int ref);                         // вернулся из отряда: снова на базе
    int  (*NpcStars)(int ref);                            // розыск NPC (0..6)
    void (*SetNpcStars)(int ref, int stars, const char* why);
    void (*CallBackup)(int kind, float x, float y, float z);
    void (*Arrested)(int ref);                            // задержан (отрядом)
    void (*Delivered)(int ref);                           // доставлен в камеру базы
    bool (*AlarmOn)();
    void (*RaiseAlarm)(const char* why);
};
