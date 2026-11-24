// Заглушки AML для сборки превью меню на компьютере (не для телефона).
#pragma once
#include <stdint.h>
#include <stddef.h>
struct IAML
{
    uintptr_t GetSym(void*, const char*) { return 0; }
    bool Hook(void*, void*, void**) { return true; }
};
extern IAML* aml;
#define DECL_HOOK(_ret, _name, ...) _ret (*_name)(__VA_ARGS__); _ret HookOf_##_name(__VA_ARGS__)
#define DECL_HOOKv(_name, ...) void (*_name)(__VA_ARGS__); void HookOf_##_name(__VA_ARGS__)
