#pragma once
// Жизнь города: персонажи появляются по распорядку, цели, инициатива, банды, режиссёр, профили ИИ.

#include "world/world.h"

namespace life
{
    void Frame();
    void RequestProfile(world::Character& c);     // придумать прошлое/манеру речи (ИИ, в фоне)
    void OnKill(uintptr_t victim, int weapon, int playerId);   // игрок убил педа
    // позвать своих (call gang); target — к кому (0 — к игроку этого телефона; у хоста можно второго CJ)
    void SpawnHelpers(int faction, int count, bool hostile, uintptr_t target = 0);
    void Ambush(int faction, int count, uintptr_t target = 0);         // засада на игрока (или второго CJ)
    void HelpersForFriend(int faction, int count, bool hostile);      // хост: друг позвал своих — к нему (сообщение «helpers»)
    void RunDirector(bool force);
}
