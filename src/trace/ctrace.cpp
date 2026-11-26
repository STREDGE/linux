#include "trace/ctrace.h"

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
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
    FILE* g_file = nullptr;
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
            if(!chunk.empty() && g_file) { fwrite(chunk.data(), 1, chunk.size(), g_file); fflush(g_file); }
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

    bool MkDirs(const std::string& p)
    {
        std::string cur;
        for(size_t i = 0; i < p.size(); ++i)
        {
            cur += p[i];
            if(p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0777);
        }
        return mkdir(p.c_str(), 0777) == 0 || errno == EEXIST;
    }

    // оставить последние 10 файлов сессий (без сжатия)
    void Prune(const std::string& dir)
    {
        DIR* d = opendir(dir.c_str()); if(!d) return;
        std::vector<std::string> names;
        while(dirent* e = readdir(d)) { std::string n = e->d_name; if(n.rfind("trace_", 0) == 0) names.push_back(n); }
        closedir(d);
        std::sort(names.begin(), names.end());
        while(names.size() > 10) { remove((dir + "/" + names.front()).c_str()); names.erase(names.begin()); }
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
    s += ts; s += " ["; s += tag; s += "] "; s += buf; s += '\n';
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
    std::string dir = std::string(root && root[0] ? root : "/sdcard") + "/Documents/AMLModLogs/" + modinfo->GUID() + "/trace";
    MkDirs(dir);
    time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
    char name[64]; strftime(name, sizeof(name), "trace_%Y-%m-%d_%H-%M-%S.txt", &tm);
    g_path = dir + "/" + name;
    g_file = fopen(g_path.c_str(), "wb");
    if(!g_file)
    {   // папка Documents недоступна — личная папка игры
        const char* data = aml->GetDataPath();
        dir = std::string(data ? data : ".") + "/SACoop/trace";
        MkDirs(dir);
        g_path = dir + "/" + name;
        g_file = fopen(g_path.c_str(), "wb");
    }
    if(!g_file) { logger->Error("Подробный лог коопа: не удалось создать файл (%s)", strerror(errno)); return; }
    Prune(dir);
    g_started = true;
    std::thread(Writer).detach();

    Line("СЕССИЯ", "SA Coop %s, начало %04d-%02d-%02d %02d:%02d:%02d", modinfo->VersionString(),
         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    Line("СЕССИЯ", "телефон: %s %s, Android %s (API %s), процессор %s", Prop("ro.product.manufacturer").c_str(), Prop("ro.product.model").c_str(),
         Prop("ro.build.version.release").c_str(), Prop("ro.build.version.sdk").c_str(), Prop("ro.product.cpu.abi").c_str());
    Line("СЕССИЯ", "файл: %s", g_path.c_str());
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

void Crash(int sig, unsigned long pc, unsigned long lr, const char* lib, unsigned long off, const char* where)
{
    if(!g_started || !g_file) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "XX:XX:XX.XXX [ВЫЛЕТ] сигнал %d, библиотека %s, адрес +0x%lX, pc=0x%lX, lr=0x%lX, мод: %s\n",
             sig, lib ? lib : "?", off, pc, lr, where ? where : "?");
    // в обработчике вылета: дописать хвост очереди без ожидания потока записи
    if(g_mx.try_lock()) { std::string rest; rest.swap(g_pending); g_mx.unlock(); fwrite(rest.data(), 1, rest.size(), g_file); }
    fwrite(buf, 1, strlen(buf), g_file);
    fflush(g_file);
}
}
