#include "script.h"

#include <mod/amlmod.h>
#include <string.h>

#include "game/game.h"
#include "util/log.h"

namespace script
{
namespace
{
    constexpr size_t kScriptSize = 0x130;      // sizeof(CRunningScript), GTA SA 2.10 arm64
    constexpr size_t kOffName = 0x10, kOffBase = 0x18, kOffPC = 0x20, kOffLocals = 0x6C, kOffActive = 0x114, kOffCmp = 0x115;

    typedef int8_t (*ProcessOne_t)(void* script);
    ProcessOne_t ProcessOne = nullptr;
    int (*GetVehicleRef)(uintptr_t) = nullptr;
    bool g_ready = false;
}

bool Init(void* h)
{
    ProcessOne = (ProcessOne_t)aml->GetSym(h, "_ZN14CRunningScript17ProcessOneCommandEv");
    GetVehicleRef = (int(*)(uintptr_t))aml->GetSym(h, "_ZN6CPools13GetVehicleRefEP8CVehicle");
    g_ready = ProcessOne != nullptr;
    if(!g_ready) LOG_ERROR("Джарвис: нет CRunningScript::ProcessOneCommand — команды игры недоступны");
    return g_ready;
}
bool Ready() { return g_ready; }

bool Run(uint16_t opcode, std::vector<Arg>& args, bool* flag)
{
    if(!g_ready) return false;
    alignas(16) uint8_t scr[kScriptSize + 16];
    memset(scr, 0, sizeof(scr));
    uint8_t code[1024]; size_t n = 0;
    auto put = [&](const void* p, size_t k) { if(n + k <= sizeof(code)) { memcpy(code + n, p, k); n += k; } };
    put(&opcode, 2);
    int outIndex = 0;
    for(Arg& a : args)
    {
        uint8_t t;
        switch(a.type)
        {
            case Arg::INT:   t = 0x01; put(&t, 1); put(&a.i, 4); break;
            case Arg::FLOAT: t = 0x06; put(&t, 1); put(&a.f, 4); break;
            case Arg::STR:
                if(a.s.size() <= 7) { t = 0x09; put(&t, 1); char b[8] = {}; memcpy(b, a.s.data(), a.s.size()); put(b, 8); }
                else { t = 0x0F; put(&t, 1); char b[16] = {}; memcpy(b, a.s.data(), a.s.size() > 15 ? 15 : a.s.size()); put(b, 16); }   // строка на 16 байт; длиннее игра не вместит
                break;
            case Arg::OUT_INT: case Arg::OUT_FLOAT:
            { t = 0x03; put(&t, 1); uint16_t idx = (uint16_t)outIndex++; put(&idx, 2); break; }
        }
    }
    if(n + 8 > sizeof(code) || outIndex > 30) return false;
    memset(code + n, 0, 8);
    memcpy(scr + kOffName, "jarvis", 6);
    uint8_t* base = code;
    memcpy(scr + kOffBase, &base, sizeof(base));
    memcpy(scr + kOffPC, &base, sizeof(base));
    scr[kOffActive] = 1;
    ProcessOne(scr);
    if(flag) *flag = scr[kOffCmp] != 0;
    outIndex = 0;
    for(Arg& a : args)
    {
        if(a.type == Arg::OUT_INT)   { memcpy(&a.i, scr + kOffLocals + 4 * outIndex++, 4); }
        if(a.type == Arg::OUT_FLOAT) { memcpy(&a.f, scr + kOffLocals + 4 * outIndex++, 4); }
    }
    return true;
}

bool Do(uint16_t opcode, std::vector<Arg> args) { return Run(opcode, args, nullptr); }

int PlayerChar() { uintptr_t p = game::Player(); return p ? game::Ref(p) : -1; }
int CarHandle(uintptr_t veh) { return veh && GetVehicleRef ? GetVehicleRef(veh) : -1; }
}
