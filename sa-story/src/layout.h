#pragma once
// План базы S.W.A.T. v2 в мировых координатах. Постройки и точки — в base_plan.h (генерируется tools/base_plan.py).
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
    constexpr float kLiveRadius = 300.0f;                      // база «живёт», когда игрок ближе
}

#include "base_plan.h"   // точки и постройки плана v2 (генерируется tools/base_plan.py)
