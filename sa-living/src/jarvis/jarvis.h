#pragma once
// «Джарвис» — личный голосовой помощник игрока (не путать с мозгом педов: src/talk, src/sim).
// Слушает команду, выполняет её в игре и отвечает своим голосом. С ключом ИИ понимает свободную речь
// и умеет собирать нужное из команд сценария игры; без ключа — простые фразы по шаблонам.
// Всё — из игрового потока.

#include <string>
#include <vector>

#include "util/json.h"

namespace jarvis
{
    void Init(void* libHandle, const std::string& baseDir);
    void Frame();

    // Вызов
    void ButtonDown();                 // кнопка на экране: держишь — говоришь
    void ButtonUp();
    void Trigger();                    // пункт меню CLEO: слушает до 6 с (или до касания плашки)
    void TapPlate();                   // закончить слушать / скрыть плашку

    enum State { IDLE, LISTENING, RECOGNIZING, THINKING, SPEAKING };
    struct View
    {
        bool visible = false;
        State state = IDLE;
        std::string heard, reply;      // что услышал, что ответил
        std::vector<std::string> done; // что сделано («Выдан миниган, 1000 патронов»)
        float listenLeft = 0;          // сколько ещё слушает (вызов из меню CLEO)
    };
    View GetView();

    void Preview();                    // сказать пробную фразу своим голосом (настройки)

    // Справочник команд игры (для просьб «чего нет в списке»)
    bool LibraryReady();
    bool LibraryFilePresent();
    void DownloadLibrary();

    // Кооп: друг получил «выдать другу»
    void OnCoopMessage(const json::Value& m);

    // Перехват «подсказки» с меткой SALJRV — так нас зовёт скрипт из меню CLEO
    bool OnHelpLabel(const char* label);
}
