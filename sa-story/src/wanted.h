#pragma once
// Розыск NPC 1–6 звёзд «как полиция против CJ», рация и подмога, вызовы (кабинет Фрэнка), задержание игроком,
// доставка в камеру базы (ТЗ-SWAT-MODE §2, исходное ТЗ §9).
namespace wanted
{
    void Init();
    void Reset();                       // новая игра / загрузка
    void Frame();                       // каждый кадр (режим S.W.A.T.)

    int  Stars(int ref);
    void SetStars(int ref, int stars, const char* why);
    // Подмога по рации: 0 полиция, 1 S.W.A.T., 2 FBI, 3 армия, 4 вертолёт, 5 скорая. Появляется из-за угла, не с базы.
    void CallBackup(int kind, float x, float y, float z, const char* why);
    bool StartCall();                   // «Взять вызов» (false — прошлый не закрыт)
    void DeliverAtCell();               // игрок в метке камеры — сдать задержанных
    void Arrested(int ref);             // задержан отрядом (SA Living)
    void Delivered(int ref);            // доставлен в камеру отрядом
    void Surrendered(int ref);
    int  Custody();                     // сколько задержанных ведёт игрок/отряд
    void BeforeSave();
}
