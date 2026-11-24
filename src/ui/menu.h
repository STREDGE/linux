#pragma once
// Вкладка «Co-op» в настройках игры (через SAUtils).

class ISAUtils;

namespace menu
{
    // Создаёт вкладку. Вызывать из OnAllModsLoaded, когда SAUtils уже загружен.
    void Init(ISAUtils* sautils);
}
