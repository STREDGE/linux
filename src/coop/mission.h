#pragma once
// Миссии вместе. Ведёт хост (один сюжет на двоих):
//  * у друга маркеры миссий спрятаны и сами миссии не запускаются (стоит на маркере — ничего не будет);
//  * катсцена у хоста → у друга та же катсцена (по имени), пропускает её хост; после — друг рядом с хостом;
//  * тексты миссии (задания, подсказки, «Миссия пройдена» / «Провал») — у друга те же;
//  * метки миссии на карте (куда ехать, цели) — у друга те же;
//  * миссия началась в машине → друга сажают на свободное место; мест нет — один пед миссии
//    прячется (остаётся в машине невидимым, скрипт не ломается), друг садится на его место;
//    пед возвращается, когда друг вышел или началась катсцена;
//  * друг зовёт педа банды в группу → у хоста этот пед идёт за вторым CJ.
// Без коопа ничего не делает (обычная одиночная игра).

#include <stdint.h>

#include "net/protocol.h"
#include "net/session.h"

class FLogger;

namespace coop { namespace mission {

    bool Install(void* libHandle, FLogger* log);
    void SetSession(net::Session* s);

    // Каждый игровой кадр (после crowd::Frame)
    void Frame(net::Role role, bool connected, uintptr_t local, uintptr_t remote,
               const net::PlayerStatePacket* rs, uint64_t now);
    void OnPacket(uint8_t type, const uint8_t* data, int len);   // PKT_BLIPS и REL_TEXT … REL_RECRUIT
    bool HandlesReliable(uint8_t relType);
    void Reset(const char* why);
    // Друг начинает новую игру по команде хоста: миссии (вступление) не запускать с первого кадра,
    // даже пока свой CJ ещё не появился и обычная проверка «друг на связи» не сработала
    void ForceClientBlock(bool on);

    bool ClientCutscene();              // друг смотрит катсцену хоста (coop не двигает своего CJ)
    bool IsHiddenPed(uintptr_t ped);    // хост: пед миссии уступил место другу
}}
