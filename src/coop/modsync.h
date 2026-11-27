#pragma once
// Сверка модов с хостом — ТЗ 1.1 §8а (1.5.1, протокол 18).
//  * Сверяется по имени, размеру и контрольной сумме: AML-моды (.so в папке модов AML), скрипты CLEO (SA/cleo, с папками),
//    файлы игры и данных модов (папка files игры: замены файлов, голоса/распознавание SA Living).
//  * Не сверяется: SA Coop, логи, сохранения, настройки (ключ API SA Living не уходит никогда), личные папки модов.
//    Файл с ключом внутри (sk-ant-, tskey-auth-) не отправляется (в лог «похож на секрет»).
//  * Друг: лишние (у хоста нет) — сами в SA/SACoop/Отключённые моды (кнопка «Вернуть мои моды», сами — без коопа);
//    не хватает / другая версия — кнопка «Установить от хоста»: файлы идут с телефона хоста кусками с подтверждением,
//    кладутся туда же, что у хоста; старая версия — в SA/SACoop/Резерв модов. Потом — перезапуск игры.
//  * Хост: «Запуск» только когда моды совпали (или «Играть без сверки»).
//  * Свой SA Coop не трогается никогда.

#include <stdint.h>
#include <string>
#include <vector>
#include "net/session.h"

namespace modsync
{
    void Init(net::Role role);                         // при загрузке мода: вернуть отключённое, если не «Подключиться»
    void SetSession(net::Session* s);
    void Frame(net::Role role, bool connected, bool inGame, uint64_t now);
    void OnPacket(uint8_t type, const uint8_t* data, int len);
    void OnRoleChanged(net::Role role);                // меню: режим сменился

    struct Line { std::string text; int level; };      // 0 — ок, 1 — внимание, 2 — важно
    std::vector<Line> LobbyLines();                    // что показать в лобби (пусто — ещё не сверяли)
    bool PopNotice(std::string& out);                  // сообщение для ленты меню

    // Кнопки лобби
    struct Actions
    {
        bool canInstall = false;  std::string installLabel;   // друг: «Установить от хоста (N, X МБ)»
        bool canRestore = false;                              // друг/любой: «Вернуть мои моды»
        bool hostCanSkip = false; bool skipping = false;      // хост: «Играть без сверки»
    };
    Actions GetActions();
    void StartInstall();
    void RestoreNow();
    void SetSkip(bool on);
    bool LaunchAllowed();                              // хост: моды совпали или «без сверки»
}
