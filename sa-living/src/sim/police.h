#pragma once
// Полиция: розыск только по свидетелям, задержание по шагам, участок, суд, тюрьма.

#include <stdint.h>
#include <string>

namespace police
{
    bool Install(void* libHandle);   // перехваты CWanted::RegisterCrime*, CPlayerInfo::ArrestPlayer, CDarkel::RegisterKillByPlayer
    void Frame();

    // Задержание
    bool Arresting();                 // идёт наш сценарий задержания
    void Surrender();                 // игрок сдался (кнопка)
    void StartArrest(uintptr_t cop);  // коп решил задержать
    void Release(const char* why);    // отпустить (взятка, договорились)
    const char* ArrestStage();        // для HUD

    // Суд и тюрьма (сцена поверх игры)
    enum SceneKind { SCENE_NONE, SCENE_STATION, SCENE_COURT, SCENE_JAIL, SCENE_RELEASE };
    struct Scene
    {
        SceneKind kind = SCENE_NONE;
        std::string title, text;      // крупно / текст (реплики судьи и т.п.)
        std::string verdict;          // итог
        bool waitingAi = false;
        bool canSpeak = false;        // можно сказать слово (кнопка «Говорить»)
        bool canLawyer = false;       // можно позвонить адвокату/своему копу
        int  fine = 0, days = 0;
    };
    const Scene& CurrentScene();
    void SceneContinue();             // кнопка «Дальше»
    void SceneSpeak(const std::string& text);  // игрок сказал речь в суде
    void SceneCallHelp();             // звонок адвокату / своему человеку

    void Crime(int crimeType, int severity, const char* text, bool cops);   // зарегистрировать событие вручную
}
