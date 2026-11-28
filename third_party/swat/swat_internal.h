#pragma once
// Внутреннее для библиотеки тактики: свой водитель (Д25–Д38) и пилот (Д39–Д50).
#include "sgame.h"

namespace swat
{
namespace drv
{
    // ---- машина ----
    enum Mode { D_NONE, D_ROUTE, D_FOLLOW, D_SIDE, D_PIT, D_BLOCK, D_STOP_AT, D_REVERSE_TO, D_OFFROAD, D_CONVOY, D_WAIT, D_RAM };
    // car/driver — handle; point — цель; targetCar — машина-цель (0 — нет); speed м/с; dist — дистанция/сторона
    void Drive(int car, int driver, int mode, const V3& point, int targetCar, float speed, float dist, float heading = -1);
    void Release(int car);
    bool Active(int car);
    bool Arrived(int car, float radius = 4.0f);
    const char* Doing(int car);

    // ---- вертолёт ----
    enum HMode { H_NONE, H_TAKEOFF, H_GOTO, H_HOVER, H_ORBIT, H_BROADSIDE, H_FOLLOW, H_LOWPASS, H_LAND, H_EVADE };
    void Fly(int heli, int pilot, int mode, const V3& point, int targetPed, int targetCar, float radius, float alt);
    void Ground(int heli);
    bool Landed(int heli);
    const char* HeliDoing(int heli);

    void Frame();
}
}
