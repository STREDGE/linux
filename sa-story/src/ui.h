#pragma once
// Текст миссий на экране (Dear ImGui через RenderWare игры): название, цель, субтитры, таймер, счётчик, итог.
// Свой шрифт с кириллицей (Russo One, Rubik). Все вызовы — из игрового потока.

#include <string>

namespace ui
{
    bool Init(void* libHandle);

    void Title(const std::string& text, float sec = 3.5f);                     // большое название миссии
    void Objective(const std::string& text, float sec = 6.0f);                 // цель внизу экрана
    void Subtitle(const std::string& who, const std::string& text, float sec, unsigned rgb);   // реплика героя
    void ClearSubtitle();
    void Timer(int secondsLeft);                                               // -1 — убрать
    void Counter(const std::string& label, int value);                         // value < 0 — убрать
    void Result(bool passed, const std::string& text, const std::string& sub); // «МИССИЯ ПРОЙДЕНА/ПРОВАЛЕНА»
    void Notice(const std::string& text, float sec = 5.0f);                    // сообщение мода вверху
    void Clear();                                                              // убрать всё (кроме итога)
}
