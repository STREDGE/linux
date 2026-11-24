#pragma once
typedef void (*SimpleDataFn)(void* data);
enum eSettingsTabButtonLoc : unsigned char { STB_MainMenu = 0, STB_Pause, STB_StartGame, STB_Settings };
class ISAUtils { public: void AddSettingsTabButton(const char*, SimpleDataFn, eSettingsTabButtonLoc, const char*, void*) {} };
