#include "nettest.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <thread>

namespace net
{
namespace
{
    constexpr uint32_t kMagic    = 0x50434153; // "SACP"
    constexpr uint8_t  kVersion  = 1;
    constexpr uint8_t  kPing     = 1;
    constexpr uint8_t  kPong     = 2;

    #pragma pack(push, 1)
    struct Packet
    {
        uint32_t magic;
        uint8_t  version;
        uint8_t  type;
        uint16_t reserved;
        uint32_t seq;
        uint64_t sentUs;   // время отправки у клиента, хост возвращает его как есть
    };
    #pragma pack(pop)

    FLogger*   g_log = nullptr;
    std::mutex g_logMutex;

    void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool error, const char* fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(g_logMutex);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        if(g_log) { if(error) g_log->Error("%s", buf); else g_log->Info("%s", buf); }
    }

    uint64_t NowUs()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
    }

    bool IsValid(const Packet& p, ssize_t len)
    {
        return len == (ssize_t)sizeof(Packet) && p.magic == kMagic && p.version == kVersion;
    }

    const char* AddrToStr(const sockaddr_in& a, char* buf, size_t len)
    {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        snprintf(buf, len, "%s:%u", ip, (unsigned)ntohs(a.sin_port));
        return buf;
    }

    int OpenSocket(uint16_t bindPort)
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if(s < 0)
        {
            Log(true, "socket() не создан: %s (нет разрешения INTERNET у игры?)", strerror(errno));
            return -1;
        }
        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port        = htons(bindPort);
        if(bind(s, (sockaddr*)&local, sizeof(local)) < 0)
        {
            Log(true, "bind(порт %u) не удался: %s", (unsigned)bindPort, strerror(errno));
            close(s);
            return -1;
        }
        return s;
    }

    bool WaitReadable(int s, int timeoutMs)
    {
        pollfd pfd{ s, POLLIN, 0 };
        return poll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLIN);
    }

    void RunHost(TestConfig cfg)
    {
        int s = OpenSocket(cfg.port);
        if(s < 0) return;
        Log(false, "[ХОСТ] Жду друга на UDP-порту %u. Дай ему свой адрес Tailscale (100.x.x.x).", (unsigned)cfg.port);

        sockaddr_in peer{};
        bool        hasPeer   = false;
        uint64_t    lastSeen  = 0;
        uint32_t    received  = 0;
        char        addrBuf[64];

        for(;;)
        {
            if(WaitReadable(s, 500))
            {
                Packet      p;
                sockaddr_in from{};
                socklen_t   fromLen = sizeof(from);
                ssize_t     n = recvfrom(s, &p, sizeof(p), 0, (sockaddr*)&from, &fromLen);
                if(n > 0 && IsValid(p, n) && p.type == kPing)
                {
                    bool samePeer = hasPeer && from.sin_addr.s_addr == peer.sin_addr.s_addr && from.sin_port == peer.sin_port;
                    if(!samePeer || NowUs() - lastSeen > 5000000ull)
                        Log(false, "[ХОСТ] Друг подключился: %s", AddrToStr(from, addrBuf, sizeof(addrBuf)));
                    peer = from; hasPeer = true; lastSeen = NowUs();

                    p.type = kPong;
                    sendto(s, &p, sizeof(p), 0, (sockaddr*)&from, sizeof(from));
                    if(++received % 30 == 0)
                        Log(false, "[ХОСТ] Связь есть, получено пингов: %u", received);
                }
            }
            if(hasPeer && NowUs() - lastSeen > 5000000ull)
            {
                Log(true, "[ХОСТ] Друг пропал (нет пакетов 5 сек)");
                hasPeer = false;
            }
        }
    }

    void RunClient(TestConfig cfg)
    {
        int s = OpenSocket(0);
        if(s < 0) return;

        sockaddr_in host{};
        host.sin_family = AF_INET;
        host.sin_port   = htons(cfg.port);
        if(inet_pton(AF_INET, cfg.hostAddress, &host.sin_addr) != 1)
        {
            Log(true, "[КЛИЕНТ] Неверный адрес хоста '%s' (нужен вид 100.x.x.x)", cfg.hostAddress);
            close(s);
            return;
        }
        Log(false, "[КЛИЕНТ] Подключаюсь к %s:%u ...", cfg.hostAddress, (unsigned)cfg.port);

        uint32_t seq = 0, sent = 0, got = 0;
        uint64_t rttSum = 0, rttMin = UINT64_MAX, rttMax = 0;
        uint64_t lastPong = 0, nextPing = 0;
        bool     connected = false;
        bool     warnedNoHost = false;

        for(;;)
        {
            uint64_t now = NowUs();
            if(now >= nextPing)
            {
                Packet p{ kMagic, kVersion, kPing, 0, ++seq, now };
                if(sendto(s, &p, sizeof(p), 0, (sockaddr*)&host, sizeof(host)) < 0)
                    Log(true, "[КЛИЕНТ] sendto: %s", strerror(errno));
                ++sent;
                nextPing = now + 1000000ull;

                if(sent % 10 == 0)
                {
                    if(got)
                        Log(false, "[КЛИЕНТ] Пинг: средний %llu мс, мин %llu, макс %llu, потери %u%%",
                            (unsigned long long)(rttSum / got / 1000), (unsigned long long)(rttMin / 1000),
                            (unsigned long long)(rttMax / 1000), (unsigned)(got >= 10 ? 0 : 100 - got * 10));
                    rttSum = 0; rttMin = UINT64_MAX; rttMax = 0; got = 0;
                }
            }

            if(WaitReadable(s, 100))
            {
                Packet  p;
                ssize_t n = recv(s, &p, sizeof(p), 0);
                if(n > 0 && IsValid(p, n) && p.type == kPong)
                {
                    uint64_t rtt = NowUs() - p.sentUs;
                    rttSum += rtt; ++got;
                    if(rtt < rttMin) rttMin = rtt;
                    if(rtt > rttMax) rttMax = rtt;
                    lastPong = NowUs();
                    if(!connected)
                    {
                        connected = true;
                        Log(false, "[КЛИЕНТ] Есть связь с хостом! Пинг %llu мс", (unsigned long long)(rtt / 1000));
                    }
                }
            }

            if(connected && NowUs() - lastPong > 5000000ull)
            {
                connected = false;
                Log(true, "[КЛИЕНТ] Связь потеряна (нет ответа 5 сек)");
            }
            else if(!connected && !warnedNoHost && sent >= 10 && lastPong == 0)
            {
                warnedNoHost = true;
                Log(true, "[КЛИЕНТ] Хост не отвечает. Проверь: Tailscale включён у обоих, адрес и порт верные, у хоста игра запущена.");
            }
        }
    }
}

void StartConnectionTest(const TestConfig& config, FLogger* log)
{
    g_log = log;
    TestConfig copy = config;
    if(config.role == Role::Host) std::thread(RunHost, copy).detach();
    else                          std::thread(RunClient, copy).detach();
}

} // namespace net
