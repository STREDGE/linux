#pragma once
// Снос участка порта под базу: объекты невидимы и без столкновений, дороги выключены, прохожих нет.
#include <stdint.h>

namespace demolish
{
    bool Init(void* libHandle);
    void Reset();              // новая игра / загрузка (дороги выключить заново)
    void Frame(bool swat);     // каждый кадр
    int  Hidden();
}
