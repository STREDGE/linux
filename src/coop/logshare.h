#pragma once
// Лог друга — в файл хоста (1.4.9, протокол 16). Игрок: «чтобы логи друга были в этом же файле, ему не пересылать».
// Друг (клиент), пока на связи, отправляет хосту свой Documents/SA_LOG.txt по кускам с подтверждением; хост дописывает
// строки в свой SA_LOG.txt с пометкой «Д » в начале. Отправляется текущий запуск игры друга и прошлый, если тот
// кончился вылетом. Связь пропала — после переподключения продолжает с первого неподтверждённого куска.

#include <stdint.h>
#include "net/session.h"

namespace logshare
{
    void SetSession(net::Session* s);
    void Frame(net::Role role, bool connected, uint64_t now, bool inGame = false);   // inGame: идёт игра — тихо (≈5 КБ/с)   // игровой кадр или меню (игровой поток)
    void OnPacket(uint8_t type, const uint8_t* data, int len);
}
