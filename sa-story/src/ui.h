#pragma once
// Экран SA Story (Dear ImGui через RenderWare игры): название, цель, субтитры, таймер, счётчик, итог; выбор режима при
// «Новой игре»; меню базы (оружейная, кабинет Фрэнка, гараж…); кнопки действий («Задержать»); звёзды розыска над NPC.
// Касания — опросом массива Points (через GOT, как SA Coop/SA Living), игре касания не перехватываем.
// Все вызовы — из игрового потока.

#include <functional>
#include <string>
#include <vector>

#include "game.h"

namespace ui
{
    bool Init(void* libHandle);

    void Title(const std::string& text, float sec = 3.5f);                     // большое название
    void Objective(const std::string& text, float sec = 6.0f);                 // цель внизу экрана
    void Subtitle(const std::string& who, const std::string& text, float sec, unsigned rgb);   // реплика
    void ClearSubtitle();
    void Timer(int secondsLeft);                                               // -1 — убрать
    void Counter(const std::string& label, int value);                         // value < 0 — убрать
    void Result(bool passed, const std::string& text, const std::string& sub);
    void Notice(const std::string& text, float sec = 5.0f);                    // сообщение вверху
    void Clear();

    // «Новая игра» в меню игры: окно «Обычная GTA / S.W.A.T. / Назад». choice: 0 обычная, 1 S.W.A.T.
    void SetNewGameHandler(std::function<void(int choice)> fn);

    // Меню в игре: пункты; выбор → fn(index), закрыть → fn(-1)
    void OpenMenu(const std::string& title, const std::vector<std::string>& items, std::function<void(int)> fn, const std::string& footer = "");
    void CloseMenu();
    bool MenuOpen();

    // Кнопка действия: показать в этом кадре (вызывать каждый кадр, пока нужна)
    void ActionButton(int slot, const std::string& label, std::function<void()> fn);
    // Звёзды розыска над человеком (в этом кадре)
    void Stars(const game::Vec& world, int stars, bool surrendered);
    // Надпись над точкой в мире (в этом кадре): название и подсказка под ним
    void Sign(const game::Vec& world, const std::string& text, const std::string& hint, unsigned rgb);
    // Строка справа вверху (статус базы/тревога) в этом кадре
    void Status(const std::string& text, unsigned rgb);
}
