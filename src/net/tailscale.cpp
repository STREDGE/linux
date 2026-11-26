#include "tailscale.h"

#include "../trace/ctrace.h"

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <stdarg.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// Вшитая сжатая библиотека (src/net/ts_blob.S, .incbin third_party/tsbridge/sacoopts.gz)
extern "C" const unsigned char sacoop_ts_blob[];
extern "C" const unsigned char sacoop_ts_blob_end[];

namespace ts
{
namespace
{
    std::mutex        g_mu;
    std::atomic<int>  g_phase{ 0 };      // 0 — не включали, 1 — распаковка, 2 — загружено, -1 — ошибка
    char              g_err[256] = "";

    int  (*f_start)(char*, char*, char*) = nullptr;
    int  (*f_status)(char*, int) = nullptr;
    int  (*f_find)(char*, char*, int) = nullptr;
    int  (*f_bridge)(int, int) = nullptr;
    int  (*f_setPeer)(char*, int) = nullptr;
    void (*f_close)() = nullptr;
    int  (*f_logout)() = nullptr;

    void (*g_sink)(bool, const char*) = nullptr;

    // Шаг запуска — в sacoop.txt (пишется сразу, с fflush): если игра закроется, видно, на каком шаге
    void Note(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Note(bool error, const char* fmt, ...)
    {
        char body[400], line[440];
        va_list a; va_start(a, fmt); vsnprintf(body, sizeof(body), fmt, a); va_end(a);
        snprintf(line, sizeof(line), "[TAILSCALE] %s", body);
        if(g_sink) g_sink(error, line);
        else if(error) logger->Error("%s", line);
        else logger->Info("%s", line);
    }

    void Fail(const char* fmt, const char* a = "", const char* b = "")
    {
        char msg[256];
        snprintf(msg, sizeof(msg), fmt, a, b);
        {
            std::lock_guard<std::mutex> l(g_mu);
            snprintf(g_err, sizeof(g_err), "%s", msg);
            g_phase = -1;
        }
        Note(true, "%s", msg);
    }

    // Папка подробного лога коопа (Documents/AMLModLogs/net.sacoop.mod/trace) — туда же журнал и вылеты Tailscale
    void LogDir(char* out, size_t len)
    {
        out[0] = 0;
        const char* p = ctrace::Path();
        if(!p || !p[0]) return;
        snprintf(out, len, "%s", p);
        char* slash = strrchr(out, '/');
        if(slash) *slash = 0; else out[0] = 0;
    }

    // Распаковать вшитую библиотеку в личную папку игры (там можно загружать .so)
    bool Extract(char* outPath, size_t len)
    {
        size_t blobSize = (size_t)(sacoop_ts_blob_end - sacoop_ts_blob);
        if(blobSize < 1024) { Fail("в моде нет встроенного Tailscale (собран без него)"); return false; }
        uint32_t crc = (uint32_t)crc32(0L, sacoop_ts_blob, (uInt)blobSize);
        const char* dir = aml->GetDataPath();
        if(!dir || !dir[0]) { Fail("нет личной папки игры"); return false; }
        snprintf(outPath, len, "%s/sacoop_ts_%08x.so", dir, crc);
        struct stat st;
        if(stat(outPath, &st) == 0 && st.st_size > 1000000)
        {
            Note(false, "шаг 1: уже распакован (%ld КБ)", (long)(st.st_size / 1024));
            return true;    // уже распакована эта версия
        }
        Note(false, "шаг 1: распаковываю (%u КБ сжато) в %s", (unsigned)(blobSize / 1024), outPath);

        char tmp[600]; snprintf(tmp, sizeof(tmp), "%s.tmp", outPath);
        FILE* f = fopen(tmp, "wb");
        if(!f) { Fail("не могу записать %s: %s", tmp, strerror(errno)); return false; }
        z_stream z{};
        if(inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) { fclose(f); Fail("zlib не запустился"); return false; }
        z.next_in = (Bytef*)sacoop_ts_blob; z.avail_in = (uInt)blobSize;
        static unsigned char out[256 * 1024];
        int r = Z_OK;
        size_t total = 0;
        while(r != Z_STREAM_END)
        {
            z.next_out = out; z.avail_out = sizeof(out);
            r = inflate(&z, Z_NO_FLUSH);
            if(r != Z_OK && r != Z_STREAM_END) break;
            size_t n = sizeof(out) - z.avail_out;
            if(n && fwrite(out, 1, n, f) != n) { r = Z_ERRNO; break; }
            total += n;
        }
        inflateEnd(&z);
        fclose(f);
        if(r != Z_STREAM_END) { unlink(tmp); Fail("распаковка не удалась (мало места?)"); return false; }
        chmod(tmp, 0700);
        if(rename(tmp, outPath) != 0) { unlink(tmp); Fail("не могу переименовать %s", tmp); return false; }
        Note(false, "шаг 1: распакован (%u КБ)", (unsigned)(total / 1024));
        // старые версии (по ~21 МБ) — убрать, иначе копятся с каждым обновлением мода
        const char* mine = strrchr(outPath, '/');
        mine = mine ? mine + 1 : outPath;
        if(DIR* d = opendir(dir))
        {
            while(dirent* e = readdir(d))
            {
                const char* n = e->d_name;
                size_t l = strlen(n);
                if(strncmp(n, "sacoop_ts_", 10) || l < 4 || strcmp(n + l - 3, ".so") || !strcmp(n, mine)) continue;
                char old[600]; snprintf(old, sizeof(old), "%s/%s", dir, n);
                if(unlink(old) == 0) Note(false, "убрал старую версию: %s", n);
            }
            closedir(d);
        }
        return true;
    }

    void Worker(std::string id)
    {
        char path[600];
        Note(false, "включаю (мой ID %s)", id.c_str());
        if(!Extract(path, sizeof(path))) return;
        Note(false, "шаг 2: загружаю библиотеку (если игра закроется сейчас — не запустился Go)");
        void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if(!h) { Fail("не загрузился: %s", dlerror()); return; }
        Note(false, "шаг 2: библиотека загружена");
        f_start   = (int(*)(char*, char*, char*))dlsym(h, "sacoop_ts_start");
        f_status  = (int(*)(char*, int))dlsym(h, "sacoop_ts_status");
        f_find    = (int(*)(char*, char*, int))dlsym(h, "sacoop_ts_find_peer");
        f_bridge  = (int(*)(int, int))dlsym(h, "sacoop_ts_bridge");
        f_setPeer = (int(*)(char*, int))dlsym(h, "sacoop_ts_set_peer");
        f_close   = (void(*)())dlsym(h, "sacoop_ts_close_bridge");
        f_logout  = (int(*)())dlsym(h, "sacoop_ts_logout");
        if(!f_start || !f_status || !f_find || !f_bridge || !f_setPeer || !f_close || !f_logout) { Fail("в библиотеке нет нужных функций"); return; }
        char dir[600]; snprintf(dir, sizeof(dir), "%s/sacoop_tailscale", aml->GetDataPath());
        char name[64]; NodeName(id.c_str(), name, sizeof(name));
        char logDir[600]; LogDir(logDir, sizeof(logDir));
        Note(false, "шаг 3: запускаю «%s» (журнал и вылеты: %s/tailscale_log.txt, tailscale_crash.txt)", name, logDir[0] ? logDir : dir);
        if(f_start(dir, name, logDir) != 0) { Fail("не запустился (папка %s)", dir); return; }
        Note(false, "шаг 3: запущен, подключаюсь к серверам Tailscale");
        g_phase = 2;
    }
}

void NodeName(const char* id, char* out, size_t len) { snprintf(out, len, "sacoop-%s", id ? id : ""); }
void SetLogSink(void (*fn)(bool, const char*)) { g_sink = fn; }

void Start(const char* myId)
{
    int expected = 0;
    if(!g_phase.compare_exchange_strong(expected, 1)) return;
    std::thread(Worker, std::string(myId ? myId : "")).detach();
}

Status GetStatus()
{
    Status s;
    int ph = g_phase;
    if(ph == 0) { s.state = State::Off; return s; }
    if(ph == 1) { s.state = State::Loading; return s; }
    if(ph < 0)  { s.state = State::Error; std::lock_guard<std::mutex> l(g_mu); snprintf(s.error, sizeof(s.error), "%s", g_err); return s; }
    char buf[1024] = "";
    f_status(buf, sizeof(buf));
    // состояние \t ссылка \t адрес \t имя \t ошибка
    char* parts[5] = {};
    int n = 0; char* p = buf;
    parts[n++] = p;
    while(*p && n < 5) { if(*p == '\t') { *p = 0; parts[n++] = p + 1; } ++p; }
    const char* st = parts[0] ? parts[0] : "";
    if(!strcmp(st, "Running"))               s.state = State::Running;
    else if(!strcmp(st, "NeedsLogin"))       s.state = State::NeedsLogin;
    else if(!strcmp(st, "NeedsMachineAuth")) s.state = State::NeedsApproval;
    else if(!strcmp(st, "Stopped"))          s.state = State::Stopped;
    else if(!strcmp(st, "Error"))            s.state = State::Error;
    else                                     s.state = State::Starting;
    if(n > 1) snprintf(s.loginUrl, sizeof(s.loginUrl), "%s", parts[1]);
    if(n > 2) snprintf(s.ip, sizeof(s.ip), "%s", parts[2]);
    if(n > 4) snprintf(s.error, sizeof(s.error), "%s", parts[4]);
    return s;
}

const char* StateText(State s)
{
    switch(s)
    {
        case State::Off:           return "выключен";
        case State::Loading:       return "запускается…";
        case State::Starting:      return "подключается к Tailscale…";
        case State::NeedsLogin:    return "нужно войти в аккаунт";
        case State::NeedsApproval: return "ждёт одобрения в аккаунте";
        case State::Running:       return "в сети";
        case State::Stopped:       return "остановлен";
        case State::Error:         return "ошибка";
    }
    return "";
}

int FindHost(const char* id, char* ipOut, size_t len)
{
    if(g_phase != 2) return -1;
    char name[64]; NodeName(id, name, sizeof(name));
    char ip[64] = "";
    int r = f_find(name, ip, sizeof(ip));
    if(ipOut && len) snprintf(ipOut, len, "%s", ip);
    return r;
}

int OpenBridge(int tsPort, int gamePort) { return g_phase == 2 ? f_bridge(tsPort, gamePort) : 0; }
void SetPeer(const char* ip, int port) { if(g_phase == 2) { char b[64]; snprintf(b, sizeof(b), "%s", ip); f_setPeer(b, port); } }
void CloseBridge() { if(g_phase == 2) f_close(); }
bool Logout() { return g_phase == 2 && f_logout() == 0; }
}
