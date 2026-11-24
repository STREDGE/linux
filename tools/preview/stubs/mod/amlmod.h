// Заглушки AML для сборки превью меню на компьютере (не для телефона).
#pragma once
#include <stdint.h>
#include <stddef.h>
extern int g_fakePoints[14]; // 2 пальца по 0x1C байт, как массив Points в игре
struct IAML
{
    uintptr_t GetSym(void*, const char* n);
    bool Hook(void*, void*, void**) { return true; }
    uintptr_t GetLib(const char*) { return 0; }
};
extern IAML* aml;
#define DECL_HOOK(_ret, _name, ...) _ret (*_name)(__VA_ARGS__); _ret HookOf_##_name(__VA_ARGS__)
#define DECL_HOOKv(_name, ...) void (*_name)(__VA_ARGS__); void HookOf_##_name(__VA_ARGS__)
