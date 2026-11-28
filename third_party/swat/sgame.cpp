#include "sgame.h"

#include <mod/amlmod.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace swat
{
namespace
{
    template<typename T> T& F(uintptr_t p, uintptr_t off) { return *(T*)(p + off); }

    // ---- смещения (DWARF libGTASA 2.10 arm64) ----
    constexpr uintptr_t kPlayerInfoSize = 0x1D8;
    constexpr uintptr_t kEntPlace = 0x08, kEntHeading = 0x14, kEntMatrix = 0x18, kEntModel = 0x32, kEntType = 0x5A;
    constexpr uintptr_t kMoveSpeed = 0x68;
    constexpr uintptr_t kPedIntel = 0x538, kPedState = 0x54C, kPedFlags = 0x584, kPedHealth = 0x6AC, kPedArmour = 0x6B4;
    constexpr uintptr_t kPedRotCur = 0x6C4, kPedRotAim = 0x6C8, kPedVehicle = 0x708, kPedType = 0x720;
    constexpr uintptr_t kPedWeapons = 0x730, kPedCurSlot = 0x8DC, kPedLockOn = 0x8E0;
    constexpr uintptr_t kVehStride = 0xC68;
    constexpr uintptr_t kVehDriver = 0x5A0, kVehPass = 0x5A8, kVehMaxPass = 0x5EC, kVehSteer = 0x600, kVehGas = 0x608, kVehBrake = 0x60C;
    constexpr uintptr_t kVehHealth = 0x634, kVehType = 0x738;
    constexpr uintptr_t kHeliYaw = 0xBCC;     // yaw, pitch, roll, throttle
    constexpr float kTimeStep = 50.0f;         // скорость в игре — единиц за 1/50 с

    uintptr_t g_players = 0, g_focus = 0, g_timeMs = 0, g_userPause = 0, g_codePause = 0;
    uintptr_t g_pedPool = 0, g_vehPool = 0, g_entryPool = 0, g_radarTrace = 0;
    void* (*ProcessOne)(void*) = nullptr;
    int  (*GetPedRefFn)(uintptr_t) = nullptr;
    uintptr_t (*GetPedFn)(int) = nullptr;
    int  (*GetVehRefFn)(uintptr_t) = nullptr;
    uintptr_t (*GetVehFn)(int) = nullptr;
    float (*GroundZFn)(float, float, float, bool*, void**) = nullptr;
    bool (*WaterFn)(float, float, float, float*, bool, void*) = nullptr;
    bool (*LosFn)(const V3&, const V3&, void*, uintptr_t&, bool, bool, bool, bool, bool, bool, bool, bool) = nullptr;
    uintptr_t (*WeaponInfoFn)(int, int8_t) = nullptr;
    bool (*OnScreenFn)(const V3&, float, bool) = nullptr;
    uintptr_t g_camera = 0;
    void (*SetCreatedByFn)(uintptr_t, uint8_t) = nullptr;
    void (*LogFn)(const char*, const char*) = nullptr;
    int g_rays = 0;
    bool g_ready = false;

    struct Pool { uintptr_t objs, flags; int size; };
    bool GetPool(uintptr_t sym, Pool& p)
    {
        uintptr_t pp = sym ? *(uintptr_t*)sym : 0;
        if(!pp) return false;
        p.objs = F<uintptr_t>(pp, 0); p.flags = F<uintptr_t>(pp, 8); p.size = F<int>(pp, 0x10);
        return p.objs && p.flags && p.size > 0 && p.size <= 20000;
    }
    uintptr_t PlayerInfo()
    {
        if(!g_players) return 0;
        int f = g_focus ? *(uint8_t*)g_focus : 0; if(f > 1) f = 0;
        return g_players + (uintptr_t)f * kPlayerInfoSize;
    }
    uintptr_t Mgr(uintptr_t ped) { uintptr_t in = ped ? F<uintptr_t>(ped, kPedIntel) : 0; return in ? in + 8 : 0; }
    uintptr_t Sym(void* h, const char* s) { uintptr_t a = aml->GetSym(h, s); if(!a) Log("ОШИБКА", "нет символа игры %s", s); return a; }
}

// ---------- математика ----------
float Dist(const V3& a, const V3& b) { float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return sqrtf(dx * dx + dy * dy + dz * dz); }
float Dist2(const V3& a, const V3& b) { float dx = a.x - b.x, dy = a.y - b.y; return sqrtf(dx * dx + dy * dy); }
V3 Add(const V3& a, const V3& b) { return Mk(a.x + b.x, a.y + b.y, a.z + b.z); }
V3 Sub(const V3& a, const V3& b) { return Mk(a.x - b.x, a.y - b.y, a.z - b.z); }
V3 Mul(const V3& a, float k) { return Mk(a.x * k, a.y * k, a.z * k); }
V3 Norm2(const V3& a) { float l = sqrtf(a.x * a.x + a.y * a.y); return l > 1e-4f ? Mk(a.x / l, a.y / l, 0) : Mk(0, 1, 0); }
float Heading(const V3& f, const V3& t) { float h = atan2f(-(t.x - f.x), t.y - f.y) * 57.29578f; if(h < 0) h += 360; return h; }
V3 Dir(float h) { float r = h * 0.0174533f; return Mk(-sinf(r), cosf(r), 0); }
V3 Offset(const V3& at, float h, float right, float fwd)
{
    V3 f = Dir(h), r = Mk(f.y, -f.x, 0);
    return Mk(at.x + r.x * right + f.x * fwd, at.y + r.y * right + f.y * fwd, at.z);
}
float AngDiff(float a, float b) { float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f; return d; }
float Rnd(float a, float b) { return a + (b - a) * (float)(rand() % 10000) / 10000.0f; }
int   RndI(int a, int b) { return b <= a ? a : a + rand() % (b - a + 1); }

void SetLog(void (*fn)(const char*, const char*)) { LogFn = fn; }
void Log(const char* tag, const char* fmt, ...)
{
    if(!LogFn) return;
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    LogFn(tag, b);
}

bool Init(void* h)
{
    if(g_ready) return true;
    g_players   = Sym(h, "_ZN6CWorld7PlayersE");
    g_focus     = Sym(h, "_ZN6CWorld13PlayerInFocusE");
    g_timeMs    = Sym(h, "_ZN6CTimer22m_snTimeInMillisecondsE");
    g_userPause = aml->GetSym(h, "_ZN6CTimer11m_UserPauseE");
    g_codePause = aml->GetSym(h, "_ZN6CTimer11m_CodePauseE");
    g_pedPool   = Sym(h, "_ZN6CPools11ms_pPedPoolE");
    g_vehPool   = Sym(h, "_ZN6CPools15ms_pVehiclePoolE");
    g_entryPool = aml->GetSym(h, "_ZN17CEntryExitManager17mp_poolEntryExitsE");
    g_radarTrace= aml->GetSym(h, "_ZN6CRadar13ms_RadarTraceE");
    ProcessOne  = (void*(*)(void*))Sym(h, "_ZN14CRunningScript17ProcessOneCommandEv");
    GetPedRefFn = (int(*)(uintptr_t))Sym(h, "_ZN6CPools9GetPedRefEP4CPed");
    GetPedFn    = (uintptr_t(*)(int))Sym(h, "_ZN6CPools6GetPedEi");
    GetVehRefFn = (int(*)(uintptr_t))Sym(h, "_ZN6CPools13GetVehicleRefEP8CVehicle");
    GetVehFn    = (uintptr_t(*)(int))Sym(h, "_ZN6CPools10GetVehicleEi");
    GroundZFn   = (float(*)(float, float, float, bool*, void**))Sym(h, "_ZN6CWorld21FindGroundZFor3DCoordEfffPbPP7CEntity");
    WaterFn     = (bool(*)(float, float, float, float*, bool, void*))aml->GetSym(h, "_ZN11CWaterLevel13GetWaterLevelEfffPfbP7CVector");
    LosFn       = (bool(*)(const V3&, const V3&, void*, uintptr_t&, bool, bool, bool, bool, bool, bool, bool, bool))
                  Sym(h, "_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntitybbbbbbbb");
    WeaponInfoFn= (uintptr_t(*)(int, int8_t))aml->GetSym(h, "_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponTypea");
    OnScreenFn  = (bool(*)(const V3&, float, bool))aml->GetSym(h, "_Z10IsOnScreenRK7CVectorfb");
    g_camera    = aml->GetSym(h, "TheCamera");
    SetCreatedByFn = (void(*)(uintptr_t, uint8_t))aml->GetSym(h, "_ZN4CPed16SetCharCreatedByEh");
    g_ready = g_players && g_timeMs && ProcessOne && GetPedRefFn && GetPedFn && g_pedPool && g_vehPool;
    Log("ТАКТИКА", "игра: %s (луч %s, вода %s, входы %s)", g_ready ? "готово" : "НЕ ВСЁ НАЙДЕНО", LosFn ? "да" : "нет",
        WaterFn ? "да" : "нет", g_entryPool ? "да" : "нет");
    return g_ready;
}
bool Ready() { return g_ready; }
// Камера игры: позиция и направление взгляда (CCamera::m_cameraMatrix, как в SA Living — проверено в игре)
bool Camera(V3& pos, V3& front)
{
    if(!g_camera) return false;
    uintptr_t m = g_camera + 0x970;
    front = Mk(F<float>(m, 0x10), F<float>(m, 0x14), F<float>(m, 0x18));
    pos   = Mk(F<float>(m, 0x30), F<float>(m, 0x34), F<float>(m, 0x38));
    return true;
}
// Случайного прохожего — в «сюжетные» (как делает игра в GET_RANDOM_CHAR…): игра его больше не удалит сама.
// Счётчики населения игра ведёт по типу педа, не по «кто создал», поэтому этого достаточно. Отпустить — 01C2.
bool MakeMissionPed(uintptr_t ped)
{
    if(!ped || !SetCreatedByFn) return false;
    if(F<uint8_t>(ped, 0x548) == 2) return false;                    // уже сюжетный (создан скриптом)
    SetCreatedByFn(ped, 2);
    return true;
}
uint32_t NowMs() { return g_timeMs ? *(uint32_t*)g_timeMs : 0; }
bool Paused() { return (g_userPause && *(uint8_t*)g_userPause) || (g_codePause && *(uint8_t*)g_codePause); }

// ---------- исполнитель команд сценария (как SA Story script.cpp) ----------
bool Cmd(uint16_t op, std::vector<A>& args, bool* flag, bool quiet)
{
    if(!ProcessOne || !Player()) return false;
    constexpr size_t kSize = 0x130, kName = 0x10, kBase = 0x18, kPC = 0x20, kLocals = 0x6C, kActive = 0x114, kCmp = 0x115;
    alignas(16) uint8_t scr[kSize + 16]; memset(scr, 0, sizeof(scr));
    uint8_t code[512]; size_t n = 0;
    auto put = [&](const void* p, size_t k) { if(n + k <= sizeof(code)) { memcpy(code + n, p, k); n += k; } };
    put(&op, 2);
    int outs = 0;
    for(A& a : args)
    {
        uint8_t t;
        switch(a.t)
        {
            case A::I: t = 0x01; put(&t, 1); put(&a.i, 4); break;
            case A::F: t = 0x06; put(&t, 1); put(&a.f, 4); break;
            case A::S:
                if(a.s.size() <= 7) { t = 0x09; put(&t, 1); char b[8] = {}; memcpy(b, a.s.data(), a.s.size()); put(b, 8); }
                else { t = 0x0F; put(&t, 1); char b[16] = {}; memcpy(b, a.s.data(), a.s.size() > 15 ? 15 : a.s.size()); put(b, 16); }
                break;
            case A::OI: case A::OF: { t = 0x03; put(&t, 1); uint16_t idx = (uint16_t)outs++; put(&idx, 2); break; }
        }
    }
    if(n + 8 > sizeof(code) || outs > 30) return false;
    memset(code + n, 0, 8);
    memcpy(scr + kName, "swattac", 7);
    uint8_t* base = code;
    memcpy(scr + kBase, &base, sizeof(base)); memcpy(scr + kPC, &base, sizeof(base));
    scr[kActive] = 1;
    if(!quiet)
    {
        std::string s; char b[48];
        for(A& x : args)
            switch(x.t)
            {
                case A::I: snprintf(b, sizeof(b), " %d", x.i); s += b; break;
                case A::F: snprintf(b, sizeof(b), " %.2f", x.f); s += b; break;
                case A::S: s += " \"" + x.s + "\""; break;
                default: s += " >"; break;
            }
        Log("КОМАНДА", "%04X%s", op, s.c_str());
    }
    ProcessOne(scr);
    if(flag) *flag = scr[kCmp] != 0;
    outs = 0;
    for(A& a : args)
    {
        if(a.t == A::OI) memcpy(&a.i, scr + kLocals + 4 * outs++, 4);
        else if(a.t == A::OF) memcpy(&a.f, scr + kLocals + 4 * outs++, 4);
    }
    return true;
}
void Do(uint16_t op, std::vector<A> a) { Cmd(op, a, nullptr, true); }
bool Is(uint16_t op, std::vector<A> a) { bool f = false; if(!Cmd(op, a, &f, true)) return false; return f; }
int  OutI(uint16_t op, std::vector<A> a) { if(!Cmd(op, a, nullptr, true)) return -1; for(A& x : a) if(x.t == A::OI) return x.i; return -1; }
bool GiveWeapon(int ped, int w, int ammo)
{
    if(ped <= 0 || w <= 0) return false;
    int model = OutI(0x0781, { Ai(w), Aoi() });                                   // GET_WEAPONTYPE_MODEL
    if(model > 0 && !Is(0x0248, { Ai(model) }))                                   // HAS_MODEL_LOADED
    {
        Do(0x0247, { Ai(model) });                                                // REQUEST_MODEL
        Do(0x038B, {});                                                           // LOAD_ALL_MODELS_NOW
        if(!Is(0x0248, { Ai(model) })) { Log("ОРУЖИЕ", "модель %d для оружия %d не загрузилась — не выдаю", model, w); return false; }
    }
    Do(0x01B2, { Ai(ped), Ai(w), Ai(ammo) });                                     // GIVE_WEAPON_TO_CHAR
    return true;
}
bool Out3(uint16_t op, std::vector<A> a, V3& out)
{
    if(!Cmd(op, a, nullptr, true)) return false;
    float v[4] = {}; int k = 0;
    for(A& x : a) if(x.t == A::OF && k < 4) v[k++] = x.f;
    if(k < 3) return false;
    out = Mk(v[0], v[1], v[2]);
    return true;
}

// ---------- педы ----------
uintptr_t Player() { uintptr_t pi = PlayerInfo(); return pi ? F<uintptr_t>(pi, 0) : 0; }
int PlayerRef() { return Ref(Player()); }
uintptr_t Ped(int ref) { return ref > 0 && GetPedFn ? GetPedFn(ref) : 0; }
int Ref(uintptr_t p) { return p && GetPedRefFn ? GetPedRefFn(p) : 0; }
bool PedAlive(uintptr_t p) { if(!p) return false; int s = F<int>(p, kPedState); return s != 54 && s != 55 && F<float>(p, kPedHealth) > 0; }
bool RefAlive(int ref) { return PedAlive(Ped(ref)); }
V3 Pos(uintptr_t e)
{
    if(!e) return V3();
    uintptr_t m = F<uintptr_t>(e, kEntMatrix);
    const float* f = m ? (const float*)(m + 0x30) : (const float*)(e + kEntPlace);
    return Mk(f[0], f[1], f[2]);
}
V3 RefPos(int ref) { return Pos(Ped(ref)); }
float EntHeading(uintptr_t e)
{
    if(!e) return 0;
    uintptr_t m = F<uintptr_t>(e, kEntMatrix);
    float h;
    if(m) { const float* fw = (const float*)(m + 0x10); h = atan2f(-fw[0], fw[1]); }
    else h = F<float>(e, kEntHeading);
    h *= 57.29578f; if(h < 0) h += 360; return h;
}
V3 Velocity(uintptr_t e) { if(!e) return V3(); const float* v = (const float*)(e + kMoveSpeed); return Mk(v[0] * kTimeStep, v[1] * kTimeStep, v[2] * kTimeStep); }
int Model(uintptr_t e) { return e ? F<int16_t>(e, kEntModel) : -1; }
int PedType(uintptr_t p) { return p ? F<int>(p, kPedType) : -1; }
float Health(uintptr_t p) { return p ? F<float>(p, kPedHealth) : 0; }
float Armour(uintptr_t p) { return p ? F<float>(p, kPedArmour) : 0; }
uintptr_t PedVehicle(uintptr_t p) { return p && (F<uint8_t>(p, kPedFlags + 1) & 1) ? F<uintptr_t>(p, kPedVehicle) : 0; }
int PedState(uintptr_t p) { return p ? F<int>(p, kPedState) : 0; }
bool Ducking(uintptr_t p) { return p && (F<uint32_t>(p, kPedFlags) & (1u << 26)); }
static uintptr_t Slot(uintptr_t p) { int s = F<int8_t>(p, kPedCurSlot); if(s < 0 || s > 12) s = 0; return p + kPedWeapons + (uintptr_t)s * 0x20; }
int CurWeapon(uintptr_t p) { return p ? F<int>(Slot(p), 0) : 0; }
int ClipAmmo(uintptr_t p) { return p ? F<int>(Slot(p), 8) : 0; }
int TotalAmmo(uintptr_t p) { return p ? F<int>(Slot(p), 12) : 0; }
bool HasWeapon(uintptr_t p, int type)
{
    if(!p) return false;
    for(int i = 0; i < 13; ++i) { uintptr_t w = p + kPedWeapons + (uintptr_t)i * 0x20; if(F<int>(w, 0) == type && F<int>(w, 12) > 0) return true; }
    return false;
}
int ClipSize(int type) { uintptr_t wi = WeaponInfoFn ? WeaponInfoFn(type, 1) : 0; int n = wi ? F<int16_t>(wi, 32) : 0; return n > 0 ? n : 30; }
uintptr_t AimTarget(uintptr_t ped)
{
    if(!ped) return 0;
    uintptr_t lo = F<uintptr_t>(ped, kPedLockOn);
    if(lo) return lo;
    uintptr_t m = Mgr(ped); if(!m) return 0;
    uintptr_t t = 0;
    for(int s = 0; s < 5 && !t; ++s) t = F<uintptr_t>(m, s * 8);
    typedef int (*GetType_t)(uintptr_t);
    typedef uintptr_t (*GetSub_t)(uintptr_t);
    for(int depth = 0; t && depth < 12; ++depth)
    {
        uintptr_t vt = F<uintptr_t>(t, 0); if(!vt) return 0;
        GetType_t gt = F<GetType_t>(vt, 0x28);
        int type = gt ? gt(t) : -1, off = 0;
        switch(type)
        {
            case 1000: case 1028: off = 0x20; break;
            case 1001: case 1002: off = 0x18; break;
            case 1016: off = 0x20; break;
            case 1017: off = 0x28; break;
            case 1019: off = 0x18; break;
            case 933:  off = 0x18; break;
        }
        if(off) { uintptr_t e = F<uintptr_t>(t, off); if(e) return e; }
        GetSub_t gs = F<GetSub_t>(vt, 0x18);
        t = gs ? gs(t) : 0;
    }
    return 0;
}
bool Shooting(int ref) { return ref > 0 && Is(0x02E0, { Ai(ref) }); }          // IS_CHAR_SHOOTING
bool InWater(int ref) { return ref > 0 && Is(0x04AD, { Ai(ref) }); }           // IS_CHAR_IN_WATER
void ForEachPed(const std::function<void(uintptr_t)>& fn)
{
    Pool p; if(!GetPool(g_pedPool, p)) return;
    // пул педов хранит CPlayerPed-размер записи (0x9C8 в SA Living — проверено в игре)
    for(int i = 0; i < p.size; ++i) { if(F<uint8_t>(p.flags, i) & 0x80) continue; fn(p.objs + (uintptr_t)i * 0x9C8); }
}
void ForEachVehicle(const std::function<void(uintptr_t)>& fn)
{
    Pool p; if(!GetPool(g_vehPool, p)) return;
    for(int i = 0; i < p.size; ++i) { if(F<uint8_t>(p.flags, i) & 0x80) continue; fn(p.objs + (uintptr_t)i * kVehStride); }
}
void SetHeadingNow(uintptr_t p, float deg) { if(!p) return; float r = deg * 0.0174533f; F<float>(p, kPedRotCur) = r; F<float>(p, kPedRotAim) = r; }

// ---------- машины ----------
int VehRef(uintptr_t v) { return v && GetVehRefFn ? GetVehRefFn(v) : 0; }
uintptr_t Veh(int ref) { return ref > 0 && GetVehFn ? GetVehFn(ref) : 0; }
float VehHealth(uintptr_t v) { return v ? F<float>(v, kVehHealth) : 0; }
uintptr_t VehDriver(uintptr_t v) { return v ? F<uintptr_t>(v, kVehDriver) : 0; }
uintptr_t VehPassenger(uintptr_t v, int s) { return v && s >= 0 && s < 8 ? F<uintptr_t>(v, kVehPass + s * 8) : 0; }
int VehMaxPassengers(uintptr_t v) { int n = v ? F<uint8_t>(v, kVehMaxPass) : 0; return n > 8 ? 8 : n; }
int VehType(uintptr_t v) { return v ? F<int>(v, kVehType) : -1; }
bool IsHeli(uintptr_t v) { return VehType(v) == 3; }
float Speed(uintptr_t e) { V3 s = Velocity(e); return sqrtf(s.x * s.x + s.y * s.y + s.z * s.z); }
void SetMoveSpeed(uintptr_t e, const V3& v) { if(!e) return; float* s = (float*)(e + kMoveSpeed); s[0] = v.x / kTimeStep; s[1] = v.y / kTimeStep; s[2] = v.z / kTimeStep; }
void SetControls(uintptr_t v, float steer, float gas, float brake)
{
    if(!v) return;
    F<float>(v, kVehSteer) = steer; F<float>(v, kVehGas) = gas; F<float>(v, kVehBrake) = brake;
}
float* HeliControls(uintptr_t h) { return IsHeli(h) ? (float*)(h + kHeliYaw) : nullptr; }

// ---------- мир ----------
float GroundZ(float x, float y, float z)
{
    if(!GroundZFn) return z;
    bool found = false; void* e = nullptr;
    float g = GroundZFn(x, y, z + 3.0f, &found, &e);
    return found ? g : z;
}
float WaterZ(float x, float y, float z, bool* has)
{
    float w = -100;
    bool ok = WaterFn && WaterFn(x, y, z, &w, false, nullptr);
    if(has) *has = ok;
    return ok ? w : -100;
}
bool Ray(const V3& a, const V3& b, V3* hit, uintptr_t* hitEnt, bool bld, bool veh, bool peds, bool obj)
{
    g_rays++;
    if(!LosFn) return !Is(0x06BD, { Af(a.x), Af(a.y), Af(a.z), Af(b.x), Af(b.y), Af(b.z), Ai(bld), Ai(veh), Ai(peds), Ai(obj), Ai(0) });
    alignas(16) uint8_t cp[64]; memset(cp, 0, sizeof(cp));
    uintptr_t ent = 0;
    bool r = LosFn(a, b, cp, ent, bld, veh, peds, obj, false, true, false, true);
    if(r && hit) memcpy(hit, cp, sizeof(V3));
    if(hitEnt) *hitEnt = r ? ent : 0;
    return r;
}
int RaysThisFrame() { return g_rays; }
void NewFrame() { g_rays = 0; }
bool OnScreen(const V3& p, float r) { return OnScreenFn ? OnScreenFn(p, r, false) : Is(0x00C2, { Af(p.x), Af(p.y), Af(p.z), Af(r) }); }
bool ClosestCarNode(const V3& at, V3& out) { return Out3(0x02C1, { Af(at.x), Af(at.y), Af(at.z), Aof(), Aof(), Aof() }, out) && (out.x != 0 || out.y != 0); }
bool ClosestPedNode(const V3& at, V3& out) { return Out3(0x02C0, { Af(at.x), Af(at.y), Af(at.z), Aof(), Aof(), Aof() }, out) && (out.x != 0 || out.y != 0); }
bool NthCarNode(const V3& at, int n, V3& out, float& heading)
{
    std::vector<A> a = { Af(at.x), Af(at.y), Af(at.z), Ai(n), Aof(), Aof(), Aof(), Aof() };
    if(!Cmd(0x06F8, a, nullptr, true)) return false;
    out = Mk(a[4].f, a[5].f, a[6].f); heading = a[7].f;
    return out.x != 0 || out.y != 0;
}
void Doors(const V3& near, float radius, std::vector<Door>& out)
{
    out.clear();
    uintptr_t pp = g_entryPool ? *(uintptr_t*)g_entryPool : 0;
    if(!pp) return;
    uintptr_t objs = F<uintptr_t>(pp, 0), flags = F<uintptr_t>(pp, 8); int size = F<int>(pp, 0x10);
    if(!objs || !flags || size <= 0 || size > 2000) return;
    for(int i = 0; i < size; ++i)
    {
        if(F<uint8_t>(flags, i) & 0x80) continue;
        uintptr_t e = objs + (uintptr_t)i * 0x40;
        float l = F<float>(e, 8), t = F<float>(e, 12), r = F<float>(e, 16), b = F<float>(e, 20);   // CRect: left, top, right, bottom
        V3 c = Mk((l + r) * 0.5f, (t + b) * 0.5f, F<float>(e, 24));
        if(Dist2(c, near) > radius) continue;
        Door d; d.at = c; d.rot = F<float>(e, 28);
        uintptr_t link = F<uintptr_t>(e, 56);
        d.inside = link ? Mk(F<float>(link, 32), F<float>(link, 36), F<float>(link, 40)) : c;
        d.area = link ? F<uint8_t>(link, 50) : 0;
        memcpy(d.title, (void*)e, 8); d.title[8] = 0;
        out.push_back(d);
    }
}

// Кого держит в захвате игрок: человек — он сам; машина — её водитель
uintptr_t PlayerTargetPed()
{
    uintptr_t p = Player(); if(!p) return 0;
    uintptr_t t = F<uintptr_t>(p, kPedLockOn);
    if(!t) return 0;
    int type = F<uint8_t>(t, kEntType) & 7;
    if(type == 3) return t;
    if(type == 2) return VehDriver(t);
    return 0;
}
uintptr_t PlayerTarget()
{
    uintptr_t p = Player(); if(!p) return 0;
    uintptr_t t = F<uintptr_t>(p, kPedLockOn);
    if(!t || (F<uint8_t>(t, kEntType) & 7) != 3) return 0;
    return t;
}
bool PlayerWaypoint(V3& out)
{
    if(!g_radarTrace) return false;
    for(int i = 0; i < 250; ++i)
    {
        uintptr_t t = g_radarTrace + (uintptr_t)i * 0x30;
        uint16_t bits; memcpy(&bits, (void*)(t + 0x29), 2);
        if(!(bits & 2) || F<uint8_t>(t, 0x28) != 41) continue;
        memcpy(&out, (void*)(t + 0x08), sizeof(V3));
        out.z = GroundZ(out.x, out.y, 200.0f);
        return true;
    }
    return false;
}
bool IsCopType(int t) { return t == 6; }
}
