// Свой водитель (Д25–Д38) и свой пилот (Д39–Д50).
// Водитель: маршрут по дорогам ведёт родной автопилот игры (запасной и для дальних поездок), а точные манёвры —
// остановка в точке, держать дистанцию, бок о бок, PIT, заслон поперёк, задний ход, бездорожье — наш руль/газ/тормоз
// каждый кадр через перехват CCarCtrl::SteerAICarWithPhysics. Пилот: траекторию (круг, бортом к цели, следование,
// низкий проход, уход от огня) считает наш код и каждые полсекунды подаёт вертолёту точку; висение и посадку на крышу
// доводит сам по скорости (м/с), без «примерно рядом».
#include "swat_internal.h"

#include <mod/amlmod.h>
#include <math.h>
#include <map>

#include "tactics.h"

namespace swat
{
namespace drv
{
namespace
{
    struct Car
    {
        int car = 0, driver = 0, mode = D_NONE, target = 0;
        V3 point; float speed = 15, dist = 10, heading = -1;
        uint32_t lastNative = 0, pitStart = 0, lastLog = 0;
        int nativeMode = -1;
        bool precise = false;                     // руль наш (перехват)
        float steer = 0, gas = 0, brake = 0;      // посчитано в кадре
        int phase = 0;
    };
    std::map<int, Car> g_cars;
    std::map<uintptr_t, int> g_byPtr;            // CVehicle* → handle (для перехвата)

    struct Heli
    {
        int heli = 0, pilot = 0, mode = H_NONE, ped = 0, car = 0;
        V3 point; float radius = 30, alt = 25;
        float orbitAng = 0;
        uint32_t lastCmd = 0;
        bool landed = false, kinematic = false;
        V3 lastGoto;
    };
    std::map<int, Heli> g_helis;

    float Clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
    uint32_t Now() { return NowMs(); }
    bool Every(uint32_t& last, uint32_t ms) { uint32_t n = Now(); if(n - last >= ms || n < last) { last = n; return true; } return false; }

    // Родной маршрут к точке (дороги, объезд, светофоры по стилю)
    void NativeRoute(Car& c, const V3& to, float speed, int style)
    {
        Do(0x05D1, { Ai(c.driver), Ai(c.car), Af(to.x), Af(to.y), Af(to.z), Af(speed), Ai(0), Ai(-1), Ai(style) });   // TASK_CAR_DRIVE_TO_COORD
        c.nativeMode = 1;
    }
    void NativeMission(Car& c, int mission, float speed)
    {
        Do(0x06E1, { Ai(c.driver), Ai(c.car), Ai(c.target), Ai(mission), Af(speed), Ai(2) });                           // TASK_CAR_MISSION
        c.nativeMode = 2;
    }

    // Наш руль к точке: угол → руль, скорость → газ/тормоз. reverse — задним ходом.
    void SteerTo(Car& c, uintptr_t v, const V3& to, float wantSpeed, bool reverse)
    {
        V3 p = Pos(v); float h = EntHeading(v);
        float want = Heading(p, to);
        float err = AngDiff(want, h);
        if(reverse) err = AngDiff(want, h + 180.0f);
        float steer = Clamp(err / 35.0f, -1.0f, 1.0f) * 0.6f;      // радианы руля (у игры предел ~0.6)
        if(reverse) steer = -steer;
        // поворот носом влево — у игры положительный руль
        float sp = Speed(v);
        V3 vel = Velocity(v), fw = Dir(h);
        float fwdSpeed = vel.x * fw.x + vel.y * fw.y;
        float gas = 0, brake = 0;
        float target = wantSpeed * (fabsf(err) > 60 ? 0.45f : 1.0f);
        if(!reverse)
        {
            if(fwdSpeed < target - 1.0f) gas = Clamp((target - fwdSpeed) / 6.0f, 0.25f, 1.0f);
            else if(fwdSpeed > target + 2.0f) brake = Clamp((fwdSpeed - target) / 8.0f, 0.2f, 1.0f);
        }
        else
        {
            if(fwdSpeed > 0.5f) brake = 1.0f;
            else if(-fwdSpeed < target) gas = -0.7f;
        }
        (void)sp;
        c.steer = steer; c.gas = gas; c.brake = brake;
    }

    // Скорость торможения к точке (м/с): чем ближе — тем медленнее
    float ApproachSpeed(float d, float vmax) { return Clamp(sqrtf(2.0f * 4.0f * (d > 0 ? d : 0)), 0.0f, vmax); }

    void CarFrame(Car& c)
    {
        uintptr_t v = Veh(c.car);
        if(!v || VehHealth(v) <= 0) { c.mode = D_NONE; return; }
        g_byPtr[v] = c.car;
        V3 p = Pos(v);
        uintptr_t tv = c.target ? Veh(c.target) : 0;
        V3 tp = tv ? Pos(tv) : c.point;
        float th = tv ? EntHeading(tv) : 0;
        float tspeed = tv ? Speed(tv) : 0;
        c.precise = false;
        switch(c.mode)
        {
            case D_ROUTE:
            {   // Д25/Д32/Д33: далеко — родной маршрут (объезд, обгон по стилю «объезжать»), последние 25 м — точно (Д26)
                float d = Dist2(p, c.point);
                if(d > 25.0f)
                {
                    if(c.nativeMode != 1 || Every(c.lastNative, 4000)) NativeRoute(c, c.point, c.speed, 2);
                }
                else { c.precise = true; SteerTo(c, v, c.point, ApproachSpeed(d - 2.0f, c.speed * 0.6f), false); }
                break;
            }
            case D_STOP_AT:
            {   // Д26: точно в точке и под углом
                float d = Dist2(p, c.point);
                c.precise = true;
                if(d > 1.5f) SteerTo(c, v, c.point, ApproachSpeed(d - 0.8f, c.speed), false);
                else { c.steer = 0; c.gas = 0; c.brake = 1; }
                if(d > 60.0f) { c.precise = false; if(c.nativeMode != 1 || Every(c.lastNative, 4000)) NativeRoute(c, c.point, c.speed, 2); }
                break;
            }
            case D_REVERSE_TO:
            {   // Д27
                float d = Dist2(p, c.point);
                c.precise = true;
                if(d > 1.5f) SteerTo(c, v, c.point, Clamp(d * 0.5f, 1.5f, 6.0f), true);
                else { c.steer = 0; c.gas = 0; c.brake = 1; }
                break;
            }
            case D_FOLLOW: case D_CONVOY:
            {   // Д28/Д37: держать дистанцию за целью — не таранить и не терять
                if(!tv) { c.mode = D_WAIT; break; }
                float d = Dist(p, tp);
                if(d > 90.0f)
                {   // далеко — родная погоня (догнать), ближе — свой руль
                    if(c.nativeMode != 2 || Every(c.lastNative, 3000)) NativeMission(c, 28 /*FOLLOWCAR_FARAWAY*/, c.speed);
                    break;
                }
                c.precise = true;
                V3 behind = Offset(tp, th, 0, -c.dist);
                float want = tspeed + Clamp((d - c.dist) * 0.6f, -6.0f, 10.0f);
                SteerTo(c, v, Dist(p, behind) > 4 ? behind : tp, Clamp(want, 0, c.speed * 1.3f), false);
                break;
            }
            case D_SIDE:
            {   // Д29: бок о бок (dist = смещение вбок, знак — сторона)
                if(!tv) { c.mode = D_WAIT; break; }
                c.precise = true;
                V3 side = Offset(tp, th, c.dist, 1.0f);
                float along = (p.x - tp.x) * Dir(th).x + (p.y - tp.y) * Dir(th).y;
                SteerTo(c, v, Offset(side, th, 0, 6.0f), tspeed + Clamp(-along * 0.8f, -5, 6), false);
                break;
            }
            case D_PIT:
            {   // Д30: подойти к заднему крылу и толкнуть под углом — цель разворачивает
                if(!tv) { c.mode = D_WAIT; break; }
                c.precise = true;
                float side = c.dist >= 0 ? 1.0f : -1.0f;
                V3 quarter = Offset(tp, th, side * 1.4f, -2.6f);
                float d = Dist2(p, quarter);
                if(c.phase == 0)
                {
                    SteerTo(c, v, Offset(quarter, th, 0, 3.0f), tspeed + Clamp(d * 0.6f, 1, 8), false);
                    if(d < 2.5f) { c.phase = 1; c.pitStart = Now(); Log("ВОДИТЕЛЬ", "PIT: толчок (машина %d)", c.car); }
                }
                else if(c.phase == 1)
                {   // доворот в сторону цели и газ
                    SteerTo(c, v, Offset(tp, th, -side * 1.5f, 1.0f), tspeed + 4, false);
                    c.gas = 1.0f;
                    if(Now() - c.pitStart > 1200) { c.phase = 2; c.pitStart = Now(); }
                }
                else
                {   // отойти и держать дистанцию
                    SteerTo(c, v, Offset(tp, th, 0, -12.0f), tspeed * 0.6f, false);
                    if(Now() - c.pitStart > 2500) c.phase = 0;
                }
                break;
            }
            case D_BLOCK:
            {   // Д31: встать поперёк дороги в точке; heading — направление дороги
                float d = Dist2(p, c.point);
                c.precise = true;
                float wantH = (c.heading >= 0 ? c.heading : 0) + 90.0f;
                if(d > 4.0f) SteerTo(c, v, c.point, ApproachSpeed(d, c.speed), false);
                else
                {
                    float e = AngDiff(wantH, EntHeading(v));
                    float e2 = AngDiff(wantH + 180, EntHeading(v));
                    if(fabsf(e2) < fabsf(e)) e = e2;
                    if(fabsf(e) < 15.0f) { c.steer = 0; c.gas = 0; c.brake = 1; }
                    else
                    {   // доворачиваем «вперёд-назад» (как разворот в три приёма)
                        bool fwd = ((Now() / 1200) % 2) == 0;
                        c.steer = (e > 0 ? 0.6f : -0.6f) * (fwd ? 1 : -1);
                        c.gas = fwd ? 0.45f : -0.45f; c.brake = 0;
                    }
                }
                break;
            }
            case D_OFFROAD:
            {   // Д38: напрямик по бездорожью
                float d = Dist2(p, c.point);
                c.precise = true;
                if(d > 3) SteerTo(c, v, c.point, ApproachSpeed(d, c.speed), false); else { c.gas = 0; c.brake = 1; c.steer = 0; }
                break;
            }
            case D_RAM:
            {   // таран (родной: «таранить машину рядом»)
                if(!tv) { c.mode = D_WAIT; break; }
                if(c.nativeMode != 2 || Every(c.lastNative, 3000)) NativeMission(c, 16 /*RAMCAR_CLOSE*/, c.speed);
                break;
            }
            case D_WAIT:
            {   // Д36: стоит с мотором, ждёт посадки
                c.precise = true; c.steer = 0; c.gas = 0; c.brake = 1;
                if(Every(c.lastNative, 5000)) Do(0x0918, { Ai(c.car), Ai(1) });   // SET_CAR_ENGINE_ON
                break;
            }
            default: break;
        }
        if(c.precise && c.nativeMode != 3)
        {   // чтобы игра вызывала руль ИИ (перехват) — машина должна «ехать по заданию»
            Do(0x05D1, { Ai(c.driver), Ai(c.car), Af(c.point.x), Af(c.point.y), Af(c.point.z), Af(c.speed), Ai(1), Ai(-1), Ai(2) });
            c.nativeMode = 3;
        }
    }

    // ---------------- вертолёт ----------------
    void HeliGoto(Heli& h, const V3& to, float minAlt, float maxAlt)
    {
        if(Dist(to, h.lastGoto) < 2.0f && Now() - h.lastCmd < 2500) return;
        h.lastGoto = to; h.lastCmd = Now();
        Do(0x04A2, { Ai(h.heli), Af(to.x), Af(to.y), Af(to.z), Af(minAlt), Af(maxAlt) });   // HELI_GOTO_COORDS
    }

    void HeliFrame(Heli& h)
    {
        uintptr_t v = Veh(h.heli);
        if(!v || VehHealth(v) <= 0) { h.mode = H_NONE; return; }
        V3 p = Pos(v);
        float ground = GroundZ(p.x, p.y, p.z);
        // Д50: сильный урон — аварийная посадка где есть
        if(VehHealth(v) < 350 && h.mode != H_LAND)
        {
            Log("ПИЛОТ", "вертолёт %d сильно повреждён — аварийная посадка", h.heli);
            h.mode = H_LAND; h.point = Mk(p.x, p.y, ground);
        }
        V3 tgt = h.point;
        if(h.ped) { uintptr_t pp = Ped(h.ped); if(pp) tgt = Pos(pp); }
        if(h.car) { uintptr_t cv = Veh(h.car); if(cv) { tgt = Pos(cv); V3 vel = Velocity(cv); tgt = Add(tgt, Mul(vel, 2.0f)); } }
        h.kinematic = false;
        switch(h.mode)
        {
            case H_TAKEOFF:   // Д39
                HeliGoto(h, Mk(p.x, p.y, ground + h.alt), h.alt, h.alt + 20);
                if(p.z - ground > h.alt - 3) h.mode = H_HOVER, h.point = p;
                break;
            case H_GOTO:      // Д40
                HeliGoto(h, Mk(tgt.x, tgt.y, tgt.z + h.alt), h.alt, h.alt + 30);
                break;
            case H_LOWPASS:   // Д45
                HeliGoto(h, Mk(tgt.x, tgt.y, tgt.z + 9.0f), 8.0f, 14.0f);
                break;
            case H_HOVER:     // Д41/Д47: зависнуть точно — сами гасим скорость и тянем к точке
            {
                V3 want = Mk(tgt.x, tgt.y, tgt.z + h.alt);
                float d = Dist(p, want);
                if(d > 12) HeliGoto(h, want, h.alt, h.alt + 10);
                else
                {
                    h.kinematic = true;
                    V3 vel = Mul(Sub(want, p), 0.8f);
                    float l = sqrtf(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
                    if(l > 6) vel = Mul(vel, 6 / l);
                    SetMoveSpeed(v, vel);
                }
                break;
            }
            case H_ORBIT:     // Д42: круг над целью
            case H_BROADSIDE: // Д43: бортом к цели
            {
                h.orbitAng += 0.6f; if(h.orbitAng > 360) h.orbitAng -= 360;   // ~36°/с при 60 кадрах
                V3 on = Add(tgt, Mul(Dir(h.orbitAng + 40.0f), h.radius)); on.z = tgt.z + h.alt;
                HeliGoto(h, on, h.alt * 0.8f, h.alt + 15);
                if(h.mode == H_BROADSIDE && Now() - h.lastCmd < 50)
                    Do(0x04D0, { Ai(h.heli), Af(Heading(p, tgt) + 90.0f) });  // SET_HELI_ORIENTATION — правый борт к цели
                break;
            }
            case H_FOLLOW:    // Д44: следовать за машиной сверху (с упреждением)
                HeliGoto(h, Add(tgt, Mk(0, 0, h.alt)), h.alt, h.alt + 20);
                break;
            case H_EVADE:     // Д49: уход от огня — вбок и вверх
            {
                V3 away = Add(p, Mul(Dir(EntHeading(v) + 90.0f), 40.0f)); away.z = p.z + 25;
                HeliGoto(h, away, h.alt + 20, h.alt + 45);
                if(Dist(p, away) < 10) h.mode = H_HOVER, h.point = Mk(p.x, p.y, p.z - h.alt);
                break;
            }
            case H_LAND:      // Д46/Д48/Д50: точная посадка — последние метры сами
            {
                float dh = Dist2(p, tgt);
                float above = p.z - tgt.z;
                if(dh > 14 || above > 22)
                {
                    if(Now() - h.lastCmd > 2500) { h.lastCmd = Now(); Do(0x0743, { Ai(h.heli), Af(tgt.x), Af(tgt.y), Af(tgt.z), Af(5.0f), Af(25.0f) }); }   // HELI_LAND_AT_COORDS
                }
                else if(above > 0.9f)
                {
                    h.kinematic = true;
                    V3 vel = Mk((tgt.x - p.x) * 0.9f, (tgt.y - p.y) * 0.9f, -Clamp(above * 0.5f, 0.8f, 3.0f));
                    if(dh > 2.0f) vel.z = -0.3f;          // сначала точно над площадкой, потом вниз
                    SetMoveSpeed(v, vel);
                }
                else
                {
                    SetMoveSpeed(v, Mk(0, 0, -0.2f));
                    if(!h.landed) { h.landed = true; Log("ПИЛОТ", "вертолёт %d сел (%.1f, %.1f, %.1f)", h.heli, p.x, p.y, p.z); }
                }
                break;
            }
            default: break;
        }
    }

    // Перехват руля ИИ: наша машина — свой руль/газ/тормоз
    SDECL_HOOKv(SteerAIH, uintptr_t veh)
    {
        auto it = g_byPtr.find(veh);
        if(it != g_byPtr.end())
        {
            auto c = g_cars.find(it->second);
            if(c != g_cars.end() && c->second.precise && c->second.mode != D_NONE)
            {
                SetControls(veh, c->second.steer, c->second.gas, c->second.brake);
                return;
            }
        }
        SteerAIH(veh);
    }
    bool g_hooked = false;
}

void Drive(int car, int driver, int mode, const V3& point, int targetCar, float speed, float dist, float heading)
{
    Car& c = g_cars[car];
    bool changed = c.mode != mode || c.target != targetCar || Dist(c.point, point) > 3;
    c.car = car; c.driver = driver; c.mode = mode; c.point = point; c.target = targetCar;
    c.speed = speed > 1 ? speed : 15; c.dist = dist; c.heading = heading;
    if(changed) { c.nativeMode = -1; c.phase = 0; Log("ВОДИТЕЛЬ", "машина %d: %s (%.0f, %.0f) цель %d", car, Doing(car), point.x, point.y, targetCar); }
}
void Release(int car)
{
    auto it = g_cars.find(car);
    if(it == g_cars.end()) return;
    for(auto p = g_byPtr.begin(); p != g_byPtr.end();) { if(p->second == car) p = g_byPtr.erase(p); else ++p; }
    g_cars.erase(it);
}
bool Active(int car) { auto it = g_cars.find(car); return it != g_cars.end() && it->second.mode != D_NONE; }
bool Arrived(int car, float r) { auto it = g_cars.find(car); if(it == g_cars.end()) return false; uintptr_t v = Veh(car); return v && Dist2(Pos(v), it->second.point) < r && Speed(v) < 2.5f; }
const char* Doing(int car)
{
    auto it = g_cars.find(car); if(it == g_cars.end()) return "";
    switch(it->second.mode)
    {
        case D_ROUTE: return "едет по маршруту"; case D_FOLLOW: return "держит дистанцию за целью"; case D_SIDE: return "идёт бок о бок";
        case D_PIT: return "PIT"; case D_BLOCK: return "встаёт поперёк дороги"; case D_STOP_AT: return "встаёт в точку";
        case D_REVERSE_TO: return "сдаёт задом"; case D_OFFROAD: return "по бездорожью"; case D_CONVOY: return "в колонне";
        case D_WAIT: return "ждёт с мотором"; case D_RAM: return "таранит";
    }
    return "";
}

void Fly(int heli, int pilot, int mode, const V3& point, int ped, int car, float radius, float alt)
{
    Heli& h = g_helis[heli];
    bool changed = h.mode != mode;
    h.heli = heli; h.pilot = pilot; h.mode = mode; h.point = point; h.ped = ped; h.car = car;
    h.radius = radius > 5 ? radius : 30; h.alt = alt > 1 ? alt : 25;
    if(changed)
    {
        h.landed = false; h.lastCmd = 0;
        Do(0x0825, { Ai(heli) });                                      // SET_HELI_BLADES_FULL_SPEED
        if(mode != H_BROADSIDE) Do(0x04D1, { Ai(heli) });              // CLEAR_HELI_ORIENTATION
        Log("ПИЛОТ", "вертолёт %d: %s", heli, HeliDoing(heli));
    }
}
void Ground(int heli) { g_helis.erase(heli); }
bool Landed(int heli) { auto it = g_helis.find(heli); return it != g_helis.end() && it->second.landed; }
const char* HeliDoing(int heli)
{
    auto it = g_helis.find(heli); if(it == g_helis.end()) return "";
    switch(it->second.mode)
    {
        case H_TAKEOFF: return "взлетает"; case H_GOTO: return "летит к точке"; case H_HOVER: return "висит над точкой";
        case H_ORBIT: return "кружит над целью"; case H_BROADSIDE: return "бортом к цели"; case H_FOLLOW: return "следует за машиной";
        case H_LOWPASS: return "низкий проход"; case H_LAND: return "садится"; case H_EVADE: return "уходит от огня";
    }
    return "";
}

void Frame()
{
    if(Paused()) return;
    for(auto it = g_cars.begin(); it != g_cars.end();)
    {
        CarFrame(it->second);
        if(it->second.mode == D_NONE) { int car = it->first; ++it; Release(car); } else ++it;
    }
    for(auto it = g_helis.begin(); it != g_helis.end();)
    {
        HeliFrame(it->second);
        if(it->second.mode == H_NONE) it = g_helis.erase(it); else ++it;
    }
}
}

bool InstallDriverHooks(void* h)
{
    if(drv::g_hooked) return true;
    void* f = (void*)aml->GetSym(h, "_ZN8CCarCtrl21SteerAICarWithPhysicsEP8CVehicle");
    if(!f) { Log("ВОДИТЕЛЬ", "нет CCarCtrl::SteerAICarWithPhysics — точные манёвры только родным водителем"); return false; }
    aml->Hook(f, (void*)drv::HookOf_SteerAIH, (void**)&drv::SteerAIH);
    drv::g_hooked = true;
    Log("ВОДИТЕЛЬ", "свой руль подключён (перехват SteerAICarWithPhysics)");
    return true;
}
void DriverFrame() { drv::Frame(); }
}
