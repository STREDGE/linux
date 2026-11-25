#pragma once
// Настройки SA Living: Android/data/<игра>/configs/net.saliving.mod.ini
// Ключ хранится только здесь, в лог не пишется.

#include <string>

namespace settings
{
    struct Data
    {
        bool  enabled = true;
        std::string key;                    // ключ Anthropic игрока
        std::string model = "claude-haiku-5-5";
        std::string effort = "low";         // low | medium
        bool  voices = true;                // голоса педов (TextToSpeech)
        bool  subtitles = true;
        std::string lang = "ru-RU";
        int   initiative = 1;               // 0 выкл, 1 редко, 2 часто — педы сами заговаривают
        int   dailyLimit = 400;             // запросов в день
        bool  realArrest = true;            // задержание по шагам вместо «Busted»
        bool  witnesses = true;             // розыск только по свидетелям
        bool  shareKeyWithFriend = true;    // в коопе друг говорит через мой ключ
        bool  director = true;              // «режиссёр города»
        float talkX = 0.80f, talkY = 0.40f, talkS = 1.0f;     // кнопка «Говорить»
        float phoneX = 0.80f, phoneY = 0.22f, phoneS = 1.0f;  // кнопка «Телефон»
    };

    Data& Get();
    void  Load();
    void  Save();
}
