#pragma once
// Отряд с ИИ (ТЗ-SQUAD + ТЗ-SQUAD-TACTICS): только в режиме S.W.A.T. (SA Story, интерфейс AML «SAStorySwat»).
// До 3 напарников — сотрудники базы. Голова — Haiku 5.5 (размышление «Сверхвысокий» по умолчанию): одним запросом на весь
// отряд выбирает тактику из каталога (Т001–Т140), роли и реплики. Исполнитель тактик и руки — общая библиотека
// third_party/swat (свой водитель, пилот, укрытия, очереди…). Без интернета/ключа — правила и простые голосовые приказы.

#include <stdint.h>
#include <string>
#include <vector>

namespace squad
{
    void Init(void* libHandle);
    void Frame();                                  // каждый кадр (игровой поток)
    void Reset();                                  // новая игра / загрузка — отряд из файла сохранения появится заново
    void OnSave(const std::string& saveKey);       // игра сохранилась
    void OnLoad(const std::string& saveKey);       // игра загрузилась

    bool ModeOn();                                 // режим S.W.A.T. (SA Story)
    bool Active();                                 // режим и в отряде кто-то есть
    bool IsMember(uintptr_t ped);
    // Речь игрока: talkPed — с кем сейчас разговор (0 — ни с кем / зажал кнопку для отряда).
    // true — фраза отряду (или набор/отпуск) — разговору с человеком её не отдавать.
    bool OnSpeech(const std::string& text, uintptr_t talkPed);
    // Кнопку «Говорить» зажали без человека перед собой — слушаем для отряда
    bool CanTalk(uintptr_t ped);                 // сотрудник базы или боец отряда — с ним можно говорить
    bool ListenForSquad(uintptr_t pickedPed);

    struct Card { std::string name, role, doing; float hp = 0, armour = 0; bool alive = true, traveling = false; int eta = 0; };
    std::vector<Card> Cards();
    bool HeadThinking();
}
