#pragma once
// Общий мир людей: педы (прохожие, банды, копы, педы миссий) и машины вокруг друга — одни и те же у обоих.
// Хост — хозяин: его телефон считает ИИ, урон, смерть; другу шлёт педов в радиусе ~120 м от него
// (10 раз/с), машины — через общие машины (sync). У друга своё население выключено, вместо него — копии.
//  * Друг бьёт/стреляет в копию → событие хосту → хост наносит урон настоящему педу (атакующий — второй CJ),
//    пед реагирует (убегает, нападает) — это и видит друг.
//  * Пед хоста бьёт/стреляет во второго CJ → урон приходит другу (как от того педа).
//  * Копы хоста гоняются и за другом, если у него розыск; скрутили второго CJ → у друга «Арестован».

#include <stdint.h>

#include "net/protocol.h"
#include "net/session.h"

class FLogger;

namespace coop { namespace crowd {

    bool Install(void* libHandle, FLogger* log);
    void SetSession(net::Session* s);

    // Каждый игровой кадр (после sync::Frame)
    void Frame(net::Role role, bool connected, uintptr_t local, uintptr_t remote,
               const net::PlayerStatePacket* rs, uint64_t now);
    void OnPacket(uint8_t type, const uint8_t* data, int len);
    void Reset(const char* why);

    bool Active();                          // общий мир включён (хост/клиент на связи)
    bool ClientMode();                      // мы клиент: своё население выключено

    // Для sync (урон, выстрелы)
    uint16_t TrackedId(uintptr_t ped);      // хост: номер педа, если его видит друг (0 — нет)
    bool     IsCopy(uintptr_t ped);         // клиент: копия педа хоста
    bool     CopyDying(uintptr_t ped);      // клиент: копия, которую хост «положил»
    uint16_t CopyId(uintptr_t ped);
    uintptr_t CopyById(uint16_t id);
    bool     AllowCopyDamage();             // сейчас можно ранить копию (её убивает сам мод)

    // Копы хоста могут арестовать второго CJ (у друга розыск)
    bool RemoteArrestable();
    uintptr_t TrackedPed(uint16_t id);      // хост: пед по номеру
    void OnBusted(uintptr_t local);         // друг: копы хоста скрутили тебя
}}
