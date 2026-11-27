#include "game.h"

#include <mod/amlmod.h>
#include <math.h>
#include <string.h>
#include <vector>

#include "log.h"

namespace game
{
namespace
{
    template<typename T> T& F(uintptr_t p, uintptr_t off) { return *(T*)(p + off); }

    constexpr uintptr_t kPlayerInfoSize = 0x1D8;
    constexpr uintptr_t kEntMatrix = 0x18, kEntPlacePos = 0x08, kEntModel = 0x32;
    constexpr uintptr_t kPedHealth = 0x6AC;
    constexpr uintptr_t kWantedLevel = 0x2C;

    uintptr_t g_players = 0, g_focus = 0, g_timeMs = 0, g_userPause = 0, g_codePause = 0, g_cutscene = 0;
    uintptr_t g_scriptSpace = 0, g_onMissionOff = 0, g_radarTrace = 0;
    int  (*GetPedRef)(uintptr_t) = nullptr;
    void* (*FindWanted)(int) = nullptr;
    float (*GroundZFn)(float, float, float, bool*, void**) = nullptr;
    void (*ChangeBlipDisplay)(int, int) = nullptr;

    uintptr_t Sym(void* h, const char* s)
    {
        uintptr_t a = aml->GetSym(h, s);
        if(!a) LOG("ОШИБКА", "нет символа игры %s", s);
        return a;
    }

    uintptr_t PlayerInfo()
    {
        if(!g_players) return 0;
        int f = g_focus ? *(uint8_t*)g_focus : 0;
        if(f > 1) f = 0;
        return g_players + (uintptr_t)f * kPlayerInfoSize;
    }

    // ---- метки радара (CRadar::ms_RadarTrace, 250 штук по 0x30) ----
    constexpr int kTraceCount = 250, kTraceSize = 0x30;
    uintptr_t Trace(int i) { return g_radarTrace + (uintptr_t)i * kTraceSize; }
    uint16_t  TraceBits(int i) { uint16_t b; memcpy(&b, (void*)(Trace(i) + 0x29), 2); return b; }
    bool      TraceInUse(int i) { return TraceBits(i) & 0x2; }
    int       TraceType(int i) { return (TraceBits(i) >> 10) & 0xF; }
    int       TraceDisplay(int i) { return (TraceBits(i) >> 8) & 0x3; }
    uint16_t  TraceCounter(int i) { return F<uint16_t>(Trace(i), 0x14); }
    int       TraceHandle(int i) { return i | ((int)TraceCounter(i) << 16); }
    struct Hidden { int index; uint16_t counter; int display; };
    std::vector<Hidden> g_hidden;
}

float Dist(const Vec& a, const Vec& b) { float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return sqrtf(dx * dx + dy * dy + dz * dz); }
float Dist2D(const Vec& a, const Vec& b) { float dx = a.x - b.x, dy = a.y - b.y; return sqrtf(dx * dx + dy * dy); }

bool Init(void* h)
{
    g_players    = Sym(h, "_ZN6CWorld7PlayersE");
    g_focus      = Sym(h, "_ZN6CWorld13PlayerInFocusE");
    g_timeMs     = Sym(h, "_ZN6CTimer22m_snTimeInMillisecondsE");
    g_userPause  = Sym(h, "_ZN6CTimer11m_UserPauseE");
    g_codePause  = aml->GetSym(h, "_ZN6CTimer11m_CodePauseE");
    g_cutscene   = aml->GetSym(h, "_ZN12CCutsceneMgr10ms_runningE");
    g_scriptSpace= Sym(h, "_ZN11CTheScripts11ScriptSpaceE");
    g_onMissionOff = Sym(h, "_ZN11CTheScripts14OnAMissionFlagE");
    g_radarTrace = Sym(h, "_ZN6CRadar13ms_RadarTraceE");
    GetPedRef    = (int(*)(uintptr_t))Sym(h, "_ZN6CPools9GetPedRefEP4CPed");
    FindWanted   = (void*(*)(int))Sym(h, "_Z16FindPlayerWantedi");
    GroundZFn    = (float(*)(float, float, float, bool*, void**))Sym(h, "_ZN6CWorld21FindGroundZFor3DCoordEfffPbPP7CEntity");
    ChangeBlipDisplay = (void(*)(int, int))Sym(h, "_ZN6CRadar17ChangeBlipDisplayEi12eBlipDisplay");
    return g_players && g_timeMs && g_scriptSpace && GetPedRef;
}

uintptr_t Player() { uintptr_t pi = PlayerInfo(); return pi ? F<uintptr_t>(pi, 0) : 0; }
int PedRef(uintptr_t ped) { return ped && GetPedRef ? GetPedRef(ped) : -1; }
Vec Pos(uintptr_t e)
{
    Vec v; if(!e) return v;
    uintptr_t m = F<uintptr_t>(e, kEntMatrix);
    const float* f = m ? (const float*)(m + 0x30) : (const float*)(e + kEntPlacePos);
    v.x = f[0]; v.y = f[1]; v.z = f[2]; return v;
}
Vec PlayerPos() { return Pos(Player()); }
int Model(uintptr_t e) { return e ? F<int16_t>(e, kEntModel) : -1; }
float Health(uintptr_t p) { return p ? F<float>(p, kPedHealth) : 0; }
int Wanted() { void* w = FindWanted ? FindWanted(-1) : nullptr; return w ? F<int>((uintptr_t)w, kWantedLevel) : 0; }
uint32_t TimeMs() { return g_timeMs ? *(uint32_t*)g_timeMs : 0; }
bool Paused() { return (g_userPause && *(uint8_t*)g_userPause) || (g_codePause && *(uint8_t*)g_codePause); }
bool CutsceneRunning() { return g_cutscene && *(uint8_t*)g_cutscene; }
float GroundZ(float x, float y, float z)
{
    if(!GroundZFn) return z;
    bool found = false; void* ent = nullptr;
    float g = GroundZFn(x, y, z + 50.0f, &found, &ent);
    return found ? g : z;
}

int32_t* Global(int offset) { return g_scriptSpace && offset > 0 ? (int32_t*)(g_scriptSpace + offset) : nullptr; }
int OnMissionVar() { return g_onMissionOff ? *(int32_t*)g_onMissionOff : 0; }
bool OnMission() { int32_t* v = Global(OnMissionVar()); return v && *v != 0; }

void HideStoryContacts()
{
    if(!g_radarTrace || !ChangeBlipDisplay) return;
    for(int i = 0; i < kTraceCount; ++i)
    {
        if(!TraceInUse(i) || TraceType(i) != 5) continue;           // 5 — «контакт» (ADD_SPRITE_BLIP_FOR_CONTACT_POINT)
        uint8_t s = F<uint8_t>(Trace(i), 0x28);
        if(s == 31 || s == 32 || s == 35 || s == 54) continue;      // недвижимость, дом-сохранение, спортзал
        int disp = TraceDisplay(i);
        if(disp == 0) continue;
        bool known = false;
        for(auto& h : g_hidden) if(h.index == i && h.counter == TraceCounter(i)) { known = true; break; }
        if(!known)
        {
            g_hidden.push_back({ i, TraceCounter(i), disp });
            LOG("СЮЖЕТ", "спрятан значок родной миссии (значок %u)", s);
        }
        ChangeBlipDisplay(TraceHandle(i), 0);
    }
}

void RestoreStoryContacts()
{
    if(!ChangeBlipDisplay) return;
    for(auto& h : g_hidden)
        if(TraceInUse(h.index) && TraceCounter(h.index) == h.counter && TraceDisplay(h.index) == 0)
            ChangeBlipDisplay(TraceHandle(h.index), h.display);
    g_hidden.clear();
}
}
