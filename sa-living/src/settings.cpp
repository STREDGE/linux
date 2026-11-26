#include "settings.h"

#include <mod/amlmod.h>
#include <mod/config.h>

namespace settings
{
static Data g_d;
Data& Get() { return g_d; }

static std::string Str(const char* key, const char* def, const char* sec)
{
    char buf[512]; cfg->GetString(buf, sizeof(buf), key, def, sec); return buf;
}

void Load()
{
    Data d;
    d.enabled    = cfg->GetBool("Enabled", true, "General");
    d.key        = Str("ApiKey", "", "AI");
    d.model      = Str("Model", "claude-haiku-5-5", "AI");
    d.effort     = Str("Effort", "low", "AI");
    d.voices     = cfg->GetBool("Voices", true, "AI");
    d.subtitles  = cfg->GetBool("Subtitles", true, "AI");
    d.lang       = Str("Language", "ru-RU", "AI");
    d.initiative = cfg->GetInt("Initiative", 1, "AI");
    d.dailyLimit = cfg->GetInt("DailyLimit", 400, "AI");
    d.shareKeyWithFriend = cfg->GetBool("FriendUsesMyKey", true, "AI");
    d.director   = cfg->GetBool("Director", true, "AI");
    d.realArrest = cfg->GetBool("RealisticArrest", true, "Realism");
    d.witnesses  = cfg->GetBool("WantedByWitnesses", true, "Realism");
    d.talkX  = cfg->GetInt("TalkX", 8000, "Layout") / 10000.0f;  d.talkY  = cfg->GetInt("TalkY", 4000, "Layout") / 10000.0f;
    d.talkS  = cfg->GetInt("TalkScale", 100, "Layout") / 100.0f;
    d.phoneX = cfg->GetInt("PhoneX", 8000, "Layout") / 10000.0f; d.phoneY = cfg->GetInt("PhoneY", 2200, "Layout") / 10000.0f;
    d.phoneS = cfg->GetInt("PhoneScale", 100, "Layout") / 100.0f;
    d.engine     = cfg->GetInt("SpeechEngine", 0, "AI");
    d.voiceLoud  = cfg->GetInt("VoiceLoudness", 2, "AI");
    d.forgetDays = cfg->GetInt("ForgetDays", 7, "AI");
    d.jarvis        = cfg->GetBool("Enabled", true, "Jarvis");
    d.jarvisButton  = cfg->GetBool("Button", true, "Jarvis");
    d.jarvisVoice   = Str("Voice", "", "Jarvis");
    d.jarvisPitch   = cfg->GetInt("Pitch", 100, "Jarvis") / 100.0f;
    d.jarvisSpeed   = cfg->GetInt("Speed", 105, "Jarvis") / 100.0f;
    d.jarvisAddress = Str("Address", "сэр", "Jarvis");
    d.jarvisConfirm = cfg->GetInt("Confirm", 0, "Jarvis");
    d.jarvisFriend  = cfg->GetBool("FriendAllowed", false, "Jarvis");
    d.jarvisX = cfg->GetInt("X", 8000, "Jarvis") / 10000.0f; d.jarvisY = cfg->GetInt("Y", 5800, "Jarvis") / 10000.0f;
    d.jarvisS = cfg->GetInt("Scale", 100, "Jarvis") / 100.0f;
    g_d = d;
    cfg->Save();
}

void Save()
{
    const Data& d = g_d;
    ConfigEntry* e;
    #define SETB(k, v, s) e = cfg->Bind(k, false, s); e->SetBool(v); delete e
    #define SETI(k, v, s) e = cfg->Bind(k, 0, s); e->SetInt(v); delete e
    #define SETS(k, v, s) e = cfg->Bind(k, "", s); e->SetString((v).c_str()); delete e
    SETB("Enabled", d.enabled, "General");
    SETS("ApiKey", d.key, "AI"); SETS("Model", d.model, "AI"); SETS("Effort", d.effort, "AI");
    SETI("VoiceLoudness", d.voiceLoud, "AI");
    SETB("Voices", d.voices, "AI"); SETB("Subtitles", d.subtitles, "AI"); SETS("Language", d.lang, "AI");
    SETI("Initiative", d.initiative, "AI"); SETI("DailyLimit", d.dailyLimit, "AI");
    SETB("FriendUsesMyKey", d.shareKeyWithFriend, "AI"); SETB("Director", d.director, "AI");
    SETB("RealisticArrest", d.realArrest, "Realism"); SETB("WantedByWitnesses", d.witnesses, "Realism");
    SETI("TalkX", (int)(d.talkX * 10000), "Layout"); SETI("TalkY", (int)(d.talkY * 10000), "Layout"); SETI("TalkScale", (int)(d.talkS * 100), "Layout");
    SETI("PhoneX", (int)(d.phoneX * 10000), "Layout"); SETI("PhoneY", (int)(d.phoneY * 10000), "Layout"); SETI("PhoneScale", (int)(d.phoneS * 100), "Layout");
    SETI("SpeechEngine", d.engine, "AI"); SETI("ForgetDays", d.forgetDays, "AI");
    SETB("Enabled", d.jarvis, "Jarvis"); SETB("Button", d.jarvisButton, "Jarvis"); SETS("Voice", d.jarvisVoice, "Jarvis");
    SETI("Pitch", (int)(d.jarvisPitch * 100 + 0.5f), "Jarvis"); SETI("Speed", (int)(d.jarvisSpeed * 100 + 0.5f), "Jarvis");
    SETS("Address", d.jarvisAddress, "Jarvis"); SETI("Confirm", d.jarvisConfirm, "Jarvis"); SETB("FriendAllowed", d.jarvisFriend, "Jarvis");
    SETI("X", (int)(d.jarvisX * 10000), "Jarvis"); SETI("Y", (int)(d.jarvisY * 10000), "Jarvis"); SETI("Scale", (int)(d.jarvisS * 100), "Jarvis");
    #undef SETB
    #undef SETI
    #undef SETS
    cfg->Save();
}
}
