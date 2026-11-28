#pragma once
// Режим S.W.A.T. (SA Story 0.2.0).
//  * «Новая игра» → окно выбора «Обычная GTA / S.W.A.T.». Обычная — мод ничего не трогает.
//  * S.W.A.T.: родное вступление (миссия INTRO) не запускается, родные миссии выключены и спрятаны, вместо открытия
//    экрана — полёт на вертолёте к базе. Герой — Артём (скин S.W.A.T., модель 285), звёзд не получает, полиция
//    дружественна, после смерти — медпункт базы, без штрафов.
//  * Состояние — рядом с каждым сохранением: <Android/data/…/files>/SAStory/<имя файла сохранения>.txt (story=1 → режим).
//    Нет файла — обычная игра, мод в ней ничего не трогает.

#include <stdint.h>
#include <string>

namespace story
{
    struct State
    {
        bool on = false;     // режим S.W.A.T. в этой игре
        int  progress = 0;   // (запас на будущее: сюжетные миссии отложены)
        int  hero = 0;       // модель героя
        int  money = 0;
    };

    bool Install(void* libHandle);
    const State& Get();
    bool On();
    void SetProgress(int p);
    void Frame();

    // Значения режима (база, статистика, гараж, вид), пишутся в файл сохранения
    int  GetInt(const std::string& key, int def);
    void SetInt(const std::string& key, int v);
}
