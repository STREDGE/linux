#pragma once
// Своя речь без Google: распознавание GigaAM v3 (sherpa-onnx) и голоса xVibe (свой движок xvibe.*).
// Всё тяжёлое — в фоновых потоках; из игрового потока — только очереди. Вызовы — из игрового потока.
//
// Файлы:
//   <base>/asr/model.int8.onnx, tokens.txt          — распознавание (мод скачивает сам)
//   <base>/tts/*.onnx, bundle.json, vocab.json, …   — голосовой пакет (импорт zip из «Загрузок» / выбор файла)
//   <base>/tts/voices/voices.json + *.xvst          — встроенные голоса (включены сразу)
//   <base>/uservoices/voices.json + *.xvst          — голоса игрока («Добавить голос»)
//   <lib>/libonnxruntime.so, libsherpa-onnx-c-api.so — движки (личная папка игры: оттуда можно грузить код)

#include <string>
#include <vector>

namespace speech
{
    struct Voice
    {
        std::string key, title, file;   // file — полный путь к .xvst
        bool  female = false, builtin = true;
        float pitch = 1.0f, speed = 1.0f;   // «родные» поправки голоса
    };

    struct Status
    {
        bool asrFiles = false, asrReady = false;   // файлы на месте / движок загружен
        bool ttsFiles = false, ttsReady = false;
        bool loading = false;                      // движки грузятся в фоне
        bool busy = false;                         // идёт скачивание / импорт
        float progress = 0;                        // 0..1 (если известно), иначе -1
        std::string busyText;                      // «model.int8.onnx: 120 из 214 МБ»
        std::string error;                         // последняя ошибка (по-русски)
        float lastRtf = 0;                         // скорость озвучки: время синтеза / длина звука (меньше 1 — быстрее речи)
        float lastAsrSec = 0;                      // сколько заняло последнее распознавание
        int   voices = 0;
    };

    void Init(const std::string& baseDir, const std::string& libDir);
    void Frame();                                  // раз в кадр (и из меню): очереди, события
    Status Get();
    void SetSearchDirs(const std::vector<std::string>& dirs);   // где искать zip голосового пакета

    // ---- установка ----
    void DownloadAsr();                            // скачать распознавание (GigaAM, ~215 МБ)
    bool ImportVoicePackAuto();                    // найти zip пакета в «Загрузках» и распаковать; false — не нашёл
    bool PickVoicePack();                          // системное окно выбора файла → импорт
    void CancelBusy();
    // Скачать один файл тем же загрузчиком (справочник команд Джарвиса). false — загрузчик занят.
    bool DownloadExtra(const std::string& url, const std::string& path, const std::string& label);
    void RemoveAll();                              // удалить скачанное (освободить место)

    // ---- распознавание (игрок держит кнопку) ----
    bool AsrReady();
    bool Listen();                                 // false — микрофон недоступен / движок не готов
    void StopListen();                             // закончить и распознать
    void CancelListen();
    bool Listening();
    bool Recognizing();                            // запись ушла на распознавание, ждём текст
    struct Heard { bool ok = false; std::string text; std::string error; };
    bool PollHeard(Heard& out);

    // ---- озвучка ----
    bool TtsReady();
    // Фраза в очередь. voiceKey — из Voices(); id придёт в PollSpoken, когда фраза отзвучала (или сорвалась).
    void Speak(const std::string& text, const std::string& voiceKey, float pitch, float speed, const std::string& id);
    void StopSpeaking();                           // замолчать и очистить очередь
    bool Speaking();                               // что-то синтезируется или звучит
    bool PollSpoken(std::string& id);

    // ---- голоса ----
    std::vector<Voice> Voices();
    bool FindVoice(const std::string& key, Voice& out);
    // Голос для нового персонажа: по полу, стабильно по seed; avoid — не совпадать с этими (сцена)
    std::string PickVoice(bool female, unsigned seed, const std::vector<std::string>& avoid = {});

    // «Добавить голос»: запись с микрофона 15–20 с (или wav-файл) → голос в uservoices
    bool RecordVoiceStart();
    void RecordVoiceFinish(const std::string& title, bool female);   // готовится в фоне; итог — в PopNotice
    bool RecordingVoice();
    float RecordSeconds();
    bool AddVoiceFromFile(const std::string& title, bool female);    // выбор wav-файла
    void DeleteVoice(const std::string& key);                        // только свои

    bool PopNotice(std::string& out);              // сообщения для игрока («Голос добавлен», ошибки)

    // Текст для синтеза: числа — словами, лишнее убрать (ударения «+» оставить)
    std::string Normalize(const std::string& text);

    // Для проверки на компьютере: синхронные вызовы без моста
    bool TestRecognize(const std::vector<float>& pcm16k, std::string& text, std::string& err);
    bool TestSynthesize(const std::string& voiceKey, const std::string& text, std::vector<float>& pcm24k, std::string& err);
    bool TestLoad(std::string& err);               // загрузить оба движка синхронно
}
