#pragma once
// Возможности телефона: адрес Tailscale и буфер обмена.

#include <stddef.h>

namespace platform
{
    // Адрес этого телефона в сети Tailscale (100.64.0.0/10). false — Tailscale выключен.
    bool FindTailscaleIPv4(char* out, size_t len);

    // Текст из буфера обмена Android (через JNI). false — пусто или ошибка.
    bool ClipboardGetText(char* out, size_t len);
    bool ClipboardSetText(const char* text);

    // Выдано ли приложению разрешение (Context.checkSelfPermission). -1 — не удалось узнать.
    int HasPermission(const char* permission);

    // Показать системный запрос разрешения (Activity.requestPermissions). false — не удалось.
    bool RequestPermission(const char* permission);

    // Модель телефона (android.os.Build.MODEL) — ник по умолчанию. false — не удалось.
    bool DeviceModel(char* out, size_t len);

    // Достаёт из текста первый IPv4-адрес вида a.b.c.d. false — адреса нет.
    bool ExtractIPv4(const char* text, char* out, size_t len);
}
