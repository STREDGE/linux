#pragma once
// Сетевой протокол SA Coop (UDP). Все пакеты начинаются с Header.
// При несовместимом изменении — увеличить kProtocolVersion.

#include <stdint.h>

namespace net
{
    constexpr uint32_t kMagic           = 0x50434153; // "SACP"
    constexpr uint8_t  kProtocolVersion = 7;
    constexpr int      kNickLen         = 24;

    enum PacketType : uint8_t
    {
        PKT_PING         = 1, // в обе стороны раз в секунду (после принятия)
        PKT_PONG         = 2, // ответ на PING
        PKT_PLAYER_STATE = 3, // в обе стороны, ~20 раз в секунду (после принятия)
        PKT_HELLO        = 4, // клиент -> хост: «хочу в твой мир» (раз в секунду, пока не ответят)
        PKT_ACCEPT       = 5, // хост -> клиент: принят
        PKT_DENY         = 6, // хост -> клиент: отклонён / занято
        PKT_VOICE        = 7, // голос: один кадр Opus (20 мс), пока зажата кнопка
    };

    enum DenyReason : uint8_t { DENY_BY_HOST = 1, DENY_BUSY = 2, DENY_TIMEOUT = 3 };

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

    struct HelloPacket
    {
        Header hdr;
        char   nick[kNickLen];
    };

    struct AcceptPacket
    {
        Header hdr;
        char   nick[kNickLen];   // ник хоста
    };

    struct DenyPacket
    {
        Header  hdr;
        uint8_t reason;          // DenyReason
        uint8_t pad[3];
    };

    enum PlayerStateFlags : uint8_t
    {
        PSF_IN_VEHICLE = 1 << 0,
        PSF_SPRINT     = 1 << 1, // кнопка спринта нажата
        PSF_TALKING    = 1 << 2, // зажата кнопка микрофона
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
        uint8_t  armour;
        uint8_t  pad[3];
    };
    constexpr int kVoiceMaxBytes = 255;
    struct VoicePacket            // шлётся только заголовок + len байт данных
    {
        Header   hdr;
        uint16_t seq;             // номер кадра
        uint8_t  spurt;           // номер нажатия кнопки (новая фраза)
        uint8_t  len;
        uint8_t  data[kVoiceMaxBytes];
    };
    constexpr int kVoiceHeaderSize = (int)sizeof(Header) + 4;
    #pragma pack(pop)

    inline Header MakeHeader(PacketType t) { return Header{ kMagic, kProtocolVersion, (uint8_t)t, 0 }; }
}
