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

    // Состояние для своего меню и интерфейса
    struct Status
    {
        net::Role role;
        bool      pending;        // режим выбран, применится при загрузке игры
        bool      running;        // сокет открыт
        bool      connected;      // от друга идут пакеты
        int       pingMs;         // -1 — неизвестен (у хоста)
        bool      remoteSpawned;  // второй CJ на карте
        char      host[64];
    };
    Status GetStatus();

    // Вызывать каждый кадр, пока открыто меню игры: в режимах «Хост» и «Подключиться»
    // снимает паузу (CTimer::m_UserPause), чтобы мир и связь с другом не замирали.
    void KeepWorldRunningInMenu();

    // Что мод делал последним (для записи о вылете)
    const char* Where();
}
