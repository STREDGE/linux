#pragma once
// Вывод Dear ImGui через RenderWare игры (RwIm2D), GTA SA Android 2.10 arm64.

namespace imgui_rw
{
    bool Init(void* libHandle);   // найти функции RW (без создания текстур)
    bool EnsureDeviceObjects();   // создать текстуру шрифтов (когда RW уже работает)
    void NewFrame(float dt);      // размер экрана, время кадра
    void Render();                // ImGui::Render() + вывод
    bool Ready();
}
