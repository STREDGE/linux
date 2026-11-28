#pragma once
// Общая библиотека «тактика спецназа» (SA Story + SA Living): доступ к игре GTA SA Android 2.10 arm64.
// Самостоятельная: свои символы игры, свой исполнитель команд сценария. Только игровой поток.
// Смещения — по отладочной информации libGTASA.so (DWARF), см. docs/libgtasa-2.10-arm64.md.

#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

namespace swat
{
    struct V3 { float x = 0, y = 0, z = 0; };
    inline V3 Mk(float x, float y, float z) { V3 v; v.x = x; v.y = y; v.z = z; return v; }
    float Dist(const V3& a, const V3& b);
    float Dist2(const V3& a, const V3& b);                // по земле
    V3    Add(const V3& a, const V3& b);
    V3    Sub(const V3& a, const V3& b);
    V3    Mul(const V3& a, float k);
    V3    Norm2(const V3& a);                              // по земле, z = 0
    float Heading(const V3& from, const V3& to);           // градусы игры (0 — на север, против часовой)
    V3    Dir(float headingDeg);                           // единичный по земле
    V3    Offset(const V3& at, float headingDeg, float right, float fwd);
    float AngDiff(float a, float b);                       // -180..180
    float Rnd(float a, float b);
    int   RndI(int a, int b);

    // Журнал (ставит мод): тег + текст
    void SetLog(void (*fn)(const char* tag, const char* text));
    void Log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    bool Init(void* libHandle);
    bool Ready();
    uint32_t NowMs();                                      // время игры (стоит на паузе)
    bool Paused();

    // ---------- команды сценария игры ----------
    struct A
    {
        enum T { I, F, S, OI, OF } t = I;
        int i = 0; float f = 0; std::string s;
    };
    inline A Ai(int v) { A a; a.t = A::I; a.i = v; return a; }
    inline A Af(float v) { A a; a.t = A::F; a.f = v; return a; }
    inline A As(const std::string& v) { A a; a.t = A::S; a.s = v; return a; }
    inline A Aoi() { A a; a.t = A::OI; return a; }
    inline A Aof() { A a; a.t = A::OF; return a; }
    bool Cmd(uint16_t op, std::vector<A>& args, bool* flag = nullptr, bool quiet = true);
    void Do(uint16_t op, std::vector<A> args);             // действие
    bool Is(uint16_t op, std::vector<A> args);             // условие
    int  OutI(uint16_t op, std::vector<A> args);           // одна выходная (целое), -1 — нет
    bool Out3(uint16_t op, std::vector<A> args, V3& out);  // три выходных (x, y, z)
    // Дать оружие: сначала грузит модель оружия (с ней — файл анимаций rifle/uzi/…), иначе игра падает в BlendAnimation.
    bool GiveWeapon(int ped, int weaponType, int ammo);

    // ---------- педы ----------
    uintptr_t Player();
    int       PlayerRef();
    uintptr_t Ped(int ref);
    int       Ref(uintptr_t ped);
    bool      PedAlive(uintptr_t ped);
    bool      RefAlive(int ref);
    V3        Pos(uintptr_t ent);
    V3        RefPos(int ref);
    float     EntHeading(uintptr_t ent);                    // градусы
    V3        Velocity(uintptr_t ent);                      // м/с
    int       Model(uintptr_t ent);
    int       PedType(uintptr_t ped);
    float     Health(uintptr_t ped);
    float     Armour(uintptr_t ped);
    uintptr_t PedVehicle(uintptr_t ped);
    int       PedState(uintptr_t ped);
    bool      Ducking(uintptr_t ped);
    int       CurWeapon(uintptr_t ped);                     // тип оружия в руках
    int       ClipAmmo(uintptr_t ped);                      // патронов в магазине
    int       TotalAmmo(uintptr_t ped);
    bool      HasWeapon(uintptr_t ped, int type);
    int       ClipSize(int weaponType);
    uintptr_t AimTarget(uintptr_t ped);                     // в кого целится/кого атакует (по задачам) или 0
    bool      Shooting(int ref);                            // стреляет прямо сейчас
    bool      InWater(int ref);
    void      ForEachPed(const std::function<void(uintptr_t)>& fn);
    void      ForEachVehicle(const std::function<void(uintptr_t)>& fn);
    void      SetHeadingNow(uintptr_t ped, float deg);

    // ---------- машины ----------
    int       VehRef(uintptr_t veh);
    uintptr_t Veh(int ref);
    float     VehHealth(uintptr_t veh);
    uintptr_t VehDriver(uintptr_t veh);
    uintptr_t VehPassenger(uintptr_t veh, int seat);
    int       VehMaxPassengers(uintptr_t veh);
    int       VehType(uintptr_t veh);                       // 0 машина, 9 мотоцикл, 3 вертолёт, 4 самолёт, 5 лодка …
    bool      IsHeli(uintptr_t veh);
    float     Speed(uintptr_t ent);                         // м/с
    void      SetMoveSpeed(uintptr_t ent, const V3& mps);   // прямо задать скорость (м/с)
    void      SetControls(uintptr_t veh, float steer, float gas, float brake);
    float*    HeliControls(uintptr_t heli);                 // yaw, pitch, roll, throttle (4 float) или nullptr

    // ---------- мир ----------
    float     GroundZ(float x, float y, float zHint);
    float     WaterZ(float x, float y, float zHint, bool* has);
    // Линия видимости. hitOut — точка удара. Возвращает true, если что-то МЕШАЕТ (попадание).
    bool      Ray(const V3& a, const V3& b, V3* hitOut = nullptr, uintptr_t* hitEnt = nullptr,
                  bool buildings = true, bool vehicles = true, bool peds = false, bool objects = true);
    int       RaysThisFrame();                              // сколько лучей уже пустили в этом кадре
    void      NewFrame();                                   // начало кадра (бюджет лучей)
    bool      OnScreen(const V3& p, float radius);
    bool      ClosestCarNode(const V3& at, V3& out);
    bool      NthCarNode(const V3& at, int n, V3& out, float& heading);
    bool      ClosestPedNode(const V3& at, V3& out);
    // Входы в здания (маркеры игры): позиция двери и куда переносит
    struct Door { V3 at; float rot; V3 inside; int area; char title[9]; };
    void      Doors(const V3& near, float radius, std::vector<Door>& out);

    // ---------- полезное ----------
    uintptr_t PlayerTarget();                               // в кого целится игрок
    uintptr_t PlayerTargetPed();                            // в кого целится игрок; машина в прицеле — её водитель
    bool      PlayerWaypoint(V3& out);                      // метка игрока на карте
    bool      IsCopType(int pedType);
}
