#pragma once
// «Зайти сразу в мир хоста»: друг жмёт «Подключиться» (хоть в главном меню) → хост сохраняет
// свой текущий мир в отдельный файл (его ячейки не трогаются) → файл уходит другу кусками
// → игра друга загружает этот файл вместо своего сохранения → друга ставит рядом с хостом.
// Пока идёт подключение, у друга экран «Подключение к миру хоста NN %».

#include <stdint.h>
#include <stddef.h>

#include "net/session.h"

class FLogger;

namespace coop { namespace world {

    bool Install(void* libHandle, FLogger* log);
    void SetSession(net::Session* s);

    // Приём пакетов PKT_WORLD_* (вызывает coop из приёмника сессии)
    void OnPacket(uint8_t type, const uint8_t* data, int len);

    // Каждый кадр (в игре — из coop::OnFrame, в меню — из coop::MenuTick)
    void Tick(net::Role role, bool connected, bool inGame, uint64_t now);

    void OnSessionReset();              // смена режима / разрыв
    // Мир хоста получен: запустить загрузку. Из главного меню — в MobileMenu::Update (fromMenuUpdate),
    // из игры — в игровом кадре.
    bool RunDeferredLoad(bool fromMenuUpdate);   // true — загрузка запущена
    void AfterGameLoaded();             // CGenericGameStorage::GenericLoad закончил

    // После загрузки мира хоста — поставить своего CJ рядом с хостом (один раз)
    bool TeleportPending();
    void TeleportDone();

    void RequestSaveCopy();             // друг: «Сохранить мир хоста себе» (ячейка 8)

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
