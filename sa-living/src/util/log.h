#pragma once
// Лог мода: Documents/AMLModLogs/net.saliving.mod/saliving.txt (+ logcat с тегом SALiving).

void LogInfo(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Где сейчас мод (для записи о вылете). Только строковые константы.
extern const char* volatile g_where;
#define WHERE(s) (g_where = (s))

#define LOG_INFO(...)  LogInfo(__VA_ARGS__)
#define LOG_ERROR(...) LogError(__VA_ARGS__)
