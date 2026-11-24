#pragma once
// Своё меню Co-op поверх игры (рисуется через ImGui + RenderWare).
// Открывается кнопкой CO-OP в главном меню и в меню паузы (SAUtils).

class FLogger;
class ISAUtils;

namespace overlay
{
    bool Init(void* libHandle, FLogger* log);   // из OnModLoad
    void AttachSAUtils(ISAUtils* sautils);      // из OnAllModsLoaded: кнопки CO-OP
    void Open();
    bool IsOpen();
}
