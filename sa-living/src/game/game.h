#pragma once
// Доступ к libGTASA 2.10 arm64: педы, задачи, оружие, появление, деньги, розыск, время.
// Адреса — по экспортированным символам (aml->GetSym), смещения полей — как в SA Coop
// и проекте Helicover Brain (проверены на этой версии игры). Только игровой поток!

#include <stdint.h>
#include <functional>
#include <string>

namespace game
{
    struct Vec { float x = 0, y = 0, z = 0; };
    float Dist(const Vec& a, const Vec& b);
    float Dist2D(const Vec& a, const Vec& b);

    bool Init(void* libHandle);
    bool Ready();

    // ---- игрок ----
    uintptr_t Player();                     // CPlayerPed* или 0
    int  Money();
    void AddMoney(int delta);
    int  Wanted();
    void SetWanted(int stars);
    bool PlayerInVehicle();
    uintptr_t PlayerVehicle();
    void LockPlayer(bool lock);             // DisablePlayerControls пульта 0
    uintptr_t Player2();                    // второй CJ (друг в коопе) или 0
    float MaxHealth(uintptr_t ped);         // 100…176 у игрока
    float Armour(uintptr_t ped);
    bool  Waypoint(Vec& out);               // метка игрока на карте (false — не стоит)

    // ---- педы ----
    // Пед-ссылка (handle пула): устойчива, пока пед жив; 0 — нет
    int       Ref(uintptr_t ped);
    uintptr_t FromRef(int ref);
    void ForEachPed(const std::function<void(uintptr_t ped)>& fn);
    Vec   Pos(uintptr_t ent);
    float Heading(uintptr_t ent);
    int   Model(uintptr_t ent);
    int   PedType(uintptr_t ped);
    float Health(uintptr_t ped);
    bool  Alive(uintptr_t ped);
    bool  IsPlayer(uintptr_t ped);
    uintptr_t Vehicle(uintptr_t ped);       // машина, в которой сидит (или 0)
    bool  OnScreen(uintptr_t ent);
    void  SetHeading(uintptr_t ped, float h);
    void  Teleport(uintptr_t ped, const Vec& to);
    uintptr_t NearestPed(const Vec& at, float radius, const std::function<bool(uintptr_t)>& filter);
    uintptr_t NearestVehicle(const Vec& at, float radius);
    void ForEachVehicle(const std::function<void(uintptr_t veh)>& fn);
    uintptr_t VehDriver(uintptr_t veh);            // CVehicle::pDriver (+0x5A0) или 0
    int  VehFreeSeats(uintptr_t veh);              // свободных пассажирских мест (max - занято)
    int  FreePassengerSeat(uintptr_t veh, int skip = -1);   // первое свободное место пассажира (0..), -1 — нет
    bool IsPoliceCar(int model);                   // патрульные машины, в которые можно посадить задержанного
    // Ближайшая патрульная машина: без водителя или за рулём коп, есть 2 свободных места (коп + задержанный)
    uintptr_t NearestPoliceCar(const Vec& at, float radius);

    // Типы педов (ePedType)
    enum : int { PT_PLAYER1 = 0, PT_PLAYER2 = 1, PT_CIVMALE = 4, PT_CIVFEMALE = 5, PT_COP = 6,
                 PT_BALLAS = 7, PT_GROVE = 8, PT_VAGOS = 9, PT_RIFA = 10, PT_DANANG = 11, PT_MAFIA = 12,
                 PT_TRIAD = 13, PT_AZTECA = 14, PT_GANG9 = 15, PT_GANG10 = 16, PT_DEALER = 17, PT_MEDIC = 18,
                 PT_FIRE = 19, PT_CRIMINAL = 20, PT_BUM = 21, PT_PROSTITUTE = 22, PT_SPECIAL = 23, PT_MISSION1 = 24 };

    // ---- задачи (основной слот) ----
    bool TaskGoTo(uintptr_t ped, const Vec& to, int moveState /*4 шаг,6 бег,7 спринт*/, float radius = 1.0f);
    bool TaskFollow(uintptr_t ped, uintptr_t target);
    bool TaskFlee(uintptr_t ped, uintptr_t from, float safeDist = 40.0f, int ms = 20000);
    bool TaskAttack(uintptr_t ped, uintptr_t target);
    bool TaskStand(uintptr_t ped, int ms);
    bool TaskTurnTo(uintptr_t ped, uintptr_t target);
    bool TaskWander(uintptr_t ped);
    bool TaskEnterCar(uintptr_t ped, uintptr_t veh, bool driver, int seat = 0);
    bool TaskLeaveCar(uintptr_t ped);
    bool TaskDriveTo(uintptr_t ped, uintptr_t veh, const Vec& to, float speed);
    bool TaskPhone(uintptr_t ped, int ms);
    bool TaskArrest(uintptr_t cop, uintptr_t target);
    bool TaskAimAt(uintptr_t ped, uintptr_t target, int ms);
    // Анимация по имени из набора (блока). loop — повторять ms миллисекунд. false — набор ещё грузится.
    bool TaskAnim(uintptr_t ped, const char* anim, const char* block, bool loop, int ms, bool holdLast = false);
    void ClearTask(uintptr_t ped);
    void LoadScene(const Vec& at);           // подгрузить мир вокруг точки (перед дальним переносом — иначе пустота)
    bool IsScriptPed(uintptr_t ped);         // пед миссии/скрипта игры (не наш) — задачи ему не давать
    int  ActiveTaskType(uintptr_t ped);     // тип задачи в основном слоте (-1 нет)

    // ---- оружие ----
    void GiveWeapon(uintptr_t ped, int weaponType, int ammo);
    void SetCurrentWeapon(uintptr_t ped, int weaponType);
    void ClearWeapons(uintptr_t ped);

    // ---- появление ----
    uintptr_t SpawnPed(int pedType, int model, const Vec& at);   // держится (как персонаж миссии)
    void      DeletePed(uintptr_t ped);
    void      KeepPed(uintptr_t ped, bool keep);                // 2 — миссия (не удалять), 1 — обычный

    // ---- мир ----
    int  Hour();
    int  Minute();
    int  Weather();
    std::string ZoneName();                 // район, где стоит игрок (из интерфейса игры)
    bool CutsceneRunning();
    // читы игры (CCheat): номер 0–110 по имени из таблицы игры; переключатель — чит без функции (флаг m_aCheatsActive)
    int  CheatCount();                      // 111 или 0, если таблиц нет
    const char* CheatLabel(int i);          // «FLYINGCARS», «JETPACK»…
    bool CheatIsToggle(int i);
    bool CheatActive(int i);
    void CheatSetActive(int i, bool on);
    bool CheatCall(int i);                  // вызвать функцию чита (как ввод кода); false — у чита нет функции
    void CheatMarkUsed();
    int  CheatFromPcCode(const char* code); // код с ПК (HESOYAM, AEZAKMI…) → номер чита, -1 — не нашёлся
    bool SkipCutscene();                    // пропустить идущую заставку (как тап по экрану); false — заставки нет
    uintptr_t MissionCodeBase();            // где в памяти лежит код текущей миссии (ScriptSpace + MainScriptSize)
    void ForEachScript(const std::function<void(uintptr_t script)>& fn);   // все активные скрипты сценария (CRunningScript*)
    bool GamePaused();
    uint32_t TimeMs();                      // CTimer::m_snTimeInMilliseconds

    bool  WorldToScreen(const Vec& w, float& sx, float& sy);    // false — за камерой / вне экрана
    float GroundZ(float x, float y, float z);
    bool  OnMission();
    uintptr_t PlayerTarget();               // в кого целится игрок (или 0)

    // Сюжетный персонаж: ключ («sweet», «ryder», «cesar»…) или пусто. Особые персонажи миссий
    // (модели 290–299) узнаются по имени, с которым игра их загрузила (CStreaming::RequestSpecialChar).
    std::string StoryKey(uintptr_t ped);

    const char* PedTypeName(int pedType);   // «прохожий», «Ballas»…
    bool IsGang(int pedType);
    bool IsFemaleType(int pedType);

    // ---- для подробного лога (только чтение) ----
    float Speed(uintptr_t ent);                 // м/с (CPhysical::m_vecMoveSpeed)
    float VehHealth(uintptr_t veh);             // 1000 — целая, <250 горит
    int   VehMaxPassengers(uintptr_t veh);
    int   AreaCode(uintptr_t ent);              // 0 — улица, иначе интерьер
    int   PedState(uintptr_t ped);              // ePedState (1 стоит, 50 в машине, 54/55 мёртв, 63 арестован)
    bool  CurWeapon(uintptr_t ped, int& type, int& ammo);
    bool  PadLocked();                          // управление игрока заблокировано
    bool  CamFading();                          // экран в затемнении/проявлении
    void  PoolUse(int& peds, int& pedMax, int& cars, int& carMax);
    int   VehRef(uintptr_t veh);
    int   SimplestTaskType(uintptr_t ped);      // что пед делает «телом» сейчас (простейшая активная задача)
    uintptr_t LockOn(uintptr_t ped);            // в кого целится (захват прицела) или 0
    uintptr_t FightTarget(uintptr_t ped);       // кого пед бьёт/убивает/держит на прицеле (цель из его задач) или 0
    bool  IsPedEntity(uintptr_t ent);           // сущность — пед
    uintptr_t LastDamager(uintptr_t ped, int& weapon, uint32_t& timeMs);   // кто последний ранил
    std::string EntName(uintptr_t ent);         // «P#123», «V#45», «игрок», «объект»
    bool  Camera(Vec& pos, Vec& front);         // камера игры: где стоит и куда смотрит
}
