#pragma once
// Жизнь города: персонажи появляются по распорядку, цели, инициатива, банды, режиссёр, профили ИИ.

#include "world/world.h"

namespace life
{
    void Frame();
    void RequestProfile(world::Character& c);     // придумать прошлое/манеру речи (ИИ, в фоне)
    void OnKill(uintptr_t victim, int weapon);   // игрок убил педа
    void SpawnHelpers(int faction, int count, bool hostile);  // позвать своих (call gang)
    void Ambush(int faction, int count);         // засада на игрока
    void RunDirector(bool force);
}
