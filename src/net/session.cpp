#include "session.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace net
{
    constexpr uint64_t kPingIntervalUs = 1000000;   // 1 c
    constexpr uint64_t kTimeoutUs      = 10000000;  // 10 c без пакетов = связь потеряна

    uint64_t NowUs()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
    }

    static const char* AddrToStr(const sockaddr_in& a, char* buf, size_t len)
    {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        snprintf(buf, len, "%s:%u", ip, (unsigned)ntohs(a.sin_port));
        return buf;
    }

    void Session::Log(bool error, const char* fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        if(m_log) { if(error) m_log->Error("%s", buf); else m_log->Info("%s", buf); }
    }

    bool Session::Start(const Config& cfg, FLogger* log)
    {
        Stop();
        m_cfg = cfg;
        m_log = log;

        m_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if(m_sock < 0)
        {
            Log(true, "socket() не создан: %s", strerror(errno));
            return false;
        }
        fcntl(m_sock, F_SETFL, fcntl(m_sock, F_GETFL, 0) | O_NONBLOCK);

        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port        = htons(cfg.role == Role::Host ? cfg.port : 0);
        if(bind(m_sock, (sockaddr*)&local, sizeof(local)) < 0)
        {
            Log(true, "bind(порт %u) не удался: %s", (unsigned)ntohs(local.sin_port), strerror(errno));
            close(m_sock); m_sock = -1;
            return false;
        }

        if(cfg.role == Role::Client)
        {
            m_peer.sin_family = AF_INET;
            m_peer.sin_port   = htons(cfg.port);
            if(inet_pton(AF_INET, cfg.hostAddress, &m_peer.sin_addr) != 1)
            {
                Log(true, "[КЛИЕНТ] Неверный адрес хоста '%s' (нужен вид 100.x.x.x)", cfg.hostAddress);
                close(m_sock); m_sock = -1;
                return false;
            }
            m_hasPeer = true;
            Log(false, "[КЛИЕНТ] Подключаюсь к %s:%u ...", cfg.hostAddress, (unsigned)cfg.port);
        }
        else
        {
            Log(false, "[ХОСТ] Жду друга на UDP-порту %u", (unsigned)cfg.port);
        }
        return true;
    }

    void Session::Stop()
    {
        if(m_sock >= 0) close(m_sock);
        FLogger* log = m_log;
        *this = Session();
        m_log = log;
    }

    bool Session::Connected() const
    {
        return m_lastRecvUs && NowUs() - m_lastRecvUs < kTimeoutUs;
    }

    void Session::SendTo(const void* data, int len)
    {
        if(m_sock < 0 || !m_hasPeer) return;
        sendto(m_sock, data, len, 0, (const sockaddr*)&m_peer, sizeof(m_peer));
    }

    void Session::Poll()
    {
        if(m_sock < 0) return;

        uint8_t buf[512];
        for(int i = 0; i < 64; ++i) // не больше 64 пакетов за кадр
        {
            sockaddr_in from{};
            socklen_t   fromLen = sizeof(from);
            ssize_t n = recvfrom(m_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
            if(n <= 0) break;
            HandlePacket(buf, (int)n, from);
        }

        uint64_t now = NowUs();
        if(m_cfg.role == Role::Client && now >= m_nextPingUs)
        {
            PingPacket p{ MakeHeader(PKT_PING), ++m_pingSeq, now };
            SendTo(&p, sizeof(p));
            m_nextPingUs = now + kPingIntervalUs;

            if(m_pingSeq % 10 == 0 && m_pongs)
            {
                Log(false, "[КЛИЕНТ] Пинг: средний %llu мс, ответов %u из 10",
                    (unsigned long long)(m_rttSumUs / m_pongs / 1000), m_pongs > 10 ? 10 : m_pongs);
                m_pongs = 0; m_rttSumUs = 0;
            }
            else if(m_pingSeq == 10 && !m_lastRecvUs)
            {
                Log(true, "[КЛИЕНТ] Хост не отвечает. Проверь: Tailscale включён у обоих, адрес и порт верные, у хоста игра запущена.");
            }
        }

        if(m_announcedConnected && !Connected())
        {
            m_announcedConnected = false;
            m_remote.valid = false;
            Log(true, "Связь с другом потеряна (нет пакетов 10 сек)");
        }
    }

    void Session::HandlePacket(const uint8_t* data, int len, const sockaddr_in& from)
    {
        if(len < (int)sizeof(Header)) return;
        const Header* h = (const Header*)data;
        if(h->magic != kMagic) return;
        if(h->version != kProtocolVersion)
        {
            static bool warned = false;
            if(!warned) { warned = true; Log(true, "У друга другая версия мода (протокол %u, у нас %u). Обновитесь оба.", h->version, kProtocolVersion); }
            return;
        }

        // Хост запоминает адрес клиента по первому пакету
        if(m_cfg.role == Role::Host)
        {
            bool same = m_hasPeer && from.sin_addr.s_addr == m_peer.sin_addr.s_addr && from.sin_port == m_peer.sin_port;
            if(!same) { m_peer = from; m_hasPeer = true; }
        }
        else if(from.sin_addr.s_addr != m_peer.sin_addr.s_addr)
        {
            return; // клиент принимает пакеты только от хоста
        }

        m_lastRecvUs = NowUs();
        if(!m_announcedConnected)
        {
            m_announcedConnected = true;
            char addr[64];
            Log(false, m_cfg.role == Role::Host ? "[ХОСТ] Друг подключился: %s" : "[КЛИЕНТ] Есть связь с хостом %s!",
                AddrToStr(from, addr, sizeof(addr)));
        }

        switch(h->type)
        {
            case PKT_PING:
                if(len == sizeof(PingPacket) && m_cfg.role == Role::Host)
                {
                    PingPacket p = *(const PingPacket*)data;
                    p.hdr = MakeHeader(PKT_PONG);
                    SendTo(&p, sizeof(p));
                }
                break;

            case PKT_PONG:
                if(len == sizeof(PingPacket))
                {
                    const PingPacket* p = (const PingPacket*)data;
                    uint64_t rtt = NowUs() - p->sentUs;
                    m_rttSumUs += rtt;
                    m_lastRttMs = (int)(rtt / 1000);
                    ++m_pongs;
                }
                break;

            case PKT_PLAYER_STATE:
                if(len == sizeof(PlayerStatePacket))
                {
                    const PlayerStatePacket* p = (const PlayerStatePacket*)data;
                    // Отбрасываем устаревшие (пришедшие не по порядку) пакеты
                    if(!m_remote.valid || (int32_t)(p->seq - m_remote.pkt.seq) > 0 ||
                       m_lastRecvUs - m_remote.receivedUs > 2000000ull) // друг перезапустил игру
                    {
                        m_remote.pkt        = *p;
                        m_remote.receivedUs = m_lastRecvUs;
                        m_remote.valid      = true;
                    }
                }
                break;
        }
    }

    void Session::SendPlayerState(PlayerStatePacket& pkt)
    {
        pkt.hdr = MakeHeader(PKT_PLAYER_STATE);
        pkt.seq = ++m_stateSeq;
        SendTo(&pkt, sizeof(pkt));
    }
}
