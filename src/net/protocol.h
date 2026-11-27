#pragma once
// Сетевой протокол SA Coop (UDP). Все пакеты начинаются с Header.
// При несовместимом изменении — увеличить kProtocolVersion.

#include <stdint.h>

namespace net
{
    constexpr uint32_t kMagic           = 0x50434153; // "SACP"
    constexpr uint8_t  kProtocolVersion = 19;   // 19: версия SA Coop в сверке, обновление друга с телефона хоста — старые версии не соединяются; 18: передача модов от хоста (MODFILE_*), сверка по контрольной сумме; 17: сверка модов (PKT_MODS); 16: лог друга в файл хоста (LOG_CHUNK/LOG_ACK); 15: «Новая игра» в лобби
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
        PKT_MOD          = 8, // данные другого мода (SA Living): канал + байты
        PKT_VEHICLE      = 9, // состояние машины от того, кто ей управляет (20/с в движении, 1/с стоя)
        PKT_VEHICLE_GONE = 10,// машина больше не общая — удалить копию
        PKT_SHOT         = 11,// выстрел: повторить у друга (звук, вспышка, след пули)
        PKT_RELIABLE     = 12,// событие с подтверждением (урон и т.п.), повтор до ACK
        PKT_ACK          = 13,// подтверждение PKT_RELIABLE
        PKT_WORLD_REQ    = 14,// клиент -> хост: пришли свой мир (сохранение), я загружусь в него
        PKT_WORLD_CHUNK  = 15,// хост -> клиент: кусок сохранения
        PKT_WORLD_ACK    = 16,// клиент -> хост: кусок получен
        PKT_PEDS         = 17,// хост -> клиент: педы вокруг друга (10 раз/с)
        PKT_WORLD_TIME   = 18,// хост -> клиент: время и погода
        PKT_BLIPS        = 19,// хост -> клиент: метки миссии на карте (4 раза/с)
        PKT_LOBBY        = 20,// хост -> клиент: лобби (что выбрано, «Запуск»), раз в секунду
        PKT_LOG_CHUNK    = 21,// клиент -> хост: кусок его SA_LOG.txt (хост дописывает в свой с пометкой «Д»)
        PKT_LOG_ACK      = 22,// хост -> клиент: кусок записан
        PKT_MODS         = 23,// в обе стороны: список модов (AML, CLEO, файлы игры) для сверки, раз в 3 с
        PKT_MODFILE_REQ  = 24,// клиент -> хост: пришли эти файлы модов (номера в списке хоста)
        PKT_MODFILE_CHUNK= 25,// хост -> клиент: кусок файла мода
        PKT_MODFILE_ACK  = 26,// клиент -> хост: принято по порядку до seq
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
        PSF_DEAD       = 1 << 3, // погиб (лежит / ждёт больницы)
        PSF_AIMING     = 1 << 4, // целится
        PSF_DUCK       = 1 << 5, // присел
        PSF_FF         = 1 << 6, // огонь по своим включён (решает хост)
        PSF_SHARED_WANTED = 1 << 7, // розыск общий (решает хост); нет — у каждого свой
    };

    constexpr uint8_t kNoSeat = 0xFF;

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
        uint8_t  weapon;      // eWeaponType в руках
        uint8_t  meleeCount;  // счётчик ударов: изменился — второй CJ бьёт
        uint8_t  wanted;      // звёзды розыска
        uint16_t vehId;       // машина: в которой сидит или в которую садится (0 — нет)
        uint8_t  seat;        // kNoSeat — пешком, 0 — водитель, 1.. — пассажир (номер места + 1)
        uint8_t  enterSeat;   // садится: kNoSeat — нет, 0 — за руль, 1.. — пассажиром
        uint8_t  area;        // интерьер (CGame::currArea; 0 — улица)
        float    aim[3];      // куда целится (точка в мире)
    };

    enum VehicleFlags : uint8_t
    {
        VF_BLOWN   = 1 << 0,  // взорвана
        VF_SIREN   = 1 << 1,
        VF_HORN    = 1 << 2,
        VF_ENGINE  = 1 << 3,
        VF_LIGHTS  = 1 << 4,
        VF_DRIVEN  = 1 << 5,  // за рулём игрок отправителя
        VF_HANDBRAKE = 1 << 6,
    };

    struct VehiclePacket
    {
        Header   hdr;
        uint32_t seq;
        uint16_t id;          // сетевой номер машины (старший бит — чья)
        uint16_t model;
        float    pos[3];
        float    right[3];    // ориентация: правый и передний векторы матрицы
        float    fwd[3];
        float    vel[3];      // м/с
        float    turn[3];     // скорость вращения (как в игре, за кадр 50 к/с)
        float    health;
        float    steer, gas, brake;   // steer — руль водителя −1..1 (fSteer), не угол колёс
        uint8_t  col[4];
        uint8_t  flags;       // VehicleFlags
        uint8_t  wheels[4];   // состояние колёс (лопнуло)
        uint8_t  pad[3];
    };

    struct VehicleGonePacket
    {
        Header   hdr;
        uint16_t id;
        uint16_t pad;
    };

    enum ShotFlags : uint8_t { SHOT_HIT_MATE = 1 << 0 };   // попал во второго игрока
    struct ShotPacket
    {
        Header   hdr;
        uint16_t seq;
        uint8_t  weapon;
        uint8_t  flags;
        float    shooter[3];  // где стоял стрелок
        float    start[3];
        float    end[3];
        uint16_t npc;         // стрелял пед хоста (номер в общем мире), 0 — игрок
        uint16_t pad;
    };

    // Надёжные события: повторяем, пока не придёт ACK
    enum ReliableType : uint8_t
    {
        REL_DAMAGE = 1, REL_GIVE_WEAPON = 2, REL_NPC_DAMAGE = 3, REL_BUSTED = 4,
        // миссии вместе (хост ведёт)
        REL_TEXT = 5,          // текст миссии на экране (уже готовая строка игры)
        REL_CUTSCENE = 6,      // катсцена у хоста: началась / закончилась (пропущена)
        REL_MISSION_TEXT = 7,  // хост загрузил тексты миссии (нужны для субтитров катсцены)
        REL_SEAT = 8,          // хост -> друг: садись в мою машину на это место (начало миссии)
        REL_RECRUIT = 9,       // друг -> хост: позвал педа банды к себе в группу
        REL_SPECIAL = 10,      // хост -> друг: имя «особой» модели педа миссии (ячейки 290..299), с 1.5.6
        REL_SAME_VEHICLE = 11, // хост -> друг: хост в миссии сел на велик/мотоцикл без места для друга — возьми такой же, с 1.5.9
    };
    struct DamageEvent
    {
        uint8_t  type;        // REL_DAMAGE
        uint8_t  weapon;
        uint8_t  piece;       // ePedPieceTypes
        uint8_t  viaVehicle;  // удар машиной
        float    amount;
        uint16_t npc;         // урон от педа хоста (номер), 0 — от второго игрока
        uint16_t pad;
    };
    struct NpcDamageEvent     // друг ранил педа хоста
    {
        uint8_t  type;        // REL_NPC_DAMAGE
        uint8_t  weapon;
        uint8_t  piece;
        uint8_t  viaVehicle;
        uint16_t npc;
        uint16_t pad;
        float    amount;
    };
    struct BustedEvent { uint8_t type; uint8_t pad[3]; };   // REL_BUSTED: копы хоста скрутили второго CJ

    // Педы вокруг друга
    enum PedFlags : uint8_t { PDF_DEAD = 1, PDF_IN_VEHICLE = 2, PDF_DUCK = 4, PDF_AIM = 8, PDF_HIDDEN = 16 };   // HIDDEN: пед миссии уступил место другу
    struct PedEntry
    {
        uint16_t id;
        uint16_t model;
        uint8_t  pedType;
        uint8_t  flags;       // PedFlags
        uint8_t  moveState;   // 1 стоит, 4 шаг, 6 бег, 7 спринт
        uint8_t  weapon;
        uint8_t  health;
        uint8_t  seat;        // kNoSeat — не в машине
        uint16_t vehId;       // сетевой номер машины (общие машины)
        float    pos[3];
        float    heading;
        int16_t  vel[2];      // см/с
    };
    constexpr int kPedsPerPacket = 30;
    struct PedsPacket
    {
        Header   hdr;
        uint32_t seq;
        uint8_t  count;
        uint8_t  last;        // последний пакет этого кадра (список полный)
        uint16_t pad;
        PedEntry peds[kPedsPerPacket];
    };
    constexpr int kPedsHeaderSize = (int)sizeof(Header) + 8;

    struct WorldTimePacket
    {
        Header   hdr;
        uint8_t  hours, minutes;
        int16_t  oldWeather, newWeather;
        uint16_t pad;
        float    interp;
    };
    struct GiveWeaponEvent        // хост выдаёт оружие другу
    {
        uint8_t  type;            // REL_GIVE_WEAPON
        uint8_t  weapon;          // eWeaponType
        uint16_t pad;
        uint32_t ammo;
    };
    // Текст миссии: строка игры (GxtChar). Буквы < 256 — по байту, иначе по 2 байта.
    enum TextKind : uint8_t { TXT_MESSAGE = 0, TXT_MESSAGE_NOW = 1, TXT_BIG = 2, TXT_HELP = 3, TXT_HELP_CLEAR = 4 };
    constexpr int kTextMax = 236;
    struct TextEvent
    {
        uint8_t  type;            // REL_TEXT
        uint8_t  kind;            // TextKind
        uint8_t  row;             // большой текст: 0 — по центру (пройдено/провал), 1 — название миссии
        uint8_t  flags;           // подсказка: 1 — быстрая, 2 — навсегда
        uint32_t ms;
        uint16_t colour;
        uint8_t  wide;            // 1 — по 2 байта на букву
        uint8_t  len;             // байт в data
        uint8_t  data[kTextMax];
    };
    constexpr int kTextHeaderSize = 12;
    enum CutsceneOp : uint8_t { CUT_LOAD = 1, CUT_END = 2 };
    struct CutsceneEvent
    {
        uint8_t  type;            // REL_CUTSCENE
        uint8_t  op;              // CutsceneOp
        uint16_t counter;         // номер катсцены у хоста (конец раньше начала — не играть)
        char     name[8];
        char     textStem[8];     // блок текстов миссии (субтитры)
    };
    struct MissionTextEvent { uint8_t type; uint8_t pad[3]; char stem[8]; };
    struct SeatEvent        { uint8_t type; uint8_t seat; uint16_t vehId; };   // seat: 1.. — пассажир (номер места + 1)
    struct RecruitEvent     { uint8_t type; uint8_t pad; uint16_t npc; };
    struct SpecialEvent     { uint8_t type; uint8_t pad; uint16_t model; char name[24]; };   // REL_SPECIAL
    struct SameVehicleEvent { uint8_t type; uint8_t pad; uint16_t model; };                  // REL_SAME_VEHICLE

    // Метки миссии хоста (у друга — такие же на карте)
    struct BlipEntry
    {
        uint16_t key;             // номер метки у хоста
        uint8_t  kind;            // eBlipType у хоста (1 машина, 2 пед, 3 объект, 4 точка, 5 контакт, 7 пикап)
        uint8_t  display;         // 1 маркер, 2 на карте, 3 оба
        uint32_t colour;
        float    pos[3];
        uint16_t scale;
        uint8_t  flags;
        uint8_t  pad;
    };
    constexpr int kBlipsMax = 32;
    struct BlipsPacket
    {
        Header    hdr;
        uint8_t   count;
        uint8_t   pad[3];
        BlipEntry blips[kBlipsMax];
    };
    constexpr int kBlipsHeaderSize = (int)sizeof(Header) + 4;

    constexpr int kReliableMax = 250;
    struct ReliablePacket
    {
        Header   hdr;
        uint32_t seq;
        uint8_t  len;
        uint8_t  pad[3];
        uint8_t  data[kReliableMax];
    };
    constexpr int kReliableHeaderSize = (int)sizeof(Header) + 8;
    struct AckPacket
    {
        Header   hdr;
        uint32_t seq;
    };

    // Передача мира хоста (файл сохранения) — куски с подтверждением
    struct WorldReqPacket   { Header hdr; uint32_t nonce; };
    constexpr int kWorldChunkMax = 1024;   // с заголовками < 1280 (MTU Tailscale)
    struct WorldChunkPacket
    {
        Header   hdr;
        uint32_t nonce;           // номер передачи (новая просьба — новая передача)
        uint32_t total;           // размер файла
        uint32_t offset;
        uint16_t len;
        uint16_t pad;
        uint8_t  data[kWorldChunkMax];
    };
    constexpr int kWorldChunkHeaderSize = (int)sizeof(Header) + 16;
    struct WorldAckPacket   { Header hdr; uint32_t nonce; uint32_t offset; };

    // Лог друга (его Documents/SA_LOG.txt) — хосту, по кускам с подтверждением (возврат к первому неподтверждённому)
    constexpr int kLogChunkMax = 1024;
    struct LogChunkPacket
    {
        Header   hdr;
        uint32_t run;             // номер передачи (новое подключение — новая)
        uint32_t seq;             // с 0 по порядку
        uint16_t len;
        uint16_t pad;
        uint8_t  data[kLogChunkMax];
    };
    constexpr int kLogChunkHeaderSize = (int)sizeof(Header) + 12;
    struct LogAckPacket     { Header hdr; uint32_t run; uint32_t seq; };

    // Список модов («a|имя|размер», «c|имя|размер», …) — частями
    constexpr int kModsChunkMax = 1000;
    struct ModsPacket
    {
        Header   hdr;
        uint32_t hash;            // весь список (новый — значит, изменился)
        uint16_t part, parts;
        uint16_t len, pad;
        uint8_t  data[kModsChunkMax];
    };
    constexpr int kModsHeaderSize = (int)sizeof(Header) + 12;

    // Передача модов хост -> друг (ТЗ 1.1 §8а): куски по порядку с подтверждением, как лог друга
    constexpr int kModFileReqMax = 400;
    struct ModFileReqPacket
    {
        Header   hdr;
        uint32_t req;             // номер передачи
        uint32_t listHash;        // по какому списку хоста просят (другой — хост пришлёт новый список)
        uint16_t count, pad;
        uint16_t idx[kModFileReqMax];
    };
    constexpr int kModFileReqHeaderSize = (int)sizeof(Header) + 12;
    constexpr int kModFileChunkMax = 1000;
    struct ModFileChunkPacket
    {
        Header   hdr;
        uint32_t req, seq;
        uint16_t fileNo;          // номер в запросе
        uint16_t flags;           // 1 — последний кусок файла, 2 — хост не смог прочитать файл
        uint32_t offset, total;
        uint16_t len, pad;
        uint8_t  data[kModFileChunkMax];
    };
    constexpr int kModFileChunkHeaderSize = (int)sizeof(Header) + 24;
    struct ModFileAckPacket { Header hdr; uint32_t req; uint32_t seq; };

    // Лобби: хост выбирает сохранение и жмёт «Запуск» — launchId меняется, друг загружается
    // LOBBY_NEW: хост начинает новую игру — друг тоже начинает новую (мир не передаётся), сюжет ведёт хост
    enum LobbySource : uint8_t { LOBBY_NONE = 0, LOBBY_CURRENT = 1, LOBBY_FILE = 2, LOBBY_NEW = 3 };
    struct LobbyPacket
    {
        Header   hdr;
        uint32_t launchId;        // 0 — ещё не запускали
        uint8_t  source;          // LobbySource
        uint8_t  hostInGame;      // хост уже в игре
        uint8_t  hostLoading;     // хост сам загружается
        uint8_t  pad;
        char     saveName[64];    // что выбрано / запущено (UTF-8)
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

    constexpr int kModMaxBytes = 1100;
    struct ModPacket              // шлётся заголовок + len байт
    {
        Header   hdr;
        uint8_t  channel;
        uint8_t  pad;
        uint16_t len;
        uint8_t  data[kModMaxBytes];
    };
    constexpr int kModHeaderSize = (int)sizeof(Header) + 4;
    #pragma pack(pop)

    static_assert(sizeof(TextEvent) == kTextHeaderSize + kTextMax, "TextEvent");
    static_assert(sizeof(TextEvent) <= kReliableMax && sizeof(CutsceneEvent) <= kReliableMax, "reliable");
    static_assert(sizeof(BlipEntry) == 24, "BlipEntry");

    inline Header MakeHeader(PacketType t) { return Header{ kMagic, kProtocolVersion, (uint8_t)t, 0 }; }
}
