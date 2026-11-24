#pragma once
// Этап 1: второй CJ (друг) на карте.
// Друг создаётся как штатный «игрок 2» игры (CWorld::Players[1]),
// но двухпользовательский режим PS2 (общая камера, «поводок») отключается.

#include <stddef.h>

#include "net/session.h"

class FLogger;

namespace coop
{
    // Ставит перехваты в libGTASA.so. Вызывать один раз из OnModLoad.
    bool Install(void* libHandle, FLogger* log);

    // Сменить режим (выкл / эхо / хост / клиент) или адрес. Можно звать из меню
    // в любой момент: применится в ближайшем игровом кадре.
    void        SetConfig(const net::Config& cfg);
    net::Config GetConfig();

    // Короткая строка состояния для меню (латиница — шрифт меню без кириллицы).
    void StatusText(char* out, size_t len);

    // Что мод делал последним (для записи о вылете)
    const char* Where();
}
