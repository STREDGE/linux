#include "game.h"
#include <set>

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

#include "util/log.h"
#include "trace/trace.h"

namespace game
{
namespace
{
    template<typename T> T& F(uintptr_t base, uintptr_t off) { return *(T*)(base + off); }

    // ---- смещения (arm64, 2.10) ----
    constexpr uintptr_t kPlayerInfoSize = 0x1D8;
    constexpr uintptr_t kPiMoney        = 0xF0;    // CPlayerInfo::m_nMoney (CPlayerInfo::Process: деньги/показ денег)
    constexpr uintptr_t kPiDisplayMoney = 0xF4;
    constexpr uintptr_t kEntMatrix      = 0x18;
    constexpr uintptr_t kEntPlacePos    = 0x08;
    constexpr uintptr_t kEntHeading     = 0x14;
    constexpr uintptr_t kEntModel       = 0x32;    // int16
    constexpr uintptr_t kEntTypeByte    = 0x5A;    // &7: 2 машина, 3 пед
    constexpr uintptr_t kPedIntel       = 0x538;   // CPedIntelligence*; +8 — CTaskManager
    constexpr uintptr_t kPedCreatedBy   = 0x548;
    constexpr uintptr_t kPedState       = 0x54C;
    constexpr uintptr_t kPedFlags585    = 0x585;
    constexpr uintptr_t kPedHealth      = 0x6AC;
    constexpr uintptr_t kPedRotCur      = 0x6C4;
    constexpr uintptr_t kPedRotAim      = 0x6C8;
    constexpr uintptr_t kPedVehicle     = 0x708;
    constexpr uintptr_t kPedType        = 0x720;
    constexpr uintptr_t kPedStride      = 0x9C8;
    constexpr uintptr_t kWantedLevel    = 0x2C;    // CWanted::m_nWantedLevel
    constexpr uintptr_t kPadDisable     = 0x110;
    constexpr int       kSlotPrimary    = 3;       // TASK_PRIMARY_PRIMARY
    constexpr int       kIfpFirst       = 25575;   // номер потока набора анимаций 0

    bool g_ok = false;
    uintptr_t g_base = 0;
    uintptr_t g_radarTrace = 0;

    uintptr_t g_players = 0, g_focus = 0, g_pedPool = 0, g_vehPool = 0, g_timeMs = 0, g_userPause = 0, g_codePause = 0;
    uintptr_t g_camera = 0;
    int (*GetVehRefFn)(uintptr_t) = nullptr;
    uintptr_t g_cutscene = 0, g_clockH = 0, g_clockM = 0, g_weatherNew = 0, g_zoneName = 0, g_animBlocks = 0;

    typedef void* (*TaskNew_t)(unsigned long);
    typedef void  (*SetTask_t)(void* mgr, void* task, int slot, bool);
    typedef void* (*FindWanted_t)(int);
    typedef void  (*SetWanted_t)(void* wanted, int);
    typedef int   (*GetPedRef_t)(uintptr_t);
    typedef uintptr_t (*GetPed_t)(int);
    typedef void* (*GetPad_t)(int);
    typedef void  (*GiveWeapon_t)(uintptr_t, int, unsigned, bool);
    typedef void  (*SetCurWeapon_t)(uintptr_t, int);
    typedef void  (*ClearWeapons_t)(uintptr_t);
    typedef uintptr_t (*AddPed_t)(int, unsigned, const Vec&, bool);
    typedef void  (*RemovePed_t)(uintptr_t);
    typedef void  (*RequestModel_t)(int, int);
    typedef void  (*LoadAll_t)(bool);
    typedef void  (*SetCreatedBy_t)(uintptr_t, uint8_t);
    typedef int   (*AnimBlockIndex_t)(const char*);
    typedef bool  (*OnScreen_t)(uintptr_t);
    typedef bool  (*CalcScreen_t)(const Vec&, Vec*, float*, float*, bool, bool);
    typedef float (*GroundZ_t)(float, float, float, bool*, void**);
    typedef bool  (*OnMission_t)();
    CalcScreen_t CalcScreen; GroundZ_t GroundZFn; OnMission_t OnMissionFn;
    typedef void* (*GetAnimBlock_t)(const char*);
    typedef void* (*GetAnim_t)(const char*, const void*);
    GetAnimBlock_t GetAnimBlock; GetAnim_t GetAnim;
    typedef void (*Teleport_t)(uintptr_t, Vec, unsigned char);
    Teleport_t TeleportFn;

    TaskNew_t TaskNew; SetTask_t SetTask; FindWanted_t FindWanted; SetWanted_t SetWantedFn, CheatWanted;
    GetPedRef_t GetPedRef; GetPed_t GetPed; GetPad_t GetPad; GiveWeapon_t GiveWeaponFn; SetCurWeapon_t SetCurWeaponFn;
    ClearWeapons_t ClearWeaponsFn; AddPed_t AddPed; RemovePed_t RemovePed; RequestModel_t RequestModel; LoadAll_t LoadAll;
    SetCreatedBy_t SetCreatedBy; AnimBlockIndex_t AnimBlockIndex; OnScreen_t OnScreenFn;
    void (*SetDecisionMaker)(uintptr_t, int) = nullptr;   // CPedIntelligence::SetPedDecisionMakerType
    uintptr_t (*GetSimplest)(void*) = nullptr;            // CTaskManager::GetSimplestActiveTask
    int  (*ComputeDoorPass)(uintptr_t, int) = nullptr;    // CCarEnterExit::ComputeTargetDoorToEnterAsPassenger
    void (*LoadSceneFn)(const Vec&) = nullptr;            // CStreaming::LoadScene

    // конструкторы задач
    void* ctorGoTo; void* ctorFollow; void* ctorFlee; void* ctorKill; void* ctorStand; void* ctorTurn;;
    void* ctorEnterDriver; void* ctorEnterPass; void* ctorLeave; void* ctorDrive; void* ctorPhone; void* ctorArrest;
    void* ctorNamedAnim; void* ctorSeekAim;

    struct Pool { uintptr_t objs, flags; int size; };
    bool GetPool(uintptr_t sym, Pool& p)
    {
        uintptr_t pp = sym ? *(uintptr_t*)sym : 0;
        if(!pp) return false;
        p.objs = F<uintptr_t>(pp, 0); p.flags = F<uintptr_t>(pp, 8); p.size = F<int>(pp, 0x10);
        return p.objs && p.flags && p.size > 0 && p.size <= 8192;
    }

    uintptr_t Sym(void* h, const char* s, bool required = true)
    {
        uintptr_t a = aml->GetSym(h, s);
        if(!a && required) LOG_ERROR("Игра: нет символа %s", s);
        return a;
    }

    void* Mgr(uintptr_t ped)
    {
        uintptr_t intel = ped ? F<uintptr_t>(ped, kPedIntel) : 0;
        return intel ? (void*)(intel + 8) : nullptr;
    }

    bool AssignN(uintptr_t ped, void* task, const char* what)
    {
        void* mgr = Mgr(ped);
        if(!mgr || !task) return false;
        TRACE("ЗАДАЧА", "P#%d m=%d ← %s [мод: %s]", Ref(ped), Model(ped), what, g_where ? g_where : "?");
        SetTask(mgr, task, kSlotPrimary, false);
        return true;
    }
    #define Assign(ped, task) AssignN(ped, task, __func__)

    void* NewTask() { return TaskNew ? TaskNew(0x80) : nullptr; }   // все задачи берутся из пула задач (запись 0x80)
}

float Dist(const Vec& a, const Vec& b) { float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return sqrtf(dx * dx + dy * dy + dz * dz); }
float Dist2D(const Vec& a, const Vec& b) { float dx = a.x - b.x, dy = a.y - b.y; return sqrtf(dx * dx + dy * dy); }

// Особые персонажи миссий: игра грузит их в слоты 290–299 по имени
static char g_specialName[10][16];
SDECL_HOOKv(RequestSpecialCharH, int slot, const char* name, int flags)
{
    if(slot >= 0 && slot < 10 && name) { strncpy(g_specialName[slot], name, 15); g_specialName[slot][15] = 0; for(char* c = g_specialName[slot]; *c; ++c) *c = (char)tolower((unsigned char)*c); }
    RequestSpecialCharH(slot, name, flags);
}

std::string StoryKey(uintptr_t ped)
{
    if(!ped) return "";
    int m = Model(ped);
    switch(m)
    {
        case 265: return "tenpenny"; case 266: return "pulaski"; case 267: return "hernandez"; case 268: return "dwaine";
        case 269: return "smoke"; case 270: return "sweet"; case 271: return "ryder"; case 272: return "forelli";
    }
    if(m >= 290 && m <= 299 && g_specialName[m - 290][0]) return g_specialName[m - 290];
    return "";
}

bool Init(void* h)
{
    if(void* rs = (void*)aml->GetSym(h, "_ZN10CStreaming18RequestSpecialCharEiPKci")) aml->Hook(rs, (void*)HookOf_RequestSpecialCharH, (void**)&RequestSpecialCharH);

    g_base = aml->GetLib("libGTASA.so");
    g_players   = Sym(h, "_ZN6CWorld7PlayersE");
    g_focus     = Sym(h, "_ZN6CWorld13PlayerInFocusE");
    g_pedPool   = Sym(h, "_ZN6CPools11ms_pPedPoolE");
    g_vehPool   = Sym(h, "_ZN6CPools15ms_pVehiclePoolE");
    g_timeMs    = Sym(h, "_ZN6CTimer22m_snTimeInMillisecondsE");
    g_userPause = Sym(h, "_ZN6CTimer11m_UserPauseE");
    g_codePause = Sym(h, "_ZN6CTimer11m_CodePauseE", false);
    g_cutscene  = Sym(h, "_ZN12CCutsceneMgr10ms_runningE", false);
    g_camera    = Sym(h, "TheCamera", false);
    GetVehRefFn = (int(*)(uintptr_t))Sym(h, "_ZN6CPools13GetVehicleRefEP8CVehicle", false);
    g_clockH    = Sym(h, "_ZN6CClock18ms_nGameClockHoursE", false);
    g_clockM    = Sym(h, "_ZN6CClock20ms_nGameClockMinutesE", false);
    g_weatherNew= Sym(h, "_ZN8CWeather14NewWeatherTypeE", false);
    g_zoneName  = Sym(h, "_ZN4CHud11m_pZoneNameE", false);
    g_animBlocks= Sym(h, "_ZN12CAnimManager14ms_aAnimBlocksE", false);
    g_radarTrace= Sym(h, "_ZN6CRadar13ms_RadarTraceE", false);

    TaskNew      = (TaskNew_t)Sym(h, "_ZN5CTasknwEm");
    SetTask      = (SetTask_t)Sym(h, "_ZN12CTaskManager7SetTaskEP5CTaskib");
    FindWanted   = (FindWanted_t)Sym(h, "_Z16FindPlayerWantedi");
    SetWantedFn  = (SetWanted_t)Sym(h, "_ZN7CWanted14SetWantedLevelEi");
    CheatWanted  = (SetWanted_t)Sym(h, "_ZN7CWanted16CheatWantedLevelEi", false);
    GetPedRef    = (GetPedRef_t)Sym(h, "_ZN6CPools9GetPedRefEP4CPed");
    GetPed       = (GetPed_t)Sym(h, "_ZN6CPools6GetPedEi");
    GetPad       = (GetPad_t)Sym(h, "_ZN4CPad6GetPadEi");
    GiveWeaponFn = (GiveWeapon_t)Sym(h, "_ZN4CPed10GiveWeaponE11eWeaponTypejb");
    SetCurWeaponFn = (SetCurWeapon_t)Sym(h, "_ZN4CPed16SetCurrentWeaponE11eWeaponType");
    ClearWeaponsFn = (ClearWeapons_t)Sym(h, "_ZN4CPed12ClearWeaponsEv");
    AddPed       = (AddPed_t)Sym(h, "_ZN11CPopulation6AddPedE8ePedTypejRK7CVectorb");
    RemovePed    = (RemovePed_t)Sym(h, "_ZN11CPopulation9RemovePedEP4CPed");
    RequestModel = (RequestModel_t)Sym(h, "_ZN10CStreaming12RequestModelEii");
    LoadAll      = (LoadAll_t)Sym(h, "_ZN10CStreaming22LoadAllRequestedModelsEb");
    SetCreatedBy = (SetCreatedBy_t)Sym(h, "_ZN4CPed16SetCharCreatedByEh");
    SetDecisionMaker = (void(*)(uintptr_t, int))Sym(h, "_ZN16CPedIntelligence23SetPedDecisionMakerTypeEi", false);
    ComputeDoorPass = (int(*)(uintptr_t, int))Sym(h, "_ZN13CCarEnterExit35ComputeTargetDoorToEnterAsPassengerERK8CVehiclei", false);
    GetSimplest = (uintptr_t(*)(void*))Sym(h, "_ZNK12CTaskManager21GetSimplestActiveTaskEv", false);
    LoadSceneFn = (void(*)(const Vec&))Sym(h, "_ZN10CStreaming9LoadSceneERK7CVector", false);
    AnimBlockIndex = (AnimBlockIndex_t)Sym(h, "_ZN12CAnimManager22GetAnimationBlockIndexEPKc", false);
    OnScreenFn   = (OnScreen_t)Sym(h, "_ZN7CEntity13GetIsOnScreenEv", false);
    CalcScreen   = (CalcScreen_t)Sym(h, "_ZN7CSprite15CalcScreenCoorsERK5RwV3dPS0_PfS4_bb", false);
    GroundZFn    = (GroundZ_t)Sym(h, "_ZN6CWorld21FindGroundZFor3DCoordEfffPbPP7CEntity", false);
    OnMissionFn  = (OnMission_t)Sym(h, "_ZN11CTheScripts18IsPlayerOnAMissionEv", false);
    TeleportFn   = (Teleport_t)Sym(h, "_ZN4CPed8TeleportE7CVectorh", false);
    GetAnimBlock = (GetAnimBlock_t)Sym(h, "_ZN12CAnimManager17GetAnimationBlockEPKc", false);
    GetAnim      = (GetAnim_t)Sym(h, "_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock", false);

    ctorGoTo   = (void*)Sym(h, "_ZN34CTaskComplexGoToPointAndStandStillC2EiRK7CVectorffbb");
    ctorFollow = (void*)Sym(h, "_ZN30CTaskComplexFollowPedFootstepsC2EP4CPed");
    ctorFlee   = (void*)Sym(h, "_ZN27CTaskComplexSmartFleeEntityC2EP7CEntitybfiif");
    ctorKill   = (void*)Sym(h, "_ZN25CTaskComplexKillPedOnFootC2EP4CPedijjji");
    ctorStand  = (void*)Sym(h, "_ZN21CTaskSimpleStandStillC2Eibbf");
    ctorTurn   = (void*)Sym(h, "_ZN35CTaskComplexTurnToFaceEntityOrCoordC2EP7CEntityff");
    ctorEnterDriver = (void*)Sym(h, "_ZN28CTaskComplexEnterCarAsDriverC2EP8CVehicle");
    ctorEnterPass   = (void*)Sym(h, "_ZN31CTaskComplexEnterCarAsPassengerC2EP8CVehicleib");
    ctorLeave  = (void*)Sym(h, "_ZN20CTaskComplexLeaveCarC2EP8CVehicleiibb");
    ctorDrive  = (void*)Sym(h, "_ZN24CTaskComplexDriveToPointC2EP8CVehicleRK7CVectorfiifi", false);
    ctorPhone  = (void*)Sym(h, "_ZN26CTaskComplexUseMobilePhoneC2Ei", false);
    ctorArrest = (void*)Sym(h, "_ZN21CTaskComplexArrestPedC2EP4CPed", false);
    ctorNamedAnim = (void*)Sym(h, "_ZN23CTaskSimpleRunNamedAnimC2EPKcS1_ifibbbb");
    ctorSeekAim = (void*)Sym(h, "_ZN28CTaskComplexSeekEntityAimingC2EP7CEntityff", false);

    g_ok = g_players && g_pedPool && TaskNew && SetTask && GetPedRef && GetPed && ctorGoTo && ctorStand && ctorNamedAnim;
    LOG_INFO("Игра: %s", g_ok ? "все основные функции найдены" : "НЕ ВСЕ функции найдены — часть действий педов не будет работать");
    return g_ok;
}

bool Ready() { return g_ok; }

// ---------------- игрок ----------------
static uintptr_t PlayerInfo()
{
    if(!g_players) return 0;
    int f = g_focus ? *(uint8_t*)g_focus : 0;
    if(f > 1) f = 0;
    return g_players + (uintptr_t)f * kPlayerInfoSize;
}

uintptr_t Player() { uintptr_t pi = PlayerInfo(); return pi ? F<uintptr_t>(pi, 0) : 0; }
uintptr_t Player2()
{
    if(!g_players) return 0;
    int focus = g_focus ? *(uint8_t*)g_focus : 0;
    uintptr_t p = F<uintptr_t>(g_players + (focus ? 0 : 1) * kPlayerInfoSize, 0);
    return p && p != Player() ? p : 0;
}
float MaxHealth(uintptr_t ped) { if(!ped) return 100; float m = F<float>(ped, kPedHealth + 4); return (m >= 50 && m <= 400) ? m : 100; }
float Armour(uintptr_t ped) { return ped ? F<float>(ped, kPedHealth + 8) : 0; }

// Метка на карте: CRadar::ms_RadarTrace — 250 записей по 0x30; значок (+0x28) 41 = метка игрока, занято — бит 1 в +0x29
bool Waypoint(Vec& out)
{
    if(!g_radarTrace) return false;
    for(int i = 0; i < 250; ++i)
    {
        uintptr_t t = g_radarTrace + (uintptr_t)i * 0x30;
        uint16_t bits; memcpy(&bits, (void*)(t + 0x29), 2);
        if(!(bits & 2) || F<uint8_t>(t, 0x28) != 41) continue;
        out = F<Vec>(t, 0x08);
        return true;
    }
    return false;
}
int  Money() { uintptr_t pi = PlayerInfo(); return pi ? F<int>(pi, kPiMoney) : 0; }
void AddMoney(int d)
{
    uintptr_t pi = PlayerInfo(); if(!pi) return;
    int m = F<int>(pi, kPiMoney) + d; if(m < 0) m = 0;
    F<int>(pi, kPiMoney) = m;
}
int  Wanted() { void* w = FindWanted ? FindWanted(-1) : nullptr; return w ? F<int>((uintptr_t)w, kWantedLevel) : 0; }
void SetWanted(int s)
{
    void* w = FindWanted ? FindWanted(-1) : nullptr; if(!w) return;
    if(s < 0) s = 0; if(s > 6) s = 6;
    TRACE("МОД", "SetWanted %d (было %d) [мод: %s]", s, Wanted(), g_where ? g_where : "?");
    if(s < Wanted() && CheatWanted) CheatWanted(w, s); else SetWantedFn(w, s);
}
bool PlayerInVehicle() { uintptr_t p = Player(); return p && (F<uint8_t>(p, kPedFlags585) & 1) && F<uintptr_t>(p, kPedVehicle); }
uintptr_t PlayerVehicle() { return PlayerInVehicle() ? F<uintptr_t>(Player(), kPedVehicle) : 0; }
// ---------------- для подробного лога ----------------
float Speed(uintptr_t e) { if(!e) return 0; float x = F<float>(e, 0x68), y = F<float>(e, 0x6C), z = F<float>(e, 0x70); return sqrtf(x * x + y * y + z * z) * 50.0f; }
float VehHealth(uintptr_t v) { return v ? F<float>(v, 0x634) : 0; }
int   VehMaxPassengers(uintptr_t v) { return v ? (int)F<uint8_t>(v, 0x5EC) : 0; }
int   AreaCode(uintptr_t e) { return e ? (int)F<uint8_t>(e, 0x4B) : 0; }
int   PedState(uintptr_t p) { return p ? F<int>(p, kPedState) : -1; }
bool  CurWeapon(uintptr_t p, int& type, int& ammo)
{
    if(!p) return false;
    int slot = F<int8_t>(p, 0x8DC);
    if(slot < 0 || slot > 12) return false;
    uintptr_t w = p + 0x730 + (uintptr_t)slot * 0x20;          // CPed::m_WeaponSlots[13], CWeapon 0x20
    type = F<int>(w, 0); ammo = F<int>(w, 0xC);
    return true;
}
bool  PadLocked() { void* pad = GetPad ? GetPad(0) : nullptr; return pad && F<uint16_t>((uintptr_t)pad, kPadDisable) != 0; }
bool  CamFading() { return g_camera && F<uint8_t>(g_camera, 0x57) != 0; }     // CCamera::m_bFading
void  PoolUse(int& peds, int& pedMax, int& cars, int& carMax)
{
    peds = pedMax = cars = carMax = 0;
    Pool p;
    if(GetPool(g_pedPool, p)) { pedMax = p.size; for(int i = 0; i < p.size; ++i) if(!(F<uint8_t>(p.flags, i) & 0x80)) ++peds; }
    if(GetPool(g_vehPool, p)) { carMax = p.size; for(int i = 0; i < p.size; ++i) if(!(F<uint8_t>(p.flags, i) & 0x80)) ++cars; }
}
int   VehRef(uintptr_t v) { return v && GetVehRefFn ? GetVehRefFn(v) : 0; }
int   SimplestTaskType(uintptr_t ped)
{
    void* m = Mgr(ped); if(!m || !GetSimplest) return -1;
    uintptr_t t = GetSimplest(m); if(!t) return -1;
    uintptr_t vt = F<uintptr_t>(t, 0); if(!vt) return -1;
    typedef int (*GetType_t)(uintptr_t);
    GetType_t fn = F<GetType_t>(vt, 0x28);
    return fn ? fn(t) : -1;
}
uintptr_t LockOn(uintptr_t ped) { return ped ? F<uintptr_t>(ped, 0x8E0) : 0; }                 // CPed::m_pEntLockOnTarget
// Кого пед сейчас бьёт / убивает / держит на прицеле. У педов захват прицела (+0x8E0) пустой — цель лежит
// в его задачах. Идём от активной задачи (первый непустой слот: физ. ответ → ответ на событие → основная →
// по умолчанию) вниз по подзадачам и берём цель из первой «боевой».
uintptr_t FightTarget(uintptr_t ped)
{
    void* m = Mgr(ped); if(!m) return 0;
    uintptr_t t = 0;
    for(int s = 0; s < 5 && !t; ++s) t = F<uintptr_t>((uintptr_t)m, s * 8);
    typedef int (*GetType_t)(uintptr_t);
    typedef uintptr_t (*GetSub_t)(uintptr_t);
    for(int depth = 0; t && depth < 12; ++depth)
    {
        uintptr_t vt = F<uintptr_t>(t, 0); if(!vt) return 0;
        GetType_t gt = F<GetType_t>(vt, 0x28);                 // GetTaskType (слот 5)
        int type = gt ? gt(t) : -1;
        int off = 0;
        switch(type)
        {
            case 1000: case 1028: off = 0x20; break;            // KILL_PED_ON_FOOT(_STEALTH)::m_pTargetPed
            case 1001: case 1002: off = 0x18; break;            // KILL_PED_ON_FOOT_MELEE / _ARMED
            case 1016:            off = 0x20; break;            // SIMPLE_FIGHT::m_pTargetEntity
            case 1017:            off = 0x28; break;            // SIMPLE_USE_GUN::m_pTargetEntity
            case 1019:            off = 0x18; break;            // SIMPLE_FIGHT_CTRL::m_pTargetEntity
            case 933:             off = 0x18; break;            // SEEK_ENTITY_AIMING (наш «держать на прицеле»)
        }
        if(off) { uintptr_t e = F<uintptr_t>(t, off); if(e) return e; }
        GetSub_t gs = F<GetSub_t>(vt, 0x18);                    // GetSubTask (слот 3; у простых задач — 0)
        t = gs ? gs(t) : 0;
    }
    return 0;
}
bool IsPedEntity(uintptr_t e) { return e && (F<uint8_t>(e, kEntTypeByte) & 7) == 3; }
uintptr_t LastDamager(uintptr_t ped, int& weapon, uint32_t& timeMs)
{
    if(!ped) return 0;
    weapon = F<int8_t>(ped, 0x93C); timeMs = F<uint32_t>(ped, 0x948);                          // LastDamagedWeaponType / LastDamagedTime
    return F<uintptr_t>(ped, 0x940);                                                           // pLastDamageEntity
}
std::string EntName(uintptr_t e)
{
    if(!e) return "-";
    int type = F<uint8_t>(e, kEntTypeByte) & 7;
    char b[64];
    if(type == 3) { if(e == Player()) return "игрок"; if(e == Player2()) return "друг"; snprintf(b, sizeof(b), "P#%d", Ref(e)); return b; }
    if(type == 2) { snprintf(b, sizeof(b), "V#%d", VehRef(e)); return b; }
    snprintf(b, sizeof(b), "объект(тип %d, модель %d)", type, Model(e)); return b;
}
bool  Camera(Vec& pos, Vec& front)
{
    if(!g_camera) return false;
    uintptr_t m = g_camera + 0x970;                                                            // CCamera::m_cameraMatrix
    front = { F<float>(m, 0x10), F<float>(m, 0x14), F<float>(m, 0x18) };
    pos   = { F<float>(m, 0x30), F<float>(m, 0x34), F<float>(m, 0x38) };
    return true;
}

void LockPlayer(bool lock)
{
    void* pad = GetPad ? GetPad(0) : nullptr; if(!pad) return;
    TRACE("МОД", "LockPlayer %d [мод: %s]", lock, g_where ? g_where : "?");
    F<uint16_t>((uintptr_t)pad, kPadDisable) = lock ? 1 : 0;
}

// ---------------- педы ----------------
int Ref(uintptr_t ped) { return ped && GetPedRef ? GetPedRef(ped) : 0; }
uintptr_t FromRef(int ref) { return ref && GetPed ? GetPed(ref) : 0; }

void ForEachPed(const std::function<void(uintptr_t)>& fn)
{
    Pool p; if(!GetPool(g_pedPool, p)) return;
    for(int i = 0; i < p.size; ++i)
    {
        if(F<uint8_t>(p.flags, i) & 0x80) continue;
        fn(p.objs + (uintptr_t)i * kPedStride);
    }
}

Vec Pos(uintptr_t e)
{
    Vec v; if(!e) return v;
    uintptr_t m = F<uintptr_t>(e, kEntMatrix);
    const float* f = m ? (const float*)(m + 0x30) : (const float*)(e + kEntPlacePos);
    v.x = f[0]; v.y = f[1]; v.z = f[2]; return v;
}
float Heading(uintptr_t e)
{
    uintptr_t m = F<uintptr_t>(e, kEntMatrix);
    if(m) { const float* fw = (const float*)(m + 0x10); return atan2f(-fw[0], fw[1]); }
    return F<float>(e, kEntHeading);
}
int   Model(uintptr_t e) { return e ? F<int16_t>(e, kEntModel) : -1; }
int   PedType(uintptr_t p) { return p ? F<int>(p, kPedType) : -1; }
float Health(uintptr_t p) { return p ? F<float>(p, kPedHealth) : 0; }
bool  Alive(uintptr_t p) { if(!p) return false; int s = F<int>(p, kPedState); return s != 54 && s != 55 && Health(p) > 0; }
bool  IsPlayer(uintptr_t p) { int t = PedType(p); return t == PT_PLAYER1 || t == PT_PLAYER2; }
uintptr_t Vehicle(uintptr_t p) { return p && (F<uint8_t>(p, kPedFlags585) & 1) ? F<uintptr_t>(p, kPedVehicle) : 0; }
bool  OnScreen(uintptr_t e) { return e && OnScreenFn ? OnScreenFn(e) : true; }
void  SetHeading(uintptr_t p, float h) { if(!p) return; F<float>(p, kPedRotCur) = h; F<float>(p, kPedRotAim) = h; }
void  Teleport(uintptr_t p, const Vec& to) { if(p && TeleportFn) TeleportFn(p, to, 0); }

uintptr_t NearestPed(const Vec& at, float radius, const std::function<bool(uintptr_t)>& filter)
{
    uintptr_t best = 0; float bd = radius;
    ForEachPed([&](uintptr_t ped) {
        if(!Alive(ped)) return;
        float d = Dist(Pos(ped), at);
        if(d < bd && (!filter || filter(ped))) { bd = d; best = ped; }
    });
    return best;
}

uintptr_t VehDriver(uintptr_t veh) { return veh ? F<uintptr_t>(veh, 0x5A0) : 0; }            // CVehicle::pDriver
int VehFreeSeats(uintptr_t veh) { return veh ? (int)F<uint8_t>(veh, 0x5EC) - (int)F<uint8_t>(veh, 0x5E8) : 0; }   // m_nMaxPassengers - m_nNumPassengers
bool IsPoliceCar(int m) { return m == 596 || m == 597 || m == 598 || m == 599 || m == 427 || m == 490 || m == 528 || m == 601; }   // не мотоцикл (523) и не вертолёт

uintptr_t NearestPoliceCar(const Vec& at, float radius)
{
    Pool p; if(!GetPool(g_vehPool, p)) return 0;
    uintptr_t best = 0; float bd = radius;
    for(int i = 0; i < p.size; ++i)
    {
        if(F<uint8_t>(p.flags, i) & 0x80) continue;
        uintptr_t v = p.objs + (uintptr_t)i * 0xC68;
        if(!IsPoliceCar(Model(v))) continue;
        uintptr_t d = VehDriver(v);
        if(d && PedType(d) != PT_COP) continue;               // за рулём не коп — не наша машина
        if(VehFreeSeats(v) < (d ? 2 : 1)) continue;           // нужно место задержанному (и копу, если водитель уже сидит)
        float dist = Dist(Pos(v), at);
        if(dist < bd) { bd = dist; best = v; }
    }
    return best;
}

void ForEachVehicle(const std::function<void(uintptr_t veh)>& fn)
{
    Pool p; if(!GetPool(g_vehPool, p)) return;
    for(int i = 0; i < p.size; ++i)
    {
        if(F<uint8_t>(p.flags, i) & 0x80) continue;
        fn(p.objs + (uintptr_t)i * 0xC68);
    }
}

uintptr_t NearestVehicle(const Vec& at, float radius)
{
    Pool p; if(!GetPool(g_vehPool, p)) return 0;
    uintptr_t best = 0; float bd = radius;
    for(int i = 0; i < p.size; ++i)
    {
        if(F<uint8_t>(p.flags, i) & 0x80) continue;
        uintptr_t v = p.objs + (uintptr_t)i * 0xC68;
        float d = Dist(Pos(v), at);
        if(d < bd) { bd = d; best = v; }
    }
    return best;
}

// ---------------- задачи ----------------
bool TaskGoTo(uintptr_t ped, const Vec& to, int ms, float radius)
{
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, int, const Vec&, float, float, bool, bool))ctorGoTo)(t, ms, to, radius, 2.0f, false, false);
    return Assign(ped, t);
}
bool TaskFollow(uintptr_t ped, uintptr_t target)
{
    if(!ctorFollow || !target) return false;
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, uintptr_t))ctorFollow)(t, target);
    return Assign(ped, t);
}
bool TaskFlee(uintptr_t ped, uintptr_t from, float safe, int ms)
{
    if(!ctorFlee || !from) return false;
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, uintptr_t, bool, float, int, int, float))ctorFlee)(t, from, false, safe, ms, 1000, 1.0f);
    return Assign(ped, t);
}
bool TaskAttack(uintptr_t ped, uintptr_t target)
{
    if(!ctorKill || !target) return false;
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, uintptr_t, int, unsigned, unsigned, unsigned, int))ctorKill)(t, target, 0, 0, 0, 0, 4);
    return Assign(ped, t);
}
bool TaskStand(uintptr_t ped, int ms)
{
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, int, bool, bool, float))ctorStand)(t, ms, false, false, 8.0f);   // bForever=false: иначе ms не действует и пед стоит вечно
    return Assign(ped, t);
}
bool TaskTurnTo(uintptr_t ped, uintptr_t target)
{
    if(!ctorTurn || !target) return false;
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, uintptr_t, float, float))ctorTurn)(t, target, 0.5f, 0.2f);
    return Assign(ped, t);
}
// «Иди гуляй»: снимаем нашу задачу — пед возвращается к своей обычной (у прохожего — прогулка, у бандита — своя).
// CTaskComplexWander самому создавать нельзя: это незаконченная заготовка (нет GetWanderType/ScanForStuff) —
// игра падала на «Уйти» (0.2.0).
bool TaskWander(uintptr_t ped)
{
    ClearTask(ped);
    return true;
}
bool TaskEnterCar(uintptr_t ped, uintptr_t veh, bool driver, int seat)
{
    if(!veh) return false;
    void* t = NewTask(); if(!t) return false;
    if(driver) ((void(*)(void*, uintptr_t))ctorEnterDriver)(t, veh);
    else
    {   // задача ждёт НОМЕР ДВЕРИ, а не места (0 — «любая»). Номер места 1 игра принимала за дверь и падала
        // в CVehicleAnimGroup::ComputeAnimDoorOffsets (вылет при аресте, 0.2.3). Переводим место → дверь, как скрипты игры.
        int door = 0;
        if(seat >= 0 && seat < (int)F<uint8_t>(veh, 0x5EC) && ComputeDoorPass) door = ComputeDoorPass(veh, seat);
        ((void(*)(void*, uintptr_t, int, bool))ctorEnterPass)(t, veh, door, false);
    }
    return Assign(ped, t);
}
bool TaskLeaveCar(uintptr_t ped)
{
    uintptr_t veh = Vehicle(ped); if(!veh) return false;
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, uintptr_t, int, int, bool, bool))ctorLeave)(t, veh, 0, 0, true, false);
    return Assign(ped, t);
}
bool TaskDriveTo(uintptr_t ped, uintptr_t veh, const Vec& to, float speed)
{
    if(!ctorDrive || !veh) return false;
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, uintptr_t, const Vec&, float, int, int, float, int))ctorDrive)(t, veh, to, speed, 0, -1, 10.0f, 2);
    return Assign(ped, t);
}
bool TaskPhone(uintptr_t ped, int ms)
{
    if(!ctorPhone) return TaskAnim(ped, "phone_talk", "ped", true, ms);
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, int))ctorPhone)(t, ms);
    return Assign(ped, t);
}
bool TaskArrest(uintptr_t cop, uintptr_t target)
{
    if(!ctorArrest || !target) return false;
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, uintptr_t))ctorArrest)(t, target);
    return Assign(cop, t);
}
bool TaskAimAt(uintptr_t ped, uintptr_t target, int)
{
    if(!ctorSeekAim || !target) return TaskTurnTo(ped, target);
    void* t = NewTask(); if(!t) return false;
    ((void(*)(void*, uintptr_t, float, float))ctorSeekAim)(t, target, 8.0f, 2.0f);
    return Assign(ped, t);
}

bool TaskAnim(uintptr_t ped, const char* anim, const char* block, bool loop, int ms, bool holdLast)
{
    if(!ped || !anim || !block) return false;
    if(!GetAnimBlock || !GetAnim) return false;
    void* blk = GetAnimBlock(block);
    if(!blk) return false;
    if(!F<uint8_t>((uintptr_t)blk, 0x10))
    {   // набор не в памяти: просим загрузить, анимация будет в следующий раз
        if(g_animBlocks && RequestModel)
        {
            int set = (int)(((uintptr_t)blk - g_animBlocks) / 0x20);
            if(set >= 0 && set < 180) RequestModel(kIfpFirst + set, 4);
        }
        return false;
    }
    if(!GetAnim(anim, blk)) { LOG_ERROR("Анимации %s/%s нет в игре", block, anim); return false; }
    void* t = NewTask(); if(!t) return false;
    // 0x08 — по окончании плавно выйти из анимации (без него пед застывал на последнем кадре, задача не кончалась).
    // «Держать позу» (руки вверх) — тоже с таймером; holdLast у игры оставляет позу на теле навсегда.
    // Три вида:
    //  loop     — повторять ms (0x02 цикл, 0x08 плавный выход в конце);
    //  holdLast — проиграть один раз и ДЕРЖАТЬ последний кадр ms (без 0x08 игра замирает на последнем кадре,
    //             задача кончается по таймеру и плавно снимает позу). Раньше «руки вверх» шли циклом:
    //             руки поднимались и опускались по кругу;
    //  иначе    — один раз и плавно выйти.
    int flags, time;
    if(loop)          { flags = 0x10 | 0x08 | 0x02; time = ms > 0 ? ms : 4000; }
    else if(holdLast) { flags = 0x10;               time = ms > 0 ? ms : 8000; }
    else              { flags = 0x10 | 0x08;        time = -1; }
    ((void(*)(void*, const char*, const char*, int, float, int, bool, bool, bool, bool))ctorNamedAnim)
        (t, anim, block, flags, 4.0f, time, false, false, false, false);
    return Assign(ped, t);
}

void LoadScene(const Vec& at) { TRACE("МОД", "LoadScene (%.1f,%.1f,%.1f)", at.x, at.y, at.z); if(LoadSceneFn) LoadSceneFn(at); }

void ClearTask(uintptr_t ped) { void* m = Mgr(ped); if(m) { TRACE("ЗАДАЧА", "P#%d m=%d ← снять задачу [мод: %s]", Ref(ped), Model(ped), g_where ? g_where : "?"); SetTask(m, nullptr, kSlotPrimary, false); } }

int ActiveTaskType(uintptr_t ped)
{
    void* m = Mgr(ped); if(!m) return -1;
    uintptr_t t = F<uintptr_t>((uintptr_t)m, kSlotPrimary * 8);
    if(!t) return -1;
    uintptr_t vt = F<uintptr_t>(t, 0);
    if(!vt) return -1;
    typedef int (*GetType_t)(uintptr_t);
    GetType_t fn = F<GetType_t>(vt, 0x28);   // CTask::GetTaskType (5-й слот vtable)
    return fn ? fn(t) : -1;
}

// ---------------- оружие ----------------
void GiveWeapon(uintptr_t ped, int w, int ammo)
{
    if(!ped || !GiveWeaponFn) return;
    static const int kModels[] = { 0, 331, 333, 334, 335, 336, 337, 338, 339, 341, 321, 322, 323, 0, 325, 326, 342, 343, 344, 0, 0, 0,
                                   346, 347, 348, 349, 350, 351, 352, 353, 355, 356, 372, 357, 358, 359, 360, 361, 362, 363, 364, 365, 366, 367, 368, 369, 371 };
    if(w > 0 && w < (int)(sizeof(kModels) / sizeof(kModels[0])) && kModels[w]) { RequestModel(kModels[w], 2); LoadAll(false); }
    TRACE("МОД", "GiveWeapon P#%d оружие %d патроны %d", Ref(ped), w, ammo);
    GiveWeaponFn(ped, w, (unsigned)ammo, true);
}
void SetCurrentWeapon(uintptr_t ped, int w) { if(ped && SetCurWeaponFn) SetCurWeaponFn(ped, w); }
void ClearWeapons(uintptr_t ped) { if(ped && ClearWeaponsFn) ClearWeaponsFn(ped); }

// ---------------- появление ----------------
uintptr_t SpawnPed(int pedType, int model, const Vec& at)
{
    if(!AddPed || !RequestModel || !LoadAll) return 0;
    {   // пул педов почти полон — не создаём (при полном пуле игра падает в конструкторе педа)
        Pool p; int freeSlots = 0;
        if(GetPool(g_pedPool, p)) for(int i = 0; i < p.size; ++i) if(F<uint8_t>(p.flags, i) & 0x80) ++freeSlots;
        if(freeSlots < 12) { LOG_INFO("Не создаю педа: свободных мест в пуле %d", freeSlots); return 0; }
    }
    RequestModel(model, 2);
    LoadAll(false);
    uintptr_t ped = AddPed(pedType, (unsigned)model, at, true);   // true — игра ставит обычную прогулку по умолчанию (без неё пед стоял статуей)
    TRACE("МОД", "SpawnPed тип %d модель %d (%.1f,%.1f,%.1f) → P#%d [мод: %s]", pedType, model, at.x, at.y, at.z, ped ? Ref(ped) : 0, g_where ? g_where : "?");
    if(ped) KeepPed(ped, true);
    return ped;
}
void DeletePed(uintptr_t ped) { if(ped && RemovePed && !IsPlayer(ped)) { TRACE("МОД", "DeletePed P#%d", Ref(ped)); RemovePed(ped); } }
// Держать педа (игра не удаляет его, пока игрок рядом): ставим ТОЛЬКО байт «создан скриптом».
// CPed::SetCharCreatedBy(2) заодно ставил «решатель» миссий (-1) и дальность зрения миссий —
// пед переставал бояться ствола, убегать, звать своих. Чужих педов миссий не трогаем вовсе.
namespace { std::set<int> g_kept; }
bool IsScriptPed(uintptr_t ped) { return ped && F<uint8_t>(ped, kPedCreatedBy) == 2 && !g_kept.count(Ref(ped)); }
void KeepPed(uintptr_t ped, bool keep)
{
    if(!ped) return;
    int ref = Ref(ped);
    if(keep)
    {
        if(F<uint8_t>(ped, kPedCreatedBy) == 2 && !g_kept.count(ref)) return;   // пед миссии/скрипта — он не наш
        F<uint8_t>(ped, kPedCreatedBy) = 2; g_kept.insert(ref);
        TRACE("МОД", "KeepPed P#%d (держать)", ref);
    }
    else if(g_kept.count(ref)) { F<uint8_t>(ped, kPedCreatedBy) = 1; g_kept.erase(ref); TRACE("МОД", "KeepPed P#%d (отпустить игре)", ref); }
}

// ---------------- мир ----------------
int Hour()   { return g_clockH ? *(uint8_t*)g_clockH : 12; }
int Minute() { return g_clockM ? *(uint8_t*)g_clockM : 0; }
int Weather(){ return g_weatherNew ? *(int16_t*)g_weatherNew : 0; }
bool CutsceneRunning() { return g_cutscene && *(uint8_t*)g_cutscene; }
bool GamePaused() { return (g_userPause && *(uint8_t*)g_userPause) || (g_codePause && *(uint8_t*)g_codePause); }
uint32_t TimeMs() { return g_timeMs ? *(uint32_t*)g_timeMs : 0; }

std::string ZoneName()
{
    std::string out;
    if(!g_zoneName) return out;
    const uint16_t* w = *(const uint16_t**)g_zoneName;
    if(!w) return out;
    for(int i = 0; i < 48 && w[i]; ++i)
    {
        uint16_t c = w[i];
        if(c >= 0x80 && c <= 0xBF) c = 0x0410 + (c - 0x80);   // русский GXT игры: 0x80–0xBF — А…я
        if(c < 0x80) out += (char)c;
        else if(c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

bool WorldToScreen(const Vec& w, float& sx, float& sy)
{
    if(!CalcScreen) return false;
    Vec out; float cw = 0, ch = 0;
    if(!CalcScreen(w, &out, &cw, &ch, true, true)) return false;
    if(out.z <= 1.0f) return false;
    sx = out.x; sy = out.y; return true;
}

float GroundZ(float x, float y, float z)
{
    if(!GroundZFn) return z;
    bool found = false; void* ent = nullptr;
    float g = GroundZFn(x, y, z + 50.0f, &found, &ent);
    return found ? g : z;
}

bool OnMission() { return OnMissionFn && OnMissionFn(); }

uintptr_t PlayerTarget()
{
    uintptr_t p = Player(); if(!p) return 0;
    uintptr_t t = F<uintptr_t>(p, 0x8E0);       // цель захвата прицела (как читает её Helicover Brain)
    if(!t || (F<uint8_t>(t, kEntTypeByte) & 7) != 3) return 0;
    return t;
}

const char* PedTypeName(int t)
{
    switch(t)
    {
        case PT_CIVMALE: return "прохожий"; case PT_CIVFEMALE: return "прохожая"; case PT_COP: return "полицейский";
        case PT_BALLAS: return "Ballas"; case PT_GROVE: return "Grove Street Families"; case PT_VAGOS: return "Los Santos Vagos";
        case PT_RIFA: return "San Fierro Rifa"; case PT_DANANG: return "Da Nang Boys"; case PT_MAFIA: return "мафия";
        case PT_TRIAD: return "триады"; case PT_AZTECA: return "Varrios Los Aztecas"; case PT_DEALER: return "дилер";
        case PT_MEDIC: return "медик"; case PT_FIRE: return "пожарный"; case PT_CRIMINAL: return "уголовник";
        case PT_BUM: return "бездомный"; case PT_PROSTITUTE: return "девушка лёгкого поведения";
        case PT_PLAYER1: case PT_PLAYER2: return "игрок";
        default: return "человек";
    }
}
bool IsGang(int t) { return t >= PT_BALLAS && t <= PT_GANG10; }
bool IsFemaleType(int t) { return t == PT_CIVFEMALE || t == PT_PROSTITUTE; }

} // namespace game
