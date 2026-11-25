#pragma once
// Мир для двоих и лобби.
//  * Лобби: друг подключился → оба в лобби. Хост выбирает, во что играть: текущая игра,
//    кооп-сохранение из папки SA/SACoop/Сохранения или файл с телефона — и жмёт «Запуск».
//    Хост загружает выбранное у себя, тот же файл уходит другу кусками, друг загружается,
//    его ставит рядом с хостом. У друга экран «Подключение к миру хоста NN %».
//  * Кооп-сохранения отдельно от одиночных: пока идёт кооп-мир, все ячейки сохранения игры
//    ведут в файлы SACoop_N.b (одиночные GTASAsfN.b не трогаются), а копия каждого
//    сохранения кладётся в папку SA/SACoop/Сохранения — её видно в файлах телефона.
//  * Без коопа ничего не меняется: обычная одиночная игра и её ячейки.

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include <vector>

#include "net/session.h"

class FLogger;

namespace coop { namespace world {

    bool Install(void* libHandle, FLogger* log);
    void SetSession(net::Session* s);

    // Приём пакетов PKT_WORLD_* и PKT_LOBBY (вызывает coop из приёмника сессии)
    void OnPacket(uint8_t type, const uint8_t* data, int len);

    // Каждый кадр (в игре — из coop::OnFrame, в меню — из coop::MenuTick)
    void Tick(net::Role role, bool connected, bool inGame, uint64_t now);

    void OnSessionReset();              // смена режима / разрыв
    // Загрузка (мир хоста у друга / выбранное сохранение у хоста). Из главного меню —
    // в MobileMenu::Update (fromMenuUpdate), из игры — в игровом кадре.
    bool RunDeferredLoad(bool fromMenuUpdate);   // true — загрузка запущена
    void AfterGameLoaded();             // CGenericGameStorage::GenericLoad закончил

    // После загрузки мира хоста — поставить своего CJ рядом с хостом (один раз)
    bool TeleportPending();
    void TeleportDone();

    void RequestSaveCopy();             // друг: «Сохранить мир хоста себе» (ячейка 8 + папка)

    // ---- лобби ----
    struct SaveEntry { char name[96]; char path[320]; long size; time_t mtime; };
    std::vector<SaveEntry> ListSaves();          // папка SA/SACoop/Сохранения, новые сверху
    const char* SavesFolder();                   // полный путь папки (для подсказки)
    // Хост: «Запуск». path == nullptr — текущая игра (хост уже играет). false — не вышло (why).
    bool HostLaunch(const char* path, const char* name, char* why, size_t whyLen);
    struct Lobby
    {
        uint32_t launchId = 0;
        int      source = 0;            // net::LobbySource
        bool     hostInGame = false, hostLoading = false;
        char     saveName[64] = "";
    };
    Lobby GetLobby();                   // у хоста — своё, у друга — что прислал хост
    bool CoopWorld();                   // идёт кооп-мир: сохранения — в кооп-ячейки

    struct Progress
    {
        bool active = false;            // показывать экран подключения
        int  percent = 0;
        char title[96] = "";
        char detail[160] = "";
    };
    Progress GetProgress(net::Link link);
    bool PopNotice(char* out, size_t len);
}}
