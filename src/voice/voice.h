#pragma once
// Голосовой чат: микрофон и динамик через AAudio (Android 8+), сжатие Opus.
// Внутри — 48 кГц моно, кадры по 20 мс (960 отсчётов). Если телефон работает
// на другой частоте, звук пересчитывается. Всё, кроме колбэков AAudio,
// вызывается из игрового потока.

#include <stddef.h>
#include <stdint.h>

class FLogger;

namespace voice
{
    constexpr int kRate         = 48000;
    constexpr int kFrameSamples = 960;      // 20 мс
    constexpr int kMaxPacket    = 400;      // байт на кадр Opus (с запасом)

    bool Init(FLogger* log);                // создать кодер/декодер, найти AAudio
    bool Available();                       // AAudio есть и кодек готов

    void SetTransmit(bool on);              // кнопка микрофона зажата / отпущена
    bool Transmitting();

    // Следующий сжатый кадр для отправки (0 — нет готового кадра).
    // spurt меняется при каждом новом нажатии кнопки.
    int  PopEncodedFrame(uint8_t* out, int maxLen, uint16_t* seq, uint8_t* spurt);
    // Кадр от друга: в буфер, откуда он уйдёт на динамик
    void PushReceivedFrame(uint16_t seq, uint8_t spurt, const uint8_t* data, int len);

    // Режим «тест в одиночку»: свои кадры возвращаются в динамик через delayMs
    void SetLoopback(bool on, int delayMs);
    void Pump();                            // раз в кадр игры

    void Stop();                            // закрыть микрофон и динамик, очистить буферы

    float InputLevel();                     // 0..1 — громкость микрофона (для индикатора)
    float OutputLevel();                    // 0..1 — громкость друга
    bool  Playing();                        // сейчас звучит голос друга (или эхо)
}
