#pragma once
// Встроенный Tailscale: своя сеть игры без отдельного приложения и без VPN.
// Библиотека (tsbridge/, Go) вшита в мод сжатой; при первом включении распаковывается
// в личную папку игры и загружается. В одиночной игре не трогается вовсе.
//
// Хост «открывает точку»: узел sacoop-<ID> в аккаунте Tailscale, UDP-порт 7777.
// Друг вводит ID → находим узел среди устройств аккаунта → мост на 127.0.0.1 ↔ хост.

#include <stddef.h>

namespace ts
{
    enum class State { Off, Loading, Starting, NeedsLogin, NeedsApproval, Running, Stopped, Error };

    struct Status
    {
        State state = State::Off;
        char  loginUrl[512] = "";   // NeedsLogin: открыть в браузере
        char  ip[48] = "";          // свой адрес в сети Tailscale
        char  error[256] = "";
    };

    // Включить (один раз за запуск игры; повтор — ничего). Распаковка — в фоне.
    // authKey — ключ из кода приглашения: вход без аккаунта и браузера (пусто — обычный вход).
    void   Start(const char* myId, const char* authKey = nullptr);
    // Войти по ключу, если Tailscale уже включён без него и ждёт входа
    void   UseKey(const char* authKey);
    Status GetStatus();
    const char* StateText(State s);   // по-русски, для меню и лога

    // Найти точку хоста по ID: 1 — в сети (ipOut — адрес), 2 — есть, но выключена, 0 — нет, -1 — сеть не готова
    int  FindHost(const char* id, char* ipOut, size_t len);
    // Мост: порт на 127.0.0.1 для игры (>0), 0 — сеть ещё не готова, -1 — ошибка
    int  OpenBridge(int tsPort, int gamePort);
    void SetPeer(const char* ip, int port);
    void CloseBridge();
    bool Logout();

    // Своё имя в сети: «sacoop-<id>»
    void NodeName(const char* id, char* out, size_t len);

    // Куда писать шаги запуска (sacoop.txt + подробный лог). Строки уже с «[TAILSCALE] ». Зовётся из фонового потока.
    void SetLogSink(void (*fn)(bool error, const char* line));
}
