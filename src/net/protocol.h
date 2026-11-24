#pragma once
// Сетевой протокол SA Coop (UDP). Все пакеты начинаются с Header.
// При несовместимом изменении — увеличить kProtocolVersion.

#include <stdint.h>

namespace net
{
    constexpr uint32_t kMagic           = 0x50434153; // "SACP"
    constexpr uint8_t  kProtocolVersion = 5;

    enum PacketType : uint8_t
    {
        PKT_PING        = 1, // клиент -> хост, раз в секунду (и как «я тут»)
        PKT_PONG        = 2, // хост -> клиент, ответ на PING
        PKT_PLAYER_STATE = 3, // в обе стороны, ~20 раз в секунду
    };

    #pragma pack(push, 1)
    struct Header
    {
        uint32_t magic;
        uint8_t  version;
        uint8_t  type;
        uint16_t reserved;
    };

    struct PingPacket
    {
        Header   hdr;
        uint32_t seq;
        uint64_t sentUs;
    };

    enum PlayerStateFlags : uint8_t
    {
        PSF_IN_VEHICLE = 1 << 0,
        PSF_SPRINT     = 1 << 1, // кнопка спринта нажата
    };

    struct PlayerStatePacket
    {
        Header   hdr;
        uint32_t seq;
        float    pos[3];
        float    vel[3];      // м/с
        float    heading;     // радианы
        uint8_t  health;
        uint8_t  flags;       // PlayerStateFlags
        uint8_t  moveState;   // eMoveState игры: 1 стоит, 4 шаг, 6 бег/спринт
        uint8_t  jumpCount;   // счётчик прыжков: изменился — второй CJ прыгает
        float    moveRatio;   // сила движения 0..~2 (как от джойстика: 1 = шаг, больше — бег)
    };
    #pragma pack(pop)

    inline Header MakeHeader(PacketType t) { return Header{ kMagic, kProtocolVersion, (uint8_t)t, 0 }; }
}
