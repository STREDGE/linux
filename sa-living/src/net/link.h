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
    const char* MyNick();
    // SA Coop 1.1+ (API v2): один микрофон и одна кнопка «Говорить» на оба мода
    bool  MicShared();             // на связи, микрофон SA Coop работает — говорим через него (друг слышит)
    void  SetTalk(bool on);        // «говорить другу» (голосовой чат SA Coop)
}
