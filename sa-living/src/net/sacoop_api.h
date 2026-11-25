#pragma once
// Интерфейс SA Coop для других модов (например, SA Living): канал данных между хостом и другом.
// Регистрируется в AML под именем "SACoopNet". Только C-функции — без зависимостей от компилятора.
// Копия этого файла лежит в src/net/sacoop_api.h (SA Coop) — менять обе одинаково.

#include <stdint.h>

struct SACoopNetApi
{
    uint32_t version;                                   // 1
    int  (*GetRole)();                                  // 0 выкл, 1 тест в одиночку, 2 хост, 3 друг
    bool (*Connected)();                                // друг принят, связь есть
    bool (*Send)(uint8_t channel, const void* data, int len);          // до ~1100 байт, без гарантии доставки
    void (*SetReceiver)(uint8_t channel, void (*fn)(const void* data, int len)); // вызывается в игровом потоке
    const char* (*PeerNick)();
    const char* (*MyNick)();
};

#define SACOOP_NET_INTERFACE "SACoopNet"
#define SACOOP_MAX_MOD_DATA  1100
