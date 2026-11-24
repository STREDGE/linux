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
    constexpr uint64_t kPingIntervalUs  = 1000000;   // 1 c
    constexpr uint64_t kHelloIntervalUs = 1000000;   // клиент стучится раз в секунду
    constexpr uint64_t kTimeoutUs       = 10000000;  // 10 c без пакетов = связь потеряна
    constexpr uint64_t kRequestTtlUs    = 15000000;  // запрос висит 15 с
    constexpr uint64_t kRejoinWindowUs  = 300000000; // тот же друг в течение 5 мин — без вопроса
    constexpr uint64_t kDenyHoldUs      = 30000000;  // отказанный адрес молчит 30 с

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
    static bool SameAddr(const sockaddr_in& a, const sockaddr_in& b)
    {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }
    static void CopyNick(char* dst, const char* src)
    {
        strncpy(dst, src, kNickLen - 1);
        dst[kNickLen - 1] = 0;
        for(char* p = dst; *p; ++p) if((unsigned char)*p < 0x20) *p = '?';
        if(!dst[0]) strcpy(dst, "Player");
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

    void Session::PushEvent(EventType t, const char* nick)
    {
        if(m_events.size() > 32) m_events.erase(m_events.begin());
        Event e{ t, "", m_lastRttMs };
        CopyNick(e.nick, nick ? nick : "");
        m_events.push_back(e);
    }

    bool Session::PopEvent(Event& out)
    {
        if(m_events.empty()) return false;
        out = m_events.front();
        m_events.erase(m_events.begin());
        return true;
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
            m_link = Link::Requesting;
            Log(false, "[КЛИЕНТ] Стучусь к %s:%u как «%s» ...", cfg.hostAddress, (unsigned)cfg.port, cfg.nick);
        }
        else
        {
            m_link = Link::Idle;
            Log(false, "[ХОСТ] Мир открыт на UDP-порту %u, ник «%s»", (unsigned)cfg.port, cfg.nick);
        }
        return true;
    }

    void Session::Stop()
    {
        if(m_sock >= 0)
        {
            // Вежливо сообщаем другу, что уходим (не обязательно дойдёт)
            if(m_link == Link::Connected && m_hasPeer)
            {
                DenyPacket d{ MakeHeader(PKT_DENY), DENY_TIMEOUT, {} };
                SendTo(&d, sizeof(d));
            }
            close(m_sock);
        }
        FLogger* log = m_log;
        *this = Session();
        m_log = log;
    }

    bool Session::Connected() const
    {
        return m_link == Link::Connected && m_lastRecvUs && NowUs() - m_lastRecvUs < kTimeoutUs;
    }

    void Session::SendToAddr(const void* data, int len, const sockaddr_in& to)
    {
        if(m_sock < 0) return;
        sendto(m_sock, data, len, 0, (const sockaddr*)&to, sizeof(to));
    }
    void Session::SendTo(const void* data, int len)
    {
        if(m_hasPeer) SendToAddr(data, len, m_peer);
    }

    void Session::DropPeer(const char* why)
    {
        Log(true, "Связь с «%s» потеряна (%s)", m_peerNick, why);
        PushEvent(EventType::Disconnected, m_peerNick);
        m_remote.valid = false;
        if(m_cfg.role == Role::Host) { m_hasPeer = false; m_link = Link::Idle; }
        else                         { m_link = Link::Requesting; m_helloSent = 0; } // клиент стучится снова
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

        // Клиент стучится, пока не приняли
        if(m_cfg.role == Role::Client && m_link == Link::Requesting && now >= m_nextHelloUs)
        {
            HelloPacket h{ MakeHeader(PKT_HELLO), "" };
            CopyNick(h.nick, m_cfg.nick);
            SendTo(&h, sizeof(h));
            m_nextHelloUs = now + kHelloIntervalUs;
            if(++m_helloSent == 10)
                Log(true, "[КЛИЕНТ] Хост молчит 10 с. Проверь: Tailscale включён у обоих, адрес верный, у хоста режим «Хост».");
        }

        // Пинги в обе стороны после принятия
        if(m_link == Link::Connected && now >= m_nextPingUs)
        {
            PingPacket p{ MakeHeader(PKT_PING), ++m_pingSeq, now };
            SendTo(&p, sizeof(p));
            m_nextPingUs = now + kPingIntervalUs;
        }

        // Запрос висит слишком долго — отказ
        if(m_link == Link::Pending && now - m_pendingSinceUs > kRequestTtlUs)
        {
            DenyPacket d{ MakeHeader(PKT_DENY), DENY_TIMEOUT, {} };
            SendToAddr(&d, sizeof(d), m_pending);
            PushEvent(EventType::RequestExpired, m_pendingNick);
            Log(false, "[ХОСТ] Запрос «%s» истёк без ответа", m_pendingNick);
            m_denied = m_pending; m_deniedUntilUs = now + kDenyHoldUs;
            m_link = Link::Idle;
        }

        if(m_link == Link::Connected && !Connected()) DropPeer("нет пакетов 10 с");
    }

    void Session::AnswerJoin(bool accept)
    {
        if(m_link != Link::Pending) return;
        uint64_t now = NowUs();
        if(accept)
        {
            m_peer = m_pending; m_hasPeer = true;
            CopyNick(m_peerNick, m_pendingNick);
            AcceptPacket a{ MakeHeader(PKT_ACCEPT), "" };
            CopyNick(a.nick, m_cfg.nick);
            SendTo(&a, sizeof(a));
            m_link = Link::Connected; m_lastRecvUs = now; m_nextPingUs = 0;
            m_lastAccepted = m_peer; CopyNick(m_lastAcceptedNick, m_peerNick); m_lastAcceptedUs = now;
            PushEvent(EventType::Connected, m_peerNick);
            Log(false, "[ХОСТ] «%s» принят", m_peerNick);
        }
        else
        {
            DenyPacket d{ MakeHeader(PKT_DENY), DENY_BY_HOST, {} };
            SendToAddr(&d, sizeof(d), m_pending);
            m_denied = m_pending; m_deniedUntilUs = now + kDenyHoldUs;
            m_link = Link::Idle;
            Log(false, "[ХОСТ] «%s» отклонён", m_pendingNick);
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
        uint64_t now = NowUs();

        if(m_cfg.role == Role::Host)
        {
            if(h->type == PKT_HELLO && len == sizeof(HelloPacket))
            {
                const HelloPacket* hp = (const HelloPacket*)data;
                char nick[kNickLen]; CopyNick(nick, hp->nick);
                if(m_link == Link::Connected)
                {
                    if(SameAddr(from, m_peer)) // друг переподключился, пока мы его ещё держим
                    {
                        AcceptPacket a{ MakeHeader(PKT_ACCEPT), "" }; CopyNick(a.nick, m_cfg.nick);
                        SendTo(&a, sizeof(a)); m_lastRecvUs = now;
                    }
                    else { DenyPacket d{ MakeHeader(PKT_DENY), DENY_BUSY, {} }; SendToAddr(&d, sizeof(d), from); }
                    return;
                }
                if(m_deniedUntilUs > now && SameAddr(from, m_denied)) return;
                // Тот же друг недавно был принят — пускаем без вопроса
                if(m_lastAcceptedUs && now - m_lastAcceptedUs < kRejoinWindowUs &&
                   from.sin_addr.s_addr == m_lastAccepted.sin_addr.s_addr && !strcmp(nick, m_lastAcceptedNick))
                {
                    m_pending = from; CopyNick(m_pendingNick, nick); m_link = Link::Pending;
                    AnswerJoin(true);
                    return;
                }
                if(m_link != Link::Pending || !SameAddr(from, m_pending))
                {
                    m_pending = from; CopyNick(m_pendingNick, nick);
                    m_pendingSinceUs = now; m_link = Link::Pending;
                    char addr[64];
                    Log(false, "[ХОСТ] «%s» (%s) просится в твой мир", nick, AddrToStr(from, addr, sizeof(addr)));
                    PushEvent(EventType::JoinRequest, nick);
                }
                return;
            }
            // Остальное — только от принятого друга
            if(m_link != Link::Connected || !SameAddr(from, m_peer)) return;
        }
        else
        {
            if(from.sin_addr.s_addr != m_peer.sin_addr.s_addr) return; // клиент слушает только хоста
            if(h->type == PKT_ACCEPT && len == sizeof(AcceptPacket))
            {
                if(m_link != Link::Connected)
                {
                    CopyNick(m_peerNick, ((const AcceptPacket*)data)->nick);
                    m_link = Link::Connected; m_lastRecvUs = now; m_nextPingUs = 0;
                    PushEvent(EventType::Connected, m_peerNick);
                    Log(false, "[КЛИЕНТ] «%s» принял тебя в свой мир", m_peerNick);
                }
                return;
            }
            if(h->type == PKT_DENY && len == sizeof(DenyPacket))
            {
                uint8_t r = ((const DenyPacket*)data)->reason;
                if(m_link == Link::Connected) { DropPeer("хост закрыл мир"); return; }
                if(r == DENY_BUSY) { PushEvent(EventType::HostBusy, ""); Log(true, "[КЛИЕНТ] У хоста уже кто-то играет"); }
                else               { PushEvent(EventType::DeniedByHost, ""); Log(true, "[КЛИЕНТ] Хост отклонил запрос"); }
                m_link = Link::Denied;
                return;
            }
            if(m_link != Link::Connected) return;
        }

        m_lastRecvUs = now;
        switch(h->type)
        {
            case PKT_PING:
                if(len == sizeof(PingPacket))
                {
                    PingPacket p = *(const PingPacket*)data;
                    p.hdr = MakeHeader(PKT_PONG);
                    SendTo(&p, sizeof(p));
                }
                break;

            case PKT_PONG:
                if(len == sizeof(PingPacket))
                    m_lastRttMs = (int)((now - ((const PingPacket*)data)->sentUs) / 1000);
                break;

            case PKT_PLAYER_STATE:
                if(len == sizeof(PlayerStatePacket))
                {
                    const PlayerStatePacket* p = (const PlayerStatePacket*)data;
                    if(!m_remote.valid || (int32_t)(p->seq - m_remote.pkt.seq) > 0 ||
                       now - m_remote.receivedUs > 2000000ull) // друг перезапустил игру
                    {
                        m_remote.pkt        = *p;
                        m_remote.receivedUs = now;
                        m_remote.valid      = true;
                    }
                }
                break;

            case PKT_DENY: // хост/клиент ушёл
                if(len == sizeof(DenyPacket)) DropPeer("друг вышел из коопа");
                break;
        }
    }

    void Session::SendPlayerState(PlayerStatePacket& pkt)
    {
        if(m_link != Link::Connected) return;
        pkt.hdr = MakeHeader(PKT_PLAYER_STATE);
        pkt.seq = ++m_stateSeq;
        SendTo(&pkt, sizeof(pkt));
    }
}
