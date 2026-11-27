#include "sync.h"
#include "trace/ctrace.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include <string>
#include <vector>

#include "game/gta.h"
#include "coop/crowd.h"
#include "coop/mission.h"

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
    constexpr int      kMaxVehicles    = 64;     // свои, копии и машины вокруг друга (общий мир)
    constexpr float    kCrowdVehKeep   = 160.0f; // машина вокруг друга: дальше от него — не общая
    constexpr int      kDriverDoor     = 10;        // дверь водителя (CCarEnterExit)
    constexpr uint64_t kEnterTimeoutUs = 7000000;   // не сел с анимацией за 7 с — сажаем сразу
    constexpr uint64_t kLeaveTimeoutUs = 2500000;
    constexpr uint64_t kEchoDelayUs    = 500000;
    constexpr float    kEchoCarSide    = 6.0f;      // эхо: машина двойника в 6 м справа от твоей
    constexpr uintptr_t kVehPoolEntry  = 0xC68;     // размер записи пула машин (CPlane — самая большая)

    // Типы задач игры
    constexpr int TASK_ENTER_AS_PASSENGER = 700;
    constexpr int TASK_ENTER_AS_DRIVER    = 701;
    constexpr int TASK_LEAVE_CAR          = 704;

    FLogger*      g_log = nullptr;
    uintptr_t     g_libBase = 0;
    uintptr_t     g_pedTypesSym = 0;    // CPedType::ms_apPedTypes: 32 типа × 5 масок отношений
    net::Session* g_session = nullptr;
    bool          g_installed = false;

    void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool error, const char* fmt, ...)
    {
        char buf[512];
        va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        if(g_log) { if(error) g_log->Error("%s", buf); else g_log->Info("%s", buf); }   // → SA_LOG.txt (FLogger::Sink)
        else ctrace::FromLog(error, buf);
    }

    // ---------- текущий кадр ----------
    Ctx        g_ctx;
    bool       g_active = false;   // кооп или эхо идёт
    bool       g_echo = false;
    uint16_t   g_myBit = 0;        // старший бит своих номеров машин (хост 0, клиент 0x8000)
    bool       g_ff = false;       // огонь по своим (решение хоста)
    bool       g_sharedWanted = true; // общий розыск (решение хоста)

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
        bool      crowd = false;       // машина вокруг друга (общий мир): игра её убирает как обычно
        // для подробного лога
        float     lastErr = 0;         // расхождение с присланным, м
        int       mode = -1;           // 0 перенос, 1 стоит, 2 едет
        int       snaps = 0;           // переносов за секунду
        int       corrections = 0;     // поправок положения за секунду
        float     lastYawErr = 0;
        uint32_t  appliedSeq = 0;      // последний применённый пакет
        bool      appliedSeqValid = false;
        uint32_t  pktsIn = 0, pktsOut = 0;
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
            {   // машину двойника ставим сбоку от твоей (вдоль её правого вектора), чтобы не столкнулись
                auto* p = (net::VehiclePacket*)e.data; p->id ^= 0x8000;
                CVector r = Norm(CVector{ p->right[0], p->right[1], 0.0f });
                p->pos[0] += r.x * kEchoCarSide; p->pos[1] += r.y * kEchoCarSide;
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

    bool DoorAnimsReady(uintptr_t veh, bool load);

    // Высадить персонажа из машины сразу (как скриптовая WARP_CHAR_FROM_CAR_TO_COORD)
    void WarpOut(uintptr_t ped)
    {
        uintptr_t veh = PedVehicle(ped);
        if(!veh) return;
        DoorAnimsReady(veh, true);                   // высадить нужно всё равно (машину могут удалять) — но с анимациями
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

    NetVeh* Register(uintptr_t veh, bool crowd = false)
    {
        if(NetVeh* v = FindVeh(veh)) { if(!crowd) v->crowd = false; return v; }
        if((int)g_vehs.size() >= kMaxVehicles) return nullptr;
        if(Field<int>(veh, off::VehBaseType) == 6) return nullptr;      // поезда не трогаем
        NetVeh v;
        v.id = (uint16_t)(g_myBit | (g_nextId & 0x7FFF));
        if(!(g_nextId = (uint16_t)((g_nextId + 1) & 0x7FFF))) g_nextId = 1;
        v.veh = veh; v.ref = GetVehicleRef(veh); v.copy = false; v.model = ModelOf(veh);
        v.origCreatedBy = (int8_t)Field<uint8_t>(veh, off::VehCreatedBy);
        v.crowd = crowd;
        if(!crowd && v.origCreatedBy == 1) Field<uint8_t>(veh, off::VehCreatedBy) = 2;   // пока общая — игра не удаляет
        g_vehs.push_back(v);
        if(!crowd) Log(false, "[МАШИНЫ] Машина %d стала общей (номер %04X)", v.model, v.id);
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

    // Пустые случайные машины на месте копии убираем: иначе копия появится внутри них,
    // и игра раскидает обе машины столкновением (в тесте машины улетали за 200 м)
    uintptr_t g_vehPoolSym = 0;
    void ClearSpaceFor(const CVector& pos, float radius)
    {
        if(!g_vehPoolSym) return;
        uintptr_t pool = *(uintptr_t*)g_vehPoolSym;
        if(!pool) return;
        uintptr_t objs = Field<uintptr_t>(pool, 0), flags = Field<uintptr_t>(pool, 8);
        int size = Field<int>(pool, 0x10);
        if(!objs || !flags || size <= 0 || size > 4096) return;
        int removed = 0;
        for(int i = 0; i < size; ++i)
        {
            if(Field<uint8_t>(flags, i) & 0x80) continue;
            uintptr_t veh = objs + (uintptr_t)i * kVehPoolEntry;
            if(FindVeh(veh) || Field<uint8_t>(veh, off::VehCreatedBy) != 1) continue;
            if(Field<uintptr_t>(veh, off::VehDriver)) continue;
            bool occupied = false;
            for(int k = 0; k < 8; ++k) if(Field<uintptr_t>(veh, off::VehPassengers + (uintptr_t)k * 8)) { occupied = true; break; }
            if(occupied || veh == PedVehicle(g_ctx.local) || veh == PedVehicle(g_ctx.remote)) continue;
            if(Dist(GetPosition(veh), pos) > radius) continue;
            DeleteVehicle(veh);
            ++removed;
        }
        if(removed) Log(false, "[МАШИНЫ] На месте копии убрано пустых машин: %d", removed);
    }

    bool CreateCopy(NetVeh& v)
    {
        int model = v.st.model;
        if(!ModelLoaded(model))
        {
            if(!ModelOnDisk(model)) { Log(true, "[МАШИНЫ] Модели %d нет в файлах игры — копии %04X не будет", model, v.id); v.dead = true; return false; }
            if(!v.requestUs) { RequestModel(model, 2); v.requestUs = g_ctx.now; return false; }
            if(net::Since(g_ctx.now, v.requestUs) < 1500000) return false;
            LoadAllRequestedModels(false);
            if(!ModelLoaded(model))
            {
                if(net::Since(g_ctx.now, v.requestUs) > 8000000) { Log(true, "[МАШИНЫ] Модель %d не грузится — копии %04X не будет", model, v.id); v.dead = true; }
                return false;
            }
        }
        CVector pos = V(v.st.pos);
        ClearSpaceFor(pos, 5.5f);
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
        p.steer = Field<float>(veh, off::VehSteerInput);         // руль -1..1 (как его крутит водитель)
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
        if(f0 & 0x20) p.flags |= net::VF_HANDBRAKE;
        if(driven) p.flags |= net::VF_DRIVEN;
        if(AutomobileFamily(veh)) memcpy(p.wheels, (const void*)(veh + off::AutoWheels), 4);
        else memset(p.wheels, 0, 4);
        memset(p.pad, 0, sizeof(p.pad));
    }

    float WrapA(float a) { while(a > (float)M_PI) a -= 2.0f * (float)M_PI; while(a < -(float)M_PI) a += 2.0f * (float)M_PI; return a; }

    void ApplyCosmetics(NetVeh& v);
    void ApplyVehicle(NetVeh& v, float dt)
    {
        (void)dt;
        uintptr_t veh = v.veh;
        const net::VehiclePacket& s = v.st;
        // Как в больших мультиплеерах: поправляем машину только когда пришёл НОВЫЙ пакет (20 раз/с),
        // а между пакетами её ведёт физика игры с педалями и рулём водителя (пульт 2).
        // Раньше мод правил скорость и курс каждый кадр — колёса проскальзывали (дым, звук ручника),
        // машина «переставлялась» вбок без поворота колёс.
        if(v.appliedSeq == s.seq && v.appliedSeqValid) return ApplyCosmetics(v);
        v.appliedSeq = s.seq; v.appliedSeqValid = true;

        float age = (float)(net::Since(g_ctx.now, v.stUs)) / 1e6f;
        if(age > 0.15f) age = 0.15f;
        CVector target{ s.pos[0] + s.vel[0] * age, s.pos[1] + s.vel[1] * age, s.pos[2] + s.vel[2] * age };
        CVector cur = GetPosition(veh);
        uintptr_t m = Field<uintptr_t>(veh, off::EntMatrix);
        float dx = target.x - cur.x, dy = target.y - cur.y, dz = target.z - cur.z;
        float err = sqrtf(dx * dx + dy * dy + dz * dz);
        float speed = sqrtf(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]);
        float yawErr = 0;
        if(m)
        {
            CVector f = Field<CVector>(m, off::MatForward);
            yawErr = fabsf(WrapA(atan2f(-s.fwd[0], s.fwd[1]) - atan2f(-f.x, f.y))) * 57.2958f;
        }
        v.lastErr = err; v.lastYawErr = yawErr;

        if(!m || err > kVehSnapDist || fabsf(dz) > 3.0f)
        {
            SetMatrix(veh, target, V(s.right), V(s.fwd));
            PhysicalRemoveAndAdd(veh);
            Field<CVector>(veh, off::PhysMoveSpeed) = CVector{ s.vel[0] / 50.0f, s.vel[1] / 50.0f, s.vel[2] / 50.0f };
            Field<CVector>(veh, off::PhysTurnSpeed) = V(s.turn);
            v.mode = 0; ++v.snaps;
            Log(false, "[МАШИНА %04X] ПЕРЕНОС: расхождение %.2f м (по высоте %.2f), курс %.0f°, скорость друга %.1f м/с",
                v.id, err, dz, yawErr, speed);
            return ApplyCosmetics(v);
        }
        if(speed < 0.3f && err < 0.8f && yawErr < 10.0f)
        {
            // Стоит — не трогаем совсем: стоит сама, на земле, как в обычной игре
            v.mode = 1;
            return ApplyCosmetics(v);
        }
        if(err > 1.2f || yawErr > 12.0f)
        {
            // Заметно разошлись — ставим положение и поворот, как у друга (один раз на пакет)
            SetMatrix(veh, target, V(s.right), V(s.fwd));
            ++v.corrections;
        }
        // Скорость и вращение — как у друга, плюс небольшая поправка к точке (не больше 3 м/с)
        float cx = dx * 2.0f, cy = dy * 2.0f;
        float cl = sqrtf(cx * cx + cy * cy);
        if(cl > 3.0f) { cx *= 3.0f / cl; cy *= 3.0f / cl; }
        Field<CVector>(veh, off::PhysMoveSpeed) = CVector{ (s.vel[0] + cx) / 50.0f, (s.vel[1] + cy) / 50.0f, s.vel[2] / 50.0f };
        Field<CVector>(veh, off::PhysTurnSpeed) = V(s.turn);
        v.mode = 2;
        ApplyCosmetics(v);
    }

    // Внешний вид и состояние (каждый кадр — дёшево и без физики)
    void ApplyCosmetics(NetVeh& v)
    {
        uintptr_t veh = v.veh;
        const net::VehiclePacket& s = v.st;
        if(!(s.flags & net::VF_BLOWN)) Field<float>(veh, off::VehHealth) = s.health;
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

    // Управление машиной за второго CJ (пульт 2): как жмёт друг — так «жмём» и мы,
    // чтобы физика копии ехала сама, без борьбы с тормозом
    struct CarInput { bool on = false; int16_t accel = 0, brake = 0, steer = 0, handbrake = 0; };
    CarInput g_carIn;

    void UpdateCarInput()
    {
        g_carIn = CarInput();
        uintptr_t r = g_ctx.remote;
        uintptr_t veh = PedVehicle(r);
        if(!veh || Field<uintptr_t>(veh, off::VehDriver) != r) return;
        NetVeh* v = FindVeh(veh);
        if(!v || !v->haveState) return;
        const net::VehiclePacket& s = v->st;
        auto clamp = [](float x) { return (int16_t)(x < -255 ? -255 : x > 255 ? 255 : x); };
        g_carIn.on = true;
        g_carIn.accel = clamp(s.gas > 0 ? s.gas * 255.0f : 0.0f);
        float br = s.gas < 0 ? -s.gas : s.brake;
        g_carIn.brake = clamp(br * 255.0f);
        // Игра: руль = −(пульт)/128 → пульт = −руль·128 (CAutomobile::ProcessControlInputs)
        float st = -s.steer * 128.0f;
        g_carIn.steer = (int16_t)(st < -128 ? -128 : st > 128 ? 128 : st);
        float spd = sqrtf(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]);
        g_carIn.handbrake = ((s.flags & net::VF_HANDBRAKE) || (spd < 0.2f && s.gas == 0.0f)) ? 255 : 0;
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
    bool     g_jackWaitLogged = false;
    uint16_t g_enterVehId = 0;
    uint64_t g_leaveSinceUs = 0;
    uint64_t g_onFootSinceUs = 0;    // с какого момента друг «пешком» (для задержки выхода)
    constexpr uint64_t kOnFootConfirmUs = 400000;
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

    // ---------- анимации дверей (вылет 1.4.4) ----------
    // У машины может быть своя группа анимаций (фургон, автобус, байк…) в отдельном блоке. Если блок не загружен,
    // игра при посадке/высадке берёт пустую анимацию двери → SIGSEGV в CVehicleAnimGroup::ComputeAnimDoorOffsets
    // (тест 1.4.4: хост, второму CJ дали «сесть»). Перед посадкой и высадкой второго CJ проверяем и, если надо, грузим блок.
    constexpr int kIfpBase = 25575;                       // номера блоков анимаций в CStreaming (2.10: 0x63E7)
    bool DoorAnimsReady(uintptr_t veh, bool load)
    {
        if(!veh || !VehicleAnimGroups || !AnimAssocGroupsVar || !VehAnimGroupGetGroup) return true;   // нечем проверить — как раньше
        uintptr_t hd = Field<uintptr_t>(veh, 0x4A8);       // CVehicle::m_pHandlingData; +0xDE — группа анимаций
        uintptr_t groups = Field<uintptr_t>(AnimAssocGroupsVar, 0);
        if(!hd || !groups) return true;
        uintptr_t vag = VehicleAnimGroups + (uintptr_t)Field<uint8_t>(hd, 0xDE) * 0x94;
        static const int kIds[] = { 359, 361, 363, 373, 375, 378, 379 };   // анимации, по которым игра считает двери
        bool ready = true, asked = false;
        for(int id : kIds)
        {
            int g = VehAnimGroupGetGroup(vag, id);
            if(g < 0 || g > 255 || !AnimBlockName || !AnimBlockIndex || !AnimBlocksVar) continue;
            // Решаем по блоку анимаций, а не по самой анимации (1.5.3: вылет у друга — мод счёл блок «ped» незагруженным
            // и заказал его; «ped» игра держит в памяти всегда и не грузит потоком — CdStreamThread упал).
            const char* name = AnimBlockName(g);
            if(!name || !name[0] || !strcasecmp(name, "ped")) continue;
            int bi = AnimBlockIndex(name);
            if(bi < 0 || bi > 255) continue;
            if(Field<uint8_t>(AnimBlocksVar + (uintptr_t)bi * 32, 0x10)) continue;          // CAnimBlock::bLoaded — уже в памяти
            ready = false;
            if(load) { RequestModel(kIfpBase + bi, 2); asked = true; Log(false, "[МАШИНЫ] Гружу анимации дверей «%s» (группа %d)", name, g); }
        }
        if(ready || !asked) return ready;
        LoadAllRequestedModels(false);
        return DoorAnimsReady(veh, false);
    }

    uint64_t g_lastEnterTaskUs = 0;
    bool GiveEnterTask(uintptr_t ped, uintptr_t veh, int seat)
    {
        uintptr_t mgr = TaskManager(ped);
        if(!mgr) return false;
        // Не чаще раза в 2 с: в 0.8.7 задача «сесть» выдавалась десятки раз в секунду — игра упала
        // в CVehicleAnimGroup::ComputeAnimDoorOffsets (анимация двери для удалённого CJ)
        if(ped == g_ctx.remote)
        {
            if(g_lastEnterTaskUs && net::Since(g_ctx.now, g_lastEnterTaskUs) < 2000000) return false;
            g_lastEnterTaskUs = g_ctx.now;
        }
        if(!DoorAnimsReady(veh, true)) { Log(true, "[МАШИНЫ] Анимации дверей машины не загрузились — посадку пропускаю"); return false; }
        void* t = TaskNew(0x80);
        if(!t) return false;
        if(seat == 0) TaskEnterAsDriver(t, veh);
        else          TaskEnterAsPassenger(t, veh, seat - 1, false);
        TaskMgrSetTask(mgr, t, 3, false);
        return true;
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
        if(!DoorAnimsReady(veh, true)) { Log(true, "[МАШИНЫ] Анимации дверей машины не загрузились — пересадку пропускаю"); return; }
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
            g_onFootSinceUs = 0;
            int seat = s.seat;
            // уже в нужной машине (место могло быть занято — тогда сидит на другом, это нормально)
            if(cur && cur == tv) { g_remoteBusy = true; g_enterSinceUs = 0; g_leaveSinceUs = 0; return; }
            if(cur && cur != tv) { WarpOut(r); return; }
            if(!tv) return;                                  // копия ещё не готова — пусть идёт пешком
            g_remoteBusy = true;
            float d0 = Dist(GetPosition(r), GetPosition(tv));
            if(seat == 0)
            {   // друг угнал машину с педом за рулём.
                // 1.5.8: у друга угон доигрывает раньше (у хоста задача дана позже + сеть) — раньше хост в этот миг
                // выкидывал водителя сразу, анимация второго CJ обрывалась и он «телепортировался» на место.
                // Пока второй CJ сам вытаскивает водителя (задача «сесть за руль», не дольше 7 с) — ждём.
                uintptr_t drv = Field<uintptr_t>(tv, off::VehDriver);
                if(drv && drv != r && drv != g_ctx.local)
                {
                    bool jacking = tt == TASK_ENTER_AS_DRIVER && g_enterSinceUs && g_enterVehId == s.vehId && d0 < 12.0f &&
                                   net::Since(now, g_enterSinceUs) < kEnterTimeoutUs;
                    if(jacking)
                    {
                        if(!g_jackWaitLogged) { g_jackWaitLogged = true; Log(false, "[МАШИНЫ] Друг уже за рулём %04X — второй CJ доигрывает угон", s.vehId); }
                        return;
                    }
                    WarpOut(drv); Log(false, "[МАШИНЫ] Друг угнал машину %04X — водитель-пед выкинут", s.vehId);
                }
            }
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
                if(!GiveEnterTask(r, tv, seat)) return;      // не дали (пауза 2 с) — попробуем в следующем кадре
                g_enterSinceUs = now; g_enterVehId = s.vehId; g_jackWaitLogged = false;
                Log(false, "[МАШИНЫ] Друг в машине %04X — второй CJ садится (место %d)", s.vehId, seat);
                return;
            }
            if(d >= 12.0f || (g_enterSinceUs && net::Since(now, g_enterSinceUs) > kEnterTimeoutUs))
            {
                WarpIn(r, tv, seat);
                g_enterSinceUs = 0;
                Log(false, "[МАШИНЫ] Второй CJ посажен в машину %04X (место %d) без анимации", s.vehId, seat);
            }
            return;
        }

        // Друг пешком. Короткие «пешком» между «садится» и «сел» бывают — не реагируем
        // на них сразу (иначе второй CJ садился, выходил и садился снова).
        bool onFoot = s.enterSeat == net::kNoSeat;
        if(!onFoot) g_onFootSinceUs = 0;
        else if(!g_onFootSinceUs) g_onFootSinceUs = now;
        bool surelyOnFoot = onFoot && net::Since(now, g_onFootSinceUs) > kOnFootConfirmUs;
        if(cur && !surelyOnFoot) { g_remoteBusy = true; return; }
        if(onFoot && !surelyOnFoot && (tt == TASK_ENTER_AS_DRIVER || tt == TASK_ENTER_AS_PASSENGER)) { g_remoteBusy = true; return; }
        if(surelyOnFoot) g_enterSinceUs = 0;
        if(cur)
        {
            g_remoteBusy = true;
            if(!g_leaveSinceUs) { GiveLeaveTask(r, cur); g_leaveSinceUs = now; Log(false, "[МАШИНЫ] Друг вышел — второй CJ выходит"); }
            else if(net::Since(now, g_leaveSinceUs) > kLeaveTimeoutUs) { WarpOut(r); g_leaveSinceUs = 0; }
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
                // Угон: друг садится за руль, а там пед игры — второй CJ тоже вытаскивает водителя (как в игре).
                // Раньше место «занято» → второго CJ сажали пассажиром, пед ехал дальше вместе с ним (тест 1.4.4).
                // Пассажиром — только если за рулём ты (тебя не выкидываем).
                uintptr_t drv = seat == 0 ? Field<uintptr_t>(tv, off::VehDriver) : 0;
                bool jack = drv && drv != r && drv != g_ctx.local;
                if(!jack && !SeatFree(tv, seat, r)) { int p = FreePassengerSeat(tv, seat - 1); seat = p >= 0 ? p + 1 : -1; }
                if(seat >= 0 && Dist(GetPosition(r), GetPosition(tv)) < 15.0f && GiveEnterTask(r, tv, seat))
                {
                    g_enterSinceUs = now; g_enterVehId = s.vehId; g_jackWaitLogged = false;
                    if(jack) Log(false, "[МАШИНЫ] Друг угоняет %04X — второй CJ вытаскивает водителя", s.vehId);
                    Log(false, "[МАШИНЫ] Друг садится в %04X — второй CJ тоже (место %d)", s.vehId, seat);
                }
            }
            return;
        }
        // Передумал садиться — отменить нашу задачу «сесть»
        if(tt == TASK_ENTER_AS_DRIVER || tt == TASK_ENTER_AS_PASSENGER) ClearPrimaryTask(r);
    }

    // Оба сели за руль одной машины одновременно (каждый у себя — водитель, второй — пассажир): руль у хоста,
    // друг пересаживается пассажиром. Решает друг (у хоста копия друга и так пассажир). Тест 1.4.3.
    uint64_t g_bothDriveSinceUs = 0;
    void ResolveBothDrivers()
    {
        const net::PlayerStatePacket* rs = g_ctx.rs;
        uintptr_t local = g_ctx.local;
        bool conflict = g_ctx.role == net::Role::Client && rs && g_localSeat.veh && g_localSeat.seat == 0 && rs->seat == 0;
        NetVeh* v = conflict ? FindVeh(g_localSeat.veh) : nullptr;
        if(!v || v->id != rs->vehId) { g_bothDriveSinceUs = 0; return; }
        if(!g_bothDriveSinceUs) { g_bothDriveSinceUs = g_ctx.now; return; }
        if(net::Since(g_ctx.now, g_bothDriveSinceUs) < 400000) return;      // не дёргаем на мгновенных состояниях
        g_bothDriveSinceUs = 0;
        int p = FreePassengerSeat(g_localSeat.veh, 0);
        if(p < 0) { WarpOut(local); Log(false, "[МАШИНЫ] За рулём %04X хост, мест нет — ты вышел", v->id); return; }
        WarpIn(local, g_localSeat.veh, p + 1);
        Log(false, "[МАШИНЫ] Вы оба сели за руль %04X — руль у хоста, ты пассажир (место %d)", v->id, p + 1);
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
        ResolveBothDrivers();
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
                if(v.crowd && !localUses && !remoteUses && (!haveRemote || Dist(vp, rp) > kCrowdVehKeep))
                {   // машина вокруг друга уехала от него — не общая (игра уберёт её сама, как обычно)
                    SendGone(v.id); v.dead = true;
                    continue;
                }
                if(!localUses && !remoteUses && Dist(vp, lp) > kVehKeepDist && (!haveRemote || Dist(vp, rp) > kVehKeepDist))
                {
                    ReleaseOwn(v); SendGone(v.id); v.dead = true;
                    Log(false, "[МАШИНЫ] Машина %04X далеко от обоих — больше не общая", v.id);
                    continue;
                }
            }
            else
            {
                if(v.haveState && net::Since(g_ctx.now, v.stUs) > kVehStaleUs && !localDriver)
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
                if(net::Since(g_ctx.now, v.lastSendUs) >= interval)
                {
                    net::VehiclePacket p;
                    FillVehicle(v, localDriver, p);
                    SendGame(&p, sizeof(p));
                    v.lastSendUs = g_ctx.now; ++v.pktsOut;
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
    bool     g_npcFire = false;            // хост: внутри выстрела педа, которого видит друг
    uintptr_t g_npcFireOwner = 0;
    int      g_shotsOut = 0, g_shotsIn = 0, g_shotsReplayed = 0;
    bool     g_inLocalFire = false;
    bool     g_shotCaptured = false;
    CVector  g_shotStart{}, g_shotEnd{};
    bool     g_shotHitMate = false;
    CVector  g_lastAim{};
    uint64_t g_lastAimUs = 0;
    bool     g_replaying = false;          // повтор выстрела друга
    bool     g_applyingDamage = false;     // урон от друга по событию
    bool     g_applyingNpcDamage = false;  // хост: урон педу по событию от друга
    bool     g_allowRemoteDamage = false;  // убить второго CJ (друг погиб)
    uint64_t g_nextWeaponTryUs = 0;
    bool     g_remoteAiming = false;
    int      g_lastRemoteWanted = -1;

    bool GunWeapon(int w) { return w >= 22 && w <= 38; }

    void EnsureWeapon(uintptr_t ped, int w)
    {
        if(CurrentWeaponType(ped) == w) return;
        if(ped == g_ctx.remote) { if(g_ctx.now < g_nextWeaponTryUs) return; g_nextWeaponTryUs = g_ctx.now + 500000; }
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

    // ---------- вид стрельбы друга из машины (1.4.7) ----------
    // Друг стреляет из машины — у тебя его CJ сидел ровно, пули летели «из салона». Теперь при каждом выстреле
    // друга из машины его CJ высовывается из окна в сторону выстрела (те же анимации, что у игры в
    // CTaskSimpleGangDriveBy: группа 72 машина / 73–74 байк / 75, анимации 231–238 по четверти и стороне).
    // Задачу игры второму CJ не даём: для «игрока» она переключила бы ТВОЮ камеру в прицел. Стреляет по-прежнему
    // только повтор выстрелов друга — анимация ничего сама не стреляет. Нет выстрелов 1,2 с — CJ садится обратно.
    struct RemoteDbPose { uintptr_t ped = 0, assoc = 0; int anim = -1; uint64_t untilUs = 0; };
    RemoteDbPose g_rdb;
    int g_poseLogs = 0;
    bool g_animBlockRef[256] = {};
    uintptr_t PedClump(uintptr_t ped) { return ped ? Field<uintptr_t>(ped, 0x20) : 0; }   // CEntity::m_pRwClump
    bool PoseAlive()
    {
        if(!g_rdb.assoc || !ClumpGetAssociation || !g_rdb.ped || g_rdb.ped != g_ctx.remote) return false;   // второго CJ пересоздали — старое не трогать
        uintptr_t clump = PedClump(g_rdb.ped);
        return clump && ClumpGetAssociation(clump, (unsigned)g_rdb.anim) == g_rdb.assoc;
    }
    void PoseEnd()
    {
        if(PoseAlive())
        {
            Field<float>(g_rdb.assoc, 0x34) = -8.0f;                                     // m_fBlendDelta — плавно убрать (как игра)
            if(AnimSetDeleteCallback && DefaultAnimCB) AnimSetDeleteCallback(g_rdb.assoc, DefaultAnimCB, nullptr);
        }
        g_rdb = RemoteDbPose{};
    }
    void RemoteDriveByPose(uintptr_t r, const CVector& target, int weapon)
    {
        if(!r || r != g_ctx.remote || !BlendAnimation || !ClumpGetAssociation || !AnimBlocksVar || !AnimBlockName || !AnimBlockIndex) return;
        uintptr_t veh = PedVehicle(r), clump = PedClump(r);
        if(!veh || !clump) return;
        int vtype = Field<int>(veh, 0x734), sub = Field<int>(veh, 0x738);
        uintptr_t drv = Field<uintptr_t>(veh, off::VehDriver);
        int group = sub == 2 ? 75 : vtype == 9 ? 73 + ((Field<int>(veh, 0x830) == 2 && drv == r) ? 1 : 0) : 72;
        bool right = false; int window = -1;
        if(drv != r)
        {
            if(Field<uintptr_t>(veh, off::VehPassengers) == r)          { right = true;  window = 8;  }
            else if(Field<uintptr_t>(veh, off::VehPassengers + 8) == r)  { right = false; window = 11; }
            else if(Field<uintptr_t>(veh, off::VehPassengers + 16) == r) { right = true;  window = 9;  }
        }
        // блок анимаций группы: не загружен — заказать (следующие выстрелы уже с анимацией)
        const char* bn = AnimBlockName(group);
        int bi = bn && bn[0] ? AnimBlockIndex(bn) : -1;
        if(bi < 0 || bi >= 256) return;
        bool pedBlock = !strcasecmp(bn, "ped");                                         // «ped» всегда в памяти — не заказывать (вылет 1.5.2)
        if(!pedBlock && !Field<uint8_t>(AnimBlocksVar + (uintptr_t)bi * 32, 0x10)) { RequestModel(kIfpBase + bi, 8); return; }
        if(!pedBlock && !g_animBlockRef[bi] && AddAnimBlockRef) { AddAnimBlockRef(bi); g_animBlockRef[bi] = true; Log(false, "[ВЫСТРЕЛ] Анимации стрельбы из машины «%s» загружены", bn); }
        // четверть: куда стреляет относительно носа машины (как CTaskSimpleGangDriveBy::PlayerTarget)
        CVector vp = GetPosition(veh);
        const float kPi = 3.14159265f;
        float a = atan2f(-(target.x - vp.x), target.y - vp.y);
        uintptr_t m = Field<uintptr_t>(veh, off::EntMatrix);
        float h = m ? atan2f(-Field<float>(m, 0x10), Field<float>(m, 0x14)) : Field<float>(veh, 0x14);
        float d = a - h;
        if(d > kPi) d -= 2 * kPi; else if(d < -kPi) d += 2 * kPi;
        d += kPi / 4; if(d < 0) d += 2 * kPi;
        int q = (int)(d / (kPi / 2)) & 3;
        static const int kLeft[4] = { 233, 231, 234, 232 }, kRight[4] = { 237, 236, 238, 235 };
        int anim = right ? kRight[q] : kLeft[q];
        if(window >= 0 && VehSetWindowOpen) VehSetWindowOpen(veh, (uint8_t)window);
        if(PedAddWeaponModel && GetWeaponInfo && !PoseAlive())
            if(uintptr_t wi = GetWeaponInfo(weapon, 1)) PedAddWeaponModel(r, Field<int>(wi, 0xC));   // в машине игра прячет оружие
        if(PoseAlive() && g_rdb.anim != anim) PoseEnd();
        uintptr_t as = ClumpGetAssociation(clump, (unsigned)anim);
        if(!as) as = BlendAnimation(clump, group, anim, 4.0f);
        if(!as) return;
        bool first = !g_rdb.assoc;
        g_rdb.ped = r; g_rdb.assoc = as; g_rdb.anim = anim; g_rdb.untilUs = g_ctx.now + 1200000;
        if(first && g_poseLogs < 5) { ++g_poseLogs; Log(false, "[ВЫСТРЕЛ] Друг стреляет из машины — его CJ высунулся из окна (анимация %d, группа %d)", anim, group); }
    }
    void RemoteDriveByTick()
    {
        if(!g_rdb.assoc) return;
        if(!PoseAlive()) { g_rdb = RemoteDbPose{}; return; }
        if(!PedVehicle(g_rdb.ped) || g_ctx.now >= g_rdb.untilUs) { PoseEnd(); return; }
        // пока стреляет — держим «огонь»: дошла до конца цикла стрельбы — назад к его началу
        if(AnimSetCurrentTime && DriveByLoopStart && DriveByLoopEnd)
        {
            float t = Field<float>(g_rdb.assoc, 0x38), end = *(float*)DriveByLoopEnd;
            if(end > 0 && t >= end) AnimSetCurrentTime(g_rdb.assoc, *(float*)DriveByLoopStart);
        }
    }

    void ReplayShot(const net::ShotPacket& p)
    {
        uintptr_t r = p.npc ? crowd::CopyById(p.npc) : g_ctx.remote;
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
        if(!p.npc && PedVehicle(r)) RemoteDriveByPose(r, end, w);
        g_replaying = true;
        bool ok = WeaponFire(weap, r, &start, &start, 0, &end, nullptr);
        g_replaying = false;
        if(ok) ++g_shotsReplayed;
        if(g_shotsIn <= 10)
            Log(false, "[ВЫСТРЕЛ] друга: оружие %d, повторён %s, второй CJ от места выстрела в %.1f м%s", w, ok ? "да" : "НЕТ",
                Dist(rp, V(p.shooter)), (p.flags & net::SHOT_HIT_MATE) ? ", попал в тебя" : "");
    }

    // ---------- стрельба пассажира из окна (1.4.5) ----------
    // В игре пассажир стреляет только в режиме «двое на одном экране» (CTaskSimpleCarDrive, IsCoopGameGoingOn) — мод
    // этот режим выключает, поэтому в коопе пассажиры не стреляли вовсе. Делаем как миссии («Драйв-бай»): пассажиру с
    // огнестрельным оружием даём CTaskSimpleGangDriveBy через событие «команда сценария» (как сама игра). Задача сама
    // переводит камеру в прицел из машины и стреляет по кнопке огня. Любое оружие с пулями (пистолеты, дробовики,
    // автоматы, АК, М4, миниган) — у игры только «узи/TEC-9/MP5». Убрать стрельбу — переключить на кулак.
    constexpr int kTaskGangDriveBy = 0x3FE;
    // 1.5.7 (игрок: «сразу высовывается и нельзя залезть обратно»): высовываться — только по своей кнопке «Прицел»
    // (рисует слой поверх игры), второе нажатие — обратно в машину (CTask::MakeAbortable, срочно — как сама игра,
    // когда пед вышел из машины). Кнопка рисуется в другом потоке — здесь только флаги.
    uint64_t g_dbTryUs = 0;
    int      g_dbLogged = -1;
    bool     g_dbWant = false;                 // игрок хочет быть высунутым
    volatile bool g_dbToggle = false;          // нажата кнопка (слой поверх игры)
    volatile int  g_dbState = 0;               // 0 — кнопки нет, 1 — в машине, 2 — высунулся
    void PassengerDriveBy()
    {
        uintptr_t me = g_ctx.local;
        auto off_ = [&]() { if(g_dbWant) Log(false, "[МАШИНЫ] Стрельба из окна выключена"); g_dbWant = false; g_dbState = 0; g_dbToggle = false; };
        if(!me || !GangDriveByCtor || !VehCanPedLeanOut || !GetWeaponInfo || !EventScriptCommandCtor || !EventGroupAdd || !EventScriptCommandDtor) { off_(); return; }
        uintptr_t veh = PedVehicle(me);
        if(!veh || Field<uintptr_t>(veh, off::VehDriver) == me) { g_dbLogged = -1; off_(); return; }      // пешком или за рулём
        int slot = Field<int8_t>(me, off::PedCurWeaponSlot);
        if(slot < 2 || slot > 7) { off_(); return; }                                          // кулак, ближний бой, гранаты — нет
        uintptr_t w = me + off::PedWeapons + (uintptr_t)slot * off::WeaponSize;
        int type = Field<int>(w, off::WeapType);
        if(type < 22 || type > 38 || type == 34 || Field<int>(w, off::WeapTotal) <= 0) { off_(); return; }   // снайперка — своя камера
        uintptr_t wi = GetWeaponInfo(type, 1);
        if(!wi || Field<int>(wi, 0) != 1) { off_(); return; }                                 // только оружие «с пулями»
        if(PedAddWeaponModel && g_dbLogged != type) PedAddWeaponModel(me, Field<int>(wi, 0xC)); // при посадке игра прячет «не своё» оружие
        if(g_dbLogged != type) { g_dbLogged = type; Log(false, "[МАШИНЫ] Ты пассажир с оружием %d — кнопка «Прицел»: высунуться и стрелять", type); }

        uintptr_t task = 0;
        int tt = ActiveTaskType(me, &task);
        bool out = tt == kTaskGangDriveBy;
        if(g_dbToggle)
        {
            g_dbToggle = false;
            g_dbWant = !out;
            Log(false, "[МАШИНЫ] Кнопка «Прицел»: %s", g_dbWant ? "высовываюсь из окна" : "обратно в машину");
            g_dbTryUs = 0;
        }
        if(out && !g_dbWant)
        {
            // обратно в машину: как игра сама — MakeAbortable(ped, срочно, без события)
            uintptr_t vt = task ? Field<uintptr_t>(task, 0) : 0;
            if(vt) ((bool(*)(uintptr_t, uintptr_t, int, const void*))Field<uintptr_t>(vt, 0x38))(task, me, 1, nullptr);
            g_dbState = 1;
            return;
        }
        if(!out && g_dbWant)
        {
            if(!g_dbTryUs || net::Since(g_ctx.now, g_dbTryUs) >= 1500000)
            {
                g_dbTryUs = g_ctx.now;
                if(!VehCanPedLeanOut(veh, me)) { Log(false, "[МАШИНЫ] Отсюда высунуться нельзя (место или машина)"); g_dbWant = false; g_dbState = 1; return; }
                if(PedAddWeaponModel) PedAddWeaponModel(me, Field<int>(wi, 0xC));
                uintptr_t intel = Field<uintptr_t>(me, off::PedIntel);
                void* t = intel ? TaskNew(0x60) : nullptr;
                if(t)
                {
                    bool right = Field<uintptr_t>(veh, off::VehPassengers + 8) != me;         // как у игры: левое заднее — «не справа»
                    GangDriveByCtor(t, 0, nullptr, 100.0f, 100, 8, right);
                    alignas(16) uint8_t ev[0x40] = {};
                    EventScriptCommandCtor(ev, 3, t, false);
                    EventGroupAdd(intel + 0xC8, ev, false);
                    EventScriptCommandDtor(ev);                                               // как у игры: событие копируется, своё удаляем
                }
            }
            g_dbState = 2;
            return;
        }
        g_dbState = out ? 2 : 1;
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
        if(g_echo || !g_ctx.rs || !FindPlayerWanted || !g_sharedWanted) return;
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

// ---------- отношения педов ко второму CJ ----------
namespace
{
    // Банды, копы и прохожие относятся ко второму игроку (тип PLAYER2) так же, как к тебе (PLAYER1):
    // Grove уважают, враждебные банды ненавидят — как сейчас выставили скрипты игры.
    // Скрипты меняют отношения по ходу сюжета, поэтому повторяем раз в секунду.
    uint64_t g_nextRelUs = 0;
    void MirrorPlayer2Relations()
    {
        if(!g_pedTypesSym || g_ctx.now < g_nextRelUs) return;
        g_nextRelUs = g_ctx.now + 1000000;
        uintptr_t base = *(uintptr_t*)g_pedTypesSym;
        if(!base) return;
        int changed = 0;
        for(int t = 0; t < 32; ++t)
            for(int a = 0; a < 5; ++a)
            {
                uint32_t& m = *(uint32_t*)(base + (uintptr_t)t * 20 + (uintptr_t)a * 4);
                uint32_t want = (m & 1u) ? (m | 2u) : (m & ~2u);
                if(want != m) { m = want; ++changed; }
            }
        if(changed) Log(false, "[ОТНОШЕНИЯ] педы теперь относятся ко второму CJ как к тебе (изменено %d)", changed);
    }
}

// ---------- подробный лог ----------
namespace
{
    const char* WhoDrives(uintptr_t veh)
    {
        uintptr_t d = Field<uintptr_t>(veh, off::VehDriver);
        if(!d) return "никто";
        if(d == g_ctx.local) return "ты";
        if(d == g_ctx.remote) return "второй CJ";
        return "пед игры";
    }
    const char* ModeName(int m) __attribute__((unused));
    const char* ModeName(int m) { return m == 0 ? "перенос" : m == 1 ? "стоит" : m == 2 ? "едет" : "-"; }

    // Переходы (пишем сразу, как изменилось) и снимки раз в секунду
    struct PedTrace { int task = -9, state = -9, weapon = -1, area = -1; uintptr_t veh = 1; int seat = -9;
                      bool aim = false, duck = false, alive = true, disabled = false; float health = -1, armour = -1; };
    PedTrace g_tl, g_tr;
    int      g_lastWanted = -1;
    uint32_t g_lastFriendSeq = 0; uint64_t g_lastFriendSeqUs = 0; int g_friendPkts = 0, g_friendLost = 0;
    CarInput g_lastCarIn;

    void TracePed(const char* who, uintptr_t ped, PedTrace& t, bool local)
    {
        int task = ActiveTaskType(ped), st = Field<int>(ped, off::PedState);
        uintptr_t v = PedVehicle(ped);
        int seat = v ? SeatOf(ped, v) : -1;
        int w = CurrentWeaponType(ped);
        bool aim = (Field<uint8_t>(ped, off::PedFlagsWord) & 0x10) != 0;
        bool duck = (Field<uint8_t>(ped, off::PedFlags587) & 0x04) != 0;
        bool alive = PedIsAlive(ped);
        int area = Field<uint8_t>(ped, off::EntArea);
        float hp = Field<float>(ped, off::PedHealth), ar = Field<float>(ped, off::PedArmour);
        if(task != t.task || st != t.state)
        { Log(false, "[%s] задача %d -> %d, состояние %d -> %d", who, t.task, task, t.state, st); t.task = task; t.state = st; }
        if(v != t.veh || seat != t.seat)
        {
            NetVeh* nv = FindVeh(v);
            if(v) Log(false, "[%s] в машине %d (%s %04X), место %d, за рулём: %s", who, ModelOf(v), nv ? (nv->copy ? "копия" : "общая") : "обычная", nv ? nv->id : 0, seat, WhoDrives(v));
            else  Log(false, "[%s] вне машины", who);
            t.veh = v; t.seat = seat;
        }
        if(w != t.weapon) { Log(false, "[%s] оружие %d -> %d", who, t.weapon, w); t.weapon = w; }
        if(aim != t.aim)  { Log(false, "[%s] %s", who, aim ? "целится" : "опустил оружие"); t.aim = aim; }
        if(duck != t.duck){ Log(false, "[%s] %s", who, duck ? "присел" : "встал"); t.duck = duck; }
        if(alive != t.alive) { Log(false, "[%s] %s", who, alive ? "ЖИВ" : "ПОГИБ"); t.alive = alive; }
        if(area != t.area){ Log(false, "[%s] интерьер %d -> %d", who, t.area, area); t.area = area; }
        if(fabsf(hp - t.health) >= 1.0f || fabsf(ar - t.armour) >= 1.0f)
        { Log(false, "[%s] здоровье %.0f -> %.0f, броня %.0f -> %.0f", who, t.health, hp, t.armour, ar); t.health = hp; t.armour = ar; }
        if(local)
        {
            bool dis = Field<uint16_t>(GetPad(0), off::PadDisableCtrl) != 0;
            if(dis != t.disabled) { Log(false, "[%s] управление %s", who, dis ? "ВЫКЛЮЧЕНО игрой" : "включено"); t.disabled = dis; }
        }
    }

    void TraceTransitions()
    {
        if(g_ctx.local) TracePed("ТЫ", g_ctx.local, g_tl, true);
        if(g_ctx.remote) TracePed("ВТОРОЙ CJ", g_ctx.remote, g_tr, false);
        else g_tr = PedTrace();

        if(FindPlayerWanted)
        {
            uintptr_t w = FindPlayerWanted(0);
            int lv = w ? Field<int>(w, off::WantedStars) : 0;
            if(lv != g_lastWanted) { Log(false, "[РОЗЫСК] у тебя %d -> %d зв.", g_lastWanted, lv); g_lastWanted = lv; }
        }
        // Пакеты друга: пропуски и потери
        if(g_ctx.rs)
        {
            uint32_t sq = g_ctx.rs->seq;
            if(sq != g_lastFriendSeq)
            {
                if(g_lastFriendSeqUs)
                {
                    uint64_t gap = net::Since(g_ctx.now, g_lastFriendSeqUs);
                    int lost = (int)(sq - g_lastFriendSeq) - 1;
                    if(lost > 0 && lost < 1000) g_friendLost += lost;
                    if(gap > 300000) Log(false, "[СЕТЬ] от друга не было пакетов %.0f мс (потеряно %d)", gap / 1000.0f, lost > 0 ? lost : 0);
                }
                g_lastFriendSeq = sq; g_lastFriendSeqUs = g_ctx.now; ++g_friendPkts;
            }
        }
        // Педали и руль, которые мод «жмёт» за второго CJ
        if(g_carIn.on != g_lastCarIn.on || abs(g_carIn.accel - g_lastCarIn.accel) > 40 || abs(g_carIn.brake - g_lastCarIn.brake) > 40 ||
           abs(g_carIn.steer - g_lastCarIn.steer) > 24 || g_carIn.handbrake != g_lastCarIn.handbrake)
        {
            if(g_carIn.on) Log(false, "[ПЕДАЛИ ВТОРОГО CJ] газ %d, тормоз %d, руль %d, ручник %s", g_carIn.accel, g_carIn.brake, g_carIn.steer, g_carIn.handbrake ? "да" : "нет");
            else if(g_lastCarIn.on) Log(false, "[ПЕДАЛИ ВТОРОГО CJ] не за рулём");
            g_lastCarIn = g_carIn;
        }
    }

    // Раз в секунду, пока есть общие машины
    uint64_t g_nextDebugUs = 0;
    void DebugTick()
    {
        if(g_ctx.now < g_nextDebugUs) return;
        g_nextDebugUs = g_ctx.now + 1000000;
        if(g_shotsOut || g_shotsIn)
            Log(false, "[ВЫСТРЕЛЫ] за секунду: твоих %d, от друга %d (повторено %d)", g_shotsOut, g_shotsIn, g_shotsReplayed);
        g_shotsOut = g_shotsIn = g_shotsReplayed = 0;
        if(g_friendPkts || g_friendLost)
            Log(false, "[СЕТЬ] за секунду от друга пакетов %d, потеряно %d", g_friendPkts, g_friendLost);
        g_friendPkts = g_friendLost = 0;
        // Снимок своего и второго CJ
        for(int k = 0; k < 2; ++k)
        {
            uintptr_t ped = k ? g_ctx.remote : g_ctx.local;
            if(!ped) continue;
            CVector p = GetPosition(ped);
            uintptr_t pv = PedVehicle(ped);
            CVector ms = Field<CVector>(pv ? pv : ped, off::PhysMoveSpeed);
            Log(false, "[%s] снимок: %.1f,%.1f,%.1f, скорость %.1f м/с, курс %.0f°, задача %d, состояние %d, оружие %d, здоровье %.0f%s",
                k ? "ВТОРОЙ CJ" : "ТЫ", p.x, p.y, p.z, sqrtf(ms.x * ms.x + ms.y * ms.y) * 50.0f,
                Field<float>(ped, off::PedRotCurrent) * 57.2958f, ActiveTaskType(ped), Field<int>(ped, off::PedState),
                CurrentWeaponType(ped), Field<float>(ped, off::PedHealth), pv ? ", в машине" : "");
        }
        if(g_ctx.rs && g_ctx.remote)
        {
            CVector rp = GetPosition(g_ctx.remote), pp = V(g_ctx.rs->pos);
            Log(false, "[ДРУГ] по пакету: %.1f,%.1f,%.1f — второй CJ от этой точки в %.1f м", pp.x, pp.y, pp.z, Dist(rp, pp));
        }
        if(g_vehs.empty()) return;
        CVector lp = GetPosition(g_ctx.local);
        for(auto& v : g_vehs)
        {
            if(v.dead) continue;
            if(!Valid(v))
                Log(false, "[МАШИНА %04X] %s, нет на карте%s, пакетов за с: пришло %u", v.id, v.copy ? "копия" : "своя",
                    v.haveState ? " (появится, когда подойдёшь)" : "", v.pktsIn);
            else
            {
                CVector p = GetPosition(v.veh);
                CVector ms = Field<CVector>(v.veh, off::PhysMoveSpeed);
                float kmh = sqrtf(ms.x * ms.x + ms.y * ms.y) * 50.0f * 3.6f;
                uintptr_t r = g_ctx.remote;
                bool remoteIn = r && PedVehicle(r) == v.veh;
                uintptr_t mm = Field<uintptr_t>(v.veh, off::EntMatrix);
                CVector fw = mm ? Field<CVector>(mm, off::MatForward) : CVector{ 0, 1, 0 };
                CVector ts = Field<CVector>(v.veh, off::PhysTurnSpeed);
                Log(false, "[МАШИНА %04X] %s, модель %d, %.1f,%.1f,%.1f, курс %.0f°, до тебя %.0f м, %.0f км/ч, вращение %.3f, здоровье %.0f, за рулём: %s%s; "
                           "руль %.2f, газ %.2f, тормоз %.2f, ручник %d; режим %s, расхождение %.2f м / %.0f°, поправок %d, переносов %d; "
                           "пакетов за с: ушло %u, пришло %u, последнее %.0f мс назад",
                    v.id, v.copy ? "копия" : "своя", ModelOf(v.veh), p.x, p.y, p.z, atan2f(-fw.x, fw.y) * 57.2958f, Dist(p, lp), kmh, ts.z,
                    Field<float>(v.veh, off::VehHealth), WhoDrives(v.veh), remoteIn ? " (второй CJ внутри)" : "",
                    Field<float>(v.veh, off::VehSteerInput), Field<float>(v.veh, off::VehGas), Field<float>(v.veh, off::VehBrake),
                    (Field<uint8_t>(v.veh, off::VehFlags) >> 5) & 1,
                    ModeName(v.mode), v.lastErr, v.lastYawErr, v.corrections, v.snaps, v.pktsOut, v.pktsIn,
                    v.stUs ? (float)(net::Since(g_ctx.now, v.stUs)) / 1000.0f : -1.0f);
            }
            v.snaps = 0; v.corrections = 0; v.pktsIn = v.pktsOut = 0;
        }
        if(g_ctx.rs)
            Log(false, "[ДРУГ] машина %04X, место %d, садится %d, второй CJ занят машиной: %s, пешком %.1f с",
                g_ctx.rs->vehId, g_ctx.rs->seat == net::kNoSeat ? -1 : g_ctx.rs->seat,
                g_ctx.rs->enterSeat == net::kNoSeat ? -1 : g_ctx.rs->enterSeat, g_remoteBusy ? "да" : "нет",
                g_onFootSinceUs ? (float)(net::Since(g_ctx.now, g_onFootSinceUs)) / 1e6f : 0.0f);
    }
}


// ---------- перехваты ----------

// Кнопка «сесть» у машины, где за рулём друг: садимся пассажиром, а не выкидываем его
DECL_HOOKv(EnterAsDriverCtor, void* task, uintptr_t veh)
{
    if(g_active && veh)
        Log(false, "[ПОСАДКА] игра создаёт «сесть за руль»: машина %d, за рулём %s, вызвано из +0x%lX",
            ModelOf(veh), WhoDrives(veh), (unsigned long)((uintptr_t)__builtin_return_address(0) - g_libBase));
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

// Кнопка «сесть» самой игры: пишем каждое изменение (что показала / спрятала и кто вызвал)
DECL_HOOKv(EnterCarWidgetSet, uintptr_t self, int vehType, int flags, bool entering, const CVector& at, int model)
{
    static int lastType = -100, lastModel = -100; static bool lastEntering = false;
    if(g_active && (vehType != lastType || model != lastModel || entering != lastEntering))
    {
        Log(false, "[КНОПКА ИГРЫ] %s: тип машины %d, модель %d, %s, вызвано из +0x%lX",
            vehType < 0 ? "СПРЯТАНА" : "показана", vehType, model, entering ? "сесть" : "выйти",
            (unsigned long)((uintptr_t)__builtin_return_address(0) - g_libBase));
        lastType = vehType; lastModel = model; lastEntering = entering;
    }
    EnterCarWidgetSet(self, vehType, flags, entering, at, model);
}

// Педали и руль второго CJ в машине (пульт 2)
static bool RemoteCarPad(uintptr_t pad) { return g_active && g_carIn.on && pad == GetPad(1); }
DECL_HOOK(int16_t, PadAccelerate, uintptr_t pad) { return RemoteCarPad(pad) ? g_carIn.accel : PadAccelerate(pad); }
DECL_HOOK(int16_t, PadBrake, uintptr_t pad)      { return RemoteCarPad(pad) ? g_carIn.brake : PadBrake(pad); }
DECL_HOOK(int16_t, PadSteer, uintptr_t pad)      { return RemoteCarPad(pad) ? g_carIn.steer : PadSteer(pad); }
DECL_HOOK(int16_t, PadHandBrake, uintptr_t pad)  { return RemoteCarPad(pad) ? g_carIn.handbrake : PadHandBrake(pad); }

// Второго CJ полиция у тебя не арестовывает: арест обрабатывает только телефон друга
// (у себя он завис бы в наручниках — игра «сажает» только первого игрока)
DECL_HOOK(bool, PedCanBeArrested, uintptr_t ped)
{
    if(g_active && ped && ped == g_ctx.remote && !crowd::RemoteArrestable()) return false;   // копы хоста — могут, если у друга розыск
    return PedCanBeArrested(ped);
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
    if(g_active && g_npcFire && owner == g_npcFireOwner && start && end && !g_shotCaptured)
    {   // выстрел педа хоста: линия пули и попадание во второго CJ
        g_shotCaptured = true; g_shotStart = *start; g_shotEnd = *end; g_shotHitMate = hit && hit == g_ctx.remote;
    }
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
            SendGame(&p, sizeof(p)); ++g_shotsOut;
        }
    }
    DoBulletImpact(weapon, owner, hit, start, end, colPoint, inc);
}

// Выстрел своего CJ → другу (повторит у себя)
DECL_HOOK(bool, WeaponFireHook, uintptr_t weapon, uintptr_t owner, CVector* start, CVector* barrel, uintptr_t targetEnt, CVector* target, CVector* alt)
{
    // 1.6.0: кто не должен стрелять сам
    //  * пед миссии, спрятанный ради места другу в машине хоста, — невидимка не стреляет;
    //  * у друга копия педа хоста стреляет только повтором выстрела хоста (свой ИИ копии — нет, иначе двойная стрельба).
    if(g_active && owner && !g_replaying && owner != g_ctx.local && owner != g_ctx.remote)
    {
        if(mission::IsHiddenPed(owner)) return false;
        if(g_ctx.role == net::Role::Client && crowd::IsCopy(owner))
        {
            static uint64_t lastLogUs = 0;
            if(!lastLogUs || net::Since(g_ctx.now, lastLogUs) > 5000000) { lastLogUs = g_ctx.now; Log(false, "[ВЫСТРЕЛ] копия педа хоста стреляла сама — не даю (стреляет только как у хоста)"); }
            return false;
        }
    }
    bool local = g_active && owner && owner == g_ctx.local && !g_replaying;
    uint16_t npc = (!local && g_active && owner && !g_replaying) ? crowd::TrackedId(owner) : 0;
    if(npc)
    {   // хост: стреляет пед, которого видит друг — друг повторит выстрел у себя
        static uint64_t winUs = 0; static int inWin = 0;
        if(net::Since(g_ctx.now, winUs) > 1000000) { winUs = g_ctx.now; inWin = 0; }
        g_npcFire = true; g_npcFireOwner = owner; g_shotCaptured = false; g_shotHitMate = false;
        bool r = WeaponFireHook(weapon, owner, start, barrel, targetEnt, target, alt);
        g_npcFire = false; g_npcFireOwner = 0;
        if(r && ++inWin <= 25)
        {
            net::ShotPacket p{};
            p.hdr = net::MakeHeader(net::PKT_SHOT);
            p.seq = ++g_shotSeq; p.npc = npc;
            p.weapon = (uint8_t)Field<int>(weapon, off::WeapType);
            CVector op = GetPosition(owner);
            Put(p.shooter, op);
            CVector s0 = g_shotCaptured ? g_shotStart : (start ? *start : CVector{ op.x, op.y, op.z + 0.5f });
            CVector e0 = g_shotCaptured ? g_shotEnd : (target ? *target : CVector{ op.x - sinf(Field<float>(owner, off::PedRotCurrent)) * 40.0f, op.y + cosf(Field<float>(owner, off::PedRotCurrent)) * 40.0f, op.z });
            Put(p.start, s0); Put(p.end, e0);
            if(g_shotCaptured && g_shotHitMate) p.flags |= net::SHOT_HIT_MATE;
            SendGame(&p, sizeof(p));
        }
        return r;
    }
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
    SendGame(&p, sizeof(p)); ++g_shotsOut;
    if(g_shotsOut <= 10)
        Log(false, "[ВЫСТРЕЛ] твой: оружие %d, из %.1f,%.1f,%.1f в %.1f,%.1f,%.1f%s", p.weapon, p.start[0], p.start[1], p.start[2],
            p.end[0], p.end[1], p.end[2], (p.flags & net::SHOT_HIT_MATE) ? ", попал во второго CJ" : "");
    return r;
}

// Урон: второго CJ у себя не раним (урон считает телефон друга),
// по себе — только событием от друга
DECL_HOOKv(ComputeDamage, uintptr_t calc, uintptr_t victim, uintptr_t resp, bool painAudio)
{
    if(g_active && victim && (victim == g_ctx.local || victim == g_ctx.remote))
    {
        uintptr_t inf = Field<uintptr_t>(calc, off::CalcInflictor);
        const char* who = !inf ? "ничего" : inf == g_ctx.local ? "ты" : inf == g_ctx.remote ? "второй CJ" :
                          EntType(inf) == 2 ? (IsLocalSide(inf) ? "твоя машина" : IsRemoteSide(inf) ? "машина второго CJ" : "машина") :
                          EntType(inf) == 3 ? "пед" : "что-то";
        bool blocked = (victim == g_ctx.remote && !g_allowRemoteDamage) || (victim == g_ctx.local && IsRemoteSide(inf) && !g_applyingDamage);
        Log(false, "[УРОН] по %s: %.1f от «%s», оружие %d, часть тела %d%s", victim == g_ctx.local ? "тебе" : "второму CJ",
            Field<float>(calc, off::CalcDamage), who, Field<int>(calc, off::CalcWeapon), Field<int>(calc, off::CalcPiece),
            blocked ? " — НЕ применён (урон считает телефон друга)" : "");
    }
    if(g_active && victim && crowd::ClientMode())
    {   // друг: копии педов хоста раним только через хоста; их удары по тебе — тоже только от хоста
        uintptr_t inf = Field<uintptr_t>(calc, off::CalcInflictor);
        uint16_t cid = crowd::CopyId(victim);
        if(cid && !crowd::AllowCopyDamage())
        {
            if(IsLocalSide(inf))
            {
                net::NpcDamageEvent e{};
                e.type = net::REL_NPC_DAMAGE; e.npc = cid;
                e.weapon = (uint8_t)Field<int>(calc, off::CalcWeapon);
                e.piece = (uint8_t)Field<int>(calc, off::CalcPiece);
                e.viaVehicle = inf != g_ctx.local;
                e.amount = Field<float>(calc, off::CalcDamage);
                SendReliable(&e, sizeof(e));
            }
            ZeroResponse(resp);
            return;
        }
        bool fromCopy = inf && (crowd::IsCopy(inf) || (EntType(inf) == 2 && crowd::IsCopy(Field<uintptr_t>(inf, off::VehDriver))));
        if(victim == g_ctx.local && fromCopy && !g_applyingDamage) { ZeroResponse(resp); return; }
    }
    if(g_active && victim && victim != g_ctx.remote && victim != g_ctx.local && !g_applyingNpcDamage &&
       g_ctx.role == net::Role::Host && crowd::Active() && crowd::TrackedId(victim) && IsRemoteSide(Field<uintptr_t>(calc, off::CalcInflictor)))
    {   // хост: педа ранил второй CJ у меня — урон придёт точным событием от друга, здесь не считаем (иначе дважды)
        ZeroResponse(resp);
        return;
    }
    if(g_active && victim && victim == g_ctx.remote && !g_allowRemoteDamage && crowd::Active() && g_ctx.role == net::Role::Host)
    {   // хост: пед хоста ранил второго CJ — урон уходит другу (как от этого педа)
        uintptr_t inf = Field<uintptr_t>(calc, off::CalcInflictor);
        uintptr_t src = (inf && EntType(inf) == 2) ? Field<uintptr_t>(inf, off::VehDriver) : inf;
        uint16_t npc = crowd::TrackedId(src);
        if(npc && !IsLocalSide(inf))
        {
            net::DamageEvent e{};
            e.type = net::REL_DAMAGE; e.npc = npc;
            e.weapon = (uint8_t)Field<int>(calc, off::CalcWeapon);
            e.piece = (uint8_t)Field<int>(calc, off::CalcPiece);
            e.viaVehicle = inf != src;
            e.amount = Field<float>(calc, off::CalcDamage);
            SendReliable(&e, sizeof(e));
            ZeroResponse(resp);
            return;
        }
    }
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
    g_vehPoolSym = aml->GetSym(h, "_ZN6CPools15ms_pVehiclePoolE");
    g_libBase = aml->GetLib("libGTASA.so");
    g_pedTypesSym = aml->GetSym(h, "_ZN8CPedType13ms_apPedTypesE");
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
        { "_ZN21CWidgetButtonEnterCar22SetEnterCarVehicleTypeEiibRK7CVectori", (void*)HookOf_EnterCarWidgetSet, (void**)&EnterCarWidgetSet },
        { "_ZN4CPed13CanBeArrestedEv",        (void*)HookOf_PedCanBeArrested, (void**)&PedCanBeArrested },
        { "_ZN4CPad13GetAccelerateEv",        (void*)HookOf_PadAccelerate, (void**)&PadAccelerate },
        { "_ZN4CPad8GetBrakeEv",              (void*)HookOf_PadBrake,      (void**)&PadBrake },
        { "_ZN4CPad20GetSteeringLeftRightEv", (void*)HookOf_PadSteer,      (void**)&PadSteer },
        { "_ZN4CPad12GetHandBrakeEv",         (void*)HookOf_PadHandBrake,  (void**)&PadHandBrake },
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
    if(c.role != net::Role::Client && c.sharedWantedLocal) s.flags |= net::PSF_SHARED_WANTED;

    // Куда целится: захваченная цель, свежий выстрел или прямо перед собой
    uintptr_t lock = Field<uintptr_t>(l, off::PedLockOnTarget);
    CVector lp = GetPosition(l);
    CVector aim;
    if(lock && (EntType(lock) == 2 || EntType(lock) == 3)) { aim = GetPosition(lock); aim.z += 0.5f; s.flags |= net::PSF_AIMING; }
    else if(g_lastAimUs && net::Since(c.now, g_lastAimUs) < 600000) aim = g_lastAim;
    else { float h = Field<float>(l, off::PedRotCurrent); aim = CVector{ lp.x - sinf(h) * 10.0f, lp.y + cosf(h) * 10.0f, lp.z + 0.5f }; }
    Put(s.aim, aim);

    if(FindPlayerWanted) { uintptr_t w = FindPlayerWanted(0); if(w) s.wanted = (uint8_t)Field<int>(w, off::WantedStars); }

    // Машина: та, в которой сидишь, или в которую садишься
    LocalSeat ls = ReadLocalSeat(l);
    {
        static int lastState = -2;
        int st = ls.veh ? 100 + ls.seat : ls.enterVeh ? 200 + ls.enterSeat : -1;
        if(st != lastState)
        {
            if(st < 0)        Log(false, "[МАШИНЫ] Ты пешком");
            else if(st < 200) Log(false, "[МАШИНЫ] Ты в машине %d, место %d", ModelOf(ls.veh), ls.seat);
            else              Log(false, "[МАШИНЫ] Ты садишься в машину %d, место %d", ModelOf(ls.enterVeh), ls.enterSeat);
            lastState = st;
        }
    }
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
    {
        bool sw = c.role == net::Role::Client ? (g_ctx.rs ? (g_ctx.rs->flags & net::PSF_SHARED_WANTED) != 0 : g_sharedWanted)
                                              : c.sharedWantedLocal;
        if(sw != g_sharedWanted) { Log(false, "[РОЗЫСК] режим: %s", sw ? "общий на двоих" : "у каждого свой"); g_lastRemoteWanted = -1; }
        g_sharedWanted = sw;
    }

    // «Не вытаскивать из машины» — только пока ты сидишь в той же машине (копы не выволакивают
    // второго CJ). Когда ты снаружи, флаг снят: с ним игра прячет кнопку «сесть»
    if(c.remote)
    {
        uintptr_t rv = PedVehicle(c.remote);
        uint8_t& f = Field<uint8_t>(c.remote, off::PedFlags587);
        if(rv && rv == PedVehicle(c.local)) f |= 0x20; else f &= ~0x20;
    }
    // Флаг «не вытаскивать из машины» второму CJ постоянно НЕ ставим: с ним игра не предлагает дверь водителя
    // и прячет кнопку «сесть» (CCarEnterExit::GetNearestCarDoor). Выкидывания нет и так —
    // перехват EnterAsDriverCtor сажает пассажиром.

    UpdateVehicles(c.dt);
    if(g_ctx.rs && c.remote) ManageRemoteSeat(*g_ctx.rs);
    else g_remoteBusy = false;
    UpdateCarInput();
    UpdateCombat();
    PassengerDriveBy();
    RemoteDriveByTick();
    UpdateWanted();
    if(c.remote) MirrorPlayer2Relations();
    TraceTransitions();
    DebugTick();
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
            if(v->haveState && (int32_t)(p->seq - v->st.seq) <= 0 && net::Since(g_ctx.now, v->stUs) < 2000000) return;
            v->st = *p; v->stUs = g_ctx.now; v->haveState = true; ++v->pktsIn;
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
            if(len == sizeof(net::ShotPacket)) { ++g_shotsIn; ReplayShot(*(const net::ShotPacket*)data); }
            break;
        case net::PKT_RELIABLE:
            if(len >= 1 && data[0] == net::REL_GIVE_WEAPON && len == sizeof(net::GiveWeaponEvent))
            {
                const auto* e = (const net::GiveWeaponEvent*)data;
                if(g_ctx.role == net::Role::Client) GiveWeaponLocal(e->weapon, (int)e->ammo, "Хост выдал тебе");
                return;
            }
            if(len >= 1 && data[0] == net::REL_NPC_DAMAGE && len == sizeof(net::NpcDamageEvent))
            {   // хост: друг ранил моего педа — наношу урон по-настоящему (атакующий — второй CJ)
                const auto* e = (const net::NpcDamageEvent*)data;
                uintptr_t ped = crowd::TrackedPed(e->npc);
                if(ped && PedIsAlive(ped))
                {
                    g_applyingNpcDamage = true;
                    GenerateDamageEvent(ped, g_ctx.remote, e->weapon, (int)(e->amount + 0.5f), e->piece, 0);
                    g_applyingNpcDamage = false;
                    Log(false, "[УРОН] Друг ранил педа %u: %.0f (оружие %d)%s", e->npc, e->amount, e->weapon, PedIsAlive(ped) ? "" : " — убит");
                }
                return;
            }
            if(len >= 1 && data[0] == net::REL_BUSTED && len == sizeof(net::BustedEvent))
            {
                if(g_ctx.role == net::Role::Client) crowd::OnBusted(g_ctx.local);
                return;
            }
            if(len >= 1 && data[0] == net::REL_DAMAGE && len == sizeof(net::DamageEvent))
            {
                const auto* e = (const net::DamageEvent*)data;
                uintptr_t l = g_ctx.local;
                if(!l || !PedIsAlive(l)) return;
                if(!e->npc && !g_ff) return;                         // от друга — только если огонь по своим
                uintptr_t attacker = e->npc ? crowd::CopyById(e->npc) : g_ctx.remote;
                g_applyingDamage = true;
                GenerateDamageEvent(l, attacker, e->weapon, (int)(e->amount + 0.5f), e->piece, 0);
                g_applyingDamage = false;
                if(e->npc) Log(false, "[УРОН] Пед хоста (%u) ранил тебя: %.0f (оружие %d)", e->npc, e->amount, e->weapon);
                else       Log(false, "[УРОН] Друг попал в тебя: %.0f (оружие %d)", e->amount, e->weapon);
            }
            break;
    }
}

namespace
{
    std::vector<std::string> g_notices;
    const char* WeaponName(int w)
    {
        switch(w)
        {
            case 1: return "кастет"; case 2: return "клюшка"; case 3: return "дубинка"; case 4: return "нож"; case 5: return "бита";
            case 6: return "лопата"; case 7: return "кий"; case 8: return "катана"; case 9: return "бензопила";
            case 16: return "гранаты"; case 17: return "слезоточивый газ"; case 18: return "коктейль Молотова";
            case 22: return "пистолет"; case 23: return "пистолет с глушителем"; case 24: return "Desert Eagle";
            case 25: return "дробовик"; case 26: return "обрез"; case 27: return "боевой дробовик";
            case 28: return "Micro Uzi"; case 29: return "MP5"; case 30: return "AK-47"; case 31: return "M4"; case 32: return "Tec-9";
            case 33: return "винтовка"; case 34: return "снайперская винтовка"; case 35: return "РПГ"; case 36: return "ракетница с наведением";
            case 37: return "огнемёт"; case 38: return "миниган"; case 39: return "взрывчатка"; case 41: return "баллончик";
            case 42: return "огнетушитель"; case 43: return "фотоаппарат"; case 46: return "парашют";
            default: return "оружие";
        }
    }
}

void GiveWeaponLocal(int w, int ammo, const char* from)
{
    uintptr_t l = g_ctx.local;
    if(!g_installed || !l || w <= 0 || w > 46) return;
    int model = WeaponModel(w);
    if(model && !ModelLoaded(model)) { RequestModel(model, 2); LoadAllRequestedModels(false); }
    if(model && !ModelLoaded(model)) { Log(true, "[ОРУЖИЕ] модель %d не загрузилась — %s не выдано", model, WeaponName(w)); return; }
    if(ammo < 1) ammo = 1;
    GiveWeapon(l, w, (unsigned)ammo, false);
    SetCurrentWeaponType(l, w);
    Log(false, "[ОРУЖИЕ] %s: %s, патронов %d", from, WeaponName(w), ammo);
    char t[160];
    snprintf(t, sizeof(t), "%s: %s (%d)", from, WeaponName(w), ammo);
    if(g_notices.size() < 8) g_notices.push_back(t);
}

bool SendGiveWeapon(int w, int ammo)
{
    if(!g_installed || g_echo || !g_ctx.connected || w <= 0 || w > 46) return false;
    net::GiveWeaponEvent e{ net::REL_GIVE_WEAPON, (uint8_t)w, 0, (uint32_t)(ammo < 1 ? 1 : ammo) };
    SendReliable(&e, sizeof(e));
    Log(false, "[ОРУЖИЕ] выдано другу: %s, патронов %d", WeaponName(w), ammo);
    return true;
}

bool PopNotice(char* out, int len)
{
    if(g_notices.empty()) return false;
    snprintf(out, len, "%s", g_notices.front().c_str());
    g_notices.erase(g_notices.begin());
    return true;
}

uint16_t CrowdVehicleId(uintptr_t veh)
{
    if(!g_installed || !g_active || !veh) return 0;
    if(NetVeh* v = FindVeh(veh)) return v->id;
    NetVeh* v = Register(veh, true);
    return v ? v->id : 0;
}
uintptr_t VehicleById(uint16_t id)
{
    NetVeh* v = FindId(id);
    return (v && Valid(*v)) ? v->veh : 0;
}
bool LocalTakesSeat(uintptr_t local, uintptr_t veh, int seat)
{
    if(!g_installed || !local || !veh) return false;
    LocalSeat ls = ReadLocalSeat(local);
    if(ls.veh) return ls.veh == veh && ls.seat == seat;
    return ls.enterVeh == veh && ls.enterSeat == seat;
}

bool EnterPedInto(uintptr_t ped, uintptr_t veh, int seat)
{
    if(!ped || !veh) return false;
    return GiveEnterTask(ped, veh, seat);
}

void WarpPedInto(uintptr_t ped, uintptr_t veh, int seat)
{
    if(!ped || !veh) return;
    if(!SeatFree(veh, seat, ped))
    {
        int p = FreePassengerSeat(veh, seat > 0 ? seat - 1 : 0);
        if(p < 0) return;
        seat = p + 1;
    }
    WarpIn(ped, veh, seat);
}
void WarpPedOut(uintptr_t ped) { if(ped && PedVehicle(ped)) WarpOut(ped); }
void EnsurePedWeapon(uintptr_t ped, int w) { if(ped && w >= 0 && w <= 46) EnsureWeapon(ped, w); }
int ClearRandomVehicles()
{
    if(!g_vehPoolSym) return 0;
    uintptr_t pool = *(uintptr_t*)g_vehPoolSym;
    if(!pool) return 0;
    uintptr_t objs = Field<uintptr_t>(pool, 0), flags = Field<uintptr_t>(pool, 8);
    int size = Field<int>(pool, 0x10), n = 0;
    if(!objs || !flags || size <= 0 || size > 4096) return 0;
    for(int i = 0; i < size; ++i)
    {
        if(Field<uint8_t>(flags, i) & 0x80) continue;
        uintptr_t veh = objs + (uintptr_t)i * kVehPoolEntry;
        if(FindVeh(veh) || Field<uint8_t>(veh, off::VehCreatedBy) != 1) continue;
        if(veh == PedVehicle(g_ctx.local) || veh == PedVehicle(g_ctx.remote)) continue;
        DeleteVehicle(veh); ++n;
    }
    return n;
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
    g_dbState = 0; g_dbWant = false; g_dbToggle = false;
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

int  PassengerAimState() { return g_dbState; }
void PassengerAimToggle() { g_dbToggle = true; }
}}
