#include "sync.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <math.h>
#include <stdarg.h>
#include <string.h>

#include <vector>

#include "game/gta.h"

using namespace gta;

namespace coop { namespace sync {
namespace
{
    constexpr uint64_t kVehFastUs      = 50000;     // машина в деле — 20 раз/с
    constexpr uint64_t kVehSlowUs      = 1000000;   // стоит пустая — раз в секунду
    constexpr uint64_t kVehStaleUs     = 6000000;   // копия без вестей 6 с — убрать
    constexpr float    kVehKeepDist    = 220.0f;    // своя общая машина дальше от обоих — не общая
    constexpr float    kCopyCreateDist = 300.0f;    // копию создаём ближе этого
    constexpr float    kCopyDropDist   = 360.0f;    // и убираем дальше этого
    constexpr float    kVehSnapDist    = 6.0f;      // расхождение больше — переносим сразу
    constexpr float    kVehSmooth      = 10.0f;     // скорость подтягивания (1/с)
    constexpr int      kMaxVehicles    = 16;
    constexpr int      kDriverDoor     = 10;        // дверь водителя (CCarEnterExit)
    constexpr uint64_t kEnterTimeoutUs = 4000000;   // не сел с анимацией за 4 с — сажаем сразу
    constexpr uint64_t kLeaveTimeoutUs = 2500000;
    constexpr uint64_t kEchoDelayUs    = 500000;

    // Типы задач игры
    constexpr int TASK_ENTER_AS_PASSENGER = 700;
    constexpr int TASK_ENTER_AS_DRIVER    = 701;
    constexpr int TASK_LEAVE_CAR          = 704;

    FLogger*      g_log = nullptr;
    net::Session* g_session = nullptr;
    bool          g_installed = false;

    void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool error, const char* fmt, ...)
    {
        char buf[512];
        va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        if(g_log) { if(error) g_log->Error("%s", buf); else g_log->Info("%s", buf); }
    }

    // ---------- текущий кадр ----------
    Ctx        g_ctx;
    bool       g_active = false;   // кооп или эхо идёт
    bool       g_echo = false;
    uint16_t   g_myBit = 0;        // старший бит своих номеров машин (хост 0, клиент 0x8000)
    bool       g_ff = false;       // огонь по своим (решение хоста)

    // ---------- машины ----------
    struct NetVeh
    {
        uint16_t  id = 0;
        uintptr_t veh = 0;
        int       ref = -1;
        bool      copy = false;        // создана модом по данным друга
        int       model = -1;
        int8_t    origCreatedBy = -1;  // своя: как была помечена до того, как стала общей
        bool      haveState = false;
        net::VehiclePacket st{};
        uint64_t  stUs = 0;
        uint64_t  lastSendUs = 0;
        uint64_t  requestUs = 0;       // копия: когда попросили модель
        bool      blownDone = false;
        bool      dead = false;        // удалить из списка
    };
    std::vector<NetVeh> g_vehs;
    uint16_t g_nextId = 1;
    uint32_t g_vehSeq = 0;

    bool Valid(NetVeh& v)
    {
        if(!v.veh) return false;
        if(GetVehicleFromRef(v.ref) == v.veh) return true;
        v.veh = 0; v.ref = -1;
        return false;
    }

    NetVeh* FindId(uint16_t id)
    {
        if(!id) return nullptr;
        for(auto& v : g_vehs) if(!v.dead && v.id == id) return &v;
        return nullptr;
    }
    NetVeh* FindVeh(uintptr_t veh)
    {
        if(!veh) return nullptr;
        for(auto& v : g_vehs) if(!v.dead && v.veh == veh && Valid(v)) return &v;
        return nullptr;
    }
    bool Foreign(uint16_t id) { return (id & 0x8000) != g_myBit; }

    float Dist(const CVector& a, const CVector& b)
    {
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }
    CVector V(const float* f) { return CVector{ f[0], f[1], f[2] }; }
    void    Put(float* f, const CVector& v) { f[0] = v.x; f[1] = v.y; f[2] = v.z; }
    CVector Cross(const CVector& a, const CVector& b) { return CVector{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    CVector Norm(const CVector& a) { float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); return l > 1e-5f ? CVector{ a.x / l, a.y / l, a.z / l } : CVector{ 0, 1, 0 }; }
    CVector Lerp(const CVector& a, const CVector& b, float k) { return CVector{ a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k }; }

    uintptr_t PedVehicle(uintptr_t ped) { return (ped && InVehicle(ped)) ? Field<uintptr_t>(ped, off::PedVehicle) : 0; }

    bool AutomobileFamily(uintptr_t veh)
    {
        int t = Field<int>(veh, off::VehBaseType);
        return t == 0 || t == 1 || t == 2 || t == 3 || t == 4 || t == 11;   // CAutomobile и наследники
    }

    // ---------- эхо: свои пакеты возвращаются через 0,5 с как чужие ----------
    struct EchoPkt { uint64_t at; uint8_t type; int len; uint8_t data[sizeof(net::VehiclePacket) + 64]; };
    std::vector<EchoPkt> g_echoQ;

    void SendGame(const void* pkt, int len)
    {
        if(g_echo)
        {
            if(g_echoQ.size() > 512 || len > (int)sizeof(EchoPkt::data)) return;
            EchoPkt e; e.at = g_ctx.now + kEchoDelayUs; e.type = ((const net::Header*)pkt)->type; e.len = len;
            memcpy(e.data, pkt, len);
            g_echoQ.push_back(e);
            return;
        }
        if(g_session) g_session->SendGame(pkt, len);
    }
    void SendReliable(const void* data, int len)
    {
        if(!g_echo && g_session) g_session->SendReliable(data, len);
    }

    void EchoDeliver()
    {
        for(size_t i = 0; i < g_echoQ.size();)
        {
            EchoPkt& e = g_echoQ[i];
            if(g_ctx.now < e.at) { ++i; continue; }
            float off = g_ctx.echoOffset;
            if(e.type == net::PKT_VEHICLE)
            {
                auto* p = (net::VehiclePacket*)e.data; p->id ^= 0x8000; p->pos[0] += off;
            }
            else if(e.type == net::PKT_VEHICLE_GONE) ((net::VehicleGonePacket*)e.data)->id ^= 0x8000;
            else if(e.type == net::PKT_SHOT)
            {
                auto* p = (net::ShotPacket*)e.data; p->shooter[0] += off; p->start[0] += off; p->end[0] += off;
                p->flags &= ~net::SHOT_HIT_MATE;
            }
            EchoPkt copy = e;
            g_echoQ.erase(g_echoQ.begin() + i);
            OnPacket(copy.type, copy.data, copy.len);
        }
    }

    // ---------- создание / удаление машин ----------
    void SetMatrix(uintptr_t veh, const CVector& pos, const CVector& right, const CVector& fwd)
    {
        uintptr_t m = Field<uintptr_t>(veh, off::EntMatrix);
        if(!m) { SetPosition(veh, pos); return; }
        CVector up = Norm(Cross(right, fwd));
        CVector r  = Norm(Cross(fwd, up));
        Field<CVector>(m, off::MatRight)   = r;
        Field<CVector>(m, off::MatForward) = Norm(fwd);
        Field<CVector>(m, off::MatUp)      = up;
        Field<CVector>(m, off::MatPos)     = pos;
        MatrixUpdateRW(m);
        if(Field<uintptr_t>(veh, off::EntRwObject)) EntityUpdateRwFrame(veh);
    }

    // Высадить персонажа из машины сразу (как скриптовая WARP_CHAR_FROM_CAR_TO_COORD)
    void WarpOut(uintptr_t ped)
    {
        uintptr_t veh = PedVehicle(ped);
        if(!veh) return;
        alignas(16) uint8_t task[0x60] = {};
        int seat = SeatOf(ped, veh);
        int door = seat <= 0 ? kDriverDoor : DoorForPassengerSeat(veh, seat - 1);
        if(door < 0) door = 8;
        TaskSetPedOutCtor(task, veh, door, false);
        task[0x1D] = 1;                              // m_bWarpingOutOfCar — без анимации
        TaskSetPedOutProcess(task, ped);
        TaskSetPedOutDtor(task);
    }

    void DeleteVehicle(uintptr_t veh)
    {
        // Сначала высадить игроков: удаление машины с игроком внутри — висячий указатель
        if(g_ctx.remote && PedVehicle(g_ctx.remote) == veh) WarpOut(g_ctx.remote);
        if(g_ctx.local && PedVehicle(g_ctx.local) == veh) WarpOut(g_ctx.local);
        WorldRemove(veh);
        uintptr_t vt = Field<uintptr_t>(veh, 0);
        ((void(*)(uintptr_t))Field<uintptr_t>(vt, off::VtDeletingDtor))(veh);
    }

    void ReleaseOwn(NetVeh& v)
    {
        // Своя машина больше не общая: вернуть, как было (игра снова может её убрать)
        if(Valid(v) && v.origCreatedBy >= 0 && Field<uint8_t>(v.veh, off::VehCreatedBy) == 2)
            Field<uint8_t>(v.veh, off::VehCreatedBy) = (uint8_t)v.origCreatedBy;
    }

    void SendGone(uint16_t id)
    {
        net::VehicleGonePacket g{ net::MakeHeader(net::PKT_VEHICLE_GONE), id, 0 };
        SendGame(&g, sizeof(g));
    }

    NetVeh* Register(uintptr_t veh)
    {
        if(NetVeh* v = FindVeh(veh)) return v;
        if((int)g_vehs.size() >= kMaxVehicles) return nullptr;
        if(Field<int>(veh, off::VehBaseType) == 6) return nullptr;      // поезда не трогаем
        NetVeh v;
        v.id = (uint16_t)(g_myBit | (g_nextId & 0x7FFF));
        if(!(g_nextId = (uint16_t)((g_nextId + 1) & 0x7FFF))) g_nextId = 1;
        v.veh = veh; v.ref = GetVehicleRef(veh); v.copy = false; v.model = ModelOf(veh);
        v.origCreatedBy = (int8_t)Field<uint8_t>(veh, off::VehCreatedBy);
        if(v.origCreatedBy == 1) Field<uint8_t>(veh, off::VehCreatedBy) = 2;   // пока общая — игра не удаляет
        g_vehs.push_back(v);
        Log(false, "[МАШИНЫ] Машина %d стала общей (номер %04X)", v.model, v.id);
        return &g_vehs.back();
    }

    // Чужая копия уходит, а свой CJ в ней сидит — забираем машину себе под новым номером
    void AdoptCopy(NetVeh& v)
    {
        uint16_t old = v.id;
        v.id = (uint16_t)(g_myBit | (g_nextId & 0x7FFF));
        if(!(g_nextId = (uint16_t)((g_nextId + 1) & 0x7FFF))) g_nextId = 1;
        v.copy = false; v.origCreatedBy = 1; v.haveState = false;
        Log(false, "[МАШИНЫ] Копия %04X стала своей (%04X): в ней сидишь ты", old, v.id);
    }

    void DropCopy(NetVeh& v, const char* why)
    {
        if(Valid(v))
        {
            if(g_ctx.local && PedVehicle(g_ctx.local) == v.veh) { AdoptCopy(v); return; }
            DeleteVehicle(v.veh);
        }
        v.veh = 0; v.ref = -1;
        Log(false, "[МАШИНЫ] Копия %04X убрана (%s)", v.id, why);
    }

    bool CreateCopy(NetVeh& v)
    {
        int model = v.st.model;
        if(!ModelLoaded(model))
        {
            if(!v.requestUs) { RequestModel(model, 2); v.requestUs = g_ctx.now; return false; }
            if(g_ctx.now - v.requestUs < 1500000) return false;
            LoadAllRequestedModels(false);
            if(!ModelLoaded(model))
            {
                if(g_ctx.now - v.requestUs > 8000000) { Log(true, "[МАШИНЫ] Модель %d не грузится — копии %04X не будет", model, v.id); v.dead = true; }
                return false;
            }
        }
        CVector pos = V(v.st.pos);
        uintptr_t veh = CreateCarForScript(model, pos, false);
        if(!veh) { Log(true, "[МАШИНЫ] Игра не создала машину %d", model); v.dead = true; return false; }
        v.veh = veh; v.ref = GetVehicleRef(veh); v.model = model;
        Field<uint8_t>(veh, off::VehCreatedBy) = 2;
        Field<int>(veh, off::VehDoorLock) = 1;                       // открыта
        memcpy((void*)(veh + off::VehColours), v.st.col, 4);
        SetMatrix(veh, pos, V(v.st.right), V(v.st.fwd));
        PhysicalRemoveAndAdd(veh);
        v.blownDone = false;
        Log(false, "[МАШИНЫ] Копия машины друга: модель %d, номер %04X", model, v.id);
        return true;
    }

    // ---------- состояние машины ----------
    void FillVehicle(NetVeh& v, bool driven, net::VehiclePacket& p)
    {
        uintptr_t veh = v.veh;
        p.hdr = net::MakeHeader(net::PKT_VEHICLE);
        p.seq = ++g_vehSeq; p.id = v.id; p.model = (uint16_t)ModelOf(veh);
        uintptr_t m = Field<uintptr_t>(veh, off::EntMatrix);
        Put(p.pos, GetPosition(veh));
        if(m) { Put(p.right, Field<CVector>(m, off::MatRight)); Put(p.fwd, Field<CVector>(m, off::MatForward)); }
        else  { float h = Field<float>(veh, off::EntPlacementHead); p.right[0] = cosf(h); p.right[1] = sinf(h); p.right[2] = 0; p.fwd[0] = -sinf(h); p.fwd[1] = cosf(h); p.fwd[2] = 0; }
        CVector ms = Field<CVector>(veh, off::PhysMoveSpeed);
        p.vel[0] = ms.x * 50.0f; p.vel[1] = ms.y * 50.0f; p.vel[2] = ms.z * 50.0f;
        Put(p.turn, Field<CVector>(veh, off::PhysTurnSpeed));
        p.health = Field<float>(veh, off::VehHealth);
        p.steer = Field<float>(veh, off::VehSteer);
        p.gas   = Field<float>(veh, off::VehGas);
        p.brake = Field<float>(veh, off::VehBrake);
        memcpy(p.col, (const void*)(veh + off::VehColours), 4);
        uint8_t f0 = Field<uint8_t>(veh, off::VehFlags), f5 = Field<uint8_t>(veh, off::VehFlags + 5);
        p.flags = 0;
        if(EntStatus(veh) == 5 || p.health <= 0) p.flags |= net::VF_BLOWN;
        if(f5 & 0x80) p.flags |= net::VF_SIREN;
        if(Field<uint32_t>(veh, off::VehHorn)) p.flags |= net::VF_HORN;
        if(f0 & 0x10) p.flags |= net::VF_ENGINE;
        if(f0 & 0x40) p.flags |= net::VF_LIGHTS;
        if(driven) p.flags |= net::VF_DRIVEN;
        if(AutomobileFamily(veh)) memcpy(p.wheels, (const void*)(veh + off::AutoWheels), 4);
        else memset(p.wheels, 0, 4);
        memset(p.pad, 0, sizeof(p.pad));
    }

    void ApplyVehicle(NetVeh& v, float dt)
    {
        uintptr_t veh = v.veh;
        const net::VehiclePacket& s = v.st;
        float age = (float)(g_ctx.now - v.stUs) / 1e6f;
        if(age > 0.3f) age = 0.3f;
        CVector target{ s.pos[0] + s.vel[0] * age, s.pos[1] + s.vel[1] * age, s.pos[2] + s.vel[2] * age };
        CVector cur = GetPosition(veh);
        uintptr_t m = Field<uintptr_t>(veh, off::EntMatrix);
        if(!m || Dist(cur, target) > kVehSnapDist)
        {
            SetMatrix(veh, target, V(s.right), V(s.fwd));
            PhysicalRemoveAndAdd(veh);
        }
        else
        {
            float k = 1.0f - expf(-kVehSmooth * dt);
            CVector r = Lerp(Field<CVector>(m, off::MatRight),   V(s.right), k);
            CVector f = Lerp(Field<CVector>(m, off::MatForward), V(s.fwd),   k);
            SetMatrix(veh, Lerp(cur, target, k), r, f);
        }
        Field<CVector>(veh, off::PhysMoveSpeed) = CVector{ s.vel[0] / 50.0f, s.vel[1] / 50.0f, s.vel[2] / 50.0f };
        Field<CVector>(veh, off::PhysTurnSpeed) = V(s.turn);
        if(!(s.flags & net::VF_BLOWN)) Field<float>(veh, off::VehHealth) = s.health;
        Field<float>(veh, off::VehSteer) = s.steer;
        Field<float>(veh, off::VehGas)   = s.gas;
        Field<float>(veh, off::VehBrake) = s.brake;
        memcpy((void*)(veh + off::VehColours), s.col, 4);
        uint8_t& f0 = Field<uint8_t>(veh, off::VehFlags);
        uint8_t& f5 = Field<uint8_t>(veh, off::VehFlags + 5);
        f0 = (uint8_t)((s.flags & net::VF_ENGINE) ? (f0 | 0x10) : (f0 & ~0x10));
        f0 = (uint8_t)((s.flags & net::VF_LIGHTS) ? (f0 | 0x40) : (f0 & ~0x40));
        f5 = (uint8_t)((s.flags & net::VF_SIREN)  ? (f5 | 0x80) : (f5 & ~0x80));
        Field<uint32_t>(veh, off::VehHorn) = (s.flags & net::VF_HORN) ? 1 : 0;
        if(AutomobileFamily(veh)) memcpy((void*)(veh + off::AutoWheels), s.wheels, 4);

        if((s.flags & net::VF_BLOWN) && !v.blownDone)
        {
            v.blownDone = true;
            if(EntStatus(veh) != 5)
            {
                uintptr_t vt = Field<uintptr_t>(veh, 0);
                ((void(*)(uintptr_t, uintptr_t, bool))Field<uintptr_t>(vt, off::VtBlowUpCar))(veh, 0, false);
                Log(false, "[МАШИНЫ] Машина %04X взорвалась у друга — взрываю копию", v.id);
            }
        }
    }

    // ---------- свой CJ: где сидит / куда садится ----------
    struct LocalSeat { uintptr_t veh = 0; int seat = -1; uintptr_t enterVeh = 0; int enterSeat = -1; };
    LocalSeat g_localSeat;

    LocalSeat ReadLocalSeat(uintptr_t local)
    {
        LocalSeat ls;
        uintptr_t veh = PedVehicle(local);
        if(veh) { ls.veh = veh; ls.seat = SeatOf(local, veh); if(ls.seat < 0) ls.seat = 0; }
        uintptr_t task = 0;
        int tt = ActiveTaskType(local, &task);
        if(!veh && task && (tt == TASK_ENTER_AS_DRIVER || tt == TASK_ENTER_AS_PASSENGER))
        {
            ls.enterVeh = Field<uintptr_t>(task, off::TaskEnterVehicle);
            if(ls.enterVeh && EntType(ls.enterVeh) != 2) ls.enterVeh = 0;
            ls.enterSeat = tt == TASK_ENTER_AS_DRIVER ? 0 : Field<int>(task, off::TaskEnterSeat) + 1;
            if(ls.enterSeat < 0) ls.enterSeat = 1;
        }
        return ls;
    }

    // ---------- второй CJ: посадка ----------
    uint64_t g_enterSinceUs = 0;     // когда дали задачу «сесть»
    uint16_t g_enterVehId = 0;
    uint64_t g_leaveSinceUs = 0;
    bool     g_remoteBusy = false;

    int FreePassengerSeat(uintptr_t veh, int preferred)
    {
        int maxp = Field<uint8_t>(veh, off::VehMaxPassengers);
        if(maxp > 8) maxp = 8;
        if(preferred >= 0 && preferred < maxp && !Field<uintptr_t>(veh, off::VehPassengers + (uintptr_t)preferred * 8)) return preferred;
        for(int i = 0; i < maxp; ++i) if(!Field<uintptr_t>(veh, off::VehPassengers + (uintptr_t)i * 8)) return i;
        return -1;
    }

    bool SeatFree(uintptr_t veh, int seat, uintptr_t forPed)
    {
        uintptr_t who = seat == 0 ? Field<uintptr_t>(veh, off::VehDriver)
                                  : Field<uintptr_t>(veh, off::VehPassengers + (uintptr_t)(seat - 1) * 8);
        return !who || who == forPed;
    }

    void GiveEnterTask(uintptr_t ped, uintptr_t veh, int seat)
    {
        uintptr_t mgr = TaskManager(ped);
        if(!mgr) return;
        void* t = TaskNew(0x80);
        if(!t) return;
        if(seat == 0) TaskEnterAsDriver(t, veh);
        else          TaskEnterAsPassenger(t, veh, seat - 1, false);
        TaskMgrSetTask(mgr, t, 3, false);
    }

    void GiveLeaveTask(uintptr_t ped, uintptr_t veh)
    {
        uintptr_t mgr = TaskManager(ped);
        if(!mgr) return;
        void* t = TaskNew(0x80);
        if(!t) return;
        TaskLeaveCar(t, veh, 0, 0, true, false);
        TaskMgrSetTask(mgr, t, 3, false);
    }

    void ClearPrimaryTask(uintptr_t ped)
    {
        uintptr_t mgr = TaskManager(ped);
        if(mgr) TaskMgrSetTask(mgr, nullptr, 3, false);
    }

    void WarpIn(uintptr_t ped, uintptr_t veh, int seat)
    {
        if(PedVehicle(ped)) WarpOut(ped);
        int door = seat == 0 ? kDriverDoor : DoorForPassengerSeat(veh, seat - 1);
        if(door < 0) door = 8;
        SetPedInCarDirect(ped, veh, door, seat == 0);
    }

    void ManageRemoteSeat(const net::PlayerStatePacket& s)
    {
        uintptr_t r = g_ctx.remote;
        g_remoteBusy = false;
        if(!r || !PedIsAlive(r) || (s.flags & net::PSF_DEAD)) return;

        NetVeh*   nv  = FindId(s.vehId);
        uintptr_t tv  = (nv && Valid(*nv)) ? nv->veh : 0;
        uintptr_t cur = PedVehicle(r);
        int tt = ActiveTaskType(r);
        uint64_t now = g_ctx.now;

        if(s.seat != net::kNoSeat)
        {
            int seat = s.seat;
            // уже в нужной машине (место могло быть занято — тогда сидит на другом, это нормально)
            if(cur && cur == tv) { g_remoteBusy = true; g_enterSinceUs = 0; g_leaveSinceUs = 0; return; }
            if(cur && cur != tv) { WarpOut(r); return; }
            if(!tv) return;                                  // копия ещё не готова — пусть идёт пешком
            g_remoteBusy = true;
            if(!SeatFree(tv, seat, r))
            {
                // место занято (например, за рулём сидишь ты) — свободное пассажирское
                int p = FreePassengerSeat(tv, seat - 1);
                if(seat == 0 || p < 0) { g_remoteBusy = false; return; }
                seat = p + 1;
            }
            float d = Dist(GetPosition(r), GetPosition(tv));
            if(d < 12.0f && (!g_enterSinceUs || g_enterVehId != s.vehId))
            {
                GiveEnterTask(r, tv, seat);
                g_enterSinceUs = now; g_enterVehId = s.vehId;
                return;
            }
            if(d >= 12.0f || (g_enterSinceUs && now - g_enterSinceUs > kEnterTimeoutUs))
            {
                WarpIn(r, tv, seat);
                g_enterSinceUs = 0;
                Log(false, "[МАШИНЫ] Второй CJ посажен в машину %04X (место %d) без анимации", s.vehId, seat);
            }
            return;
        }

        // Друг пешком
        g_enterSinceUs = (s.enterSeat != net::kNoSeat) ? g_enterSinceUs : 0;
        if(cur)
        {
            g_remoteBusy = true;
            if(!g_leaveSinceUs) { GiveLeaveTask(r, cur); g_leaveSinceUs = now; }
            else if(now - g_leaveSinceUs > kLeaveTimeoutUs) { WarpOut(r); g_leaveSinceUs = 0; }
            return;
        }
        g_leaveSinceUs = 0;
        if(tt == TASK_LEAVE_CAR) { g_remoteBusy = true; return; }

        if(s.enterSeat != net::kNoSeat && tv)
        {
            g_remoteBusy = true;
            if(!g_enterSinceUs || g_enterVehId != s.vehId)
            {
                int seat = s.enterSeat;
                if(!SeatFree(tv, seat, r)) { int p = FreePassengerSeat(tv, seat - 1); seat = p >= 0 ? p + 1 : -1; }
                if(seat >= 0 && Dist(GetPosition(r), GetPosition(tv)) < 15.0f)
                {
                    GiveEnterTask(r, tv, seat);
                    g_enterSinceUs = now; g_enterVehId = s.vehId;
                }
            }
            return;
        }
        // Передумал садиться — отменить нашу задачу «сесть»
        if(tt == TASK_ENTER_AS_DRIVER || tt == TASK_ENTER_AS_PASSENGER) ClearPrimaryTask(r);
    }

    void UpdateVehicles(float dt)
    {
        const net::PlayerStatePacket* rs = g_ctx.rs;
        uintptr_t local = g_ctx.local;
        CVector lp = GetPosition(local);
        bool haveRemote = rs != nullptr;
        CVector rp = haveRemote ? V(rs->pos) : lp;

        // Своя машина: в которой сижу или в которую сажусь — общая
        g_localSeat = ReadLocalSeat(local);
        uintptr_t mine = g_localSeat.veh ? g_localSeat.veh : g_localSeat.enterVeh;
        if(mine && !FindVeh(mine)) Register(mine);

        for(auto& v : g_vehs)
        {
            if(v.dead) continue;
            bool valid = Valid(v);
            bool remoteUses = haveRemote && rs->vehId == v.id;
            bool localUses  = valid && (v.veh == g_localSeat.veh || v.veh == g_localSeat.enterVeh);
            bool localDriver  = valid && g_localSeat.veh == v.veh && g_localSeat.seat == 0;
            bool remoteDriver = remoteUses && rs->seat == 0;

            if(!v.copy)
            {
                if(!valid) { SendGone(v.id); v.dead = true; Log(false, "[МАШИНЫ] Своя машина %04X исчезла у тебя", v.id); continue; }
                CVector vp = GetPosition(v.veh);
                if(!localUses && !remoteUses && Dist(vp, lp) > kVehKeepDist && (!haveRemote || Dist(vp, rp) > kVehKeepDist))
                {
                    ReleaseOwn(v); SendGone(v.id); v.dead = true;
                    Log(false, "[МАШИНЫ] Машина %04X далеко от обоих — больше не общая", v.id);
                    continue;
                }
            }
            else
            {
                if(v.haveState && g_ctx.now - v.stUs > kVehStaleUs && !localDriver)
                {   // хозяин замолчал
                    DropCopy(v, "нет вестей");
                    if(v.copy) v.dead = true;
                    continue;
                }
                if(!valid)
                {
                    if(v.haveState && Dist(V(v.st.pos), lp) < kCopyCreateDist) CreateCopy(v);
                    if(!Valid(v)) continue;
                    valid = true;
                }
                else if(!localUses && !remoteUses && Dist(GetPosition(v.veh), lp) > kCopyDropDist)
                {
                    DropCopy(v, "далеко");
                    continue;
                }
            }

            bool authority = localDriver || (!remoteDriver && !v.copy);
            if(authority)
            {
                bool moving = false;
                CVector ms = Field<CVector>(v.veh, off::PhysMoveSpeed);
                if(ms.x * ms.x + ms.y * ms.y + ms.z * ms.z > 0.0001f) moving = true;
                uint64_t interval = (localUses || remoteUses || moving) ? kVehFastUs : kVehSlowUs;
                if(g_ctx.now - v.lastSendUs >= interval)
                {
                    net::VehiclePacket p;
                    FillVehicle(v, localDriver, p);
                    SendGame(&p, sizeof(p));
                    v.lastSendUs = g_ctx.now;
                    // помним своё же состояние: когда управление перейдёт к другу, копия не «прыгнет» назад
                    v.st = p; v.stUs = g_ctx.now; v.haveState = true;
                }
            }
            else if(v.haveState)
            {
                ApplyVehicle(v, dt);
            }
        }
        for(size_t i = 0; i < g_vehs.size();) { if(g_vehs[i].dead) g_vehs.erase(g_vehs.begin() + i); else ++i; }
    }

    // ---------- оружие, выстрелы, удары ----------
    uint8_t  g_localMelee = 0;
    uint32_t g_lastMeleeFrame = 0;
    uint32_t g_frame = 0;
    bool     g_haveRemoteMelee = false;
    uint8_t  g_remoteMelee = 0;
    int      g_pendingRemoteMelee = 0;
    uint16_t g_shotSeq = 0;
    uint32_t g_lastShotFrame = 0;
    bool     g_inLocalFire = false;
    bool     g_shotCaptured = false;
    CVector  g_shotStart{}, g_shotEnd{};
    bool     g_shotHitMate = false;
    CVector  g_lastAim{};
    uint64_t g_lastAimUs = 0;
    bool     g_replaying = false;          // повтор выстрела друга
    bool     g_applyingDamage = false;     // урон от друга по событию
    bool     g_allowRemoteDamage = false;  // убить второго CJ (друг погиб)
    uint64_t g_nextWeaponTryUs = 0;
    bool     g_remoteAiming = false;
    int      g_lastRemoteWanted = -1;

    bool GunWeapon(int w) { return w >= 22 && w <= 38; }

    void EnsureWeapon(uintptr_t ped, int w)
    {
        if(CurrentWeaponType(ped) == w || g_ctx.now < g_nextWeaponTryUs) return;
        g_nextWeaponTryUs = g_ctx.now + 500000;
        if(w > 0)
        {
            int model = WeaponModel(w);
            if(model && !ModelLoaded(model)) { RequestModel(model, 2); LoadAllRequestedModels(false); }
            if(model && !ModelLoaded(model)) return;
            if(GetWeaponSlot(ped, w) < 0 || Field<int>(ped + off::PedWeapons + (uintptr_t)GetWeaponSlot(ped, w) * off::WeaponSize, off::WeapType) != w)
                GiveWeapon(ped, w, 9999, false);
        }
        SetCurrentWeaponType(ped, w);
    }

    void ReplayShot(const net::ShotPacket& p)
    {
        uintptr_t r = g_ctx.remote;
        if(!r || !PedIsAlive(r) || !CombatReady()) return;
        int w = p.weapon;
        if(w <= 0 || w > 46) return;
        g_nextWeaponTryUs = 0;
        EnsureWeapon(r, w);
        int slot = GetWeaponSlot(r, w);
        if(slot < 0 || slot > 12) return;
        uintptr_t weap = r + off::PedWeapons + (uintptr_t)slot * off::WeaponSize;
        if(Field<int>(weap, off::WeapType) != w) return;
        Field<int>(weap, off::WeapState) = 0;
        Field<uint32_t>(weap, off::WeapTimer) = 0;
        Field<int>(weap, off::WeapClip) = 500;
        Field<int>(weap, off::WeapTotal) = 9999;

        CVector rp = GetPosition(r);
        CVector delta{ rp.x - p.shooter[0], rp.y - p.shooter[1], rp.z - p.shooter[2] };
        if(Dist(rp, V(p.shooter)) > 15.0f) return;           // второй CJ ещё не там — не стрелять «из воздуха»
        CVector start{ p.start[0] + delta.x, p.start[1] + delta.y, p.start[2] + delta.z };
        CVector end = V(p.end);
        if((p.flags & net::SHOT_HIT_MATE) && g_ctx.local)
        {   // друг попал в тебя: след пули — в тебя (урон придёт отдельным событием)
            end = GetPosition(g_ctx.local); end.z += 0.4f;
        }
        g_replaying = true;
        WeaponFire(weap, r, &start, &start, 0, &end, nullptr);
        g_replaying = false;
    }

    void UpdateCombat()
    {
        uintptr_t r = g_ctx.remote;
        const net::PlayerStatePacket* rs = g_ctx.rs;
        if(!r || !rs || !CombatReady()) return;
        bool dead = (rs->flags & net::PSF_DEAD) != 0;

        if(dead)
        {
            if(PedIsAlive(r))
            {
                g_allowRemoteDamage = true;
                GenerateDamageEvent(r, 0, 0, 1000, 3, 0);
                g_allowRemoteDamage = false;
                if(PedIsAlive(r)) Field<float>(r, off::PedHealth) = 0.0f;
                Log(false, "Друг погиб — второй CJ падает");
            }
            return;
        }
        if(!PedIsAlive(r)) return;

        EnsureWeapon(r, rs->weapon);

        // Удары: счётчик изменился — второй CJ бьёт
        if(!g_haveRemoteMelee) { g_haveRemoteMelee = true; g_remoteMelee = rs->meleeCount; }
        else if(rs->meleeCount != g_remoteMelee)
        {
            int n = (uint8_t)(rs->meleeCount - g_remoteMelee);
            g_remoteMelee = rs->meleeCount;
            if(n > 0 && n < 8 && !PedVehicle(r)) g_pendingRemoteMelee += n;
        }

        // Прицел: поднять оружие в сторону точки
        bool aim = (rs->flags & net::PSF_AIMING) && GunWeapon(rs->weapon) && !PedVehicle(r);
        if(aim)
        {
            CVector rp = GetPosition(r), at = V(rs->aim);
            float h = atan2f(-(at.x - rp.x), at.y - rp.y);
            PedSetAimFlag(r, h);
            PointGunAtPosition(r + off::PedIK, at, 1.0f);
            g_remoteAiming = true;
        }
        else if(g_remoteAiming) { PedClearAimFlag(r); g_remoteAiming = false; }
    }

    void UpdateWanted()
    {
        if(g_echo || !g_ctx.rs || !FindPlayerWanted) return;
        uintptr_t w = FindPlayerWanted(0);
        if(!w) return;
        int mine = Field<int>(w, off::WantedStars);
        int theirs = g_ctx.rs->wanted;
        if(theirs > 6) return;
        if(g_lastRemoteWanted < 0) { g_lastRemoteWanted = theirs; return; }
        if(theirs == g_lastRemoteWanted) return;
        if(theirs > mine)
        {
            WantedSetLevel(w, theirs);
            Log(false, "[РОЗЫСК] У друга %d зв. — у тебя тоже", theirs);
        }
        else if(theirs < g_lastRemoteWanted && mine == g_lastRemoteWanted)
        {
            WantedCheatLevel(w, theirs);
            Log(false, "[РОЗЫСК] Друг сбросил розыск до %d — у тебя тоже", theirs);
        }
        g_lastRemoteWanted = theirs;
    }

    void ZeroResponse(uintptr_t resp)
    {
        Field<float>(resp, 0) = 0.0f; Field<float>(resp, 4) = 0.0f;
        Field<uint8_t>(resp, 8) = 0; Field<uint8_t>(resp, 9) = 0;
        Field<uint8_t>(resp, 10) = 1; Field<uint8_t>(resp, 11) = 0;
    }

    bool IsLocalSide(uintptr_t ent)
    {
        uintptr_t l = g_ctx.local;
        if(!ent || !l) return false;
        if(ent == l) return true;
        return EntType(ent) == 2 && Field<uintptr_t>(ent, off::VehDriver) == l;
    }
    bool IsRemoteSide(uintptr_t ent)
    {
        uintptr_t r = g_ctx.remote;
        if(!ent || !r) return false;
        if(ent == r) return true;
        return EntType(ent) == 2 && Field<uintptr_t>(ent, off::VehDriver) == r;
    }
}

// ---------- перехваты ----------

// Кнопка «сесть» у машины, где за рулём друг: садимся пассажиром, а не выкидываем его
DECL_HOOKv(EnterAsDriverCtor, void* task, uintptr_t veh)
{
    if(g_active && veh && g_ctx.remote && Field<uintptr_t>(veh, off::VehDriver) == g_ctx.remote)
    {
        int seat = FreePassengerSeat(veh, 0);
        if(seat >= 0)
        {
            TaskEnterAsPassenger(task, veh, seat, false);
            Log(false, "[МАШИНЫ] За рулём друг — садишься пассажиром (место %d)", seat + 1);
            return;
        }
    }
    EnterAsDriverCtor(task, veh);
}

// Свои удары считаем, второму CJ «нажимаем» удар
DECL_HOOK(bool, MeleeJustDown, uintptr_t pad)
{
    if(g_active && g_ctx.remote && pad == GetPad(1))
    {
        if(g_pendingRemoteMelee > 0) { --g_pendingRemoteMelee; return true; }
        return false;
    }
    bool r = MeleeJustDown(pad);
    if(r && pad == GetPad(0) && g_lastMeleeFrame != g_frame) { g_lastMeleeFrame = g_frame; ++g_localMelee; }
    return r;
}

// Попадание пули: запоминаем линию выстрела своего CJ
DECL_HOOKv(DoBulletImpact, uintptr_t weapon, uintptr_t owner, uintptr_t hit, CVector* start, CVector* end, void* colPoint, int inc)
{
    if(g_active && owner && owner == g_ctx.local && start && end)
    {
        if(g_inLocalFire)
        {
            if(!g_shotCaptured) { g_shotCaptured = true; g_shotStart = *start; g_shotEnd = *end; g_shotHitMate = hit && hit == g_ctx.remote; }
        }
        else if(g_lastShotFrame != g_frame)
        {   // выстрел не через CWeapon::Fire (стрельба из машины и т.п.) — шлём по попаданию
            g_lastShotFrame = g_frame;
            net::ShotPacket p{};
            p.hdr = net::MakeHeader(net::PKT_SHOT);
            p.seq = ++g_shotSeq;
            p.weapon = (uint8_t)CurrentWeaponType(owner);
            Put(p.shooter, GetPosition(owner));
            Put(p.start, *start); Put(p.end, *end);
            if(hit && hit == g_ctx.remote) p.flags |= net::SHOT_HIT_MATE;
            g_lastAim = *end; g_lastAimUs = g_ctx.now;
            SendGame(&p, sizeof(p));
        }
    }
    DoBulletImpact(weapon, owner, hit, start, end, colPoint, inc);
}

// Выстрел своего CJ → другу (повторит у себя)
DECL_HOOK(bool, WeaponFireHook, uintptr_t weapon, uintptr_t owner, CVector* start, CVector* barrel, uintptr_t targetEnt, CVector* target, CVector* alt)
{
    bool local = g_active && owner && owner == g_ctx.local && !g_replaying;
    if(!local) return WeaponFireHook(weapon, owner, start, barrel, targetEnt, target, alt);

    g_inLocalFire = true; g_shotCaptured = false; g_shotHitMate = false;
    bool r = WeaponFireHook(weapon, owner, start, barrel, targetEnt, target, alt);
    g_inLocalFire = false;
    if(!r) return r;

    net::ShotPacket p{};
    p.hdr = net::MakeHeader(net::PKT_SHOT);
    p.seq = ++g_shotSeq;
    p.weapon = (uint8_t)Field<int>(weapon, off::WeapType);
    CVector op = GetPosition(owner);
    Put(p.shooter, op);
    if(g_shotCaptured) { Put(p.start, g_shotStart); Put(p.end, g_shotEnd); if(g_shotHitMate) p.flags |= net::SHOT_HIT_MATE; }
    else
    {
        CVector s0 = start ? *start : CVector{ op.x, op.y, op.z + 0.5f };
        CVector e0;
        if(target) e0 = *target;
        else
        {
            float h = Field<float>(owner, off::PedRotCurrent);
            e0 = CVector{ s0.x - sinf(h) * 40.0f, s0.y + cosf(h) * 40.0f, s0.z };
        }
        Put(p.start, s0); Put(p.end, e0);
    }
    g_lastAim = V(p.end); g_lastAimUs = g_ctx.now;
    SendGame(&p, sizeof(p));
    return r;
}

// Урон: второго CJ у себя не раним (урон считает телефон друга),
// по себе — только событием от друга
DECL_HOOKv(ComputeDamage, uintptr_t calc, uintptr_t victim, uintptr_t resp, bool painAudio)
{
    if(g_active && victim)
    {
        uintptr_t inflictor = Field<uintptr_t>(calc, off::CalcInflictor);
        if(victim == g_ctx.remote && !g_allowRemoteDamage)
        {
            if(IsLocalSide(inflictor) && g_ff && !g_echo && g_ctx.connected)
            {
                net::DamageEvent e{};
                e.type = net::REL_DAMAGE;
                e.weapon = (uint8_t)Field<int>(calc, off::CalcWeapon);
                e.piece = (uint8_t)Field<int>(calc, off::CalcPiece);
                e.viaVehicle = inflictor != g_ctx.local;
                e.amount = Field<float>(calc, off::CalcDamage);
                SendReliable(&e, sizeof(e));
            }
            ZeroResponse(resp);
            return;
        }
        if(victim == g_ctx.local && IsRemoteSide(inflictor) && !g_applyingDamage)
        {
            ZeroResponse(resp);
            return;
        }
    }
    ComputeDamage(calc, victim, resp, painAudio);
}

// ---------- снаружи ----------

bool Install(void* h, FLogger* log)
{
    g_log = log;
    if(!VehiclesReady() || !CombatReady())
    {
        Log(true, "Машины/оружие: не все функции игры найдены — кооп только пешком");
        return false;
    }
    struct HookDef { const char* sym; void* fn; void** orig; };
    const HookDef hooks[] = {
        { "_ZN28CTaskComplexEnterCarAsDriverC2EP8CVehicle", (void*)HookOf_EnterAsDriverCtor, (void**)&EnterAsDriverCtor },
        { "_ZN4CPad19MeleeAttackJustDownEv",                (void*)HookOf_MeleeJustDown,     (void**)&MeleeJustDown },
        { "_ZN7CWeapon14DoBulletImpactEP7CEntityS1_P7CVectorS3_P9CColPointi", (void*)HookOf_DoBulletImpact, (void**)&DoBulletImpact },
        { "_ZN7CWeapon4FireEP7CEntityP7CVectorS3_S1_S3_S3_", (void*)HookOf_WeaponFireHook, (void**)&WeaponFireHook },
        { "_ZN28CPedDamageResponseCalculator21ComputeDamageResponseEP4CPedR18CPedDamageResponseb", (void*)HookOf_ComputeDamage, (void**)&ComputeDamage },
    };
    for(const auto& d : hooks)
        if(!aml->GetSym(h, d.sym)) { Log(true, "Машины/оружие: нет символа %s — кооп только пешком", d.sym); return false; }
    for(const auto& d : hooks)
        aml->Hook((void*)aml->GetSym(h, d.sym), d.fn, d.orig);
    // Наш вызов «сесть водителем» тоже идёт через перехват — так и задумано (он ничего не меняет,
    // если за рулём не друг). Пассажирский конструктор не перехвачен.
    g_installed = true;
    Log(false, "Машины, оружие и урон: перехватов %d", (int)(sizeof(hooks) / sizeof(hooks[0])));
    return true;
}

void SetSession(net::Session* s) { g_session = s; }

void Capture(const Ctx& c, net::PlayerStatePacket& s)
{
    s.weapon = 0; s.meleeCount = g_localMelee; s.wanted = 0;
    s.vehId = 0; s.seat = net::kNoSeat; s.enterSeat = net::kNoSeat;
    s.area = CurrArea ? *CurrArea : 0;
    uintptr_t l = c.local;
    if(l) Put(s.aim, GetPosition(l));
    if(!g_installed || !l || c.role == net::Role::Off) return;
    g_myBit = c.role == net::Role::Client ? 0x8000 : 0;

    s.weapon = (uint8_t)CurrentWeaponType(l);
    if(!PedIsAlive(l) || Field<float>(l, off::PedHealth) <= 0.0f) s.flags |= net::PSF_DEAD;
    if(Field<uint8_t>(l, off::PedFlagsWord) & 0x10) s.flags |= net::PSF_AIMING;
    if(Field<uint8_t>(l, off::PedFlags587) & 0x04) s.flags |= net::PSF_DUCK;
    if(c.role != net::Role::Client && c.friendlyFireLocal) s.flags |= net::PSF_FF;

    // Куда целится: захваченная цель, свежий выстрел или прямо перед собой
    uintptr_t lock = Field<uintptr_t>(l, off::PedLockOnTarget);
    CVector lp = GetPosition(l);
    CVector aim;
    if(lock && (EntType(lock) == 2 || EntType(lock) == 3)) { aim = GetPosition(lock); aim.z += 0.5f; s.flags |= net::PSF_AIMING; }
    else if(g_lastAimUs && c.now - g_lastAimUs < 600000) aim = g_lastAim;
    else { float h = Field<float>(l, off::PedRotCurrent); aim = CVector{ lp.x - sinf(h) * 10.0f, lp.y + cosf(h) * 10.0f, lp.z + 0.5f }; }
    Put(s.aim, aim);

    if(FindPlayerWanted) { uintptr_t w = FindPlayerWanted(0); if(w) s.wanted = (uint8_t)Field<int>(w, off::WantedStars); }

    // Машина: та, в которой сидишь, или в которую садишься
    LocalSeat ls = ReadLocalSeat(l);
    if(ls.veh)
    {
        NetVeh* v = FindVeh(ls.veh);
        if(!v) v = Register(ls.veh);
        if(v) { s.vehId = v->id; s.seat = (uint8_t)ls.seat; }
    }
    else if(ls.enterVeh)
    {
        NetVeh* v = FindVeh(ls.enterVeh);
        if(!v) v = Register(ls.enterVeh);
        if(v) { s.vehId = v->id; s.enterSeat = (uint8_t)ls.enterSeat; }
    }
}

void Frame(const Ctx& c)
{
    ++g_frame;
    g_ctx = c;
    g_echo = c.role == net::Role::Echo;
    bool wasActive = g_active;
    g_active = g_installed && c.local && (c.role == net::Role::Echo || c.role == net::Role::Host || c.role == net::Role::Client);
    if(!g_active) { if(wasActive) Reset("кооп выключен"); return; }
    g_myBit = c.role == net::Role::Client ? 0x8000 : 0;

    // Эхо: свои номера машин у «двойника» — чужие
    static net::PlayerStatePacket echoRs;
    if(g_echo && c.rs)
    {
        echoRs = *c.rs;
        if(echoRs.vehId) echoRs.vehId ^= 0x8000;
        echoRs.aim[0] += c.echoOffset;
        g_ctx.rs = &echoRs;
        EchoDeliver();
    }

    // Огонь по своим: решает хост
    if(c.role == net::Role::Client) g_ff = g_ctx.rs ? (g_ctx.rs->flags & net::PSF_FF) != 0 : false;
    else g_ff = c.friendlyFireLocal;

    // Второго CJ нельзя вытащить из машины
    if(c.remote) Field<uint8_t>(c.remote, off::PedFlags587) |= 0x20;

    UpdateVehicles(c.dt);
    if(g_ctx.rs && c.remote) ManageRemoteSeat(*g_ctx.rs);
    else g_remoteBusy = false;
    UpdateCombat();
    UpdateWanted();
}

void OnPacket(uint8_t type, const uint8_t* data, int len)
{
    if(!g_installed) return;
    switch(type)
    {
        case net::PKT_VEHICLE:
        {
            if(len != sizeof(net::VehiclePacket)) return;
            const auto* p = (const net::VehiclePacket*)data;
            if(!p->id || (!Foreign(p->id) && !FindId(p->id))) return;   // своя машина, которую мы уже забыли
            NetVeh* v = FindId(p->id);
            if(!v)
            {
                if((int)g_vehs.size() >= kMaxVehicles) return;
                NetVeh nv; nv.id = p->id; nv.copy = true;
                g_vehs.push_back(nv);
                v = &g_vehs.back();
            }
            if(v->haveState && (int32_t)(p->seq - v->st.seq) <= 0 && g_ctx.now - v->stUs < 2000000) return;
            v->st = *p; v->stUs = g_ctx.now; v->haveState = true;
            break;
        }
        case net::PKT_VEHICLE_GONE:
        {
            if(len != sizeof(net::VehicleGonePacket)) return;
            NetVeh* v = FindId(((const net::VehicleGonePacket*)data)->id);
            if(v && v->copy) { DropCopy(*v, "друг отпустил машину"); if(v->copy) v->dead = true; }
            break;
        }
        case net::PKT_SHOT:
            if(len == sizeof(net::ShotPacket)) ReplayShot(*(const net::ShotPacket*)data);
            break;
        case net::PKT_RELIABLE:
            if(len >= 1 && data[0] == net::REL_DAMAGE && len == sizeof(net::DamageEvent))
            {
                const auto* e = (const net::DamageEvent*)data;
                uintptr_t l = g_ctx.local;
                if(!l || !PedIsAlive(l) || !g_ff) return;
                g_applyingDamage = true;
                GenerateDamageEvent(l, g_ctx.remote, e->weapon, (int)(e->amount + 0.5f), e->piece, 0);
                g_applyingDamage = false;
                Log(false, "[УРОН] Друг попал в тебя: %.0f (оружие %d)", e->amount, e->weapon);
            }
            break;
    }
}

bool RemoteBusy() { return g_active && g_remoteBusy; }
bool RemoteShouldBeDead() { return g_active && g_ctx.rs && (g_ctx.rs->flags & net::PSF_DEAD); }

void BeforeRemoteDespawn(uintptr_t remote)
{
    if(g_installed && remote && PedVehicle(remote)) WarpOut(remote);
    if(g_ctx.remote == remote) g_ctx.remote = 0;
    g_enterSinceUs = 0; g_leaveSinceUs = 0; g_remoteBusy = false;
    g_haveRemoteMelee = false; g_pendingRemoteMelee = 0; g_remoteAiming = false;
}

void Reset(const char* why)
{
    for(auto& v : g_vehs)
    {
        if(v.copy)
        {
            if(!Valid(v)) continue;
            if(g_ctx.local && PedVehicle(g_ctx.local) == v.veh) Field<uint8_t>(v.veh, off::VehCreatedBy) = 1;   // сидишь в ней — оставить обычной машиной
            else DeleteVehicle(v.veh);
        }
        else ReleaseOwn(v);
    }
    if(!g_vehs.empty()) Log(false, "[МАШИНЫ] Общие машины забыты (%s): %d", why, (int)g_vehs.size());
    g_vehs.clear();
    g_echoQ.clear();
    g_lastRemoteWanted = -1;
    g_remoteBusy = false; g_enterSinceUs = 0; g_leaveSinceUs = 0;
    g_haveRemoteMelee = false; g_pendingRemoteMelee = 0; g_remoteAiming = false;
}

int TrackedVehicles() { return (int)g_vehs.size(); }

}}
