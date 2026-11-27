#pragma once
// Исполнитель команд сценария игры (тех же, которыми игра делает свои миссии): одна команда за вызов,
// через CRunningScript::ProcessOneCommand на временном скрипте. Только игровой поток. (Копия из SA Living.)

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
    // Выполнить команду. OUT_* заполняются в args. flag — результат условия. quiet — не писать в лог (опрос каждый кадр).
    bool Run(uint16_t opcode, std::vector<Arg>& args, bool* flag = nullptr, bool quiet = false);
    bool Do(uint16_t opcode, std::vector<Arg> args);                 // действие (пишется в лог)
    bool Check(uint16_t opcode, std::vector<Arg> args);              // условие (тихо): true/false
    int  OutInt(uint16_t opcode, std::vector<Arg> args);             // одна выходная переменная-число (-1 — не вышло)
}
