#pragma once
// Этап 1: второй CJ (друг) на карте.
// Друг создаётся как штатный «игрок 2» игры (CWorld::Players[1]),
// но двухпользовательский режим PS2 (общая камера, «поводок») отключается.

#include <stddef.h>

#include "net/session.h"
#include "net/tailscale.h"
#include "coop/world.h"

#include <vector>

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
        char      room[8];        // хост через сервер-посредник: код комнаты
        bool      relay;          // режим посредника
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
    bool TalkButtonOwned();                                          // кнопку «Говорить» рисует другой мод (SA Living) — золотую не показывать
    void SetFriendlyFire(bool on);                                   // огонь по своим (у клиента решает хост)
    void SetSharedWanted(bool on);                                   // общий розыск (у клиента решает хост)
    // Хост выдаёт оружие (меню CO-OP): toFriend — другу, иначе себе. Выполнится в игровом кадре.
    void GiveWeapon(bool toFriend, int weapon, int ammo);
    bool CanGiveToFriend();                                          // хост и друг на связи
    bool PopNotice(char* out, size_t len);                           // «Хост выдал тебе …» и т.п.

    // Сеть, пока игра не загружена (главное меню): вызывать каждый кадр меню
    void MenuTick();
    // Экран «Подключение к миру хоста NN %» (у друга)
    struct JoinProgress { bool active = false; int percent = 0; char title[96] = ""; char detail[160] = ""; };
    JoinProgress GetJoinProgress();
    void CancelJoin();                                               // «Отмена» на экране подключения
    bool MenuUpdateTick();
    bool CanSaveHostWorld();                                         // друг на связи с хостом
    void SaveHostWorld();                                            // «Сохранить мир хоста себе» (ячейка 8)                                           // из MobileMenu::Update; true — меню закрыто, идёт загрузка мира хоста

    // Встроенный Tailscale: точка хоста и поиск по ID
    struct NetInfo
    {
        ts::State tsState = ts::State::Off;
        char loginUrl[512] = "";     // нужен вход — открыть в браузере
        char tsIp[48] = "";
        char info[192] = "";         // что сейчас происходит
        char myId[8] = "";           // свой ID точки
        bool pointOpen = false;      // хост: точка открыта
        bool searching = false;      // друг: ищем точку хоста
    };
    NetInfo GetNetInfo();
    void StartTailscale();                                           // включить заранее (войти в аккаунт)
    bool TailscaleLogout();                                          // сменить аккаунт
    void SetTsKey(const char* key);                                  // ключ для кода приглашения (без сброса сессии)

    // Лобби (оба на связи): хост выбирает сохранение и жмёт «Запуск»
    struct LobbyInfo
    {
        bool  active = false;        // друг на связи — показываем лобби
        bool  isHost = false;
        char  peerNick[net::kNickLen] = "";
        int   pingMs = -1;
        bool  inGame = false;        // сам уже в игре
        world::Lobby lobby;
    };
    LobbyInfo GetLobbyInfo();
    std::vector<world::SaveEntry> ListSaves();
    const char* SavesFolder();
    bool Launch(const char* path, const char* name, char* why, size_t whyLen);   // path == nullptr — текущая игра

    // Спрятать второго CJ из слота игрока 2 на время чужого кода (например, отрисовки
    // интерфейса игры: иначе она рисует вторую иконку оружия — «дубль кулака»).
    bool      GameLoaded();                 // игрок на карте (сейв загружен)
    void      LockLocalControls(bool lock); // на время редактора раскладки CJ не двигается
    uintptr_t HideRemoteBegin();
    void      HideRemoteEnd(uintptr_t token);

    // Что мод делал последним (для записи о вылете)
    const char* Where();
}
