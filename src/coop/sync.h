#pragma once
// Общий мир двух игроков поверх «второго CJ» (coop.cpp):
//  * машины — у каждой сетевой номер; состояние шлёт тот, чей игрок за рулём
//    (никто — тот, у кого машина появилась). У друга создаётся копия той же модели;
//  * места в машине — второй CJ садится/выходит с анимацией, не успел — переносится сразу;
//    кнопка «сесть» у машины, где за рулём друг, сажает пассажиром (а не выкидывает его);
//  * оружие в руках, прицел, выстрелы (повтор у друга), удары;
//  * урон: попадание по второму CJ у стрелка → событие с подтверждением → урон у друга
//    (огонь по своим решает хост); у себя второй CJ урона не получает;
//  * смерть и возрождение второго CJ, общий розыск, интерьеры.
// Эхо-режим («тест в одиночку») прогоняет всё через себя с задержкой и сдвигом.

#include <stdint.h>

#include "net/protocol.h"
#include "net/session.h"

class FLogger;

namespace coop { namespace sync {

    struct Ctx
    {
        net::Role role = net::Role::Off;
        bool      connected = false;      // друг на связи (в эхо — всегда)
        uintptr_t local = 0;              // свой CJ
        uintptr_t remote = 0;             // второй CJ (0 — нет)
        const net::PlayerStatePacket* rs = nullptr;  // свежее состояние друга (nullptr — нет)
        uint64_t  now = 0;
        float     dt = 0;
        bool      friendlyFireLocal = false;
        bool      sharedWantedLocal = true;
        float     echoOffset = 0;         // эхо: сдвиг на восток
    };

    bool Install(void* libHandle, FLogger* log);
    void SetSession(net::Session* s);

    // Дополнить свой снимок (оружие, машина, прицел, розыск…)
    void Capture(const Ctx& c, net::PlayerStatePacket& s);
    // Раз в кадр после приёма пакетов
    void Frame(const Ctx& c);
    // Приём игровых пакетов (из Session)
    void OnPacket(uint8_t type, const uint8_t* data, int len);

    // Второй CJ сейчас в машине / садится / выходит — coop не двигает его сам
    bool RemoteBusy();
    // Друг мёртв: второго CJ не убирать, а уложить
    bool RemoteShouldBeDead();
    // Перед удалением второго CJ (высадить из машины)
    void BeforeRemoteDespawn(uintptr_t remote);
    // Смена режима / загрузка игры: убрать все копии машин, забыть номера
    void Reset(const char* why);

    // Оружие от хоста (меню CO-OP): себе — сразу, другу — надёжным событием
    void GiveWeaponLocal(int weapon, int ammo, const char* from);
    bool SendGiveWeapon(int weapon, int ammo);
    // Сообщения для ленты уведомлений («Хост выдал тебе …»)
    bool PopNotice(char* out, int len);

    // Для общего мира людей (crowd)
    uint16_t  CrowdVehicleId(uintptr_t veh);          // хост: машина вокруг друга → общий номер
    uintptr_t VehicleById(uint16_t id);               // машина по общему номеру (0 — нет)
    void      WarpPedInto(uintptr_t ped, uintptr_t veh, int seat);
    void      WarpPedOut(uintptr_t ped);
    void      EnsurePedWeapon(uintptr_t ped, int weapon);
    int       ClearRandomVehicles();                  // друг: убрать свои случайные машины

    // Для строки состояния в логе
    int  TrackedVehicles();
}}
