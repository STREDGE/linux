#pragma once
// Лог коопа: всегда включён, ОДИН общий файл с SA Living и Tailscale (с 1.4.8): Documents/SA_LOG.txt
// (third_party/onelog). Формат строки (разборщик tools/trace/trace_tool.py):
//   К ЧЧ:ММ:СС.ммм [ТЕГ] текст      (К — кооп, И — SA Living, Т — Tailscale)
// Запись — фоновым потоком; из игрового и сетевых потоков строки только кладутся в очередь под мьютексом.
// Пространство имён ctrace (не trace): у SA Living свой trace, имена не должны совпадать.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace ctrace
{
    void Init();                                   // один раз при загрузке мода
    bool Started();
    void Line(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void LineV(const char* tag, const char* fmt, va_list ap);
    // Строка из обычного лога мода: «[ТЕГ] текст» → тег из скобок, иначе ЛОГ; ошибки — с пометкой
    void FromLog(bool error, const char* text);
    void Crash(int sig, unsigned long pc, unsigned long lr, const char* lib, unsigned long off, const char* where, unsigned long lrGame);
    const char* Path();
    // Хост: лог друга (пришёл по сети) — в свой файл, каждая строка с пометкой «Д »
    void FriendHeader(const char* nick);
    void FriendRaw(const char* data, size_t len);
}

#define CTRACE(tag, ...) ctrace::Line(tag, __VA_ARGS__)
