#pragma once
// Лог SA Story: общий файл Documents/SA_LOG.txt (строки «С», вместе с SA Coop и SA Living) + logcat (тег SAStory).
// Формат строки: С ЧЧ:ММ:СС.ммм [ТЕГ] текст

#include <stdarg.h>

namespace slog
{
    void Init();
    void Line(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void LineV(const char* tag, const char* fmt, va_list ap);
    void Crash(int sig, unsigned long pc, unsigned long lr, const char* lib, unsigned long off, const char* where);
}

// Где сейчас мод (для записи о вылете). Только строковые константы.
extern const char* volatile g_where;
#define WHERE(s) (g_where = (s))

#define LOG(tag, ...) slog::Line(tag, __VA_ARGS__)
