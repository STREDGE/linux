#pragma once
// База S.W.A.T. в порту (ТЗ-BASE): постройки из деталей игры, КПП со шлагбаумом, вертолёты на крыше штаба, служебный
// и личный гаражи, оружейная, тир, медпункт, комнаты (сохранение/отдых), кабинет Фрэнка, камера, люди и оборона.
#include <stdint.h>
#include <string>

#include "game.h"

namespace base
{
    bool Init(void* libHandle);
    void Reset();                     // новая игра / загрузка: всё созданное — заново
    void BeforeSave();                // перед сохранением: только снять значок (удалять посреди кадра нельзя)
    void KeepWeather();               // «Готэм»: держать дождь/туман (игра сбрасывает)
    void HoldPad(bool on);            // вступление: площадка на крыше занята его вертолётом
    void Teardown();                  // игрок уехал далеко: убрать созданное базой
    void Frame();                     // каждый кадр (только в режиме S.W.A.T.)
    bool Live();                      // игрок рядом — база «живёт»
    bool Built();
    bool Inside(const game::Vec& p, float pad = 0);

    // для режима и SA Living
    int  StaffCount();
    int  StaffRef(int i);
    const char* StaffName(int i);
    int  StaffKind(int i);
    int  StaffModel(int i);
    int  StaffIndexOf(int ref);
    void ReleaseStaff(int ref);       // в отряд
    void ReturnStaff(int ref);        // из отряда
    bool AlarmOn();
    void RaiseAlarm(const char* why);
    void DeliverPrisoner(int ref);    // задержанный в камере (награда, статистика)

    // Вид базы: 0 «Обычная», 1 «Готэм»
    int  Look();
    void SetLook(int look);
    void HeliIntroLanded(int heliRef);  // вертолёт вступления сел на площадку — он и есть полицейский «Маврик» базы

    // Статистика (кабинет Фрэнка)
    void AddCall();
    void AddEarned(int usd);
}
