#pragma once
// Исполнитель команд сценария игры (тех же, которыми игра делает миссии): одна команда за вызов,
// через CRunningScript::ProcessOneCommand на временном скрипте. Только игровой поток.

#include <stdint.h>
#include <string>
#include <vector>

namespace script
{
    struct Arg
    {
        enum Type { INT, FLOAT, STR, OUT_INT, OUT_FLOAT } type = INT;
        int i = 0; float f = 0; std::string s;
        static Arg I(int v) { Arg a; a.type = INT; a.i = v; return a; }
        static Arg F(float v) { Arg a; a.type = FLOAT; a.f = v; return a; }
        static Arg S(const std::string& v) { Arg a; a.type = STR; a.s = v; return a; }
        static Arg OI() { Arg a; a.type = OUT_INT; return a; }
        static Arg OF() { Arg a; a.type = OUT_FLOAT; return a; }
    };

    bool Init(void* libHandle);
    bool Ready();
    // Выполнить команду. Выходные параметры (OUT_*) заполняются в args. flag — результат условия.
    bool Run(uint16_t opcode, std::vector<Arg>& args, bool* flag = nullptr);
    // То же, но с защитой: если команда уронит игру (обращение не туда), ловим сигнал и возвращаемся.
    // Для «свободных» команд Джарвиса (их параметры придумывает ИИ). crashed=true — команда упала.
    bool RunGuarded(uint16_t opcode, std::vector<Arg>& args, bool* flag, bool& crashed);
    // Коротко: без выходных
    bool Do(uint16_t opcode, std::vector<Arg> args);

    int  PlayerChar();                 // номер педа игрока для команд (Char)
    int  CarHandle(uintptr_t veh);     // номер машины для команд (Car), -1 — нет
}
