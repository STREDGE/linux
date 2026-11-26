#pragma once
// Клиент Claude Messages API (прямой HTTPS через Java-мост, потоковый ответ SSE).
// Всё вызывается из игрового потока; Pump() раз в кадр разбирает пришедшие строки.

#include <functional>
#include <string>
#include <vector>

#include "util/json.h"

namespace claude
{
    struct Settings
    {
        std::string key;                        // ключ игрока (НИКОГДА не писать в лог)
        std::string model  = "claude-haiku-5-5";   // старое (одна модель на всё) — теперь модель по «уровню» tier
        std::string effort = "low";
        // Уровень ИИ: 0 — быстрее всего (Haiku 5.5 везде), 1 — максимум (Sonnet 5.5; мгновенные реакции — Haiku)
        int tier = 1;
        std::string apiVersion = "2026-10-21";
        std::string url = "https://api.anthropic.com/v1/messages";
    };
    void Configure(const Settings& s);
    const Settings& Get();
    bool HasKey();
    std::string MaskedKey();                    // "sk-ant-…a1b2"

    struct ToolCall { std::string id, name; json::Value input; bool inputOk = true; };

    struct Result
    {
        bool ok = false;
        int  status = 0;
        std::string error;                      // для игрока, коротко
        std::string text;                       // весь текст ответа
        std::vector<ToolCall> tools;
        std::string stopReason;
        json::Value assistantContent;           // блоки ответа как есть (для истории диалога)
        int inTok = 0, outTok = 0, cacheRead = 0, cacheWrite = 0;
    };

    typedef std::function<void(const std::string& delta)> TextFn;  // кусок текста по мере прихода
    typedef std::function<void(Result& r)> DoneFn;

    enum Role { ROLE_TALK = 0, ROLE_JARVIS = 1, ROLE_WORLD = 2, ROLE_REACT = 3 };   // для чего запрос: от этого модель
    struct Request
    {
        int  role = ROLE_TALK;                  // разговор / Джарвис / мир (режиссёр, профили, суд)
        std::string model;                      // пусто — по уровню и роли
        json::Value system;                     // массив блоков {type:text,text,...}
        json::Value tools;                      // массив инструментов (может быть пустым)
        json::Value messages;                   // массив сообщений
        int  maxTokens = 600;
        std::string effort;                     // пусто — из настроек
        bool noThinking = false;
        bool warmOnly = false;                  // прогрев кэша: max_tokens 0, без потока, ответа нет                // без «размышления» (быстрее; Haiku 5.5 разрешает при effort low…high)
        json::Value format;                     // если задан — output_config.format (ответ по схеме)
        bool cache = true;                      // кэш подсказки (верхнеуровневый cache_control)
        int  timeoutMs = 45000;
        const char* tag = "";                   // для лога: кто спрашивает
    };

    // 0 — не отправлен (нет ключа / лимит / мост не готов)
    int  Send(const Request& req, TextFn onText, DoneFn onDone);
    void Cancel(int id);                        // ответ будет проигнорирован
    void AbortAll(const char* why);             // загрузка другой игры: всем ждущим — ошибка, ответы отбросить
    void Pump();
    int  InFlight();

    // ---- кооп: у друга нет ключа — запросы идут через хоста ----
    void HandleProxyRequest(const json::Value& m);   // хост: выполнить запрос друга своим ключом
    void HandleProxyResult(const json::Value& m);    // друг: пришёл ответ хоста
    bool ViaHost();                                  // сейчас запросы идут через хоста
    bool Available();                                // ИИ доступен (свой ключ или через хоста)

    // Расход
    struct Usage { int requests = 0, errors = 0; long long inTok = 0, outTok = 0, cacheRead = 0, cacheWrite = 0; double usd = 0; };
    Usage Today();
    Usage Total();
    double CostUsd(const Usage& u);             // примерно, по ценам каждой модели
    std::string ModelFor(int role);             // какая модель сейчас для этой роли
    void SetDailyLimit(int requests);
    bool LimitReached();
    void LoadUsage(const json::Value& v);
    json::Value SaveUsage();
}
