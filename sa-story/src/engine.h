#pragma once
// Движок своих миссий. Миссия = список шагов (реплика, ехать к точке, сесть в машину, убить врагов, уйти от
// полиции…), всё делается командами сценария игры (как у родных миссий). Общие проверки на каждом шаге:
// игрок погиб/пойман → провал, охраняемая машина/человек погибли → провал, время вышло → провал.
// После миссии всё созданное отпускается (метки убраны, педы/машины — «больше не нужны», модели выгружаются).

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "game.h"

namespace engine
{
    enum class R { Run, Next, Fail };

    struct Step
    {
        std::string what;                      // для лога
        std::function<void()> enter;           // один раз при входе
        std::function<R()> tick;               // каждый кадр; nullptr — сразу дальше
    };

    // Цвета имён в субтитрах
    constexpr unsigned kGold = 0xF2C14E, kGreen = 0x4FD15A, kBlue = 0x5AAAFF, kRed = 0xFF4D4D, kWhite = 0xFFFFFF;

    // Сборщик шагов миссии. Имена («car», «boss»…) — свои, внутри одной миссии.
    class Builder
    {
    public:
        explicit Builder(std::vector<Step>& s) : m_s(s) {}
        void Say(const std::string& who, const std::string& text, float sec, unsigned rgb = kWhite, const std::string& voice = "");
        void Wait(float sec);
        void Objective(const std::string& text);
        void Do(const std::string& what, std::function<void()> fn);
        // появление
        void Car(const std::string& name, int model, game::Vec at, float heading, bool locked = false);
        void Ally(const std::string& name, int pedType, int model, game::Vec at, float heading, int weapon = 0, int ammo = 0);
        void Enemy(const std::string& name, int pedType, int model, game::Vec at, float heading, int weapon, int ammo, int accuracy = 40);
        void GivePlayerWeapon(int weapon, int ammo);
        void Protect(const std::string& name, const std::string& failText);    // погиб/взорвана — провал
        // цели
        void EnterCar(const std::string& car, const std::string& objective);
        void GoTo(game::Vec at, float radius, const std::string& objective, const std::string& inCar = "");   // inCar — только в этой машине
        void KillEnemies(const std::string& objective);
        void Wanted(int stars);
        void LoseWanted(const std::string& objective);
        void TimeLimit(int sec);               // таймер на экране; 0 — убрать
    private:
        std::vector<Step>& m_s;
    };

    struct Mission
    {
        int id;                     // номер в сюжете (0, 1, 2…); -1 — пробная (можно повторять)
        const char* key;            // для лога
        const char* title;          // название на экране
        game::Vec start;            // маркер старта (пешком)
        int reward;                 // деньги за прохождение
        std::vector<int> models;    // модели (машины, люди, оружие), загрузить до начала
        void (*build)(Builder&);
    };
    void Register(const Mission& m);

    // Доступ к созданному (для своих шагов Do)
    int  Car(const std::string& name);       // номер машины для команд, -1 — нет
    int  Ped(const std::string& name);       // номер педа, -1 — нет
    int  PlayerChar();                       // номер педа игрока

    void Init();
    void Frame();
    void Abort(const char* why);             // без команд игры (мир перезагружается)
    bool Busy();                             // идёт своя миссия
    void OnNewStory();                       // началась новая игра со своим сюжетом
    void BeforeSave();                       // убрать метки маркеров (иначе попадут в сохранение)
}
