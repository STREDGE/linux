#pragma once
#include <stdio.h>
#include <stdarg.h>
struct Logger
{
    void Info(const char* f, ...)  { va_list a; va_start(a, f); vprintf(f, a); va_end(a); puts(""); }
    void Error(const char* f, ...) { va_list a; va_start(a, f); printf("ERR: "); vprintf(f, a); va_end(a); puts(""); }
};
extern Logger* logger;
