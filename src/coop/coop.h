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
        net::Link link;           // этап рукопожатия
        char      peerNick[net::kNickLen];
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

    // ---- для слоя поверх игры (панель «Команда», уведомления, микрофон) ----
    struct TeamMember
    {
        bool  present = false;    // показывать строку
        bool  lost = false;       // связь пропала (строка сереет)
        char  nick[net::kNickLen] = "";
        float health = 0, armour = 0;
        int   pingMs = -1;
        float distM = -1;
        bool  talking = false;
    };
    struct Team
    {
        bool       show = false;  // панель видна (кооп активен)
        net::Role  role = net::Role::Off;
        char       myNick[net::kNickLen] = "";
        float      myHealth = 0, myArmour = 0;
        TeamMember mate;
    };
    Team GetTeam();
    bool PopEvent(net::Event& out);
    bool PendingRequest(char* nick, size_t len, float* secondsLeft); // хост: висит запрос?
    void AnswerJoin(bool accept);
    void SetTalking(bool talking);                                   // кнопка микрофона зажата

    // Спрятать второго CJ из слота игрока 2 на время чужого кода (например, отрисовки
    // интерфейса игры: иначе она рисует вторую иконку оружия — «дубль кулака»).
    bool      GameLoaded();                 // игрок на карте (сейв загружен)
    void      LockLocalControls(bool lock); // на время редактора раскладки CJ не двигается
    uintptr_t HideRemoteBegin();
    void      HideRemoteEnd(uintptr_t token);

    // Что мод делал последним (для записи о вылете)
    const char* Where();
}
