#pragma once
// Связь с SA Coop: надёжные сообщения (JSON) между хостом и другом поверх канала коопа.

#include "util/json.h"

namespace coopnet
{
    enum Role { NONE = 0, HOST = 1, CLIENT = 2 };
    void  Init();                  // найти интерфейс SA Coop (после загрузки всех модов)
    void  Frame();
    Role  GetRole();               // кооп выключен / мы хост / мы друг
    bool  Connected();
    bool  Send(const json::Value& msg);
    const char* PeerNick();
}
