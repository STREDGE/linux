#pragma once
// UDP-сессия между двумя телефонами. Работает в игровом потоке:
// Poll() и Send*() вызываются раз в кадр, сокет неблокирующий.
//
// Рукопожатие: клиент шлёт HELLO с ником раз в секунду. Хост показывает запрос
// игроку (Принять / Отклонить) и отвечает ACCEPT или DENY. Состояния игроков
// ходят только после ACCEPT.

#include <stdint.h>
#include <netinet/in.h>

#include <vector>

#include "protocol.h"

class FLogger;

namespace net
{
    enum class Role { Off, Echo, Host, Client }; // порядок = пункты меню

    struct Config
    {
        Role     role = Role::Host;
        char     hostAddress[64] = "100.64.0.1";
        uint16_t port = 7777;
        char     nick[kNickLen] = "Player";
        char     relay[64] = "";          // сервер-посредник «адрес:порт» (пусто — напрямую, Tailscale)
        char     myId[8] = "";            // свой ID точки (6 цифр, создаётся сам при первом запуске)
        char     tsKey[128] = "";         // ключ Tailscale (tskey-auth-…): хост вшивает в код приглашения, друг входит по нему. В лог не писать!
    };

    // Последнее полученное состояние друга
    struct RemoteState
    {
        bool     valid = false;
        uint64_t receivedUs = 0;   // по нашим часам
        PlayerStatePacket pkt{};
    };

    // События для интерфейса (уведомления)
    enum class EventType { JoinRequest, Connected, Disconnected, DeniedByHost, HostBusy, RequestExpired, RoomNotFound };
    struct Event { EventType type; char nick[kNickLen]; int pingMs; };

    enum class Link
    {
        Idle,          // хост: никого нет
        Requesting,    // клиент: шлём HELLO, ответа нет
        Pending,       // хост: пришёл запрос, ждём решения игрока
        Connected,     // принят, обмен идёт
        Denied,        // клиент: хост отказал (больше не стучимся)
    };

    class Session
    {
    public:
        bool Start(const Config& cfg, FLogger* log);
        void Stop();                                   // закрыть сокет, забыть друга
        bool Running() const { return m_sock >= 0; }
        int  LastRttMs() const { return m_lastRttMs; }
        void Poll();                                   // принять всё, что пришло; пинги, HELLO
        void SendPlayerState(PlayerStatePacket& pkt);  // seq и заголовок проставит сам
        const RemoteState& Remote() const { return m_remote; }
        bool  Connected() const;
        Link  GetLink() const { return m_link; }
        const char* PeerNick() const { return m_peerNick; }
        const char* PendingNick() const { return m_pendingNick; }
        uint64_t    PendingSinceUs() const { return m_pendingSinceUs; }
        void  AnswerJoin(bool accept);                 // хост: ответ на запрос
        bool  PopEvent(Event& out);
        void  SendVoice(uint16_t seq, uint8_t spurt, const uint8_t* data, int len);
        typedef void (*VoiceSink)(uint16_t seq, uint8_t spurt, const uint8_t* data, int len);
        void  SetVoiceSink(VoiceSink fn) { m_voiceSink = fn; }
        bool  SendMod(uint8_t channel, const void* data, int len);
        typedef void (*ModSink)(uint8_t channel, const uint8_t* data, int len);
        void  SetModSink(ModSink fn) { m_modSink = fn; }

        // Игровые пакеты (машины, выстрелы) и надёжные события (урон).
        // В приёмник приходят: PKT_VEHICLE / PKT_VEHICLE_GONE / PKT_SHOT — весь пакет,
        // PKT_RELIABLE — только полезные данные события.
        typedef void (*GameSink)(uint8_t type, const uint8_t* data, int len);
        void  SetGameSink(GameSink fn) { m_gameSink = fn; }
        // Счётчики для подробного лога (только игровой поток)
        struct Stats
        {
            uint32_t sentPk[32] = {}, sentB[32] = {}, recvPk[32] = {}, recvB[32] = {};   // по типу пакета (PacketType)
            uint32_t relSent = 0, relRecv = 0, relDup = 0, relResend = 0, relDropped = 0;
            uint32_t pingSent = 0, pongRecv = 0, badVersion = 0;
            uint32_t modOut[256] = {}, modIn[256] = {};                                     // канал SACoopNet
        };
        const Stats& GetStats() const { return m_stats; }
        int   ReliableQueue() const { return (int)m_relOut.size(); }
        uint64_t LastRecvUs() const { return m_lastRecvUs; }
        bool  UsingRelay() const { return m_useRelay; }
        const char* RoomCode() const { return m_room; }     // хост через посредника: код комнаты (6 цифр)
        void  SendGame(const void* pkt, int len);       // заголовок уже заполнен
        void  SendReliable(const void* data, int len);  // до kReliableMax байт

    private:
        void HandlePacket(const uint8_t* data, int len, const sockaddr_in& from);
        void SendTo(const void* data, int len);
        void SendToAddr(const void* data, int len, const sockaddr_in& to);
        void PushEvent(EventType t, const char* nick);
        void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
        void DropPeer(const char* why);
        void ResetReliable();

        Config      m_cfg{};
        FLogger*    m_log = nullptr;
        int         m_sock = -1;
        Link        m_link = Link::Idle;

        sockaddr_in m_peer{};          // клиент: адрес хоста; хост: принятый клиент
        bool        m_hasPeer = false;
        char        m_peerNick[kNickLen] = "";
        uint64_t    m_lastRecvUs = 0;

        // хост: ожидающий запрос
        sockaddr_in m_pending{};
        char        m_pendingNick[kNickLen] = "";
        uint64_t    m_pendingSinceUs = 0;
        sockaddr_in m_lastAccepted{};  // переподключение того же друга — без вопроса
        char        m_lastAcceptedNick[kNickLen] = "";
        uint64_t    m_lastAcceptedUs = 0;
        sockaddr_in m_denied{};        // отказанный адрес: игнорируем 30 с
        uint64_t    m_deniedUntilUs = 0;

        uint32_t    m_pingSeq = 0;
        uint64_t    m_nextPingUs = 0;
        uint64_t    m_nextHelloUs = 0;
        uint32_t    m_helloSent = 0;
        uint32_t    m_stateSeq = 0;
        int         m_lastRttMs = -1;

        RemoteState m_remote;
        std::vector<Event> m_events;
        VoiceSink   m_voiceSink = nullptr;
        ModSink     m_modSink = nullptr;
        GameSink    m_gameSink = nullptr;
        bool        m_useRelay = false;     // через сервер-посредник с кодом комнаты
        sockaddr_in m_relay{};
        char        m_room[8] = "";
        uint64_t    m_nextRelayUs = 0;
        void        RelayTick(uint64_t now);
        bool        HandleRelay(const uint8_t* data, int len, const sockaddr_in& from);

        struct RelOut { uint32_t seq; uint64_t firstUs, lastUs; int len; uint8_t data[kReliableMax]; };
        std::vector<RelOut> m_relOut;
        uint32_t    m_relSeq = 0;
        uint32_t    m_relSeen[128] = {};    // последние принятые номера (повторы отбрасываем)
        int         m_relSeenHead = 0;
        uint32_t    m_voiceIn = 0, m_voiceOut = 0;
        Stats       m_stats;
    };

    uint64_t NowUs();
    // Сколько прошло от t до now. Без ухода «в минус»: пакет, принятый после начала кадра,
    // новее `now` — разность беззнаковых чисел дала бы огромное значение (0.8.7: второй CJ
    // пересоздавался 33 раза в секунду).
    inline uint64_t Since(uint64_t now, uint64_t t) { return now > t ? now - t : 0; }
}
