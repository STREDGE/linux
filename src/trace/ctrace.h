#pragma once
// Подробный лог сессии коопа («супер-лог»): всегда включён, обычный текст, один файл на запуск игры.
//   Documents/AMLModLogs/net.sacoop.mod/trace/trace_ГГГГ-ММ-ДД_ЧЧ-ММ-СС.txt  (хранятся последние 10)
// Формат строки — как у SA Living (тот же разборщик tools/trace/trace_tool.py):
//   ЧЧ:ММ:СС.ммм [ТЕГ] текст
// Запись — фоновым потоком; из игрового и сетевых потоков строки только кладутся в очередь под мьютексом.
// Пространство имён ctrace (не trace): у SA Living свой trace, имена не должны совпадать.

#include <stdarg.h>
#include <stdint.h>

namespace ctrace
{
    void Init();                                   // один раз при загрузке мода
    bool Started();
    void Line(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void LineV(const char* tag, const char* fmt, va_list ap);
    // Строка из обычного лога мода: «[ТЕГ] текст» → тег из скобок, иначе ЛОГ; ошибки — с пометкой
    void FromLog(bool error, const char* text);
    void Crash(int sig, unsigned long pc, unsigned long lr, const char* lib, unsigned long off, const char* where);
    const char* Path();
}

#define CTRACE(tag, ...) ctrace::Line(tag, __VA_ARGS__)
