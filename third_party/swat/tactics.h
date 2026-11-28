#pragma once
// Тактика спецназа: «чувства» (Ч01–Ч20), «руки» (Д01–Д50, свой водитель и пилот), тактики Т001–Т158, роли Р1–Р6.
// Голова (Haiku в SA Living или правила без ИИ) выбирает тактику + параметры, Team исполняет её 10 раз в секунду,
// руки — каждый кадр. Все остальные люди — на родном ИИ игры.

#include <stdint.h>
#include <deque>
#include <string>
#include <vector>

#include "sgame.h"

namespace swat
{
    // ---------------- роли ----------------
    enum Role { R_NONE = 0, R_LEAD = 1, R_REAR = 2, R_MARKSMAN = 3, R_DRIVER = 4, R_PILOT = 5, R_MEDIC = 6 };
    const char* RoleName(int r);

    // ---------------- каталог ----------------
    enum Prog
    {
        P_FORMATION, P_BOUND, P_WALL, P_SHIELD_CAR, P_SPLIT_BLOCK, P_CLIMB, P_LISTEN, P_SPREAD, P_GATHER,
        P_TAKE_COVER, P_RETURN_FIRE, P_SUPPRESS_FLANK, P_PINCER, P_FALLBACK, P_COVER_WOUNDED, P_MEDIC, P_SHIFT,
        P_COUNTERSNIPE, P_FIRE_VEHICLE, P_HOLD_FIRE, P_CEASE_FIRE, P_TAKE_ALIVE, P_PERIMETER, P_CHECK_BODIES,
        P_SCAVENGE, P_STAGGER_RELOAD, P_FOCUS_FIRE, P_GUARD_BACK, P_HOLD_POINT, P_SUPPRESS_WINDOW, P_FLUSH,
        P_CAR_FIGHT, P_PIE, P_STACK, P_ENTER_AFTER, P_CLEAR_AREA, P_STAIRS, P_ROOF_CHECK, P_BACK_DOOR,
        P_ARREST, P_CROWD, P_BRAWL, P_CHASE_FOOT, P_INTERCEPT_FOOT, P_HANDOVER, P_ESCORT_CELL,
        P_PURSUE, P_INTERCEPT_CAR, P_PIT, P_BOX, P_ROADBLOCK, P_FELONY_STOP, P_SHOOT_TIRES, P_PULL_DRIVER,
        P_RECOVER_CAR, P_SHORTCUT, P_HELI_GUIDE, P_SEARCH, P_EVADE_RAM, P_CONVOY, P_DEPLOY, P_BLOCK_EXIT,
        P_EVAC, P_MOTO_SCOUT, P_SWAP_DRIVER,
        P_HELI_WATCH, P_HELI_GUNNER, P_HELI_ROOF_DROP, P_HELI_ROOF_PICK, P_HELI_ORBIT, P_HELI_FOLLOW, P_HELI_DECOY,
        P_HELI_LAND, P_HELI_EVADE, P_HELI_HOME,
        P_SHADOW, P_TAKE_POS, P_TAKE_ROOF, P_CHECK_CAR, P_FETCH_ARMOR, P_BRING_CAR, P_CALL_BACKUP, P_WATCH,
        P_DRIVE_ME, P_WAIT_CAR, P_GIVE_ARMOR,
        P_TALK,
        P_DEF_ALARM, P_DEF_ARM, P_DEF_GATE, P_DEF_POSTS, P_DEF_FRANK, P_DEF_WOUNDED, P_DEF_BLOCK_GATE, P_DEF_FLUSH,
        P_DEF_CHASE_GATE, P_DEF_PRISONERS, P_DEF_ALLCLEAR, P_DEF_BACKUP, P_DEF_STANDDOWN, P_DEF_STOLEN_CAR, P_DEF_JAILBREAK,
        P_DEF_HELI_UP,
        P_COUNT
    };

    struct TacticInfo
    {
        int id;                 // 1..158
        const char* group;      // «Перемещение», «Огневой контакт»…
        const char* name;       // коротко, как в ТЗ
        int prog;               // программа исполнителя
        float a, b;             // параметры программы (строй, дистанция, сторона…)
    };
    const std::vector<TacticInfo>& Catalog();
    const TacticInfo* FindTactic(int id);
    std::string CatalogText();                     // каталог одной строкой на тактику (для подсказки ИИ)

    // Действия рук (Д01–Д50) — для лога/описания
    const char* ActionName(int d);
    const char* SenseName(int c);

    // ---------------- приказ ----------------
    struct Order
    {
        int tactic = 0;                // номер тактики (0 — нет)
        int target = 0;                // пед-цель (handle)
        int targetCar = 0;             // машина-цель (handle)
        V3  point; bool hasPoint = false;
        int side = 0;                  // -1 слева, 1 справа, 0 — сам
        float dist = 0;                // дистанция/радиус (0 — по умолчанию)
        std::vector<int> holders;      // кто «держит» (handle педов)
        std::vector<int> movers;       // кто «заходит»
        std::string why;               // для лога
    };

    // ---------------- исполнитель ----------------
    struct UnitPublic
    {
        int ref = 0;
        std::string name;
        int role = R_NONE;
        std::string doing;             // «прикрывает», «за рулём», «перезаряжается»…
        float hp = 0, armour = 0;
        int ammo = 0;
        bool alive = true;
    };

    struct Event { std::string kind, text; int who = 0; uint32_t ms = 0; };

    class TeamImpl;
    class Team
    {
    public:
        explicit Team(const char* tag);
        ~Team();
        Team(const Team&) = delete;
        Team& operator=(const Team&) = delete;

        void Add(int ref, const std::string& name, int role, bool defender = false);
        void Remove(int ref);
        void Clear();
        bool Has(int ref) const;
        int  Count() const;
        void SetRole(int ref, int role);
        void SetLeader(int ref);                    // за кем идём (Артём); 0 — нет
        void SetHome(const V3& home, float radius); // оборона: «свой» участок (база)
        void SetPosts(const std::vector<V3>& posts);// размеченные позиции обороны
        void SetPlace(int id, const V3& p);         // места: 0 медпункт, 1 оружейная, 2 камера, 3 площадка, 4 КПП, 5 гараж
        void SetEnemyFilter(bool (*isEnemy)(uintptr_t ped));   // кого считать врагом сверх обычного
        void SetSay(void (*say)(int ref, const std::string& who, const std::string& text, bool urgent));
        void SetBackup(void (*call)(int kind, const V3& at));
        void SetArrest(void (*arrested)(int ref), void (*delivered)(int ref));
        void SetStars(int (*stars)(int ref));

        void Command(const Order& o);               // приказ (от головы или правил)
        void Auto(bool on);                         // без головы: правила §9 выбирают сами
        bool AutoOn() const;
        int  Current() const;                       // текущая тактика
        void Tick();                                // каждый кадр

        std::string Situation(int maxLen = 1800);   // сводка «чувств» текстом (для головы)
        bool PopEvent(Event& e);                    // события для головы: контакт, ранен, погиб, чисто, приехали…
        std::vector<UnitPublic> Units() const;
        bool InCombat() const;
        int  Threats() const;
    private:
        TeamImpl* m;
    };

    // Свой водитель и пилот: перехваты игры (ставит мод один раз). false — не найдены (тогда родной водитель).
    bool InstallDriverHooks(void* libHandle);
    void DriverFrame();                             // каждый кадр (после игры)
}
