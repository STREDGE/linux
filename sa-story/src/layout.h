#pragma once
// План базы S.W.A.T. (ТЗ-BASE §4, схема base_plan.png) в мировых координатах.
// Место — контейнерный двор Ocean Docks: плита порта sanpdmdock2_las2, пол ровный на высоте 12.42–12.66
// (проверено по коллизии карты из OBB). Прямоугольник 160 × 120 м, центр (2684, −2396).
// Схема нарисована «дорогой сверху»; въезд в порту — с юга (портовая дорога x = 2684 → y = −2504), поэтому
// схема повёрнута на 180°: «север схемы» (КПП) = юг мира. u — вправо по схеме, v — вверх по схеме (метры).

#include "game.h"
#include <swat/sgame.h>

namespace layout
{
    constexpr float kCx = 2684.0f, kCy = -2396.0f, kZ = 12.62f;
    constexpr float kHalfU = 80.0f, kHalfV = 60.0f;          // половины сторон
    inline game::Vec W(float u, float v, float dz = 0) { game::Vec r; r.x = kCx - u; r.y = kCy - v; r.z = kZ + dz; return r; }
    inline swat::V3 WV(float u, float v, float dz = 0) { return swat::Mk(kCx - u, kCy - v, kZ + dz); }
    inline swat::V3 V(const game::Vec& g) { return swat::Mk(g.x, g.y, g.z); }
    inline game::Vec G(const swat::V3& v) { game::Vec g; g.x = v.x; g.y = v.y; g.z = v.z; return g; }
    inline float H(float planHeading) { float h = planHeading + 180.0f; while(h >= 360) h -= 360; return h; }   // курс по схеме → мир
    // Прямоугольник сноса (с запасом 4 м — стыки заборов и фонари на краю)
    constexpr float kRx0 = kCx - kHalfU - 4, kRx1 = kCx + kHalfU + 4, kRy0 = kCy - kHalfV - 4, kRy1 = kCy + kHalfV + 4;
    constexpr float kLiveRadius = 150.0f;                      // база «живёт», когда игрок ближе
    constexpr float kRoofZ = kZ + 9.04f;                       // крыша штаба: vgnwalgren1 ставится на +0.2 (низ коллизии −0.2), верх +8.84
    constexpr float kPadU = -6.75f, kPadV = -44.3f;            // ровный квадрат крыши 24×24 (по коллизии) — центр площадки
    constexpr float kHeli2U = 7.0f, kHeli2V = -44.3f;

    // ---- точки (u, v по схеме) ----
    // 1 КПП
    constexpr float kGateU = 0, kGateV = 60;
    // 2 штаб (vgnwalgren1 45×30), 3 площадка на крыше
    constexpr float kHqU = 0, kHqV = -41;
    // 4 кабинет Фрэнка — вход в штаб, 5 подъём на крышу
    constexpr float kFrankU = -15.4f, kFrankV = -24.5f;
    constexpr float kLiftU = 23.0f, kLiftV = -24.5f;
    constexpr float kRoofLiftU = 12.0f, kRoofLiftV = -36.0f;
    // 6 служебный гараж (2 ангара), стоянки машин
    constexpr float kGarageU = -67.0f;                          // ангары 20×30: u −77…−57
    // 7 личный гараж
    constexpr float kOwnGarageU = -67.0f, kOwnGarageV = -43.0f;
    // 8 оружейная
    constexpr float kArmoryU = -23.9f, kArmoryV = 43.8f;
    // 9 тир
    constexpr float kRangeStandU = -38.0f, kRangeV = 48.5f, kRangeTargetsU = -68.0f;   // дорожка 30 м (у угла — вышка)
    // 10 медпункт
    constexpr float kMedU = 27.0f, kMedV = 43.4f;
    // 11/12 комнаты
    constexpr float kRoomU = 64.9f, kArtyomV = 19.4f, kMagaV = -4.6f, kRoomDoorU = 55.0f;
    // 13 зона отдыха
    constexpr float kRestU = 64.0f, kRestV = 7.0f;
    // 14 камера и автозак
    constexpr float kCellU = 63.0f, kCellV = -44.9f, kCellDropU = 50.0f, kCellDropV = -45.0f, kVanU = 37.0f, kVanV = -50.8f;
    // 15 плац с флагом
    constexpr float kFlagU = -4.0f, kFlagV = 6.0f;
    // 16 парковка
    constexpr float kParkU = 10.0f, kParkV = -19.0f;
}
