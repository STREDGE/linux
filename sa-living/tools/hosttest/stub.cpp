// Заглушки моста Android и лога — чтобы собирать модули мода на компьютере (проверка речи и Джарвиса).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include "platform/bridge.h"
#include "util/log.h"

void LogInfo(const char* fmt, ...)  { va_list a; va_start(a, fmt); printf("[i] "); vprintf(fmt, a); printf("\n"); va_end(a); }
void LogError(const char* fmt, ...) { va_list a; va_start(a, fmt); printf("[E] "); vprintf(fmt, a); printf("\n"); va_end(a); }

namespace bridge
{
    std::vector<float> g_lastPlayed; int g_lastRate = 0;
    bool Init() { return true; }
    bool Ready() { return true; }
    void DownloadPack(const std::string&) {}
    void CancelDownload() {}
    bool PollDl(DlEvent&) { return false; }
    bool MicStart() { return false; }
    void MicStop() {}
    bool PollMic(std::vector<int16_t>&) { return false; }
    void PlayPcm(const float* s, int n, int sr, float, float, const std::string& id) { g_lastPlayed.assign(s, s + n); g_lastRate = sr; PushTtsEvent(id, 1); }
    void PlayStop() {}
    static std::vector<TtsEvent> g_ev;
    bool PollTts(TtsEvent& out) { if(g_ev.empty()) return false; out = g_ev.front(); g_ev.erase(g_ev.begin()); return true; }
    void PushTtsEvent(const std::string& id, int state) { g_ev.push_back({ id, state }); }
    bool PickFile(const std::string&) { return false; }
    int  PickState(std::string*) { return 0; }
    void PickReset() {}
    void OpenUrl(const std::string&) {}
}
