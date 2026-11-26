#pragma once
// «Знания» Джарвиса без игры: оружие, машины, места, сетка карты, разбор фраз без ИИ,
// справочник команд сценария игры (поиск и подготовка команды к запуску).
// Не зависит от игры — проверяется на компьютере (tools/hosttest/jarvis_test.cpp).

#include <string>
#include <vector>

#include "jarvis/script.h"
#include "util/json.h"

namespace jbrain
{
    std::string Lower(const std::string& utf8);        // латиница и кириллица → нижний регистр, ё → е

    struct Weapon { int type; int model; const char* key; const char* title; const char* aliases; int ammo; };
    const Weapon* FindWeapon(const std::string& text); // по ключу или слову в тексте
    const Weapon* WeaponByType(int type);
    const std::vector<Weapon>& Weapons();

    int  FindVehicle(const std::string& text);         // модель (400–611) или -1
    const char* VehicleName(int model);

    bool FindPlace(const std::string& text, float& x, float& y, float& z, std::string& title);
    std::string PlaceList();

    // Сетка карты: столбцы A–L (с запада на восток), строки 1–12 (с севера на юг), квадрат 500 м;
    // подквадрат .1–.9 (3×3, как на цифровой клавиатуре телефона: 1 — северо-запад, 9 — юго-восток)
    bool ParseGrid(const std::string& text, float& x, float& y, std::string& norm);
    std::string GridOf(float x, float y);

    int  FindWeather(const std::string& text);         // номер погоды или -1
    bool ParseNumber(const std::string& text, long long& out);   // «пятьсот», «2 тысячи», «100»

    int  FindStat(const std::string& text, bool& isFloat, int& maxValue, std::string& title);

    // Разбор фразы без ИИ → готовые команды (пусто — не понял)
    struct Cmd { std::string tool; json::Value in; };
    std::vector<Cmd> ParseOffline(const std::string& text);

    // ---- справочник команд сценария (файл sa_mobile.json библиотеки Sanny Builder, скачивается на телефон) ----
    bool LoadLibrary(const std::string& jsonPath, std::string& err);
    bool LibraryLoaded();
    int  LibrarySize();
    std::string Search(const std::string& query, int max);     // список подходящих команд для ИИ

    struct Compiled { uint16_t opcode = 0; std::vector<script::Arg> args; std::vector<std::string> outNames; bool condition = false; bool risky = false; std::string name; };
    struct Context { int playerChar = -1, car = -1; std::vector<double> prev; };   // $me, $car, $1…$N (выходы прошлых команд)
    bool Compile(const std::string& name, const json::Value& args, const Context& ctx, Compiled& out, std::string& err);
}
