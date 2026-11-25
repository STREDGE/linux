#pragma once
// Интерфейс SA Living: кнопки «Говорить» и «Телефон», субтитры, уведомления, телефон,
// сцены суда/тюрьмы и меню «ИИ» (кнопка в главном меню и паузе через SAUtils).

class ISAUtils;

namespace hud
{
    bool Init(void* libHandle);
    void AttachSAUtils(ISAUtils* sau);
    void OpenMenu();
}
