#pragma once
// Вступление: полёт на «Маврике» к базе, стрельба с борта, посадка на крышу штаба.
namespace intro
{
    void Start();
    void Frame();
    bool Active();
    bool Attached();          // Артём прикреплён к борту (модель героя не менять)
    void Abort();
}
