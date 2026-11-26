#pragma once
// Разговор с педами: кнопка «Говорить» → распознавание речи → Claude → голос и действия.

#include <stdint.h>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "ai/claude.h"
#include "util/json.h"
#include "world/world.h"

namespace talk
{
    const char* WorldRules();
    const json::Value& Tools();

    void Init();
    void Frame();                         // раз в кадр (игровой поток)

    // Кнопка «Говорить»
    void PttDown();
    void PttUp();
    bool Listening();
    bool Recognizing();                   // запись ушла на распознавание
    const std::string& Partial();
    // Разовая запись «не педу» (Джарвис, проверка микрофона): текст придёт в sink (пусто — не расслышал)
    void CaptureDown(std::function<void(const std::string&)> sink);
    void CaptureUp();
    bool Capturing();
    // Голос персонажа: выбрать, если ещё нет (avoid — не совпадать с этими голосами)
    void EnsureVoice(world::Character& c, const std::vector<std::string>& avoid = {});

    // Состояние разговора
    bool InConversation();
    bool Thinking();                      // ждём ответ ИИ
    bool PhoneMode();
    world::Character* Current();
    uintptr_t CurrentPed();
    void EndConversation();
    void StartPhoneCall(int charId);

    // Деньги: игрок протягивает собеседнику (взятка/плата)
    void OfferMoney(int amount);
    int  PendingDemand();                 // собеседник требует $ (0 — нет)
    void PayDemand();

    // Реакция персонажа на событие без слов игрока (инициатива). Возвращает false, если не отправлено.
    bool React(world::Character& c, int who, const std::string& eventText, bool important);
    // На CJ напал кто-то посреди разговора (не сам собеседник) — собеседник реагирует (crew)
    void OnPlayerAttacked(uintptr_t attacker, const std::string& who, int weapon);

    // Субтитры
    struct Sub { std::string who, text; double until; bool player; int charId; double from = 0; };   // from — когда показать (длинная речь — по частям)
    const std::deque<Sub>& Subs();
    void Say(const std::string& who, const std::string& text, bool player, int charId, float pitch, float rate, bool voice);

    // Уведомления для HUD
    void Notify(const std::string& s);
    bool PopNotify(std::string& out);

    // ---- кооп ----
    // Хост: обработать разговор друга (без его педа). Ответ — через link.
    void HandleFriendTalk(const json::Value& req);
    // Друг: получить ответ хоста и исполнить физические действия у себя
    void HandleHostReply(const json::Value& rep);
    // Хост: действие телом из разговора друга с копией педа — выполнить на настоящем педе
    void HandleFriendAct(const json::Value& m);
    // Оба: реплика со второго телефона (второй CJ рядом слышит разговор)
    void HandleHear(const json::Value& m);
    // Хост: друг навёл ствол на копию педа — рефлекс настоящего педа
    void HandleFriendAimed(const json::Value& m);
    // Сказать вслух для всех рядом: мне — если я рядом с src, второму CJ — если он рядом (кооп)
    void SayNear(world::Character* c, const std::string& who, const std::string& text, uintptr_t src);
    // У друга: пед — копия педа хоста (SA Coop) — тело ему двигает хост (RemoteBody)
    bool CoopCopy(uintptr_t ped);
    // Действие телом педа: у друга с копией — отправить хосту (true), иначе false (делай сам)
    bool RemoteBody(uintptr_t ped, const std::string& kind, const json::Value& args);

    // Исполнение действий, требующих педа и денег игрока (локально у того, кто говорил)
    std::string ExecutePhysical(world::Character* c, uintptr_t ped, int who, const std::string& name, const json::Value& in);

    double Now();                         // секунды (монотонные)

    // Куда отдавать распознанную речь вместо собеседника (суд и т.п.); nullptr — обычный разговор
    void SetSpeechSink(std::function<void(const std::string&)> fn);
}
