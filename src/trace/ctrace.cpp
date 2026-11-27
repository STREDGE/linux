#include "trace/ctrace.h"
#include <onelog/onelog.h>

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ctrace
{
namespace
{
    std::mutex g_mx;
    std::condition_variable g_cv;
    std::string g_pending;               // ещё не записано в файл
    int g_fd = -1;
    std::string g_path;
    std::atomic<bool> g_started{ false };

    void Writer()
    {
        std::string chunk;
        for(;;)
        {
            {
                std::unique_lock<std::mutex> l(g_mx);
                g_cv.wait_for(l, std::chrono::milliseconds(250));
                chunk.swap(g_pending);
            }
            if(!chunk.empty()) onelog::Write(g_fd, chunk.data(), chunk.size());
            chunk.clear();
        }
    }

    void Stamp(char* buf, size_t n)
    {
        struct timeval tv; gettimeofday(&tv, nullptr);
        struct tm t; localtime_r(&tv.tv_sec, &t);
        snprintf(buf, n, "%02d:%02d:%02d.%03d", t.tm_hour, t.tm_min, t.tm_sec, (int)(tv.tv_usec / 1000));
    }

    void Push(const std::string& s)
    {
        if(!g_started) return;
        std::lock_guard<std::mutex> l(g_mx);
        g_pending += s;
        if(g_pending.size() > (8u << 20)) g_pending.erase(0, g_pending.size() - (4u << 20));   // запись отстала — не растём бесконечно
    }



    std::string Prop(const char* k) { char v[PROP_VALUE_MAX] = ""; __system_property_get(k, v); return v; }
}

bool Started() { return g_started; }
const char* Path() { return g_path.c_str(); }

void LineV(const char* tag, const char* fmt, va_list ap)
{
    if(!g_started) return;
    char ts[24]; Stamp(ts, sizeof(ts));
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    std::string s; s.reserve(strlen(buf) + 40);
    s += "К "; s += ts; s += " ["; s += tag; s += "] "; s += buf; s += '\n';
    Push(s);
}

void Line(const char* tag, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); LineV(tag, fmt, ap); va_end(ap);
}

void FromLog(bool error, const char* text)
{
    if(!g_started || !text) return;
    // «[ТЕГ] текст» → тег из скобок. «[МАШИНА 0A1F] …» → тег МАШИНА, номер — в текст;
    // «[ВТОРОЙ CJ] …» → тег ВТОРОЙ-CJ (пробелы в тегах заменяются дефисом)
    char tag[48] = "ЛОГ", id[16] = "";
    const char* body = text;
    if(text[0] == '[')
    {
        const char* e = strchr(text, ']');
        if(e && e - text > 1 && e - text < 44)
        {
            size_t n = (size_t)(e - text - 1);
            memcpy(tag, text + 1, n); tag[n] = 0;
            body = e + 1; while(*body == ' ') ++body;
            char* sp = strchr(tag, ' ');
            if(sp)
            {
                const char* rest = sp + 1; size_t rl = strlen(rest); bool hex = rl > 0 && rl <= 6;
                for(size_t i = 0; i < rl && hex; ++i) hex = (rest[i] >= '0' && rest[i] <= '9') || (rest[i] >= 'A' && rest[i] <= 'F');
                if(hex) { snprintf(id, sizeof(id), "%s: ", rest); *sp = 0; }
                else for(char* c = tag; *c; ++c) if(*c == ' ') *c = '-';
            }
        }
    }
    if(error) Line(tag, "%sОШИБКА: %s", id, body);
    else      Line(tag, "%s%s", id, body);
}

void Init()
{
    if(g_started) return;
    const char* root = aml->GetInternalPath();
    onelog::RemoveOld(root, modinfo->GUID(), "sacoop");      // старые sacoop.txt, sacoop_prev.txt, trace/, tailscale_*.txt
    {
        std::string d = std::string(root && root[0] ? root : "/storage/emulated/0") + "/Documents/AMLModLogs/" + modinfo->GUID();
        for(const char* n : { "tailscale_log.txt", "tailscale_log_prev.txt", "tailscale_crash.txt" }) remove((d + "/trace/" + n).c_str());
        rmdir((d + "/trace").c_str()); rmdir(d.c_str());
    }
    g_fd = onelog::Open(root, &g_path);
    if(g_fd < 0)
    {   // папка Documents недоступна — личная папка игры
        const char* data = aml->GetDataPath();
        g_path = std::string(data ? data : ".") + "/SA_LOG.txt";
        g_fd = open(g_path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    }
    if(g_fd < 0) { logger->Error("Лог: не удалось открыть файл (%s)", strerror(errno)); return; }
    g_started = true;
    std::thread(Writer).detach();
    time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
    {
        char sep[160];
        int n = snprintf(sep, sizeof(sep), "\n=============== ЗАПУСК ИГРЫ %04d-%02d-%02d %02d:%02d:%02d ===============\n",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        onelog::Write(g_fd, sep, (size_t)n);
    }

    Line("СЕССИЯ", "SA Coop %s, начало %04d-%02d-%02d %02d:%02d:%02d", modinfo->VersionString(),
         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    Line("СЕССИЯ", "телефон: %s %s, Android %s (API %s), процессор %s", Prop("ro.product.manufacturer").c_str(), Prop("ro.product.model").c_str(),
         Prop("ro.build.version.release").c_str(), Prop("ro.build.version.sdk").c_str(), Prop("ro.product.cpu.abi").c_str());
    Line("СЕССИЯ", "файл: %s (К — кооп, И — SA Living, Т — Tailscale)", g_path.c_str());
    Dl_info di;
    if(dladdr((void*)&Init, &di) && di.dli_fname)
    {
        std::string self = di.dli_fname, mods = self.substr(0, self.find_last_of('/'));
        Line("СЕССИЯ", "мод: %s", self.c_str());
        if(DIR* d = opendir(mods.c_str()))
        {
            std::string list;
            while(dirent* e = readdir(d))
            {
                std::string n = e->d_name; if(n.size() < 4 || n.compare(n.size() - 3, 3, ".so")) continue;
                struct stat st; long sz = stat((mods + "/" + n).c_str(), &st) == 0 ? (long)st.st_size : -1;
                char b[160]; snprintf(b, sizeof(b), " %s(%ld КБ)", n.c_str(), sz / 1024); list += b;
            }
            closedir(d);
            Line("СЕССИЯ", "библиотеки в папке мода:%s", list.c_str());
        }
    }
}

void Crash(int sig, unsigned long pc, unsigned long lr, const char* lib, unsigned long off, const char* where, unsigned long lrGame)
{
    if(!g_started || g_fd < 0) return;
    char buf[600];
    int n = snprintf(buf, sizeof(buf), "К XX:XX:XX.XXX [ВЫЛЕТ] сигнал %d, библиотека %s, адрес +0x%lX, pc=0x%lX, lr=0x%lX (lr-libGTASA=+0x%lX), мод: %s\n",
                     sig, lib ? lib : "?", off, pc, lr, lrGame, where ? where : "?");
    // в обработчике вылета: дописать хвост очереди без ожидания потока записи
    if(g_mx.try_lock()) { std::string rest; rest.swap(g_pending); g_mx.unlock(); onelog::Write(g_fd, rest.data(), rest.size()); }
    onelog::Write(g_fd, buf, n > 0 ? (size_t)n : 0);
}
}
