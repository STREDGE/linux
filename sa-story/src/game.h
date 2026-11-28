#pragma once
// Доступ к libGTASA 2.10 arm64 (только то, что нужно сюжету). Только игровой поток.
// Всё «действие» (педы, машины, метки, задачи) делается командами сценария игры — script.h.

#include <stdint.h>

namespace game
{
    struct Vec { float x = 0, y = 0, z = 0; };
    float Dist(const Vec& a, const Vec& b);
    float Dist2D(const Vec& a, const Vec& b);

    bool Init(void* libHandle);

    uintptr_t Player();                 // CPlayerPed* или 0
    int   PedRef(uintptr_t ped);        // номер педа для команд (Char)
    Vec   Pos(uintptr_t ent);
    Vec   PlayerPos();
    int   Model(uintptr_t ent);
    float Health(uintptr_t ped);
    int   Wanted();
    uint32_t TimeMs();                  // время игры (стоит на паузе)
    bool  Paused();
    bool  CutsceneRunning();
    float GroundZ(float x, float y, float z);
    bool  WorldToScreen(const Vec& w, float& sx, float& sy);   // false — за камерой
    int   Money();
    void  AddMoney(int d);
    int   GameDay();                    // игровые сутки (счётчик мода по часам игры)
    int   Hour();

    // Переменные сценария игры ($N — смещение в байтах от начала ScriptSpace)
    int32_t* Global(int offset);
    int   OnMissionVar();               // смещение $ONMISSION (по DECLARE_MISSION_FLAG), 0 — нет
    bool  OnMission();                  // $ONMISSION != 0

    // Метки на радаре: спрятать значки родных миссий (контакты, тип 5), кроме домов/недвижимости/спортзалов
    void  HideStoryContacts();
    void  RestoreStoryContacts();
}
