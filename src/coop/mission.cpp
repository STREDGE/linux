#include "mission.h"
#include "trace/ctrace.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <math.h>
#include <stdarg.h>
#include <string.h>

#include <vector>

#include "game/gta.h"
#include "coop/sync.h"
#include "coop/crowd.h"

using namespace gta;

namespace coop { namespace mission {
namespace
{
    FLogger*      g_log = nullptr;
    net::Session* g_session = nullptr;
    bool          g_ok = false;

    // ---------- игра ----------
    uint8_t*  g_cutLoadStatus = nullptr;  // CCutsceneMgr::ms_cutsceneLoadStatus (2 — загружена)
    uint8_t*  g_cutRunning = nullptr;     // CCutsceneMgr::ms_running
    void (*CutLoad)(const char*) = nullptr;
    void (*CutStart)() = nullptr;
    bool (*CutFinished)() = nullptr;
    void (*CutSkip)() = nullptr;
    void (*CutDelete)() = nullptr;
    uintptr_t g_theText = 0;
    void      (*LoadMissionText)(uintptr_t text, char* stem) = nullptr;
    void      (*InsertNumber)(uint16_t* src, int, int, int, int, int, int, uint16_t* dst) = nullptr;
    uintptr_t g_radarTrace = 0;
    int       (*SetCoordBlip)(int type, CVector pos, unsigned colour, int display, char* name) = nullptr;
    void      (*ChangeBlipDisplay)(int blip, int display) = nullptr;
    uint8_t*  g_scriptSpace = nullptr;
    uint32_t* g_onMissionOffset = nullptr;
    uint8_t*  g_alreadyMission = nullptr;   // CTheScripts::bAlreadyRunningAMissionScript
    void      (*CollectParameters)(uintptr_t script, int16_t n) = nullptr;
    void      (*Fade)(uintptr_t cam, float seconds, int16_t direction) = nullptr;
    void      (*MakePlayerSafe)(uintptr_t playerInfo, bool on, float radius) = nullptr;
    void      (*CameraRestore)(uintptr_t cam) = nullptr;
    void      (*WideScreenOff)(uintptr_t cam) = nullptr;
    int32_t*  g_scriptParams = nullptr;

    void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool error, const char* fmt, ...)
    {
        char buf[512];
        va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        if(g_log) { if(error) g_log->Error("%s", buf); else g_log->Info("%s", buf); }
        ctrace::FromLog(error, buf);
    }

    float Dist(const CVector& a, const CVector& b) { float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return sqrtf(dx * dx + dy * dy + dz * dz); }

    void SetVisible(uintptr_t e, bool on)
    {
        uint32_t& f = Field<uint32_t>(e, off::EntFlags);
        if(on) f |= 0x80; else f &= ~0x80u;                // CEntity::bIsVisible
    }

    bool OnMission()
    {
        return g_scriptSpace && g_onMissionOffset && *g_onMissionOffset &&
               *(int32_t*)(g_scriptSpace + *g_onMissionOffset) == 1;
    }
    bool CutsceneRunning() { return g_cutRunning && *g_cutRunning; }

    // ---------- состояние ----------
    net::Role g_role = net::Role::Off;
    bool      g_connected = false;
    uint64_t  g_now = 0;
    uintptr_t g_local = 0, g_remote = 0;
    const net::PlayerStatePacket* g_rs = nullptr;

    bool HostOn()   { return g_ok && g_connected && g_role == net::Role::Host; }
    bool ClientOn() { return g_ok && g_connected && g_role == net::Role::Client; }

    // ========== тексты миссии (хост → друг) ==========
    bool g_inForward = false;           // друг показывает присланный текст — не слать обратно
    char g_textStem[8] = "";

    bool ForwardTexts() { return HostOn() && (OnMission() || CutsceneRunning()); }

    void SendText(uint8_t kind, const uint16_t* text, uint32_t ms, uint16_t colour, uint8_t row, uint8_t flags)
    {
        if(!g_session) return;
        net::TextEvent e{};
        e.type = net::REL_TEXT; e.kind = kind; e.row = row; e.flags = flags; e.ms = ms; e.colour = colour;
        int n = 0;
        bool wide = false;
        if(text) { while(n < 400 && text[n]) { if(text[n] > 0xFF) wide = true; ++n; } }
        int maxChars = wide ? net::kTextMax / 2 : net::kTextMax;
        if(n > maxChars) { Log(false, "[МИССИЯ] текст длинный (%d букв) — другу уйдут первые %d", n, maxChars); n = maxChars; }
        e.wide = wide ? 1 : 0;
        if(wide) { memcpy(e.data, text, n * 2); e.len = (uint8_t)(n * 2); }
        else     { for(int i = 0; i < n; ++i) e.data[i] = (uint8_t)text[i]; e.len = (uint8_t)n; }
        g_session->SendReliable(&e, net::kTextHeaderSize + e.len);
    }

    // Друг: строки живут, пока игра их показывает (игра хранит указатель) — кольцо буферов
    constexpr int kRing = 24, kRingLen = 240;
    uint16_t g_ring[kRing][kRingLen];
    int      g_ringHead = 0;
    uint16_t* RingDecode(const net::TextEvent& e)
    {
        uint16_t* out = g_ring[g_ringHead]; g_ringHead = (g_ringHead + 1) % kRing;
        int n = 0;
        if(e.wide) { n = e.len / 2; if(n > kRingLen - 1) n = kRingLen - 1; memcpy(out, e.data, n * 2); }
        else       { n = e.len; if(n > kRingLen - 1) n = kRingLen - 1; for(int i = 0; i < n; ++i) out[i] = e.data[i]; }
        out[n] = 0;
        return out;
    }
    std::vector<net::TextEvent> g_textQueue;   // показать в игровом кадре
}

// ---------- перехваты текстов ----------
DECL_HOOKv(AddMessage, const char* label, uint16_t* text, uint32_t ms, uint16_t colour, bool brief)
{
    AddMessage(label, text, ms, colour, brief);
    if(!g_inForward && ForwardTexts()) SendText(net::TXT_MESSAGE, text, ms, colour, 0, 0);
}
DECL_HOOKv(AddMessageJumpQ, const char* label, uint16_t* text, uint32_t ms, uint16_t colour, bool brief)
{
    AddMessageJumpQ(label, text, ms, colour, brief);
    if(!g_inForward && ForwardTexts()) SendText(net::TXT_MESSAGE_NOW, text, ms, colour, 0, 0);
}
DECL_HOOKv(AddMessageWithNumber, const char* label, uint16_t* text, uint32_t ms, uint16_t colour, int n1, int n2, int n3, int n4, int n5, int n6, bool brief)
{
    AddMessageWithNumber(label, text, ms, colour, n1, n2, n3, n4, n5, n6, brief);
    if(!g_inForward && ForwardTexts() && InsertNumber && text)
    {
        static uint16_t buf[512]; buf[0] = 0;
        InsertNumber(text, n1, n2, n3, n4, n5, n6, buf);
        SendText(net::TXT_MESSAGE, buf, ms, colour, 0, 0);
    }
}
DECL_HOOKv(AddBigMessage, uint16_t* text, uint32_t ms, uint16_t row)
{
    AddBigMessage(text, ms, row);
    // строка 2 — «ПОТРАЧЕНО» / «АРЕСТОВАН» самого хоста: другу не показываем
    if(!g_inForward && row != 2 && HostOn() && (OnMission() || CutsceneRunning() || row <= 1))
        SendText(net::TXT_BIG, text, ms, 0, (uint8_t)row, 0);
}
DECL_HOOKv(AddBigMessageWithNumber, uint16_t* text, uint32_t ms, uint16_t row, int n1, int n2, int n3, int n4, int n5, int n6)
{
    AddBigMessageWithNumber(text, ms, row, n1, n2, n3, n4, n5, n6);
    if(!g_inForward && row != 2 && HostOn() && InsertNumber && text)
    {
        static uint16_t buf[512]; buf[0] = 0;
        InsertNumber(text, n1, n2, n3, n4, n5, n6, buf);
        SendText(net::TXT_BIG, buf, ms, 0, (uint8_t)row, 0);
    }
}
DECL_HOOKv(SetHelpMessage, const char* label, uint16_t* text, bool quick, bool forever, bool brief, unsigned cond)
{
    SetHelpMessage(label, text, quick, forever, brief, cond);
    if(g_inForward || !ForwardTexts()) return;
    if(text) SendText(net::TXT_HELP, text, 0, 0, 0, (uint8_t)((quick ? 1 : 0) | (forever ? 2 : 0)));
}
DECL_HOOKv(LoadMissionTextHook, uintptr_t self, char* stem)
{
    LoadMissionTextHook(self, stem);
    if(!stem) return;
    strncpy(g_textStem, stem, 7); g_textStem[7] = 0;
    if(HostOn() && g_session)
    {
        net::MissionTextEvent e{}; e.type = net::REL_MISSION_TEXT; memcpy(e.stem, g_textStem, 8);
        g_session->SendReliable(&e, sizeof(e));
        Log(false, "[МИССИЯ] Тексты миссии «%s» — другу тоже", g_textStem);
    }
}

namespace
{
    void ShowText(const net::TextEvent& e)
    {
        uint16_t* s = RingDecode(e);
        static const char kLabel[] = "SACOOP";
        g_inForward = true;
        switch(e.kind)
        {
            case net::TXT_MESSAGE:     AddMessage(kLabel, s, e.ms, e.colour, false); break;
            case net::TXT_MESSAGE_NOW: AddMessageJumpQ(kLabel, s, e.ms, e.colour, false); break;
            case net::TXT_BIG:         AddBigMessage(s, e.ms, e.row); break;
            case net::TXT_HELP:        SetHelpMessage(kLabel, s, e.flags & 1, (e.flags & 2) != 0, false, 0); break;
            default: break;
        }
        g_inForward = false;
    }

    // ========== катсцены ==========
    // хост
    uint16_t g_cutCounter = 0;
    bool     g_cutOpen = false;          // хост: катсцена объявлена другу, конец ещё не отправлен
    // друг
    enum class Cut { None, Pending, Loading, Playing, After };
    Cut      g_cut = Cut::None;
    uint16_t g_cutId = 0, g_cutEndedId = 0;
    bool     g_cutEndReceived = false, g_allowSkip = false, g_cutHaveEnded = false;
    char     g_cutName[9] = "", g_cutStem[9] = "";
    uint64_t g_cutSinceUs = 0, g_teleportAtUs = 0;
    CVector  g_cutFreezePos{};

    void SendCutEnd(const char* why)
    {
        if(!g_cutOpen || !g_session) return;
        g_cutOpen = false;
        net::CutsceneEvent e{}; e.type = net::REL_CUTSCENE; e.op = net::CUT_END; e.counter = g_cutCounter;
        g_session->SendReliable(&e, sizeof(e));
        Log(false, "[КАТСЦЕНА] у хоста закончилась (%s) — у друга тоже", why);
    }

    bool AfterNewer(uint16_t a, uint16_t b) { return (int16_t)(a - b) > 0; }   // a новее b
}

DECL_HOOKv(CutLoadHook, const char* name)
{
    CutLoadHook(name);
    if(!HostOn() || !g_session || !name) return;
    if(g_cutOpen) SendCutEnd("новая катсцена");
    net::CutsceneEvent e{}; e.type = net::REL_CUTSCENE; e.op = net::CUT_LOAD; e.counter = ++g_cutCounter;
    strncpy(e.name, name, 8);
    memcpy(e.textStem, g_textStem, 8);
    g_session->SendReliable(&e, sizeof(e));
    g_cutOpen = true;
    Log(false, "[КАТСЦЕНА] у хоста «%.8s» (тексты «%s») — у друга та же", name, g_textStem);
}
DECL_HOOKv(CutDeleteHook)
{
    CutDeleteHook();
    if(HostOn()) SendCutEnd("убрана");
}
DECL_HOOKv(CutSkipHook)
{
    // друг: свою копию катсцены хоста пропускает только хост
    if(g_cut == Cut::Playing && !g_allowSkip && ClientOn()) return;
    CutSkipHook();
    if(HostOn()) SendCutEnd("пропущена");
}

// ========== запуск миссий у друга — нельзя (ведёт хост) ==========
namespace { uint64_t g_blockedUs = 0, g_blockLogUs = 0; bool g_blockRecover = false; bool g_forceBlock = false; }
DECL_HOOK(int8_t, ProcessCommands1000, uintptr_t script, int cmd)
{
    if(cmd == 0x417 && (ClientOn() || g_forceBlock) && CollectParameters)
    {
        CollectParameters(script, 1);              // номер миссии — забрать, миссию не грузить
        if(net::Since(g_now, g_blockLogUs) > 5000000)
        {
            g_blockLogUs = g_now;
            Log(false, "[МИССИЯ] Друг встал на маркер миссии %d — миссии ведёт хост, не запускаю", g_scriptParams ? g_scriptParams[0] : -1);
        }
        g_blockedUs = g_now; g_blockRecover = true;
        return 0;
    }
    return ProcessCommands1000(script, cmd);
}

// ========== вербовка педов банды другом ==========
DECL_HOOKv(JoinOurGroup, uintptr_t player, uintptr_t cand)
{
    if(ClientOn() && cand && crowd::IsCopy(cand) && g_session)
    {
        uint16_t id = crowd::CopyId(cand);
        net::RecruitEvent e{ net::REL_RECRUIT, 0, id };
        g_session->SendReliable(&e, sizeof(e));
        Log(false, "[ГРУППА] Ты позвал педа #%u — у хоста он пойдёт за тобой", id);
        return;
    }
    JoinOurGroup(player, cand);
}

namespace
{
    // ========== метки ==========
    constexpr int kTraceCount = 250, kTraceSize = 0x30;
    uintptr_t Trace(int i) { return g_radarTrace + (uintptr_t)i * kTraceSize; }
    uint16_t  TraceBits(int i) { uint16_t b; memcpy(&b, (void*)(Trace(i) + 0x29), 2); return b; }
    bool      TraceInUse(int i) { return TraceBits(i) & 0x2; }
    int       TraceType(int i) { return (TraceBits(i) >> 10) & 0xF; }
    int       TraceDisplay(int i) { return (TraceBits(i) >> 8) & 0x3; }
    uint16_t  TraceCounter(int i) { return Field<uint16_t>(Trace(i), 0x14); }
    int       TraceHandle(int i) { return i | ((int)TraceCounter(i) << 16); }

    // Значки тех, кто даёт миссии (у друга прячем на время коопа)
    // Маркер миссии = метка «контакт» (тип 5; ADD_SPRITE_BLIP_FOR_CONTACT_POINT 02A7/0570 в mainV1.scm).
    // Кроме таких же меток домов: 31/32 — недвижимость, 35 — дом-сохранение, 54 — спортзал.
    // Значки из сценария игры: 5, 8, 11, 12, 13, 15, 16, 23, 24, 25, 28, 33, 34, 36, 37, 38, 40, 42–44, 46, 47, 51–53.
    bool MissionContact(int i)
    {
        if(TraceType(i) != 5) return false;
        uint8_t s = Field<uint8_t>(Trace(i), 0x28);
        return s != 31 && s != 32 && s != 35 && s != 54;
    }

    // друг: спрятанные значки миссий
    struct Hidden { int index; uint16_t counter; int display; CVector pos; uint8_t sprite; };
    std::vector<Hidden> g_hiddenContacts;
    uint64_t g_nextContactScanUs = 0;
    bool     g_forcedOnMission = false;     // друг стоит у спрятанного маркера: «на миссии», чтобы скрипт не запускал её

    void HideContacts()
    {
        if(net::Since(g_now, g_nextContactScanUs) < 500000 && g_nextContactScanUs) return;
        g_nextContactScanUs = g_now;
        for(int i = 0; i < kTraceCount; ++i)
        {
            if(!TraceInUse(i)) continue;
            uint8_t sprite = Field<uint8_t>(Trace(i), 0x28);
            if(!MissionContact(i)) continue;
            int disp = TraceDisplay(i);
            bool known = false;
            for(auto& h : g_hiddenContacts) if(h.index == i && h.counter == TraceCounter(i)) { known = true; break; }
            if(known && disp == 0) continue;
            if(!known)
            {
                Hidden h{ i, TraceCounter(i), disp ? disp : 3, Field<CVector>(Trace(i), 0x08), sprite };
                g_hiddenContacts.push_back(h);
                Log(false, "[МИССИЯ] У друга спрятан маркер миссии (значок %u) в %.0f, %.0f", sprite, h.pos.x, h.pos.y);
            }
            ChangeBlipDisplay(TraceHandle(i), 0);
        }
    }

    void RestoreContacts()
    {
        for(auto& h : g_hiddenContacts)
            if(TraceInUse(h.index) && TraceCounter(h.index) == h.counter && TraceDisplay(h.index) == 0)
                ChangeBlipDisplay(TraceHandle(h.index), h.display);
        if(!g_hiddenContacts.empty()) Log(false, "[МИССИЯ] Маркеры миссий снова видны (%d)", (int)g_hiddenContacts.size());
        g_hiddenContacts.clear();
    }

    void SetOnMissionFlag(int v)
    {
        if(g_scriptSpace && g_onMissionOffset && *g_onMissionOffset) *(int32_t*)(g_scriptSpace + *g_onMissionOffset) = v;
    }

    // Стоишь на спрятанном маркере — скрипт игры думает, что ты «на миссии», и не запускает её
    void GuardContacts()
    {
        if(!g_local) return;
        CVector p = GetPosition(g_local);
        bool near = false;
        for(auto& h : g_hiddenContacts)
        {
            float dx = p.x - h.pos.x, dy = p.y - h.pos.y;
            float d2 = dx * dx + dy * dy;
            if(d2 < (g_forcedOnMission ? 12.0f * 12.0f : 7.0f * 7.0f) && fabsf(p.z - h.pos.z) < 6.0f) { near = true; break; }
        }
        if(near && !g_forcedOnMission && !OnMission())
        {
            SetOnMissionFlag(1); g_forcedOnMission = true;
            Log(false, "[МИССИЯ] Друг у маркера миссии — миссию ведёт хост, этот маркер у тебя выключен");
        }
        else if(!near && g_forcedOnMission) { SetOnMissionFlag(0); g_forcedOnMission = false; }
    }

    // Хост: метки миссии другу
    uint64_t g_nextBlipsUs = 0;
    void HostBlips()
    {
        if(g_now < g_nextBlipsUs || !g_session) return;
        g_nextBlipsUs = g_now + 250000;
        net::BlipsPacket pk{};
        pk.hdr = net::MakeHeader(net::PKT_BLIPS);
        int remoteRef = g_remote ? GetPedRef(g_remote) : -1;
        int n = 0;
        for(int i = 0; i < kTraceCount && n < net::kBlipsMax; ++i)
        {
            if(!TraceInUse(i)) continue;
            uintptr_t t = Trace(i);
            if(Field<uint8_t>(t, 0x28) != 0) continue;              // значки (магазины, дома…) у друга свои
            int type = TraceType(i), disp = TraceDisplay(i);
            if(disp == 0) continue;
            int ent = Field<int32_t>(t, 0x04);
            CVector pos{};
            if(type == 2)                                           // пед
            {
                if(ent == remoteRef) continue;                      // метка друга — у него не нужна
                uintptr_t ped = GetPedFromRef(ent);
                if(!ped || ped == g_local) continue;
                pos = GetPosition(ped);
            }
            else if(type == 1)                                      // машина
            {
                uintptr_t veh = GetVehicleFromRef ? GetVehicleFromRef(ent) : 0;
                if(!veh) continue;
                pos = GetPosition(veh);
            }
            else if(type == 4 || type == 5 || type == 7 || type == 3)   // точка, контакт, пикап, объект
            {
                pos = Field<CVector>(t, 0x08);
                if(type == 3 || type == 7) { if(pos.x == 0 && pos.y == 0) continue; }
            }
            else continue;
            net::BlipEntry& e = pk.blips[n++];
            e.key = (uint16_t)i; e.kind = (uint8_t)type; e.display = (uint8_t)disp;
            e.colour = Field<uint32_t>(t, 0x00);
            e.pos[0] = pos.x; e.pos[1] = pos.y; e.pos[2] = pos.z;
            e.scale = Field<uint16_t>(t, 0x1C);
            e.flags = (uint8_t)(TraceBits(i) & 0xFF);
        }
        pk.count = (uint8_t)n;
        g_session->SendGame(&pk, net::kBlipsHeaderSize + n * (int)sizeof(net::BlipEntry));
    }

    // Друг: копии меток хоста
    struct Mirror { uint16_t key; int handle; int kind; uint32_t colour; uint16_t scale; int display; uint64_t seenUs; };
    std::vector<Mirror> g_mirrors;
    bool g_blipsDirty = false;
    net::BlipsPacket g_lastBlips{};
    uint64_t g_lastBlipsUs = 0;

    bool MirrorValid(const Mirror& m)
    {
        int i = m.handle & 0xFFFF;
        return m.handle >= 0 && i < kTraceCount && TraceInUse(i) && TraceCounter(i) == (uint16_t)(m.handle >> 16);
    }
    void ClearMirrors()
    {
        for(auto& m : g_mirrors) if(MirrorValid(m)) ClearBlip(m.handle);
        g_mirrors.clear();
    }

    void ApplyBlips()
    {
        if(!g_blipsDirty) { if(g_lastBlipsUs && net::Since(g_now, g_lastBlipsUs) > 3000000) { ClearMirrors(); g_lastBlipsUs = 0; } return; }
        g_blipsDirty = false;
        const net::BlipsPacket& pk = g_lastBlips;
        for(int k = 0; k < pk.count; ++k)
        {
            const net::BlipEntry& e = pk.blips[k];
            CVector pos{ e.pos[0], e.pos[1], e.pos[2] };
            // цели-педы и машины — точка на карте без столба-маркера (столб — только «приезжай сюда»)
            int disp = (e.kind == 1 || e.kind == 2) ? (e.display & 2 ? 2 : e.display) : e.display;
            Mirror* m = nullptr;
            for(auto& x : g_mirrors) if(x.key == e.key) { m = &x; break; }
            if(m && (!MirrorValid(*m) || m->kind != e.kind)) { if(MirrorValid(*m)) ClearBlip(m->handle); m->handle = -1; }
            if(!m) { g_mirrors.push_back(Mirror{ e.key, -1, e.kind, 0, 0, -1, 0 }); m = &g_mirrors.back(); }
            if(m->handle < 0)
            {
                m->handle = SetCoordBlip(4, pos, e.colour, disp, nullptr);
                m->colour = ~e.colour; m->scale = 0xFFFF; m->display = disp; m->kind = e.kind;
                if(m->handle < 0) continue;
            }
            else Field<CVector>(Trace(m->handle & 0xFFFF), 0x08) = pos;
            if(m->colour != e.colour) { ChangeBlipColour(m->handle, e.colour); m->colour = e.colour; }
            if(m->scale != e.scale && e.scale) { ChangeBlipScale(m->handle, e.scale); m->scale = e.scale; }
            if(m->display != disp) { ChangeBlipDisplay(m->handle, disp); m->display = disp; }
            m->seenUs = g_now;
        }
        for(size_t i = 0; i < g_mirrors.size();)
        {
            bool still = false;
            for(int k = 0; k < pk.count; ++k) if(pk.blips[k].key == g_mirrors[i].key) { still = true; break; }
            if(!still) { if(MirrorValid(g_mirrors[i])) ClearBlip(g_mirrors[i].handle); g_mirrors.erase(g_mirrors.begin() + i); continue; }
            ++i;
        }
    }

    // ========== место в машине на старте миссии ==========
    uint64_t g_seatWindowUntil = 0;     // хост: в эти секунды усадить друга в свою машину
    bool     g_wasMission = false, g_wasCut = false;
    struct HiddenPed { uintptr_t ped = 0; int ref = -1; uintptr_t veh = 0; int vref = -1; int slot = -1; uint64_t sinceUs = 0; bool friendSat = false; };
    HiddenPed g_hidden;
    // друг: просьба сесть
    uint16_t g_seatVeh = 0; int g_seatNo = 0; uint64_t g_seatUntil = 0; bool g_seatTeleported = false;

    uintptr_t& Passenger(uintptr_t veh, int i) { return Field<uintptr_t>(veh, off::VehPassengers + (uintptr_t)i * 8); }

    bool IsPlayerPed(uintptr_t p) { int t = Field<int>(p, off::PedType); return t == 0 || t == 1; }

    void RestoreHidden(const char* why)
    {
        HiddenPed& h = g_hidden;
        if(!h.ped) return;
        bool pedOk = GetPedFromRef(h.ped ? h.ref : -1) == h.ped;
        bool vehOk = GetVehicleFromRef && GetVehicleFromRef(h.vref) == h.veh;
        if(pedOk)
        {
            if(vehOk && InVehicle(h.ped) && Field<uintptr_t>(h.ped, off::PedVehicle) == h.veh)
            {
                uintptr_t& slot = Passenger(h.veh, h.slot);
                if(slot == g_remote && g_remote) { sync::WarpPedOut(g_remote); }
                if(slot == 0)
                {
                    slot = h.ped;
                    Field<uint8_t>(h.veh, 0x5E8) += 1;              // m_nNumPassengers
                }
                else
                {
                    // место занято — найти другое свободное
                    int max = Field<uint8_t>(h.veh, off::VehMaxPassengers);
                    for(int i = 0; i < max && i < 8; ++i) if(!Passenger(h.veh, i)) { Passenger(h.veh, i) = h.ped; Field<uint8_t>(h.veh, 0x5E8) += 1; break; }
                }
            }
            SetVisible(h.ped, true);
        }
        Log(false, "[МИССИЯ] Пед миссии вернулся на своё место (%s)", why);
        g_hidden = HiddenPed();
    }

    void HostSeatFriend()
    {
        // начало миссии / конец катсцены: окно, когда друга сажаем к хосту
        bool mission = OnMission(), cut = CutsceneRunning();
        if(mission && !g_wasMission) { g_seatWindowUntil = g_now + 12000000; Log(false, "[МИССИЯ] У хоста началась миссия"); }
        if(!mission && g_wasMission) Log(false, "[МИССИЯ] У хоста миссия закончилась");
        if(g_wasCut && !cut && mission) g_seatWindowUntil = g_now + 12000000;
        g_wasMission = mission; g_wasCut = cut;

        // спрятанный пед: вернуть, когда друг вышел, все вышли или катсцена
        if(g_hidden.ped)
        {
            bool friendIn = g_remote && InVehicle(g_remote) && Field<uintptr_t>(g_remote, off::PedVehicle) == g_hidden.veh;
            if(friendIn) g_hidden.friendSat = true;
            if(cut) RestoreHidden("катсцена");
            else if(g_hidden.friendSat && !friendIn) RestoreHidden("друг вышел");
            else if(!g_hidden.friendSat && net::Since(g_now, g_hidden.sinceUs) > 10000000) RestoreHidden("друг так и не сел");
            else if(!mission) RestoreHidden("миссия закончилась");
        }

        if(!g_seatWindowUntil || g_now > g_seatWindowUntil || cut) return;
        if(!g_remote || !g_rs || g_rs->seat != net::kNoSeat || (g_rs->flags & net::PSF_DEAD) || !InVehicle(g_local)) return;
        uintptr_t veh = Field<uintptr_t>(g_local, off::PedVehicle);
        if(!veh || Field<int>(veh, off::VehBaseType) == 6) return;
        uint16_t vid = sync::CrowdVehicleId(veh);
        if(!vid) return;
        int max = Field<uint8_t>(veh, off::VehMaxPassengers);
        if(max > 8) max = 8;
        if(max <= 0) { g_seatWindowUntil = 0; Log(false, "[МИССИЯ] В машине хоста нет мест для пассажира"); return; }
        int seat = -1;
        for(int i = 0; i < max; ++i) if(!Passenger(veh, i)) { seat = i; break; }
        if(seat < 0 && !g_hidden.ped)
        {
            // все места заняты педами миссии — одного спрятать (остаётся в машине, скрипт его видит)
            for(int i = max - 1; i >= 0; --i)
            {
                uintptr_t p = Passenger(veh, i);
                if(!p || IsPlayerPed(p) || p == g_remote) continue;
                g_hidden.ped = p; g_hidden.ref = GetPedRef(p); g_hidden.veh = veh; g_hidden.vref = GetVehicleRef(veh);
                g_hidden.slot = i; g_hidden.sinceUs = g_now; g_hidden.friendSat = false;
                Passenger(veh, i) = 0;
                if(Field<uint8_t>(veh, 0x5E8)) Field<uint8_t>(veh, 0x5E8) -= 1;
                SetVisible(p, false);
                seat = i;
                Log(false, "[МИССИЯ] Все места заняты — пед миссии (модель %d) спрятан, друг сядет на его место %d", ModelOf(p), i + 1);
                break;
            }
        }
        if(seat < 0) { g_seatWindowUntil = 0; Log(false, "[МИССИЯ] Мест в машине хоста нет — друг едет сам"); return; }
        net::SeatEvent e{ net::REL_SEAT, (uint8_t)(seat + 1), vid };
        g_session->SendReliable(&e, sizeof(e));
        g_seatWindowUntil = 0;
        Log(false, "[МИССИЯ] Миссия в машине — сажаю друга к себе (место %d, машина #%04X)", seat + 1, vid);
    }

    void TeleportNearHost(const char* why)
    {
        if(!g_rs || !g_local || !PedTeleport) return;
        const net::PlayerStatePacket& h = *g_rs;
        if(InVehicle(g_local)) sync::WarpPedOut(g_local);
        uint8_t myArea = CurrArea ? *CurrArea : 0;
        if(h.area != myArea && CurrArea)
        {
            *CurrArea = h.area;
            Field<uint8_t>(g_local, off::EntArea) = h.area;
            Log(false, "[МИССИЯ] Интерьер как у хоста: %u", h.area);
        }
        float hh = h.heading;
        float side = (h.flags & net::PSF_IN_VEHICLE) ? 3.5f : 1.8f;
        CVector to{ h.pos[0] + cosf(hh) * side, h.pos[1] + sinf(hh) * side, h.pos[2] + 0.5f };
        PedTeleport(g_local, to, false);
        Log(false, "[МИССИЯ] Друг перенесён к хосту (%s): %.1f, %.1f, %.1f", why, to.x, to.y, to.z);
    }

    void ClientSeat()
    {
        if(!g_seatVeh) return;
        if(g_now > g_seatUntil) { Log(false, "[МИССИЯ] Не успел сесть к хосту (машины #%04X нет рядом)", g_seatVeh); g_seatVeh = 0; return; }
        if(g_cut != Cut::None) return;
        if(InVehicle(g_local)) { g_seatVeh = 0; return; }
        uintptr_t veh = sync::VehicleById(g_seatVeh);
        if(!veh)
        {
            if(!g_seatTeleported && g_rs) { g_seatTeleported = true; TeleportNearHost("садиться в машину хоста"); }
            return;
        }
        sync::WarpPedInto(g_local, veh, g_seatNo);
        Log(false, "[МИССИЯ] Ты сел к хосту в машину (место %d)%s", g_seatNo, InVehicle(g_local) ? "" : " — не вышло");
        g_seatVeh = 0;
    }

    // ========== катсцена у друга ==========
    void ClientCutFrame()
    {
        uintptr_t pi = Players;   // CWorld::Players[0]
        switch(g_cut)
        {
            case Cut::None: break;
            case Cut::Pending:
            {
                if(InVehicle(g_local)) sync::WarpPedOut(g_local);
                g_cutFreezePos = GetPosition(g_local);
                if(MakePlayerSafe) MakePlayerSafe(pi, true, 10.0f);
                if(g_cutStem[0] && LoadMissionText && g_theText) LoadMissionText(g_theText, g_cutStem);
                CutLoad(g_cutName);
                g_cut = Cut::Loading; g_cutSinceUs = g_now;
                Log(false, "[КАТСЦЕНА] Загружаю «%s», как у хоста", g_cutName);
                break;
            }
            case Cut::Loading:
                SetPosition(g_local, g_cutFreezePos);
                Field<CVector>(g_local, off::PhysMoveSpeed) = CVector{ 0, 0, 0 };
                if(g_cutLoadStatus && *g_cutLoadStatus == 2)
                {
                    if(g_cutEndReceived) { CutDelete(); g_cut = Cut::After; Log(false, "[КАТСЦЕНА] Хост уже пропустил — не показываю"); break; }
                    CutStart();
                    g_cut = Cut::Playing; g_cutSinceUs = g_now;
                    Log(false, "[КАТСЦЕНА] Идёт «%s» (пропускает хост)", g_cutName);
                }
                else if(net::Since(g_now, g_cutSinceUs) > 20000000)
                {
                    Log(true, "[КАТСЦЕНА] «%s» не загрузилась за 20 с — пропускаю", g_cutName);
                    CutDelete(); g_cut = Cut::After;
                }
                break;
            case Cut::Playing:
                SetPosition(g_local, g_cutFreezePos);
                Field<CVector>(g_local, off::PhysMoveSpeed) = CVector{ 0, 0, 0 };
                if(g_cutEndReceived && !CutFinished())
                {
                    g_allowSkip = true; CutSkip(); g_allowSkip = false;
                    Log(false, "[КАТСЦЕНА] Хост пропустил — пропускаю и у тебя");
                }
                if(CutFinished() || net::Since(g_now, g_cutSinceUs) > 600000000)
                {
                    CutDelete();
                    g_cut = Cut::After;
                    Log(false, "[КАТСЦЕНА] «%s» закончилась", g_cutName);
                }
                break;
            case Cut::After:
                if(CameraRestore && TheCamera) CameraRestore(TheCamera);
                if(Fade && TheCamera) Fade(TheCamera, 0.5f, 1);
                if(MakePlayerSafe) MakePlayerSafe(pi, false, 10.0f);
                g_cut = Cut::None;
                g_cutHaveEnded = true; g_cutEndedId = g_cutId;
                g_teleportAtUs = g_now + 1500000;      // скрипт хоста ставит его на место — потом и друга рядом
                break;
        }
        if(g_teleportAtUs && g_now >= g_teleportAtUs && g_cut == Cut::None)
        {
            g_teleportAtUs = 0;
            if(g_rs)
            {
                CVector hp{ g_rs->pos[0], g_rs->pos[1], g_rs->pos[2] };
                if(Dist(hp, GetPosition(g_local)) > 25.0f || g_rs->area != (CurrArea ? *CurrArea : 0)) TeleportNearHost("после катсцены");
            }
        }
    }

    // После заблокированного запуска миссии скрипт мог затемнить экран и выключить управление
    void ClientRecover()
    {
        if(!g_blockRecover || net::Since(g_now, g_blockedUs) < 600000) return;
        g_blockRecover = false;
        if(g_cut != Cut::None) return;
        // Скрипт маркера перед запуском: $ONMISSION = 1, управление выкл., широкий экран, своя камера, затемнение.
        // Миссию не запустили — вернуть всё; $ONMISSION оставить 1, пока стоишь у маркера (иначе маркер сработает снова).
        if(CameraRestore && TheCamera) CameraRestore(TheCamera);
        if(WideScreenOff && TheCamera) WideScreenOff(TheCamera);
        if(Fade && TheCamera) Fade(TheCamera, 0.3f, 1);
        if(MakePlayerSafe) MakePlayerSafe(Players, false, 10.0f);
        g_forcedOnMission = true;
        Log(false, "[МИССИЯ] Вернул тебе управление и экран после маркера миссии");
    }

    // Второго CJ не видно в катсценах (у хоста и у друга)
    bool g_remoteHidden = false; uintptr_t g_remoteHiddenPed = 0;
    void HideRemoteInCutscene()
    {
        bool want = g_remote && (CutsceneRunning() || g_cut == Cut::Loading || g_cut == Cut::Playing);
        if(want && !g_remoteHidden) { SetVisible(g_remote, false); g_remoteHidden = true; g_remoteHiddenPed = g_remote; }
        else if(!want && g_remoteHidden)
        {
            if(g_remoteHiddenPed && g_remoteHiddenPed == g_remote) SetVisible(g_remote, true);
            g_remoteHidden = false; g_remoteHiddenPed = 0;
        }
    }

    // ========== приём ==========
    struct RecruitReq { uint16_t npc; };
    std::vector<RecruitReq> g_recruits;
    std::vector<net::CutsceneEvent> g_cutEvents;
    char g_pendingStem[9] = "";

    void HostRecruit(uint16_t npc)
    {
        uintptr_t ped = crowd::TrackedPed(npc);
        if(!ped || !g_remote || !Field<uintptr_t>(g_remote, off::PedPlayerData))
        {
            Log(false, "[ГРУППА] Друг позвал педа #%u — его уже нет рядом", npc);
            return;
        }
        JoinOurGroup(g_remote, ped);
        Log(false, "[ГРУППА] Пед #%u (модель %d) — теперь в группе друга", npc, ModelOf(ped));
    }
}

bool Install(void* h, FLogger* log)
{
    g_log = log;
    auto sym = [&](const char* s) { return (void*)aml->GetSym(h, s); };
    g_cutLoadStatus = (uint8_t*)sym("_ZN12CCutsceneMgr21ms_cutsceneLoadStatusE");
    g_cutRunning    = (uint8_t*)sym("_ZN12CCutsceneMgr10ms_runningE");
    CutLoad     = (void(*)(const char*))sym("_ZN12CCutsceneMgr16LoadCutsceneDataEPKc");
    CutStart    = (void(*)())sym("_ZN12CCutsceneMgr13StartCutsceneEv");
    CutFinished = (bool(*)())sym("_ZN12CCutsceneMgr19HasCutsceneFinishedEv");
    CutSkip     = (void(*)())sym("_ZN12CCutsceneMgr12SkipCutsceneEv");
    CutDelete   = (void(*)())sym("_ZN12CCutsceneMgr18DeleteCutsceneDataEv");
    g_theText   = (uintptr_t)sym("TheText");
    LoadMissionText = (void(*)(uintptr_t, char*))sym("_ZN5CText15LoadMissionTextEPc");
    InsertNumber = (void(*)(uint16_t*, int, int, int, int, int, int, uint16_t*))sym("_ZN9CMessages20InsertNumberInStringEPtiiiiiiS0_");
    g_radarTrace = (uintptr_t)sym("_ZN6CRadar13ms_RadarTraceE");
    SetCoordBlip = (int(*)(int, CVector, unsigned, int, char*))sym("_ZN6CRadar12SetCoordBlipE9eBlipType7CVectorj12eBlipDisplayPc");
    ChangeBlipDisplay = (void(*)(int, int))sym("_ZN6CRadar17ChangeBlipDisplayEi12eBlipDisplay");
    g_scriptSpace = (uint8_t*)sym("_ZN11CTheScripts11ScriptSpaceE");
    g_onMissionOffset = (uint32_t*)sym("_ZN11CTheScripts14OnAMissionFlagE");
    g_alreadyMission = (uint8_t*)sym("_ZN11CTheScripts29bAlreadyRunningAMissionScriptE");
    CollectParameters = (void(*)(uintptr_t, int16_t))sym("_ZN14CRunningScript17CollectParametersEs");
    Fade = (void(*)(uintptr_t, float, int16_t))sym("_ZN7CCamera4FadeEfs");
    MakePlayerSafe = (void(*)(uintptr_t, bool, float))sym("_ZN11CPlayerInfo14MakePlayerSafeEbf");
    CameraRestore = (void(*)(uintptr_t))sym("_ZN7CCamera7RestoreEv");
    WideScreenOff = (void(*)(uintptr_t))sym("_ZN7CCamera16SetWideScreenOffEv");
    g_scriptParams = (int32_t*)sym("ScriptParams");

    g_ok = g_cutLoadStatus && g_cutRunning && CutLoad && CutStart && CutFinished && CutSkip && CutDelete &&
           g_radarTrace && SetCoordBlip && ChangeBlipDisplay && g_scriptSpace && g_onMissionOffset && CollectParameters &&
           VehiclesReady();
    if(!g_ok) { Log(true, "[МИССИЯ] Не все функции игры найдены — миссии вместе выключены (у каждого свои)"); return false; }

    struct HookDef { const char* sym; void* fn; void** orig; };
    const HookDef hooks[] = {
        { "_ZN9CMessages10AddMessageEPKcPtjtb",              (void*)HookOf_AddMessage,              (void**)&AddMessage },
        { "_ZN9CMessages15AddMessageJumpQEPKcPtjtb",         (void*)HookOf_AddMessageJumpQ,         (void**)&AddMessageJumpQ },
        { "_ZN9CMessages20AddMessageWithNumberEPKcPtjtiiiiiib", (void*)HookOf_AddMessageWithNumber, (void**)&AddMessageWithNumber },
        { "_ZN9CMessages13AddBigMessageEPtjt",               (void*)HookOf_AddBigMessage,           (void**)&AddBigMessage },
        { "_ZN9CMessages23AddBigMessageWithNumberEPtjtiiiiii", (void*)HookOf_AddBigMessageWithNumber, (void**)&AddBigMessageWithNumber },
        { "_ZN4CHud14SetHelpMessageEPKcPtbbbj",              (void*)HookOf_SetHelpMessage,          (void**)&SetHelpMessage },
        { "_ZN5CText15LoadMissionTextEPc",                   (void*)HookOf_LoadMissionTextHook,     (void**)&LoadMissionTextHook },
        { "_ZN12CCutsceneMgr16LoadCutsceneDataEPKc",         (void*)HookOf_CutLoadHook,             (void**)&CutLoadHook },
        { "_ZN12CCutsceneMgr18DeleteCutsceneDataEv",         (void*)HookOf_CutDeleteHook,           (void**)&CutDeleteHook },
        { "_ZN12CCutsceneMgr12SkipCutsceneEv",               (void*)HookOf_CutSkipHook,             (void**)&CutSkipHook },
        { "_ZN14CRunningScript25ProcessCommands1000To1099Ei", (void*)HookOf_ProcessCommands1000,    (void**)&ProcessCommands1000 },
        { "_ZN10CPlayerPed23MakeThisPedJoinOurGroupEP4CPed", (void*)HookOf_JoinOurGroup,            (void**)&JoinOurGroup },
    };
    for(const auto& d : hooks) if(!sym(d.sym)) { Log(true, "[МИССИЯ] нет %s — миссии вместе выключены", d.sym); g_ok = false; return false; }
    for(const auto& d : hooks) aml->Hook(sym(d.sym), d.fn, d.orig);
    // Хуки заменили CutLoad/CutDelete/CutSkip на «оригиналы» — вызывать через них (без отправки хосту)
    CutLoad = CutLoadHook; CutDelete = CutDeleteHook; CutSkip = CutSkipHook;
    LoadMissionText = LoadMissionTextHook;
    Log(false, "[МИССИЯ] Миссии вместе готовы (ведёт хост)");
    return true;
}

void SetSession(net::Session* s) { g_session = s; }

bool HandlesReliable(uint8_t t) { return t >= net::REL_TEXT && t <= net::REL_RECRUIT; }

void OnPacket(uint8_t type, const uint8_t* data, int len)
{
    if(!g_ok) return;
    if(type == net::PKT_BLIPS)
    {
        if(g_role != net::Role::Client || len < net::kBlipsHeaderSize) return;
        const auto* p = (const net::BlipsPacket*)data;
        if(p->count > net::kBlipsMax || net::kBlipsHeaderSize + p->count * (int)sizeof(net::BlipEntry) != len) return;
        memcpy(&g_lastBlips, p, len);
        g_blipsDirty = true; g_lastBlipsUs = g_now ? g_now : net::NowUs();
        return;
    }
    if(type != net::PKT_RELIABLE || len < 1) return;
    switch(data[0])
    {
        case net::REL_TEXT:
            if(g_role == net::Role::Client && len >= net::kTextHeaderSize)
            {
                net::TextEvent e{};
                memcpy(&e, data, len < (int)sizeof(e) ? len : (int)sizeof(e));
                if(net::kTextHeaderSize + e.len != len) return;
                if(g_textQueue.size() < 32) g_textQueue.push_back(e);
            }
            break;
        case net::REL_CUTSCENE:
            if(g_role == net::Role::Client && len == sizeof(net::CutsceneEvent))
                g_cutEvents.push_back(*(const net::CutsceneEvent*)data);
            break;
        case net::REL_MISSION_TEXT:
            if(g_role == net::Role::Client && len == sizeof(net::MissionTextEvent))
            {
                memcpy(g_pendingStem, ((const net::MissionTextEvent*)data)->stem, 8); g_pendingStem[8] = 0;
            }
            break;
        case net::REL_SEAT:
            if(g_role == net::Role::Client && len == sizeof(net::SeatEvent))
            {
                const auto* e = (const net::SeatEvent*)data;
                g_seatVeh = e->vehId; g_seatNo = e->seat; g_seatTeleported = false;
                g_seatUntil = (g_now ? g_now : net::NowUs()) + 6000000;
                Log(false, "[МИССИЯ] Хост зовёт в машину #%04X, место %u", e->vehId, e->seat);
            }
            break;
        case net::REL_RECRUIT:
            if(g_role == net::Role::Host && len == sizeof(net::RecruitEvent))
                g_recruits.push_back({ ((const net::RecruitEvent*)data)->npc });
            break;
    }
}

void Frame(net::Role role, bool connected, uintptr_t local, uintptr_t remote, const net::PlayerStatePacket* rs, uint64_t now)
{
    if(!g_ok) return;
    bool was = g_connected && (g_role == net::Role::Host || g_role == net::Role::Client);
    g_role = role; g_now = now; g_local = local; g_remote = remote; g_rs = rs;
    if(g_forceBlock && role != net::Role::Client) { g_forceBlock = false; Log(false, "[МИССИЯ] Кооп не «друг» — запрет миссий для новой игры снят"); }
    g_connected = connected && local && (role == net::Role::Host || role == net::Role::Client);
    if(!g_connected)
    {
        if(was)
        {
            // в игровом кадре: свою копию катсцены хоста — убрать, управление вернуть
            if(g_cut == Cut::Loading || g_cut == Cut::Playing)
            {
                CutDelete();
                if(MakePlayerSafe && Players) MakePlayerSafe(Players, false, 10.0f);
                if(Fade && TheCamera) Fade(TheCamera, 0.5f, 1);
            }
            if(g_remoteHidden && g_remote && g_remote == g_remoteHiddenPed) SetVisible(g_remote, true);
            Reset("связь пропала");
        }
        return;
    }

    HideRemoteInCutscene();

    if(role == net::Role::Host)
    {
        for(auto& r : g_recruits) HostRecruit(r.npc);
        g_recruits.clear();
        HostBlips();
        HostSeatFriend();
        return;
    }

    // ---- друг ----
    if(g_pendingStem[0] && LoadMissionText && g_theText && g_cut == Cut::None)
    {
        LoadMissionText(g_theText, g_pendingStem);
        Log(false, "[МИССИЯ] Тексты миссии хоста «%s» загружены", g_pendingStem);
        g_pendingStem[0] = 0;
    }
    for(const auto& e : g_cutEvents)
    {
        if(e.op == net::CUT_LOAD)
        {
            if(g_cutHaveEnded && !AfterNewer(e.counter, g_cutEndedId)) continue;
            if(g_cut != Cut::None && g_cut != Cut::After)
            {
                Log(false, "[КАТСЦЕНА] Новая катсцена хоста, а прошлая ещё идёт — убираю прошлую");
                CutDelete(); g_cut = Cut::None;
            }
            g_cutId = e.counter; g_cutEndReceived = false;
            memcpy(g_cutName, e.name, 8); g_cutName[8] = 0;
            memcpy(g_cutStem, e.textStem, 8); g_cutStem[8] = 0;
            g_cut = Cut::Pending;
        }
        else if(e.op == net::CUT_END)
        {
            if(e.counter == g_cutId && g_cut != Cut::None) g_cutEndReceived = true;
            else if(!g_cutHaveEnded || AfterNewer(e.counter, g_cutEndedId)) { g_cutHaveEnded = true; g_cutEndedId = e.counter; }
        }
    }
    g_cutEvents.clear();
    ClientCutFrame();
    ClientRecover();

    for(const auto& t : g_textQueue) ShowText(t);
    g_textQueue.clear();

    HideContacts();
    GuardContacts();
    ApplyBlips();
    ClientSeat();
}

void ForceClientBlock(bool on)
{
    g_forceBlock = on;
    Log(false, on ? "[МИССИЯ] Новая игра у друга: миссии ведёт хост, своё вступление не запускаю" : "[МИССИЯ] Запрет миссий для новой игры снят");
}

void Reset(const char* why)
{
    if(!g_ok) return;
    bool any = g_cut != Cut::None || !g_hiddenContacts.empty() || !g_mirrors.empty() || g_hidden.ped || g_forcedOnMission;
    g_cut = Cut::None; g_cutEndReceived = false; g_teleportAtUs = 0;
    RestoreContacts();
    if(g_forcedOnMission) { SetOnMissionFlag(0); g_forcedOnMission = false; }
    ClearMirrors();
    g_lastBlipsUs = 0; g_blipsDirty = false;
    if(g_hidden.ped) RestoreHidden(why);
    g_remoteHidden = false; g_remoteHiddenPed = 0;
    g_textQueue.clear(); g_cutEvents.clear(); g_recruits.clear();
    g_seatVeh = 0; g_seatWindowUntil = 0; g_cutOpen = false;
    g_wasMission = g_wasCut = false;
    if(any) Log(false, "[МИССИЯ] Миссии вместе выключены (%s) — всё как в одиночной игре", why);
}

bool ClientCutscene() { return g_cut == Cut::Loading || g_cut == Cut::Playing || g_cut == Cut::Pending; }
bool IsHiddenPed(uintptr_t ped) { return ped && ped == g_hidden.ped; }

}}
