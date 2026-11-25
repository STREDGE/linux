#pragma once
// Встроенная речь без Google: распознавание GigaAM v3 (русский, с пунктуацией) и голоса Piper
// (4 русских голоса) через движок sherpa-onnx (C API, библиотеки из официального пакета 1.12.21).
// Пакет (~550 МБ) скачивается один раз по кнопке в меню «AI», дальше всё работает без интернета.

#include <string>

namespace speech
{
    void Init();                         // пути, проверка: пакет уже скачан?
    void Frame();                        // раз в кадр (игровой поток)

    // Пакет
    bool Installed();                    // все файлы на месте
    bool Downloading();
    float Progress();                    // 0..1
    std::string StatusText();
    void StartDownload();
    void CancelDownload();

    // Распознавание (игрок держит кнопку). Итог придёт в talk как обычное «распознано».
    bool AsrReady();
    bool Listen();
    void StopListen();

    // Озвучка: voice 0..3 (0-2 мужские, 3 женский), pitch/speed — для разнообразия голосов
    bool TtsReady();
    void Speak(const std::string& text, int voice, float pitch, float speed, const std::string& id);
    void StopSpeaking();
    int  VoiceFor(int charId, bool female);
}
