#pragma once
// Мир SA Living: постоянные персонажи, отношения, события и свидетели, слухи, банды,
// полиция (дело на CJ, свои люди), телефон, деньги. Хранится в JSON-файле, решения
// «по-человечески» принимает ИИ, но числа меняет только этот код (с ограничениями).

#include <stdint.h>
#include <string>
#include <vector>

#include "game/game.h"
#include "util/json.h"

namespace world
{
    enum Faction { F_NONE = 0, F_POLICE, F_GROVE, F_BALLAS, F_VAGOS, F_AZTECAS, F_RIFA, F_DANANG, F_TRIADS, F_MAFIA, F_CIVIL, F_COUNT };
    const char* FactionName(int f);
    int FactionOfPedType(int pedType);

    // Кто «мы»: 0 — CJ (хост/одиночка), 1 — друг в коопе
    enum { WHO_CJ = 0, WHO_FRIEND = 1, WHO_COUNT = 2 };

    struct Traits { int loyalty = 50, greed = 50, courage = 50, aggression = 50, honor = 50, cunning = 50, talkative = 50, ambition = 50, suspicion = 50; };
    struct Relation
    {
        int trust = 0, respect = 0, liking = 0, fear = 0;   // -100..100 (страх 0..100)
        int debt = 0;                                        // >0 — он должен игроку $, <0 — игрок ему
        bool met = false;
        bool contact = false;                                // номер в телефоне игрока
        bool payroll = false; int weekly = 0; int paidDay = 0; // «на зарплате»
        bool informant = false;                              // сливает игроку сведения
        int  favors = 0;                                     // сколько услуг оказал
    };
    struct Note { int day = 0; std::string text; };

    struct Character
    {
        int id = 0;
        std::string name, nick;
        int pedType = 4, model = 0;
        bool female = false;
        int age = 30;
        int faction = F_CIVIL;
        int rank = 0;                    // полиция: 0 патрульный,1 сержант,2 детектив,3 капитан; банда: 0 молодой,1 боец,2 смотрящий,3 главный
        Traits tr;
        Relation rel[WHO_COUNT];
        std::vector<Note> mem;           // его память о событиях (коротко)
        std::string summary;             // сжатая история отношений
        std::string backstory, dream, fearOf, speech, role;
        bool  aiProfile = false;         // прошлое уже придумано ИИ
        game::Vec home; std::string homeZone;
        int   money = 100;
        bool  alive = true;
        int   jailUntilDay = 0;
        bool  secretlyLoyal = false;     // двойная игра: на словах друг, на деле сливает своим (знает только мир)
        bool  exposed = false;           // его банда узнала о связях с игроком
        std::string goal; int goalUntilMin = 0; int goalTarget = 0;   // долгая цель (минуты игрового времени)
        float pitch = 1.0f, rate = 1.0f; // голос
        json::Value dialog;              // последние сообщения разговора (для ИИ)
        int lastTalkDay = -1, lastTalkMin = 0;
        // в игре (не сохраняется)
        int pedRef = 0; uint32_t seenMs = 0;
        int hostId = 0;                  // кооп (мы друг): номер этого персонажа в общем мире хоста
    };

    struct Event
    {
        int id = 0, day = 0, minute = 0;
        std::string kind;                // kill, assault, shoot, steal_car, bribe, deal, betrayal, arrest, …
        std::string text;                // по-русски, коротко
        std::string zone; game::Vec pos;
        int severity = 1;                // 1 мелочь … 5 убийство копа
        int who = WHO_CJ;                // кто совершил
        int victimChar = 0;              // персонаж-жертва (0 — случайный)
        int victimFaction = F_NONE;
        std::vector<int> witnesses;      // персонажи-свидетели
        int anonWitnesses = 0;           // случайные свидетели
        bool reported = false;           // полиция знает
    };

    struct Rumor { int eventId = 0; std::string text, zone; int day = 0; int reach = 1; /* 1 район, 2 город, 3 все */ };

    struct FactionState
    {
        int respect[WHO_COUNT] = { 0, 0 };   // уважение к игрокам
        int hostility[WHO_COUNT] = { 0, 0 }; // вражда к игрокам (0..100)
        int tension = 0;                      // напряжение с Grove (война при >70)
        bool truce = false; int truceUntil = 0;
        std::string plan;                     // что задумала (от режиссёра)
    };

    struct PoliceCase
    {
        int heat = 0;                         // насколько на игрока «копают» (0..100)
        int evidence = 0;                     // улики (0..100)
        std::vector<std::string> charges;     // статьи
        bool iaInvestigation = false;         // ССБ проверяет продажных копов
        int  jailDays = 0;                    // всего отсидел
    };

    struct World
    {
        int nextId = 1;
        int day = 1;                          // игровой день мода (растёт при смене суток)
        int lastHour = -1;
        std::vector<Character> chars;
        std::vector<Event> events;
        std::vector<Rumor> rumors;
        FactionState fac[F_COUNT];
        PoliceCase pcase[WHO_COUNT];
        std::vector<std::string> news;        // «пока тебя не было» и новые события для игрока
        std::string playerNick = "CJ";
        std::string friendNick;
        int playerGang = F_GROVE;
        json::Value usage;                    // расход ИИ
        json::Value pendingBatch;             // id пакетной задачи «город живёт» (если есть)
    };

    World& W();
    void Reset();
    bool Load(const std::string& path);
    bool Save(const std::string& path);
    std::string DefaultPath();

    // ---- персонажи ----
    Character* Find(int id);
    Character* FindByPed(uintptr_t ped);
    Character& Promote(uintptr_t ped);            // сделать персонажа из педа (или найти существующего)
    void       Bind(Character& c, uintptr_t ped);
    uintptr_t  PedOf(const Character& c);
    void       Remember(Character& c, const std::string& text);
    void       ChangeRelation(Character& c, int who, int dTrust, int dRespect, int dLiking, int dFear, const char* why);
    std::string Card(const Character& c, int who);  // описание для ИИ: кто он, характер, отношения
    std::string KnownFacts(const Character& c, int who); // что он знает об игроке (только его знания)

    // ---- события, свидетели, слухи ----
    Event& AddEvent(const std::string& kind, const std::string& text, const game::Vec& pos, int severity, int who);
    void   AddWitness(Event& e, int charId);
    void   SpreadRumors();                        // раз в игровой час
    std::vector<std::string> RumorsFor(const Character& c, int max);

    // ---- время ----
    int  Day();
    int  NowMin();                                // минуты с начала мира
    void Tick();                                  // раз в кадр (смена часа/дня, платежи)

    // ---- банды ----
    void GangReactToKill(int faction, int who, int severity);
    std::string FactionSummary(int who);

    // ---- новости ----
    void News(const std::string& s);

    // Кооп: друг пересылает свои события хосту (ставит main.cpp)
    extern void (*g_forwardEvent)(const Event& e);

    // ---- помощники ----
    int  Clamp(int v, int lo, int hi);
    int  Rand(int lo, int hi);
    std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}
