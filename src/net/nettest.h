#pragma once
#include <stdint.h>

// Этап 0.5: проверка связи между двумя телефонами (например, через Tailscale).
// Хост слушает UDP-порт и отвечает на пинги, клиент шлёт пинг раз в секунду
// и пишет в лог задержку (ping) и потери пакетов.

class FLogger;

namespace net
{
    enum class Role { Host, Client };

    struct TestConfig
    {
        Role     role;
        char     hostAddress[64];
        uint16_t port;
    };

    // Запускает фоновый поток. Логирует в logcat (тег SACoop) и в файл log.
    void StartConnectionTest(const TestConfig& config, FLogger* log);
}
