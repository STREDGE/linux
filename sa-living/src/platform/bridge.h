#pragma once
// Мост к Java (вшитый classes.dex): HTTPS с потоковым ответом, распознавание речи, голос.
// Колбэки приходят из потоков Java и складываются в очереди; забирать — в игровом потоке (Poll*).

#include <stdint.h>
#include <string>
#include <vector>

namespace bridge
{
    bool Init();                      // загрузить dex, зарегистрировать native-методы
    bool Ready();

    // ---- HTTPS ----
    // Возвращает номер запроса (>0) или 0. Ответ построчно — PollHttp.
    int  HttpPost(const char* url, const std::vector<std::string>& headers /* имя, значение, ... */,
                  const std::string& body, int timeoutMs);
    struct HttpEvent { int id; bool done; int status; std::string text; }; // строка или (done) ошибка
    bool PollHttp(HttpEvent& out);

    // ---- распознавание речи ----
    bool SttAvailable();
    void SttStart(const char* lang);  // "ru-RU"
    void SttStop();                   // закончить и выдать итог
    void SttCancel();
    enum SttKind { STT_PARTIAL = 0, STT_FINAL = 1, STT_ERROR = 2, STT_READY = 3, STT_END = 4 };
    struct SttEvent { int kind; std::string text; };
    bool PollStt(SttEvent& out);

    // ---- голос ----
    void TtsInit(const char* lang);
    bool TtsReady();
    void TtsSpeak(const std::string& text, float pitch, float rate, const std::string& id, bool queue);
    void TtsStop();
    struct TtsEvent { std::string id; int state; };          // 0 начал, 1 закончил, 2 ошибка
    bool PollTts(TtsEvent& out);
    void PushTtsEvent(const std::string& id, int state);

    // ---- голосовой пакет: скачивание (план — строки, см. Bridge.downloadPack) ----
    void DownloadPack(const std::string& plan);
    void CancelDownload();
    struct DlEvent { int state; long long done, total; std::string msg; };   // 0 идёт, 1 готово, 2 ошибка
    bool PollDl(DlEvent& out);

    // ---- микрофон (16 кГц моно, 16 бит); запись приходит целиком после MicStop ----
    bool MicStart();
    void MicStop();
    bool PollMic(std::vector<int16_t>& out);

    // ---- воспроизведение готового звука (своя озвучка); можно из любого потока ----
    void PlayPcm(const float* samples, int n, int sampleRate, float pitch, float speed, const std::string& id);
    void PlayStop();

    // ---- выбор файла на телефоне (системное окно «Файлы»): копия кладётся в destPath ----
    bool PickFile(const std::string& destPath);
    int  PickState(std::string* result);   // 0 ничего, 1 выбирают, 2 готово (путь), -1 отмена, -2 ошибка (текст)
    void PickReset();
    void OpenUrl(const std::string& url);
}
