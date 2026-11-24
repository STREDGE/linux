#pragma once
// Этап 1: второй CJ (друг) на карте.
// Друг создаётся как штатный «игрок 2» игры (CWorld::Players[1]),
// но двухпользовательский режим PS2 (общая камера, «поводок») отключается.

#include "net/session.h"

class FLogger;

namespace coop
{
    // Ставит перехваты в libGTASA.so. Вызывать один раз из OnModLoad.
    bool Install(void* libHandle, const net::Config& cfg, FLogger* log);

    // Что мод делал последним (для записи о вылете)
    const char* Where();
}
