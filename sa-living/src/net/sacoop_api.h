#pragma once
// Интерфейс SA Coop для других модов (например, SA Living): канал данных между хостом и другом.
// Регистрируется в AML под именем "SACoopNet". Только C-функции — без зависимостей от компилятора.
// Копия этого файла лежит в src/net/sacoop_api.h (SA Coop) — менять обе одинаково.

#include <stdint.h>

struct SACoopNetApi
{
    uint32_t version;                                   // 1; 2 — с полями «один микрофон» (ниже)
    int  (*GetRole)();                                  // 0 выкл, 1 тест в одиночку, 2 хост, 3 друг
    bool (*Connected)();                                // друг принят, связь есть
    bool (*Send)(uint8_t channel, const void* data, int len);          // до ~1100 байт, без гарантии доставки
    void (*SetReceiver)(uint8_t channel, void (*fn)(const void* data, int len)); // вызывается в игровом потоке
    const char* (*PeerNick)();
    const char* (*MyNick)();
    // ---- версия 2 (SA Coop 1.1.0): один микрофон и одна кнопка «Говорить» на оба мода ----
    // Копия звука микрофона, пока SA Coop его пишет (моно, 16 бит; вызывается в игровом потоке). nullptr — снять.
    void (*SetMicTap)(void (*fn)(const int16_t* pcm, int n, int rate));
    void (*SetTalk)(bool on);                           // «говорить другу» нажато/отпущено чужой кнопкой
    void (*SetTalkButtonOwner)(bool owned);             // другой мод рисует свою кнопку — золотую не показывать
    bool (*VoiceAvailable)();                           // микрофон (AAudio) и кодек работают
};

#define SACOOP_NET_INTERFACE "SACoopNet"
#define SACOOP_MAX_MOD_DATA  1100
