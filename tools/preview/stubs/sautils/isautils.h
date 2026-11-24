#pragma once
#include <stdint.h>
typedef void (*SimpleDataFn)(void* data);
typedef void (*OnButtonPressedFn)(uintptr_t screen);
enum eTypeOfSettings : unsigned char { SetType_Controller = 0 };
enum eSettingsTabButtonLoc : unsigned char { STB_MainMenu = 0, STB_Pause, STB_StartGame, STB_Settings };
class ISAUtils { public: void AddSettingsTabButton(const char*, SimpleDataFn, eSettingsTabButtonLoc, const char*, void*) {}
    void AddButton(eTypeOfSettings, const char*, OnButtonPressedFn) {} };
