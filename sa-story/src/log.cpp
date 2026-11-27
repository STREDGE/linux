#include "log.h"
#include <onelog/onelog.h>

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace slog
{
namespace
{
    std::mutex g_mx;
    std::condition_variable g_cv;
    std::string g_pending;
    int g_fd = -1;
    std::atomic<bool> g_started{ false };

    // запись в файл — в своём потоке (игровой поток только складывает строки)
    void Writer()
    {
        std::string chunk;
        for(;;)
        {
            {
                std::unique_lock<std::mutex> l(g_mx);
                g_cv.wait_for(l, std::chrono::milliseconds(300));
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
}

void Init()
{
    if(g_started) return;
    const char* root = aml->GetInternalPath();
    std::string path;
    g_fd = onelog::Open(root, &path);
    if(g_fd < 0)
    {
        const char* data = aml->GetDataPath();
        path = std::string(data ? data : ".") + "/SA_LOG.txt";
        g_fd = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    }
    if(g_fd < 0) { logger->Error("Лог: не удалось открыть файл (%s)", strerror(errno)); return; }
    g_started = true;
    std::thread(Writer).detach();
    Line("СЕССИЯ", "SA Story %s, лог: %s (строки «С»)", modinfo->VersionString(), path.c_str());
}

void LineV(const char* tag, const char* fmt, va_list ap)
{
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    logger->Info("[%s] %s", tag, buf);
    if(!g_started) return;
    char ts[24]; Stamp(ts, sizeof(ts));
    std::string s; s.reserve(strlen(buf) + 48);
    s += "С "; s += ts; s += " ["; s += tag; s += "] "; s += buf; s += '\n';
    std::lock_guard<std::mutex> l(g_mx);
    g_pending += s;
    if(g_pending.size() > (4u << 20)) g_pending.erase(0, g_pending.size() - (2u << 20));
}

void Line(const char* tag, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); LineV(tag, fmt, ap); va_end(ap);
}

void Crash(int sig, unsigned long pc, unsigned long lr, const char* lib, unsigned long off, const char* where)
{
    if(!g_started || g_fd < 0) return;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "С XX:XX:XX.XXX [ВЫЛЕТ] сигнал %d, библиотека %s, адрес +0x%lX, pc=0x%lX, lr=0x%lX, мод: %s\n",
                     sig, lib ? lib : "?", off, pc, lr, where ? where : "?");
    if(g_mx.try_lock()) { std::string rest; rest.swap(g_pending); g_mx.unlock(); onelog::Write(g_fd, rest.data(), rest.size()); }
    onelog::Write(g_fd, buf, n > 0 ? (size_t)n : 0);
}
}
