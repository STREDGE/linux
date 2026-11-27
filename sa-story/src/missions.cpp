// Миссии своего сюжета. Сюжет пишет игрок — здесь пока только ПРОБНАЯ миссия (проверка движка в игре):
// реплики, машина, поездка с таймером, засада, розыск, награда, провал.
// Новая миссия сюжета = engine::Register({ номер, ключ, название, маркер, награда, модели, шаги }).

#include "engine.h"

using namespace engine;
using game::Vec;

namespace missions
{
namespace
{
    // Модели (GTA SA): машины 560 Sultan; люди 102–104 Ballas; оружие 346 Colt45, 352 Micro-SMG
    // Оружие (eWeaponType): 22 пистолет, 28 Micro-SMG
    constexpr int kSultan = 560, kBallas1 = 102, kBallas2 = 103, kBallas3 = 104, kColtModel = 346, kUziModel = 352;
    constexpr int kPistol = 22, kUzi = 28;
    constexpr int kPedBallas = 7;

    void TestMission(Builder& b)
    {
        b.Say("Голос в рации", "Проверка связи. Это пробная миссия — смотрим, как работает движок.", 4.5f, kBlue);
        b.Say("Голос в рации", "Держи ствол. Машина ждёт у выезда с Гроув-стрит.", 4.0f, kBlue);
        b.GivePlayerWeapon(kPistol, 150);
        b.Car("car", kSultan, Vec{ 2475.0f, -1660.0f, 13.3f }, 90.0f);
        b.Protect("car", "Машина уничтожена");
        b.EnterCar("car", "Садись в «Султан»");
        b.TimeLimit(150);
        b.GoTo(Vec{ 1941.0f, -1773.0f, 13.4f }, 6.0f, "Езжай на заправку в Айдлвуде", "car");
        b.TimeLimit(0);
        b.Say("Голос в рации", "Это засада! Отбивайся!", 3.0f, kRed);
        b.Enemy("b1", kPedBallas, kBallas1, Vec{ 1930.0f, -1785.0f, 13.5f }, 0.0f, kPistol, 200);
        b.Enemy("b2", kPedBallas, kBallas2, Vec{ 1955.0f, -1765.0f, 13.5f }, 90.0f, kUzi, 300);
        b.Enemy("b3", kPedBallas, kBallas3, Vec{ 1920.0f, -1765.0f, 13.5f }, 270.0f, kPistol, 200);
        b.KillEnemies("Убей всех нападавших");
        b.Wanted(2);
        b.Say("Голос в рации", "Копы услышали стрельбу. Уходи!", 3.0f, kBlue);
        b.LoseWanted("Уйди от полиции");
        b.Say("Голос в рации", "Чисто. Движок работает — ждём твой сюжет.", 4.0f, kBlue);
    }
}

void RegisterAll()
{
    Register({ -1, "ПРОБНАЯ", "ПРОБНАЯ МИССИЯ", Vec{ 2494.5f, -1666.9f, 13.34f }, 5000,
               { kSultan, kBallas1, kBallas2, kBallas3, kColtModel, kUziModel }, TestMission });
}
}
