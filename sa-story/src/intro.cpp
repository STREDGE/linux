// Вступление режима S.W.A.T. (ТЗ-SWAT-MODE §3): новая игра → Артём в летящем полицейском «Маврике» с пилотом-сотрудником,
// прикреплён к борту (как в миссиях игры: ATTACH_CHAR_TO_CAR), стреляет из M4. ~1 минута над городом к базе, по пути —
// бандиты в машинах. Посадка на площадку крыши штаба (свой пилот: последние метры доводит сам), отцепить — свободная игра.
#include "intro.h"

#include <math.h>
#include <vector>

#include <swat/sgame.h>
#include <swat/swat_internal.h>

#include "base.h"
#include "layout.h"
#include "log.h"
#include "ui.h"

using namespace swat;

namespace intro
{
namespace
{
    enum Stage { S_OFF, S_LOAD, S_FLY, S_APPROACH, S_LAND, S_DONE };
    Stage g_stage = S_OFF;
    int g_heli = 0, g_pilot = 0, g_wp = 0;
    uint32_t g_start = 0, g_stageAt = 0;
    bool g_enemies[2] = {};
    std::vector<int> g_spawned;
    uint32_t g_aimLostAt = 0, g_reattachAt = 0; int g_reattached = 0;
    int g_pilotOut = 0; uint32_t g_pilotOutAt = 0;

    void Attach(int pc)
    {
        // прикрепить к борту, как в миссиях игры (mainV1.scm, миссия 57): вид от первого лица с автоматом
        Do(0x0464, { Ai(pc), Ai(g_heli), Af(-1.4f), Af(1.0f), Af(-0.1f), Ai(1), Af(90.0f), Ai(31) });   // ATTACH_CHAR_TO_CAR
    }
    // Страж: меню CLEO (свайп вниз) и пауза сбрасывают режим «стрелок на борту» (состояние 12) — камера ломается.
    // Если режим сброшен дольше 0,5 с — прикрепляем заново (не чаще раза в 2 с).
    void KeepAttached(int pc)
    {
        uintptr_t pl = Player(); if(!pl || g_heli <= 0) return;
        uint32_t now = NowMs();
        if(PedState(pl) == 12) { g_aimLostAt = 0; return; }
        if(!g_aimLostAt) { g_aimLostAt = now; return; }
        if(now - g_aimLostAt < 500 || now - g_reattachAt < 2000 || g_reattached >= 12) return;
        g_reattachAt = now; g_aimLostAt = 0; g_reattached++;
        LOG("ВСТУПЛЕНИЕ", "режим стрелка сброшен (состояние %d) — прикрепляю к борту заново (%d-й раз)", PedState(pl), g_reattached);
        Attach(pc);
        Do(0x0373, {});                                                                         // камера за игроком
    }
    // маршрут над городом к порту (≈1500 м → около минуты)
    const float kRoute[][3] = { { 1480, -1250, 110 }, { 1780, -1560, 95 }, { 2150, -1880, 85 }, { 2480, -2180, 70 } };
    constexpr int kWp = 4;

    uint32_t Now() { return NowMs(); }
    bool Loaded(const std::vector<int>& m) { bool all = true; for(int x : m) if(!Is(0x0248, { Ai(x) })) { Do(0x0247, { Ai(x) }); all = false; } return all; }

    void SpawnEnemies(int i)
    {
        // 2 машины бандитов на дороге под маршрутом: стреляют по вертолёту, цели для Артёма
        V3 at = Mk(kRoute[i + 1][0], kRoute[i + 1][1], 20), node; float h = 0;
        if(!NthCarNode(at, 2, node, h)) return;
        int car = OutI(0x00A5, { Ai(i ? 492 : 412), Af(node.x), Af(node.y), Af(node.z + 0.5f), Aoi() });
        if(car <= 0) return;
        Do(0x0175, { Ai(car), Af(h) });
        for(int k = 0; k < 3; ++k)
        {
            int ped = k == 0 ? OutI(0x0129, { Ai(car), Ai(7), Ai(102 + k), Aoi() }) : OutI(0x01C8, { Ai(car), Ai(7), Ai(102 + k), Ai(k - 1), Aoi() });
            if(ped <= 0) continue;
            swat::GiveWeapon(ped, 28, 500);
            if(k) Do(0x0713, { Ai(ped), Ai(PlayerRef()), Ai(-1), Af(0), Af(0), Af(0), Af(80.0f), Ai(4), Ai(0), Ai(60) });   // TASK_DRIVE_BY по Артёму
            else Do(0x05D1, { Ai(ped), Ai(car), Af(node.x + 300), Af(node.y - 300), Af(node.z), Af(20.0f), Ai(0), Ai(-1), Ai(2) });
            g_spawned.push_back(ped);
            Do(0x01C2, { Ai(ped) });
        }
        Do(0x01C3, { Ai(car) });
        LOG("ВСТУПЛЕНИЕ", "бандиты на маршруте (%d)", i + 1);
    }
}

void Start()
{
    g_stage = S_LOAD; g_heli = g_pilot = 0; g_wp = 0; g_start = g_stageAt = Now(); g_enemies[0] = g_enemies[1] = false;
    g_aimLostAt = g_reattachAt = 0; g_reattached = 0;
    base::HoldPad(true);                                    // площадка на крыше — за нашим вертолётом
    LOG("ВСТУПЛЕНИЕ", "старт: вертолёт к базе");
}
bool Active() { return g_stage != S_OFF && g_stage != S_DONE; }
bool Attached() { return g_stage == S_FLY || g_stage == S_APPROACH || g_stage == S_LAND; }
void Abort() { g_stage = S_OFF; base::HoldPad(false); }

void Frame()
{
    if(g_pilotOut > 0 && Now() - g_pilotOutAt > 7000)
    {   // пилот вышел — «ушёл в штаб»
        if(Is(0x056D, { Ai(g_pilotOut) })) Do(0x009B, { Ai(g_pilotOut) });                      // DELETE_CHAR
        g_pilotOut = 0;
    }
    if(g_stage == S_OFF || g_stage == S_DONE || !Player()) return;
    int pc = PlayerRef();
    switch(g_stage)
    {
        case S_LOAD:
        {
            // 356 — модель M4, 352 — модель Uzi (не номера оружия 31/28): с моделью грузится файл анимаций стрельбы
            if(!Loaded({ 497, 280, 356, 412, 492, 102, 103, 104, 352 })) { if(Now() - g_stageAt > 5000) Do(0x038B, {}); return; }
            if(Model(Player()) != 285 && Now() - g_stageAt < 8000) return;          // сначала скин S.W.A.T. (к борту — уже Артёмом)
            V3 s = Mk(kRoute[0][0], kRoute[0][1], kRoute[0][2]);
            Do(0x01B4, { Ai(0), Ai(0) });                                                       // SET_PLAYER_CONTROL 0
            g_heli = OutI(0x00A5, { Ai(497), Af(s.x), Af(s.y), Af(s.z), Aoi() });
            if(g_heli <= 0) { LOG("ОШИБКА", "вступление: вертолёт не создан — сразу на базу"); g_stage = S_DONE; base::HoldPad(false); Do(0x00A1, { Ai(pc), Af(layout::kCx), Af(layout::kCy + 30), Af(layout::kZ + 1) }); Do(0x01B4, { Ai(0), Ai(1) }); Do(0x016A, { Ai(1000), Ai(1) }); return; }
            Do(0x0175, { Ai(g_heli), Af(Heading(s, Mk(kRoute[1][0], kRoute[1][1], 0))) });
            Do(0x0825, { Ai(g_heli) });                                                         // SET_HELI_BLADES_FULL_SPEED
            Do(0x02AC, { Ai(g_heli), Ai(1), Ai(1), Ai(1), Ai(1), Ai(1) });                      // SET_CAR_PROOFS — сюжетный борт не собьют
            g_pilot = OutI(0x0129, { Ai(g_heli), Ai(6), Ai(280), Aoi() });                       // пилот-сотрудник
            if(g_pilot > 0) Do(0x02AB, { Ai(g_pilot), Ai(1), Ai(1), Ai(1), Ai(1), Ai(1) });
            swat::GiveWeapon(pc, 31, 600);                                             // M4
            Attach(pc);
            Do(0x01B4, { Ai(0), Ai(1) });                                                       // управление (стрельба) — есть
            Do(0x0373, {});
            Do(0x016A, { Ai(1500), Ai(1) });                                                    // DO_FADE — открыть экран
            ui::Title("S.W.A.T.", 3.0f);
            ui::Subtitle("Пилот", "Артём, держись крепче! Через минуту база. Внизу бандиты — работай!", 5.0f, 0x9AD08A);
            g_stage = S_FLY; g_stageAt = Now(); g_wp = 1;
            break;
        }
        case S_FLY:
        {
            uintptr_t v = Veh(g_heli);
            if(!v) { g_stage = S_DONE; base::HoldPad(false); return; }
            KeepAttached(pc);
            V3 p = Pos(v);
            if(g_wp < kWp)
            {
                V3 to = Mk(kRoute[g_wp][0], kRoute[g_wp][1], kRoute[g_wp][2]);
                drv::Fly(g_heli, g_pilot, drv::H_GOTO, Mk(to.x, to.y, GroundZ(to.x, to.y, to.z)), 0, 0, 30, to.z - GroundZ(to.x, to.y, to.z));
                if(Dist2(p, to) < 60) { g_wp++; if(g_wp == 2 && !g_enemies[0]) { g_enemies[0] = true; SpawnEnemies(0); } if(g_wp == 3 && !g_enemies[1]) { g_enemies[1] = true; SpawnEnemies(1); } }
            }
            else
            {
                // заход: зависнуть в 14 м над площадкой (пилот сам гасит скорость), потом вертикально вниз
                game::Vec pad = layout::W(layout::kPadU, layout::kPadV); pad.z = layout::kRoofZ;
                drv::Fly(g_heli, g_pilot, drv::H_HOVER, Mk(pad.x, pad.y, pad.z), 0, 0, 20, 14);
                g_stage = S_APPROACH; g_stageAt = Now();
                ui::Subtitle("Пилот", "База под нами. Захожу на крышу штаба.", 4.0f, 0x9AD08A);
            }
            if(Now() - g_start > 100000) { g_wp = kWp; }
            break;
        }
        case S_APPROACH:
        {
            uintptr_t v = Veh(g_heli);
            if(!v) { g_stage = S_DONE; base::HoldPad(false); return; }
            KeepAttached(pc);
            game::Vec pad = layout::W(layout::kPadU, layout::kPadV); pad.z = layout::kRoofZ;
            V3 hov = Mk(pad.x, pad.y, pad.z + 14.0f);
            bool over = Dist(Pos(v), hov) < 3.0f && Speed(v) < 2.0f;
            if(!over && Now() - g_stageAt < 40000) return;
            drv::Fly(g_heli, g_pilot, drv::H_LAND, Mk(pad.x, pad.y, pad.z), 0, 0, 20, 14);
            g_stage = S_LAND; g_stageAt = Now();
            LOG("ВСТУПЛЕНИЕ", "над площадкой (заход %u с) — вертикальный спуск", (Now() - g_start) / 1000);
            break;
        }
        case S_LAND:
        {
            uintptr_t v = Veh(g_heli);
            KeepAttached(pc);
            bool landed = drv::Landed(g_heli) || (v && Speed(v) < 0.6f && Pos(v).z - layout::kRoofZ < 2.5f) || Now() - g_stageAt > 30000;
            if(!landed) return;
            LOG("ВСТУПЛЕНИЕ", "спуск занял %u с", (Now() - g_stageAt) / 1000);
            Do(0x0465, { Ai(pc) });                                                             // DETACH_CHAR_FROM_CAR — Артём сходит с борта там, где стоит
            Do(0x0373, {});
            Do(0x01B4, { Ai(0), Ai(1) });
            drv::Ground(g_heli);
            if(g_pilot > 0)
            {   // пилот не гуляет по крыше: уходит к спуску и исчезает
                game::Vec lift = layout::W(layout::kRoofLiftU, layout::kRoofLiftV); lift.z = layout::kRoofZ + 0.5f;
                Do(0x0633, { Ai(g_pilot) });                                                    // TASK_LEAVE_ANY_CAR
                g_pilotOut = g_pilot; g_pilotOutAt = Now();
                (void)lift;
            }
            Do(0x02AC, { Ai(g_heli), Ai(0), Ai(0), Ai(0), Ai(0), Ai(0) });
            base::HeliIntroLanded(g_heli);
            ui::Title("БАЗА S.W.A.T.", 3.0f);
            ui::Objective("Спустись с крыши — жёлтая метка «Вниз». Фрэнк ждёт у кабинета.", 8.0f);
            LOG("ВСТУПЛЕНИЕ", "посадка на крышу штаба за %u с — свободная игра", (Now() - g_start) / 1000);
            g_stage = S_DONE;
            break;
        }
        default: break;
    }
}
}
