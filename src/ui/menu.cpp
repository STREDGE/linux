#include "menu.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <sautils/isautils.h>

#include <stdio.h>
#include <string.h>

#include "coop/coop.h"
#include "net/session.h"
#include "platform/android.h"
#include "settings.h"

// Тексты меню — латиница: шрифт меню игры не показывает кириллицу из UTF-8,
// а SAUtils обрезает названия до 31 символа.

namespace menu
{
namespace
{
    const char* kModes[]  = { "Off", "Solo test", "Host", "Join friend" }; // = net::Role
    const char* kOffOn[]  = { "Off", "On" };

    settings::Data g_data;

    // Короткое сообщение после нажатия кнопки, показывается в строке Status 6 секунд
    char     g_message[64] = "";
    unsigned g_messageUntilMs = 0;
    ISAUtils* g_sautils = nullptr;

    void ShowMessage(const char* fmt, const char* arg)
    {
        snprintf(g_message, sizeof(g_message), fmt, arg);
        g_messageUntilMs = g_sautils->GetCurrentMs() + 6000;
    }

    void Apply()
    {
        settings::Save(g_data);
        coop::SetConfig(g_data.net);
    }

    // ---- обработчики ----

    void OnModeChanged(int, int newValue, void*)
    {
        if(newValue < 0 || newValue > 3) return;
        g_data.net.role = (net::Role)newValue;
        Apply();
        logger->Info("Меню: режим -> %s", settings::RoleToString(g_data.net.role));
    }

    void OnFriendlyFireChanged(int, int newValue, void*)
    {
        g_data.friendlyFire = newValue != 0;
        coop::SetFriendlyFire(g_data.friendlyFire);
        settings::Save(g_data);
    }

    void OnCopyMyAddress(uintptr_t)
    {
        char ip[32];
        if(!platform::FindTailscaleIPv4(ip, sizeof(ip)))
        {
            ShowMessage("%s", "Tailscale is off - turn it on");
            return;
        }
        if(platform::ClipboardSetText(ip)) ShowMessage("Copied %s - send it to friend", ip);
        else                               ShowMessage("Could not copy %s", ip);
    }

    void OnPasteHostAddress(uintptr_t)
    {
        char text[256], ip[32];
        if(!platform::ClipboardGetText(text, sizeof(text)))
        {
            ShowMessage("%s", "Clipboard is empty");
            return;
        }
        if(!platform::ExtractIPv4(text, ip, sizeof(ip)))
        {
            ShowMessage("%s", "No address in clipboard");
            return;
        }
        strncpy(g_data.net.hostAddress, ip, sizeof(g_data.net.hostAddress) - 1);
        Apply();
        ShowMessage("Host address set: %s", ip);
        logger->Info("Меню: адрес хоста -> %s", ip);
    }

    // ---- строки с текстом (слайдеры, у которых вместо числа — наш текст) ----

    const char* DrawStatus(int, void*)
    {
        static char buf[96];
        if(g_messageUntilMs && g_sautils->GetCurrentMs() < g_messageUntilMs)
            return g_message;
        coop::StatusText(buf, sizeof(buf));
        return buf;
    }

    const char* DrawMyAddress(int, void*)
    {
        static char buf[48];
        if(!platform::FindTailscaleIPv4(buf, sizeof(buf)))
            snprintf(buf, sizeof(buf), "Tailscale is off");
        return buf;
    }

    const char* DrawHostAddress(int, void*)
    {
        return g_data.net.hostAddress;
    }

}

void Init(ISAUtils* sau)
{
    g_sautils = sau;
    g_data = settings::Load();

    eTypeOfSettings tab = sau->AddSettingsTab("Co-op");

    sau->AddClickableItem(tab, "Mode", (int)g_data.net.role, 0, 3, kModes, OnModeChanged, nullptr);
    // Строки-надписи: ползунок без обработчика, вместо числа — наш текст.
    // (Сдвиг ползунка ни на что не влияет. GetSettingValuePointer в SAUtils 1.6
    // возвращает не тот пункт, поэтому сбрасывать его не пытаемся.)
    sau->AddSliderItem(tab, "Status", 0, 0, 1, nullptr, DrawStatus, nullptr);

    sau->AddSliderItem(tab, "My address (host)", 0, 0, 1, nullptr, DrawMyAddress, nullptr);
    sau->AddButton(tab, "Copy my address", OnCopyMyAddress);

    sau->AddSliderItem(tab, "Host address (join)", 0, 0, 1, nullptr, DrawHostAddress, nullptr);
    sau->AddButton(tab, "Paste host address", OnPasteHostAddress);

    sau->AddClickableItem(tab, "Friendly fire", g_data.friendlyFire ? 1 : 0, 0, 1, kOffOn, OnFriendlyFireChanged, nullptr);

    logger->Info("Меню Co-op добавлено в настройки игры");
}

} // namespace menu
