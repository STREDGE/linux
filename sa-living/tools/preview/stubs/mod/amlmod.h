// Заглушки AML для превью SA Living на компьютере (не для телефона).
#pragma once
#include <stdint.h>
#include <stddef.h>
extern int g_fakePoints[14];
struct IAML
{
    uintptr_t GetSym(void*, const char* n);
    bool Hook(void*, void*, void**) { return true; }
    uintptr_t GetLib(const char*) { return 0; }
};
extern IAML* aml;
#define SDECL_HOOK(_ret, _name, ...) static _ret (*_name)(__VA_ARGS__); static _ret HookOf_##_name(__VA_ARGS__)
#define SDECL_HOOKv(_name, ...) static void (*_name)(__VA_ARGS__); static void HookOf_##_name(__VA_ARGS__)
#define SDECL_HOOKb(_name, ...) static bool (*_name)(__VA_ARGS__); static bool HookOf_##_name(__VA_ARGS__)
