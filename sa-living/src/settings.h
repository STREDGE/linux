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
        int   dailyLimit = 0;               // запросов в день (0 — без лимита)
        int   tier = 1;                     // ИИ: 0 — быстрее всего (Haiku), 1 — максимум (Sonnet 5.5)
        bool  realArrest = true;            // задержание по шагам вместо «Busted»
        bool  witnesses = true;             // розыск только по свидетелям
        bool  shareKeyWithFriend = true;    // в коопе друг говорит через мой ключ
        bool  director = true;              // «режиссёр города»
        float talkX = 0.80f, talkY = 0.40f, talkS = 1.0f;     // кнопка «Говорить»
        float phoneX = 0.80f, phoneY = 0.22f, phoneS = 1.0f;  // кнопка «Телефон»
        int   engine = 0;                   // речь: 0 — своя (GigaAM + xVibe, без Google), 1 — Google (Android)
        int   forgetDays = 7;
        int   voiceLoud = 2;                // громкость голосов: 0 тише, 1 обычно, 2 громче, 3 очень громко               // неважных персонажей забывать через N игровых дней
        // Джарвис
        bool  jarvis = true;
        bool  jarvisButton = true;          // кнопка на экране (ещё есть пункт в меню CLEO)
        std::string jarvisVoice;            // ключ голоса (пусто — выбрать самому)
        float jarvisPitch = 1.0f, jarvisSpeed = 1.05f;
        std::string jarvisAddress = "сэр";  // как обращаться
        int   jarvisConfirm = 0;            // 0 — только опасное, 1 — всегда, 2 — никогда
        bool  jarvisFriend = false;         // кооп: друг может менять общий мир через Джарвиса
        float jarvisX = 0.80f, jarvisY = 0.58f, jarvisS = 1.0f;
    };

    Data& Get();
    void  Load();
    void  Save();
}
