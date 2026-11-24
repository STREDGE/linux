#pragma once
// UDP-сессия между двумя телефонами. Работает в игровом потоке:
// Poll() и Send*() вызываются раз в кадр, сокет неблокирующий.

#include <stdint.h>
#include <netinet/in.h>

#include "protocol.h"

class FLogger;

namespace net
{
    enum class Role { Host, Client, Echo };

    struct Config
    {
        Role     role = Role::Host;
        char     hostAddress[64] = "100.64.0.1";
        uint16_t port = 7777;
    };

    // Последнее полученное состояние друга
    struct RemoteState
    {
        bool     valid = false;
        uint64_t receivedUs = 0;   // по нашим часам
        PlayerStatePacket pkt{};
    };

    class Session
    {
    public:
        bool Start(const Config& cfg, FLogger* log);
        void Poll();                                   // принять всё, что пришло; пинги
        void SendPlayerState(PlayerStatePacket& pkt);  // seq и заголовок проставит сам
        const RemoteState& Remote() const { return m_remote; }
        bool  Connected() const;

    private:
        void HandlePacket(const uint8_t* data, int len, const sockaddr_in& from);
        void SendTo(const void* data, int len);
        void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

        Config      m_cfg{};
        FLogger*    m_log = nullptr;
        int         m_sock = -1;
        sockaddr_in m_peer{};
        bool        m_hasPeer = false;
        uint64_t    m_lastRecvUs = 0;
        bool        m_announcedConnected = false;

        uint32_t    m_pingSeq = 0;
        uint64_t    m_nextPingUs = 0;
        uint32_t    m_stateSeq = 0;
        uint32_t    m_pongs = 0;
        uint64_t    m_rttSumUs = 0;

        RemoteState m_remote;
    };

    uint64_t NowUs();
}
