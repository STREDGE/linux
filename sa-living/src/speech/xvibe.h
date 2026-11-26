#pragma once
// Свой движок xVibePocketTTS (русская Pocket TTS) поверх ONNX Runtime C API.
// Повторяет эталонный запуск KevinAHM/pocket-tts-onnx (pocket_tts_onnx.py) один в один,
// с точным токенизатором SentencePiece BPE (в sherpa-onnx он unigram — речь хуже).
// Модели: экспорт lomotron/pocket-tts-onnx-export + tools/xvibe/patch_for_sherpa.py
// (порог «конца речи» сдвинут так, что проверка > -4 соответствует обученному -1).
// Не потокобезопасен: все вызовы — из одного рабочего потока.

#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

namespace xvibe
{
    struct Options
    {
        int   threads = 4;
        float temperature = 0.5f;   // как у xVibe
        int   lsdSteps = 1;         // как у xVibe
        int   decodeChunk = 12;     // кадров на порцию звука (12 кадров = 0,96 с)
        int   voiceCache = 4;       // сколько голосов держать готовыми (каждый ~50 МБ памяти)
    };

    // ortLib — путь к libonnxruntime.so (или пусто, если уже загружена в процесс)
    bool Load(const std::string& modelDir, const std::string& ortLib, const Options& opt, std::string& err);
    bool Loaded();
    int  SampleRate();              // 24000

    // Голос по образцу (моно, любая частота — пересчитаем в 24 кГц). Ключ — для кэша.
    bool PrepareVoice(const std::string& key, const float* audio, int n, int sampleRate, std::string& err);

    // Готовое состояние голоса (встроенные голоса xVibe, tools/xvibe/voice_state.py, формат XVST)
    bool LoadVoiceState(const std::string& key, const std::string& path, std::string& err);

    // Сохранить подготовленный голос в файл (gzip) — в следующий раз грузится мгновенно через LoadVoiceState
    bool SaveVoiceState(const std::string& key, const std::string& path, std::string& err);
    bool HasVoice(const std::string& key);
    void DropVoice(const std::string& key);

    // Текст: ударения «+гласная» или гласная+U+0301; лишние символы вычищаются.
    // onAudio получает куски звука (24 кГц) по мере готовности; вернуть false — остановить.
    bool Generate(const std::string& voiceKey, const std::string& text,
                  const std::function<bool(const float*, int)>& onAudio, std::string& err);

    // Для проверки: токены текста (как SentencePiece BPE)
    std::vector<int> Tokenize(const std::string& text);
    std::string PrepareText(const std::string& text);
}
