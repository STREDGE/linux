#include "session.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "trace/ctrace.h"

namespace net
{
    constexpr uint64_t kPingIntervalUs  = 1000000;   // 1 c
    constexpr uint64_t kHelloIntervalUs = 1000000;   // клиент стучится раз в секунду
    constexpr uint64_t kTimeoutUs       = 60000000;  // 60 c без пакетов = связь потеряна (свернул игру / пауза — ждём)
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
        // не обрывать русскую (многобайтную) букву посередине
        size_t n = strlen(dst);
        if(n && ((unsigned char)dst[n - 1] & 0x80))
        {
            size_t i = n;
            while(i > 0 && ((unsigned char)dst[i - 1] & 0xC0) == 0x80) --i;   // хвост буквы
            if(i > 0)
            {
                unsigned char lead = (unsigned char)dst[i - 1];
                size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                if(n - (i - 1) < need) dst[i - 1] = 0;
            }
        }
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
        if(m_log) { if(error) m_log->Error("%s", buf); else m_log->Info("%s", buf); }   // → SA_LOG.txt (FLogger::Sink)
        else ctrace::FromLog(error, buf);
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

    static bool ParseHostPort(const char* s, sockaddr_in& out, uint16_t defPort)
    {
        char host[64]; snprintf(host, sizeof(host), "%s", s);
        uint16_t port = defPort;
        char* colon = strrchr(host, ':');
        if(colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }
        memset(&out, 0, sizeof(out));
        out.sin_family = AF_INET; out.sin_port = htons(port);
        if(inet_pton(AF_INET, host, &out.sin_addr) == 1) return true;
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        if(getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
        out.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
        return true;
    }

    bool Session::Start(const Config& cfg, FLogger* log)
    {
        Stop();
        m_cfg = cfg;
        m_log = log;
        m_useRelay = false;
        if(cfg.relay[0])
        {
            if(ParseHostPort(cfg.relay, m_relay, 7777)) m_useRelay = true;
            else Log(true, "Сервер-посредник «%s» не найден — пробую напрямую (Tailscale)", cfg.relay);
        }
        bool roomJoin = false;   // клиент: в «адресе» 6 цифр — код комнаты
        if(m_useRelay && cfg.role == Role::Client)
        {
            size_t n = strlen(cfg.hostAddress); roomJoin = n == 6;
            for(size_t i = 0; i < n; ++i) if(cfg.hostAddress[i] < '0' || cfg.hostAddress[i] > '9') roomJoin = false;
        }
        if(m_useRelay && cfg.role == Role::Client && !roomJoin) m_useRelay = false;   // ввели адрес — напрямую

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
        local.sin_port        = htons(cfg.role == Role::Host && !m_useRelay ? cfg.port : 0);
        if(bind(m_sock, (sockaddr*)&local, sizeof(local)) < 0)
        {
            Log(true, "bind(порт %u) не удался: %s", (unsigned)ntohs(local.sin_port), strerror(errno));
            close(m_sock); m_sock = -1;
            return false;
        }

        if(m_useRelay)
        {
            if(cfg.role == Role::Host)
            {
                srand((unsigned)NowUs());
                snprintf(m_room, sizeof(m_room), "%06d", rand() % 1000000);
                m_link = Link::Idle;
                Log(false, "[ХОСТ] Мир открыт через сервер %s, код комнаты %s", cfg.relay, m_room);
            }
            else
            {
                snprintf(m_room, sizeof(m_room), "%s", cfg.hostAddress);
                m_peer = m_relay; m_hasPeer = true;
                m_link = Link::Requesting;
                Log(false, "[КЛИЕНТ] Захожу в комнату %s через сервер %s как «%s» ...", m_room, cfg.relay, cfg.nick);
            }
            m_nextRelayUs = 0;
            return true;
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
        VoiceSink sink = m_voiceSink;
        ModSink msink = m_modSink;
        GameSink gsink = m_gameSink;
        *this = Session();
        m_log = log;
        m_voiceSink = sink;
        m_modSink = msink;
        m_gameSink = gsink;
    }

    bool Session::Connected() const
    {
        return m_link == Link::Connected && m_lastRecvUs && NowUs() - m_lastRecvUs < kTimeoutUs;
    }

    void Session::SendToAddr(const void* data, int len, const sockaddr_in& to)
    {
        if(m_sock < 0) return;
        if(len >= (int)sizeof(Header))
        {
            const Header* h = (const Header*)data;
            if(h->magic == kMagic) { m_stats.sentPk[h->type & 31]++; m_stats.sentB[h->type & 31] += (uint32_t)len; }
        }
        ssize_t r = sendto(m_sock, data, len, 0, (const sockaddr*)&to, sizeof(to));
        if(r < 0)
        {
            static uint64_t lastErrUs = 0; uint64_t n = NowUs();
            if(n - lastErrUs > 2000000) { lastErrUs = n; CTRACE("СЕТЬ", "ошибка отправки: %s (%d байт)", strerror(errno), len); }
        }
    }
    void Session::SendTo(const void* data, int len)
    {
        if(m_hasPeer) SendToAddr(data, len, m_peer);
    }

    void Session::DropPeer(const char* why)
    {
        Log(true, "Связь с «%s» потеряна (%s)", m_peerNick, why);
        PushEvent(EventType::Disconnected, m_peerNick);
        CTRACE("СВЯЗЬ", "РАЗРЫВ с «%s»: %s (последний пакет %.1f с назад, надёжных в очереди %d)", m_peerNick, why,
               m_lastRecvUs ? (NowUs() - m_lastRecvUs) / 1e6 : -1.0, (int)m_relOut.size());
        m_remote.valid = false;
        ResetReliable();
        if(m_cfg.role == Role::Host) { m_hasPeer = false; m_link = Link::Idle; }
        else                         { m_link = Link::Requesting; m_helloSent = 0; } // клиент стучится снова
    }

    void Session::Poll()
    {
        if(m_sock < 0) return;

        uint8_t buf[2048];
        for(int i = 0; i < 128; ++i) // не больше 64 пакетов за кадр
        {
            sockaddr_in from{};
            socklen_t   fromLen = sizeof(from);
            ssize_t n = recvfrom(m_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
            if(n <= 0) break;
            if(m_useRelay && HandleRelay(buf, (int)n, from)) continue;
            HandlePacket(buf, (int)n, from);
        }

        uint64_t now = NowUs();
        if(m_useRelay) RelayTick(now);

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
            SendTo(&p, sizeof(p)); m_stats.pingSent++;
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

        // Надёжные события: повтор раз в 150 мс, пока не подтвердят (не дольше 15 с)
        if(m_link == Link::Connected)
        {
            for(size_t i = 0; i < m_relOut.size();)
            {
                RelOut& r = m_relOut[i];
                if(now - r.firstUs > 15000000ull)
                {
                    m_stats.relDropped++;
                    CTRACE("НАДЁЖНОЕ", "#%u тип %u (%d байт) не подтверждено за 15 с — выброшено", r.seq, r.len ? r.data[0] : 0, r.len);
                    m_relOut.erase(m_relOut.begin() + i); continue;
                }
                if(now - r.lastUs >= 150000ull)
                {
                    if(r.lastUs) m_stats.relResend++;
                    ReliablePacket p; p.hdr = MakeHeader(PKT_RELIABLE); p.seq = r.seq; p.len = (uint8_t)r.len;
                    p.pad[0] = p.pad[1] = p.pad[2] = 0; memcpy(p.data, r.data, r.len);
                    SendTo(&p, kReliableHeaderSize + r.len);
                    r.lastUs = now;
                }
                ++i;
            }
        }

        if(m_link == Link::Connected && !Connected()) DropPeer("нет пакетов 10 с");
    }

    // Сервер-посредник: хост держит комнату (раз в 5 с), друг заходит по коду (раз в секунду, пока не приняли)
    void Session::RelayTick(uint64_t now)
    {
        if(now < m_nextRelayUs) return;
        bool host = m_cfg.role == Role::Host;
        if(!host && m_link != Link::Requesting) { m_nextRelayUs = now + 5000000; }
        uint8_t p[11]; memcpy(p, "SCRL", 4); p[4] = host ? 'H' : 'C'; memcpy(p + 5, m_room, 6);
        SendToAddr(p, sizeof(p), m_relay);
        m_nextRelayUs = now + (host ? 5000000 : 1000000);
    }

    bool Session::HandleRelay(const uint8_t* data, int len, const sockaddr_in& from)
    {
        if(len < 11 || memcmp(data, "SCRL", 4) || !SameAddr(from, m_relay)) return false;
        if(data[4] == 'N')
        {
            static uint64_t warned = 0;
            if(NowUs() - warned > 10000000) { warned = NowUs(); PushEvent(EventType::RoomNotFound, ""); Log(true, "[КЛИЕНТ] Комнаты %s нет на сервере — проверь код", m_room); }
        }
        return true;
    }

    void Session::ResetReliable()
    {
        m_relOut.clear();
        memset(m_relSeen, 0, sizeof(m_relSeen));
        m_relSeenHead = 0;
    }

    void Session::SendGame(const void* pkt, int len)
    {
        if(m_link != Link::Connected || len <= 0) return;
        SendTo(pkt, len);
    }

    void Session::SendReliable(const void* data, int len)
    {
        if(m_link != Link::Connected || len <= 0 || len > kReliableMax) return;
        if(m_relOut.size() > 256) m_relOut.erase(m_relOut.begin());
        RelOut r; r.seq = ++m_relSeq; r.firstUs = NowUs(); r.lastUs = 0; r.len = len;
        memcpy(r.data, data, len);
        m_relOut.push_back(r);
        m_stats.relSent++;
        CTRACE("НАДЁЖНОЕ", "> #%u тип %u (%d байт), в очереди %d", r.seq, ((const uint8_t*)data)[0], len, (int)m_relOut.size());
        ReliablePacket p; p.hdr = MakeHeader(PKT_RELIABLE); p.seq = r.seq; p.len = (uint8_t)len;
        p.pad[0] = p.pad[1] = p.pad[2] = 0; memcpy(p.data, data, len);
        SendTo(&p, kReliableHeaderSize + len);
        m_relOut.back().lastUs = r.firstUs;
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
            m_link = Link::Connected; m_lastRecvUs = now; m_nextPingUs = 0; ResetReliable();
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
            m_stats.badVersion++;
            if(!warned) { warned = true; Log(true, "У друга другая версия мода (протокол %u, у нас %u). Обновитесь оба.", h->version, kProtocolVersion); }
            return;
        }
        uint64_t now = NowUs();
        m_stats.recvPk[h->type & 31]++; m_stats.recvB[h->type & 31] += (uint32_t)len;

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
                        SendTo(&a, sizeof(a)); m_lastRecvUs = now; ResetReliable();
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
                    m_link = Link::Connected; m_lastRecvUs = now; m_nextPingUs = 0; ResetReliable();
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
                {
                    m_lastRttMs = (int)((now - ((const PingPacket*)data)->sentUs) / 1000);
                    m_stats.pongRecv++;
                }
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

            case PKT_VOICE:
                if(len >= kVoiceHeaderSize)
                {
                    const VoicePacket* v = (const VoicePacket*)data;
                    if(v->len && kVoiceHeaderSize + v->len == len)
                    {
                        if(!m_voiceIn++) Log(false, "Пошёл голос от друга");
                        if(m_voiceSink) m_voiceSink(v->seq, v->spurt, v->data, v->len);
                    }
                }
                break;

            case PKT_MOD:
                if(len >= kModHeaderSize)
                {
                    const ModPacket* mp = (const ModPacket*)data;
                    if(mp->len <= kModMaxBytes && kModHeaderSize + mp->len == len) { m_stats.modIn[mp->channel]++; if(m_modSink) m_modSink(mp->channel, mp->data, mp->len); }
                }
                break;

            case PKT_VEHICLE:
                if(len == sizeof(VehiclePacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_VEHICLE_GONE:
                if(len == sizeof(VehicleGonePacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_SHOT:
                if(len == sizeof(ShotPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_PEDS:
                if(len >= kPedsHeaderSize && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_WORLD_TIME:
                if(len == sizeof(WorldTimePacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_LOG_CHUNK:
                if(len >= kLogChunkHeaderSize && len <= (int)sizeof(LogChunkPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_MODFILE_REQ:
                if(len >= kModFileReqHeaderSize && len <= (int)sizeof(ModFileReqPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_MODFILE_CHUNK:
                if(len >= kModFileChunkHeaderSize && len <= (int)sizeof(ModFileChunkPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_MODFILE_ACK:
                if(len == sizeof(ModFileAckPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_MODS:
                if(len >= kModsHeaderSize && len <= (int)sizeof(ModsPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_LOG_ACK:
                if(len == sizeof(LogAckPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_LOBBY:
                if(len == sizeof(LobbyPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_BLIPS:
                if(len >= kBlipsHeaderSize && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_WORLD_REQ:
                if(len == sizeof(WorldReqPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_WORLD_ACK:
                if(len == sizeof(WorldAckPacket) && m_gameSink) m_gameSink(h->type, data, len);
                break;
            case PKT_WORLD_CHUNK:
                if(len >= kWorldChunkHeaderSize && m_gameSink) m_gameSink(h->type, data, len);
                break;

            case PKT_RELIABLE:
                if(len >= kReliableHeaderSize)
                {
                    const ReliablePacket* rp = (const ReliablePacket*)data;
                    if(rp->len == 0 || rp->len > kReliableMax || kReliableHeaderSize + rp->len != len) break;
                    AckPacket a{ MakeHeader(PKT_ACK), rp->seq };
                    SendTo(&a, sizeof(a));
                    bool seen = false;
                    for(uint32_t s2 : m_relSeen) if(s2 == rp->seq) { seen = true; break; }
                    if(seen) { m_stats.relDup++; break; }
                    m_relSeen[m_relSeenHead] = rp->seq; m_relSeenHead = (m_relSeenHead + 1) % 128;
                    m_stats.relRecv++;
                    CTRACE("НАДЁЖНОЕ", "< #%u тип %u (%d байт)", rp->seq, rp->data[0], (int)rp->len);
                    if(m_gameSink) m_gameSink(PKT_RELIABLE, rp->data, rp->len);
                }
                break;
            case PKT_ACK:
                if(len == sizeof(AckPacket))
                {
                    uint32_t sq = ((const AckPacket*)data)->seq;
                    for(size_t i = 0; i < m_relOut.size(); ++i)
                        if(m_relOut[i].seq == sq) { m_relOut.erase(m_relOut.begin() + i); break; }
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

    void Session::SendVoice(uint16_t seq, uint8_t spurt, const uint8_t* data, int len)
    {
        if(m_link != Link::Connected || len <= 0 || len > kVoiceMaxBytes) return;
        VoicePacket v;
        v.hdr = MakeHeader(PKT_VOICE);
        v.seq = seq; v.spurt = spurt; v.len = (uint8_t)len;
        memcpy(v.data, data, len);
        SendTo(&v, kVoiceHeaderSize + len);
        if(!m_voiceOut++) Log(false, "Пошёл голос другу");
    }

    bool Session::SendMod(uint8_t channel, const void* data, int len)
    {
        if(m_link != Link::Connected || len < 0 || len > kModMaxBytes) return false;
        ModPacket p;
        p.hdr = MakeHeader(PKT_MOD); p.channel = channel; p.pad = 0; p.len = (uint16_t)len;
        memcpy(p.data, data, len);
        SendTo(&p, kModHeaderSize + len);
        m_stats.modOut[channel]++;
        return true;
    }
}
