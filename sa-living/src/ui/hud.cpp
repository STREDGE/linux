#include "hud.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <imgui/imgui.h>
#include <sautils/isautils.h>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <deque>
#include <string>
#include <vector>

#include "ai/claude.h"
#include "game/game.h"
#include "net/link.h"
#include "platform/android.h"
#include "platform/bridge.h"
#include "settings.h"
#include "jarvis/jarvis.h"
#include "sim/police.h"
#include "speech/speech.h"
#include "talk/talk.h"
#include "ui/fonts_data.h"
#include "ui/imgui_rw.h"
#include "util/log.h"
#include "trace/trace.h"
#include "world/world.h"

using world::Fmt;

void SALivingEnsureSpeech();   // main.cpp: мост Android + запуск речи (нужно и в главном меню)

namespace hud
{
namespace
{
    // ---------- цвета (как меню CO-OP) ----------
    struct Rgb { uint8_t r, g, b; };
    constexpr Rgb kNight{ 7, 9, 13 }, kPanel{ 15, 20, 27 }, kPanel2{ 22, 29, 39 }, kLine{ 38, 50, 67 };
    constexpr Rgb kInk{ 238, 241, 230 }, kDim{ 138, 150, 166 }, kGrove{ 79, 209, 90 }, kGold{ 242, 193, 78 };
    constexpr Rgb kRed{ 255, 77, 77 }, kBlue{ 90, 170, 255 }, kBlack{ 0, 0, 0 }, kWhite{ 255, 255, 255 };
    ImU32 C(Rgb c, float a = 1.0f) { if(a < 0) a = 0; if(a > 1) a = 1; return IM_COL32(c.r, c.g, c.b, (int)(a * 255)); }

    enum FontId { F_HEAD, F_BODY, F_SMALL, F_MONO, F_COUNT };
    ImFont* g_fonts[F_COUNT] = {};
    bool g_fontsBuilt = false;
    const ImWchar kRanges[] = { 0x0020, 0x00FF, 0x0400, 0x045F, 0x2010, 0x2026, 0x2116, 0x2116, 0 };

    void BuildFonts(float H)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear(); io.Fonts->TexDesiredWidth = 2048;
        ImFontConfig cfg; cfg.FontDataOwnedByAtlas = false; cfg.OversampleH = 1; cfg.OversampleV = 1; cfg.PixelSnapH = true;
        g_fonts[F_HEAD]  = io.Fonts->AddFontFromMemoryTTF((void*)kFontRussoOne, (int)kFontRussoOneSize, H * 0.050f, &cfg, kRanges);
        g_fonts[F_BODY]  = io.Fonts->AddFontFromMemoryTTF((void*)kFontRubikMedium, (int)kFontRubikMediumSize, H * 0.034f, &cfg, kRanges);
        g_fonts[F_SMALL] = io.Fonts->AddFontFromMemoryTTF((void*)kFontRubikMedium, (int)kFontRubikMediumSize, H * 0.027f, &cfg, kRanges);
        g_fonts[F_MONO]  = io.Fonts->AddFontFromMemoryTTF((void*)kFontMonoBold, (int)kFontMonoBoldSize, H * 0.045f, &cfg, kRanges);
        g_fontsBuilt = io.Fonts->Build();
    }

    // ---------- касания (как в CO-OP: опрос массива Points через GOT, до 4 пальцев) ----------
    struct GamePoint { int32_t x, y, state; uint8_t rest[0x1C - 12]; };
    constexpr uintptr_t kPointsGot = 0x851D38;
    GamePoint* g_pointsOrig = nullptr; GamePoint** g_pointsGot = nullptr; GamePoint* g_points = nullptr; int g_pointsN = 2;
    struct Finger { bool down = false, pressed = false, released = false; ImVec2 pos{ -1, -1 }, downPos{ -1, -1 }; };
    Finger g_f[4];
    double g_lastPoll = 0;

    double NowSec() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

    void PollTouches()
    {
        double t = NowSec();
        if(t - g_lastPoll < 0.004) return;
        g_lastPoll = t;
        for(auto& f : g_f) f.pressed = f.released = false;
        GamePoint* p = g_pointsGot ? *g_pointsGot : nullptr;
        g_points = p ? p : g_pointsOrig;
        g_pointsN = (g_points && g_points != g_pointsOrig) ? 4 : 2;
        for(int i = 0; i < 4; ++i)
        {
            Finger& f = g_f[i];
            bool down = g_points && i < g_pointsN && g_points[i].state == 2 && g_points[i].x >= 0;
            if(down) { ImVec2 q((float)g_points[i].x, (float)g_points[i].y); if(!f.down) { f.down = true; f.pressed = true; f.downPos = q; } f.pos = q; }
            else if(f.down) { f.down = false; f.released = true; }
        }
    }
    bool AnyDown() { for(auto& f : g_f) if(f.down) return true; return false; }
    bool In(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y; }
    bool HeldFrom(ImVec2 a, ImVec2 b) { for(auto& f : g_f) if(f.down && In(f.downPos, a, b)) return true; return false; }
    bool g_dragScroll = false;      // палец тянет список — отпускание не считается нажатием
    ImVec2 g_clipA{ -1e9f, -1e9f }, g_clipB{ 1e9f, 1e9f };   // нажатия вне видимой области прокрутки не считаются
    bool Clicked(ImVec2 a, ImVec2 b)
    {
        if(g_dragScroll) return false;
        for(auto& f : g_f) if(f.released && In(f.downPos, a, b) && In(f.pos, a, b) && In(f.pos, g_clipA, g_clipB)) return true;
        return false;
    }

    // ---------- рисование ----------
    ImDrawList* DL() { return ImGui::GetForegroundDrawList(); }
    float Hh() { return ImGui::GetIO().DisplaySize.y; }
    float Ww() { return ImGui::GetIO().DisplaySize.x; }
    float U(float v) { return v * Hh() / 100.0f; }
    ImVec2 TS(FontId f, const char* s, float wrap = 0) { return g_fonts[f]->CalcTextSizeA(g_fonts[f]->FontSize, FLT_MAX, wrap, s); }
    void Tx(FontId f, ImVec2 p, ImU32 col, const char* s, float wrap = 0) { DL()->AddText(g_fonts[f], g_fonts[f]->FontSize, p, col, s, nullptr, wrap); }
    void TxShadow(FontId f, ImVec2 p, ImU32 col, const char* s, float wrap = 0)
    {
        DL()->AddText(g_fonts[f], g_fonts[f]->FontSize, ImVec2(p.x + U(0.25f), p.y + U(0.25f)), C(kBlack, 0.9f), s, nullptr, wrap);
        DL()->AddText(g_fonts[f], g_fonts[f]->FontSize, p, col, s, nullptr, wrap);
    }
    void Panel(ImVec2 a, ImVec2 b, float r = 0, float alpha = 0.92f)
    {
        if(r <= 0) r = U(1.6f);
        DL()->AddRectFilled(a, b, C(kPanel, alpha), r);
        DL()->AddRect(a, b, C(kLine, alpha), r, 0, U(0.25f));
    }
    // Кнопка-плашка: true — нажата (клик)
    bool Button(ImVec2 a, ImVec2 b, const char* label, Rgb accent, bool enabled = true)
    {
        bool held = enabled && HeldFrom(a, b);
        DL()->AddRectFilled(a, b, C(held ? accent : kPanel2, enabled ? (held ? 0.95f : 0.92f) : 0.5f), U(1.2f));
        DL()->AddRect(a, b, C(accent, enabled ? 0.9f : 0.3f), U(1.2f), 0, U(0.3f));
        ImVec2 ts = TS(F_SMALL, label);
        Tx(F_SMALL, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), C(held ? kNight : kInk, enabled ? 1 : 0.5f), label);
        bool clicked = enabled && Clicked(a, b);
        if(clicked) TRACE("КНОПКА", "«%s»", label);
        return clicked;
    }

    // ---------- состояние ----------
    bool   g_menuOpen = false;
    double g_blockMenuUntil = 0;
    bool   g_phoneOpen = false; int g_phoneTab = 0; float g_phoneScroll = 0;
    bool   g_keypad = false; std::string g_amount;
    std::deque<std::pair<std::string, double>> g_feed;
    std::string g_menuToast; double g_menuToastAt = -10;
    int    g_checkReq = 0;
    bool   g_confirmReset = false;
    double g_now = 0;
    uint64_t g_lastUs = 0;
    bool   g_talkHeld = false;
    bool   g_talkDrawn = false;      // кнопку «Говорить» нарисовали в этом кадре (иначе зажатую — отпустить)

    // Кооп (мы друг): контакты и новости приходят от хоста
    struct RemoteContact { int hostId; std::string name; std::string what; };

    void Feed(const std::string& s) { g_feed.push_back({ s, g_now }); while(g_feed.size() > 4) g_feed.pop_front(); }
    void MenuToast(const std::string& s) { g_menuToast = s; g_menuToastAt = g_now; }

    // ---------- меню «ИИ» (плитка в настройках игры) ----------
    void Row(float& y, float x0, float x1, const char* label, const char* value)
    {
        Tx(F_BODY, ImVec2(x0, y), C(kDim), label);
        if(value) { ImVec2 ts = TS(F_BODY, value); Tx(F_BODY, ImVec2(x1 - ts.x, y), C(kInk), value); }
    }

    bool Toggle(float& y, float x0, float x1, const char* label, bool& v)
    {
        Tx(F_BODY, ImVec2(x0, y + U(0.6f)), C(kInk), label);
        ImVec2 a(x1 - U(11), y), b(x1, y + U(5));
        bool clicked = Clicked(a, b);
        if(clicked) v = !v;
        DL()->AddRectFilled(a, b, C(v ? kGrove : kPanel2), U(2.5f));
        DL()->AddRect(a, b, C(kLine), U(2.5f), 0, U(0.2f));
        float kx = v ? b.x - U(2.6f) : a.x + U(2.6f);
        DL()->AddCircleFilled(ImVec2(kx, (a.y + b.y) * 0.5f), U(2.0f), C(kWhite), 24);
        y += U(6.5f);
        return clicked;
    }

    int Chips(float& y, float x0, float x1, const char* label, const char* const* opts, int n, int cur)
    {
        Tx(F_BODY, ImVec2(x0, y + U(0.6f)), C(kInk), label);
        float w = U(15), gap = U(1);
        float x = x1 - n * w - (n - 1) * gap;
        int res = cur;
        for(int i = 0; i < n; ++i)
        {
            ImVec2 a(x + i * (w + gap), y), b(a.x + w, y + U(5));
            bool on = i == cur;
            DL()->AddRectFilled(a, b, C(on ? kGold : kPanel2), U(1.2f));
            ImVec2 ts = TS(F_SMALL, opts[i]);
            Tx(F_SMALL, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), C(on ? kNight : kInk), opts[i]);
            if(Clicked(a, b)) res = i;
        }
        y += U(6.5f);
        return res;
    }

    void CheckKey()
    {
        claude::Request r;
        json::Value msg = json::Value::Object(); msg["role"] = "user"; msg["content"] = "Ответь одним словом: работает";
        r.messages = json::Value::Array(); r.messages.push(msg); r.maxTokens = 32; r.cache = false; r.tag = "проверка ключа";
        MenuToast("Проверяю ключ…");
        g_checkReq = claude::Send(r, nullptr, [](claude::Result& res) {
            g_checkReq = 0;
            MenuToast(res.ok ? "Ключ работает: «" + res.text + "»" : "Ключ не работает: " + res.error);
        });
        if(!g_checkReq) MenuToast(bridge::Ready() ? "Нет ключа или лимит на сегодня" : "Мост Android не запустился");
    }

    // ---------- меню «AI»: вкладки ----------
    int    g_tab = 0;
    double g_permAt = -10; int g_micPerm = -1;
    std::string g_micTestText; bool g_micTestHeld = false; double g_micTestAt = -10;
    bool   g_newVoiceFemale = false;
    int    g_previewSeq = 0;

    void PumpWhileMenu()
    {   // игра стоит (главное меню / пауза) — сами крутим ИИ и речь, чтобы работали проверка ключа, загрузка, микрофон
        SALivingEnsureSpeech();
        claude::Pump();
        speech::Frame();
        talk::Frame();
        std::string n;
        while(speech::PopNotice(n)) MenuToast(n);
        while(talk::PopNotify(n)) MenuToast(n);
    }

    // Строка «состояния»: точка, заголовок, пояснение. Возвращает правый край для кнопок.
    void StatusRow(float& y, float x0, float x1, Rgb dot, const char* title, const std::string& text)
    {
        Panel(ImVec2(x0 - U(2), y - U(0.8f)), ImVec2(x1 + U(2), y + U(9.2f)), U(1.4f), 0.9f);
        DL()->AddCircleFilled(ImVec2(x0 + U(0.6f), y + U(2.2f)), U(1.1f), C(dot), 20);
        Tx(F_BODY, ImVec2(x0 + U(3.2f), y), C(kInk), title);
        Tx(F_SMALL, ImVec2(x0 + U(3.2f), y + U(4.6f)), C(kDim), text.c_str(), (x1 - x0) * 0.56f);
    }
    // Кнопки справа в строке состояния (справа налево)
    bool RowButton(float y, float& xr, const char* label, Rgb col, bool enabled = true)
    {
        float w = TS(F_SMALL, label).x + U(4);
        ImVec2 a(xr - w, y + U(1.2f)), b(xr, y + U(7.2f));
        xr -= w + U(1);
        return Button(a, b, label, col, enabled);
    }
    void ProgressBar(float y, float x0, float x1, float k)
    {
        ImVec2 a(x0 + U(3.2f), y + U(8.0f)), b(x1, y + U(8.8f));
        DL()->AddRectFilled(a, b, C(kPanel2), U(0.4f));
        if(k >= 0) DL()->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * (k > 1 ? 1 : k), b.y), C(kGrove), U(0.4f));
        else { float t = fmodf((float)g_now, 1.2f) / 1.2f; float w = (b.x - a.x) * 0.25f; float x = a.x + (b.x - a.x - w) * t; DL()->AddRectFilled(ImVec2(x, a.y), ImVec2(x + w, b.y), C(kGrove), U(0.4f)); }
    }

    std::string KeyLine()
    {
        coopnet::Role r = coopnet::GetRole();
        if(r == coopnet::CLIENT) return "Используется ключ хоста";
        if(r == coopnet::HOST) return claude::HasKey() ? "Используется твой ключ (ты и друг)" : "Вставьте ключ — в коопе ИИ платит хост";
        return claude::HasKey() ? "Используется твой ключ" : "Вставьте ключ";
    }

    void PasteKey(settings::Data& s)
    {
        char buf[512] = "";
        if(platform::ClipboardGetText(buf, sizeof(buf)))
        {
            std::string k = buf; while(!k.empty() && (k.back() == ' ' || k.back() == '\n' || k.back() == '\r')) k.pop_back();
            while(!k.empty() && (k[0] == ' ' || k[0] == '\n')) k.erase(0, 1);
            if(k.rfind("sk-", 0) == 0 && (k.find("...") != std::string::npos || k.find("\xE2\x80\xA6") != std::string::npos || k.size() < 60))
                MenuToast("Это сокращённый ключ (с «...») — он не работает. В консоли создай новый ключ и нажми «Copy» сразу после создания");
            else if(k.rfind("sk-", 0) == 0 && k.size() > 20) { s.key = k; claude::Settings cs = claude::Get(); cs.key = k; claude::Configure(cs); settings::Save(); MenuToast("Ключ вставлен — нажми «Проверить»"); }
            else MenuToast("В буфере нет ключа (должен начинаться с sk-)");
        }
        else MenuToast("Буфер обмена пуст");
    }

    void TabMain(settings::Data& s, float x0, float x1, float& y)
    {
        speech::Status st = speech::Get();
        // 1. ключ
        {
            bool ok = claude::Available();
            std::string t = (claude::HasKey() ? claude::MaskedKey() + " · " : std::string()) + KeyLine();
            StatusRow(y, x0, x1, ok ? kGrove : kGold, "1. Ключ ИИ (Anthropic)", t);
            float xr = x1;
            if(RowButton(y, xr, "Удалить", kRed, claude::HasKey())) { s.key.clear(); claude::Settings cs = claude::Get(); cs.key.clear(); claude::Configure(cs); settings::Save(); MenuToast("Ключ удалён"); }
            if(RowButton(y, xr, g_checkReq ? "Проверяю…" : "Проверить", kGrove, claude::HasKey() && !g_checkReq)) CheckKey();
            if(RowButton(y, xr, "Вставить из буфера", kGold)) PasteKey(s);
            y += U(10.6f);
        }
        // 2. микрофон
        {
            if(g_now - g_permAt > 2) { g_permAt = g_now; g_micPerm = platform::HasPermission("android.permission.RECORD_AUDIO"); }
            StatusRow(y, x0, x1, g_micPerm == 1 ? kGrove : kRed, "2. Микрофон", g_micPerm == 1 ? "Разрешение есть" : "Нет разрешения на микрофон — без него тебя не слышно");
            float xr = x1;
            if(g_micPerm != 1 && RowButton(y, xr, "Разрешить", kGold)) { if(!platform::RequestPermission("android.permission.RECORD_AUDIO")) platform::OpenAppSettings(); g_permAt = -10; }
            y += U(10.6f);
        }
        // 3. распознавание
        {
            bool busyAsr = st.busy && st.busyText.find("аспозна") == std::string::npos && !st.asrFiles;
            std::string t = st.asrReady ? Fmt("Готово (GigaAM, без интернета). Последняя фраза распознана за %.1f с", st.lastAsrSec)
                          : st.asrFiles ? (st.loading ? "Загружается в память…" : "Файлы есть, движок не запустился — см. ошибку ниже")
                          : st.busy ? st.busyText : "Не установлено. Скачается один раз (~215 МБ), дальше работает без интернета";
            StatusRow(y, x0, x1, st.asrReady ? kGrove : (st.asrFiles || st.busy ? kGold : kRed), "3. Распознавание речи", t);
            float xr = x1;
            if(!st.asrFiles && !st.busy && RowButton(y, xr, "Скачать (215 МБ)", kGold)) speech::DownloadAsr();
            if(st.busy && RowButton(y, xr, "Отмена", kRed)) speech::CancelBusy();
            if(st.busy && !st.asrFiles) ProgressBar(y, x0, x1 - U(22), st.progress);
            (void)busyAsr;
            y += U(10.6f);
        }
        // 4. голоса
        {
            std::string t = st.ttsReady ? Fmt("Готово: голосов %d. Скорость озвучки x%.2f (меньше 1 — быстрее речи)", st.voices, st.lastRtf)
                          : st.ttsFiles ? (st.loading ? "Загружаются в память…" : "Файлы есть, движок не запустился — см. ошибку ниже")
                          : "Не установлены. Сохрани файл SALiving-voices.zip (пришлют в чат) в «Загрузки» и нажми «Найти»";
            StatusRow(y, x0, x1, st.ttsReady ? kGrove : (st.ttsFiles ? kGold : kRed), "4. Голоса педов и Джарвиса", t);
            float xr = x1;
            if(!st.ttsFiles && !st.busy)
            {
                if(RowButton(y, xr, "Выбрать файл", kBlue)) { if(!speech::PickVoicePack()) MenuToast("Окно выбора файла не открылось"); }
                if(RowButton(y, xr, "Найти в Загрузках", kGold)) { if(!speech::ImportVoicePackAuto()) MenuToast("Не нашёл SALiving-voices.zip в Загрузках — нажми «Выбрать файл»"); }
            }
            y += U(10.6f);
        }
        // 5. проверка
        {
            std::string t = talk::Capturing() ? (talk::Recognizing() ? "Распознаю…" : "Говори…") : g_micTestText.empty() ? "Зажми «Сказать фразу» и скажи что-нибудь — увидишь, как тебя слышно" : "Услышал: «" + g_micTestText + "»";
            StatusRow(y, x0, x1, st.asrReady && st.ttsReady ? kGrove : kGold, "5. Проверка", t);
            float xr = x1;
            if(RowButton(y, xr, "Послушать голос", kBlue, st.ttsReady))
            {
                auto vs = speech::Voices();
                if(!vs.empty()) { const speech::Voice& v = vs[g_previewSeq++ % vs.size()]; speech::StopSpeaking(); speech::Speak("Эй, ты. Д+а, ты. Чег+о уст+авился?", v.key, v.pitch, v.speed, "preview"); MenuToast("Голос: " + v.title); }
            }
            float w = TS(F_SMALL, "Сказать фразу").x + U(4);
            ImVec2 a(xr - w, y + U(1.2f)), b(xr, y + U(7.2f));
            bool held = HeldFrom(a, b) && st.asrReady;
            Button(a, b, "Сказать фразу", kGold, st.asrReady);
            if(held != g_micTestHeld)
            {
                g_micTestHeld = held;
                if(held) talk::CaptureDown([](const std::string& txt) { g_micTestText = txt.empty() ? "(ничего не расслышал)" : txt; });
                else talk::CaptureUp();
            }
            y += U(10.6f);
        }
        if(!st.error.empty()) { Tx(F_SMALL, ImVec2(x0, y), C(kRed), ("Ошибка речи: " + st.error).c_str(), x1 - x0); y += TS(F_SMALL, st.error.c_str(), x1 - x0).y + U(1.5f); }
        claude::Usage t = claude::Today(), all = claude::Total();
        std::string us = Fmt("сегодня %d запр. около $%.3f · всего %d около $%.2f · лимит %d в день", t.requests, claude::CostUsd(t), all.requests, claude::CostUsd(all), s.dailyLimit);
        Row(y, x0, x1, "Расход ИИ", us.c_str()); y += U(5.0f);
        const char* mode = coopnet::GetRole() == coopnet::HOST ? "кооп: ты хост (ИИ по твоему ключу)" : coopnet::GetRole() == coopnet::CLIENT ? "кооп: ИИ через хоста" : "одиночная игра";
        std::string mem = world::MemoryKey().empty() ? std::string("игра не загружена") : "память сохранения «" + world::MemoryKey() + Fmt("»: людей %d", (int)world::W().chars.size());
        Row(y, x0, x1, mode, mem.c_str()); y += U(5.0f);
    }

    void TabVoices(settings::Data& s, float x0, float x1, float& y)
    {
        speech::Status st = speech::Get();
        static const char* engines[] = { "Своя", "Google" };
        int ne = Chips(y, x0, x1, "Речь (распознавание и голоса)", engines, 2, s.engine);
        if(ne != s.engine) { s.engine = ne; if(ne == 1) bridge::TtsInit(s.lang.c_str()); }
        Toggle(y, x0, x1, "Голоса педов", s.voices);
        {
            static const char* loud[] = { "Тише", "Обычно", "Громче", "Очень громко" };
            int nl = Chips(y, x0, x1, "Громкость голосов", loud, 4, s.voiceLoud);
            if(nl != s.voiceLoud) { s.voiceLoud = nl; speech::SetLoudness(nl); settings::Save(); }
        }
        Toggle(y, x0, x1, "Субтитры", s.subtitles);
        Tx(F_SMALL, ImVec2(x0, y), C(kGold), "ГОЛОСА"); y += U(4);
        auto vs = speech::Voices();
        if(vs.empty()) { Tx(F_SMALL, ImVec2(x0, y), C(kDim), "Голосовой пакет не установлен — вкладка «Главное», шаг 4.", x1 - x0); y += U(5); }
        for(auto& v : vs)
        {
            ImVec2 a(x0, y), b(x1, y + U(6.2f));
            DL()->AddRectFilled(a, b, C(kPanel2), U(1));
            std::string title = v.title + (v.female ? " · женский" : " · мужской") + (v.builtin ? "" : " · свой");
            Tx(F_SMALL, ImVec2(a.x + U(1.5f), a.y + U(1.6f)), C(kInk), title.c_str());
            float xr = x1 - U(1);
            if(!v.builtin) { float w = U(12); if(Button(ImVec2(xr - w, y + U(0.6f)), ImVec2(xr, y + U(5.6f)), "Удалить", kRed)) { speech::DeleteVoice(v.key); MenuToast("Голос удалён"); } xr -= w + U(1); }
            if(Button(ImVec2(xr - U(16), y + U(0.6f)), ImVec2(xr, y + U(5.6f)), "Послушать", kBlue, st.ttsReady))
            { speech::StopSpeaking(); speech::Speak("Сл+ушай сюд+а. Ещё раз появ+ишься на н+ашей +улице — пожал+еешь.", v.key, v.pitch, v.speed, "preview"); }
            y += U(7.0f);
        }
        y += U(1);
        Tx(F_SMALL, ImVec2(x0, y), C(kGold), "ДОБАВИТЬ СВОЙ ГОЛОС (свой, друга)"); y += U(4);
        const char* hint = "Нужно 15–20 секунд спокойной речи без пауз и шума. Нажми «Записать» и читай любой текст ровным голосом, потом «Готово». Или выбери готовый WAV-файл.";
        Tx(F_SMALL, ImVec2(x0, y), C(kDim), hint, x1 - x0); y += TS(F_SMALL, hint, x1 - x0).y + U(1.5f);
        static const char* gender[] = { "Мужской", "Женский" };
        g_newVoiceFemale = Chips(y, x0, x1, "Какой голос", gender, 2, g_newVoiceFemale ? 1 : 0) == 1;
        float bw = (x1 - x0 - U(1)) / 2;
        if(speech::RecordingVoice())
        {
            float sec = speech::RecordSeconds();
            if(Button(ImVec2(x0, y), ImVec2(x0 + bw, y + U(6.5f)), Fmt("Готово (записано %.0f с)", sec).c_str(), sec >= 8 ? kGrove : kGold))
                speech::RecordVoiceFinish(Fmt("Мой голос %d", (int)speech::Voices().size() + 1), g_newVoiceFemale);
        }
        else if(Button(ImVec2(x0, y), ImVec2(x0 + bw, y + U(6.5f)), "Записать с микрофона", kGold, st.ttsReady))
        { if(!speech::RecordVoiceStart()) MenuToast("Микрофон недоступен — проверь разрешение"); }
        if(Button(ImVec2(x0 + bw + U(1), y), ImVec2(x1, y + U(6.5f)), "Из файла WAV", kBlue, st.ttsReady && !speech::RecordingVoice()))
        { if(!speech::AddVoiceFromFile(Fmt("Мой голос %d", (int)speech::Voices().size() + 1), g_newVoiceFemale)) MenuToast("Окно выбора файла не открылось"); }
        y += U(8);
    }

    // «‹ значение ›»: возвращает -1 / 0 / +1
    int Stepper(float& y, float x0, float x1, const char* label, const std::string& value)
    {
        Tx(F_BODY, ImVec2(x0, y + U(0.6f)), C(kInk), label);
        float bw = U(7), vw = U(30);
        ImVec2 la(x1 - vw - 2 * bw - U(2), y), lb(la.x + bw, y + U(5));
        ImVec2 ra(x1 - bw, y), rb(x1, y + U(5));
        int r = 0;
        if(Button(la, lb, "<", kLine)) r = -1;
        if(Button(ra, rb, ">", kLine)) r = 1;
        ImVec2 ts = TS(F_SMALL, value.c_str());
        Tx(F_SMALL, ImVec2((lb.x + ra.x - ts.x) * 0.5f, y + U(1.2f)), C(kGold), value.c_str());
        y += U(6.5f);
        return r;
    }

    std::string VoiceTitle(const std::string& key) { speech::Voice v; return speech::FindVoice(key, v) ? v.title : std::string("(выберется сам)"); }
    std::string NextVoice(const std::string& key, int dir)
    {
        auto vs = speech::Voices(); if(vs.empty()) return key;
        int idx = -1; for(int i = 0; i < (int)vs.size(); ++i) if(vs[i].key == key) idx = i;
        idx = idx < 0 ? 0 : (idx + dir + (int)vs.size()) % (int)vs.size();
        return vs[idx].key;
    }

    void TabJarvis(settings::Data& s, float x0, float x1, float& y)
    {
        Toggle(y, x0, x1, "Джарвис — личный помощник", s.jarvis);
        Toggle(y, x0, x1, "Кнопка «Джарвис» на экране (зажал — говоришь)", s.jarvisButton);
        if(int d = Stepper(y, x0, x1, "Голос Джарвиса", VoiceTitle(s.jarvisVoice))) s.jarvisVoice = NextVoice(s.jarvisVoice, d);
        if(int d = Stepper(y, x0, x1, "Высота голоса", Fmt("%.2f", s.jarvisPitch))) s.jarvisPitch = fminf(1.3f, fmaxf(0.7f, s.jarvisPitch + d * 0.03f));
        if(int d = Stepper(y, x0, x1, "Скорость речи", Fmt("%.2f", s.jarvisSpeed))) s.jarvisSpeed = fminf(1.4f, fmaxf(0.7f, s.jarvisSpeed + d * 0.03f));
        if(Button(ImVec2(x1 - U(24), y), ImVec2(x1, y + U(5.5f)), "Послушать", kBlue, speech::TtsReady())) { speech::StopSpeaking(); jarvis::Preview(); }
        y += U(7);
        static const char* addr[] = { "сэр", "босс", "шеф" };
        int ai = s.jarvisAddress == "босс" ? 1 : s.jarvisAddress == "шеф" ? 2 : 0;
        int na = Chips(y, x0, x1, "Как к тебе обращаться", addr, 3, ai);
        if(na != ai) s.jarvisAddress = addr[na];
        static const char* conf[] = { "Опасное", "Всегда", "Никогда" };
        s.jarvisConfirm = Chips(y, x0, x1, "Переспрашивать «Подтверждаете?»", conf, 3, s.jarvisConfirm);
        Toggle(y, x0, x1, "Кооп: другу можно менять общий мир через Джарвиса", s.jarvisFriend);
        // справочник
        speech::Status st = speech::Get();
        bool lib = jarvis::LibraryFilePresent();
        std::string t = lib ? "Скачан: Джарвис может собирать любые действия из команд игры (нужен ключ ИИ)" : st.busy ? st.busyText : "Не скачан (~1 МБ). Без него работают только готовые команды";
        StatusRow(y, x0, x1, lib ? kGrove : kGold, "Справочник команд игры («даже то, чего нет в списке»)", t);
        float xr = x1;
        if(!st.busy && RowButton(y, xr, lib ? "Обновить" : "Скачать", kGold)) jarvis::DownloadLibrary();
        y += U(10.6f);
        const char* hint = "Вызов: зажми кнопку «Джарвис» и говори — или пункт Jarvis в меню CLEO (слушает 6 секунд). Примеры: «дай миниган и тысячу патронов», «сними розыск», "
                           "«перенеси в квадрат C7», «перенеси на метку», «запомни это место как база», «дай инфернус», «сделай ночь и дождь», «полное здоровье и броня», «отмени». "
                           "Без ключа и интернета понимает простые команды; с ключом — свободную речь и любые просьбы.";
        Tx(F_SMALL, ImVec2(x0, y), C(kDim), hint, x1 - x0); y += TS(F_SMALL, hint, x1 - x0).y + U(1);
    }

    void TabSettings(settings::Data& s, float x0, float x1, float& y)
    {
        static const char* models[] = { "Haiku 5.5", "Sonnet 5.5" };
        static const char* modelIds[] = { "claude-haiku-5-5", "claude-sonnet-5-5" };
        int mi = s.model == modelIds[1] ? 1 : 0;
        int nm = Chips(y, x0, x1, "Модель", models, 2, mi);
        if(nm != mi) { s.model = modelIds[nm]; claude::Settings cs = claude::Get(); cs.model = s.model; claude::Configure(cs); }
        static const char* efforts[] = { "Быстро", "Умнее" };
        int ei = s.effort == "medium" ? 1 : 0;
        int ne = Chips(y, x0, x1, "Режим размышлений", efforts, 2, ei);
        if(ne != ei) { s.effort = ne ? "medium" : "low"; claude::Settings cs = claude::Get(); cs.effort = s.effort; claude::Configure(cs); }
        static const char* inits[] = { "Выкл", "Редко", "Часто" };
        s.initiative = Chips(y, x0, x1, "Педы сами заговаривают", inits, 3, s.initiative);
        static const char* limits[] = { "100", "400", "1500" };
        int li = s.dailyLimit <= 100 ? 0 : (s.dailyLimit <= 400 ? 1 : 2);
        int nl = Chips(y, x0, x1, "Лимит запросов ИИ в день", limits, 3, li);
        if(nl != li) { static const int lv[] = { 100, 400, 1500 }; s.dailyLimit = lv[nl]; claude::SetDailyLimit(s.dailyLimit); }
        static const char* forget[] = { "3 дня", "7 дней", "Никогда" };
        int fi = s.forgetDays <= 0 ? 2 : (s.forgetDays <= 3 ? 0 : 1);
        int nf = Chips(y, x0, x1, "Забывать неважных людей через", forget, 3, fi);
        if(nf != fi) { static const int fv[] = { 3, 7, 0 }; s.forgetDays = fv[nf]; world::g_forgetDays = s.forgetDays; }
        Toggle(y, x0, x1, "Задержание по шагам (наручники, участок, суд)", s.realArrest);
        Toggle(y, x0, x1, "Розыск только по свидетелям", s.witnesses);
        Toggle(y, x0, x1, "Режиссёр города (планы банд, слухи)", s.director);
        Toggle(y, x0, x1, "Кооп: друг говорит через мой ключ", s.shareKeyWithFriend);
        if(!g_confirmReset) { if(Button(ImVec2(x0, y), ImVec2(x0 + U(44), y + U(6)), "Забыть всех в этом сохранении", kRed)) g_confirmReset = true; }
        else
        {
            Tx(F_SMALL, ImVec2(x0, y + U(1.5f)), C(kRed), "Точно? Люди, связи и слухи этого сохранения будут забыты.");
            if(Button(ImVec2(x1 - U(40), y), ImVec2(x1 - U(21), y + U(6)), "Да, забыть", kRed)) { json::Value u = world::W().usage; world::Reset(); world::W().usage = u; g_confirmReset = false; MenuToast("Память этого сохранения очищена"); }
            if(Button(ImVec2(x1 - U(19), y), ImVec2(x1, y + U(6)), "Нет", kGrove)) g_confirmReset = false;
        }
        y += U(8);
    }

    void DrawMenu()
    {
        PumpWhileMenu();
        settings::Data& s = settings::Get();
        float W = Ww(), H = Hh();
        DL()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), C(kNight, 0.95f));
        float x0 = W * 0.10f, x1 = W * 0.90f;
        float y = U(3);
        Tx(F_HEAD, ImVec2(x0, y), C(kGold), "ИИ — ЖИВЫЕ ЛЮДИ");
        if(Button(ImVec2(x1 - U(14), y), ImVec2(x1, y + U(6)), "Закрыть", kGrove))
        {
            if(g_micTestHeld) { g_micTestHeld = false; talk::CaptureUp(); }
            speech::StopSpeaking();
            settings::Save(); g_menuOpen = false; g_blockMenuUntil = g_now + 0.4; return;
        }
        y += U(8);
        static const char* tabs[] = { "Главное", "Голоса", "Джарвис", "Настройки" };
        float tw = (x1 - x0 - U(3)) / 4;
        for(int i = 0; i < 4; ++i)
        {
            ImVec2 ta(x0 + i * (tw + U(1)), y), tb(ta.x + tw, y + U(5.5f));
            bool on = g_tab == i;
            DL()->AddRectFilled(ta, tb, C(on ? kGold : kPanel2), U(1));
            ImVec2 ts = TS(F_SMALL, tabs[i]);
            Tx(F_SMALL, ImVec2((ta.x + tb.x - ts.x) * 0.5f, (ta.y + tb.y - ts.y) * 0.5f), C(on ? kNight : kInk), tabs[i]);
            if(Clicked(ta, tb)) { g_tab = i; g_confirmReset = false; }
        }
        y += U(8.5f);
        // содержимое вкладки прокручивается пальцем, если не помещается
        static float scroll[4] = {}, lastY = -1; static bool moved = false;
        float top = y, bottom = H - U(2);
        bool down = false; float fy = 0;
        for(auto& f : g_f) if(f.down && f.downPos.y > top) { down = true; fy = f.pos.y; if(fabsf(f.pos.y - f.downPos.y) > U(2.5f)) moved = true; break; }
        if(down) { if(lastY >= 0 && moved) scroll[g_tab] -= fy - lastY; lastY = fy; }
        else { lastY = -1; }
        g_dragScroll = moved;
        if(!down) moved = false;
        DL()->PushClipRect(ImVec2(0, top - U(1)), ImVec2(W, bottom), true);
        g_clipA = ImVec2(0, top - U(1)); g_clipB = ImVec2(W, bottom);
        y = top - scroll[g_tab];
        if(g_tab == 0) TabMain(s, x0, x1, y);
        else if(g_tab == 1) TabVoices(s, x0, x1, y);
        else if(g_tab == 2) TabJarvis(s, x0, x1, y);
        else TabSettings(s, x0, x1, y);
        DL()->PopClipRect();
        g_clipA = ImVec2(-1e9f, -1e9f); g_clipB = ImVec2(1e9f, 1e9f);
        float maxScroll = (y + scroll[g_tab]) - bottom + U(2);
        if(maxScroll < 0) maxScroll = 0;
        if(scroll[g_tab] > maxScroll) scroll[g_tab] = maxScroll;
        if(scroll[g_tab] < 0) scroll[g_tab] = 0;
        if(maxScroll > 0)
        {   // полоска прокрутки
            float trackH = bottom - top, barH = trackH * trackH / (trackH + maxScroll), barY = top + (trackH - barH) * (scroll[g_tab] / maxScroll);
            DL()->AddRectFilled(ImVec2(x1 + U(3), barY), ImVec2(x1 + U(3.6f), barY + barH), C(kGold, 0.7f), U(0.3f));
        }

        if(g_now - g_menuToastAt < 4)
        {
            ImVec2 ts = TS(F_BODY, g_menuToast.c_str(), W * 0.7f);
            ImVec2 a((W - ts.x) * 0.5f - U(2), H - ts.y - U(6)), b((W + ts.x) * 0.5f + U(2), H - U(2));
            Panel(a, b); Tx(F_BODY, ImVec2(a.x + U(2), a.y + U(2)), C(kGold), g_menuToast.c_str(), W * 0.7f);
        }
    }

    // ---------- карточка персонажа (с выбором голоса) ----------
    int   g_cardChar = 0;
    std::string g_cardVoice; float g_cardPitch = 1, g_cardRate = 1;

    void OpenCard(int charId)
    {
        world::Character* c = world::Find(charId); if(!c) return;
        talk::EnsureVoice(*c);
        g_cardChar = charId; g_cardVoice = c->voice; g_cardPitch = c->vpitch; g_cardRate = c->vrate;
    }

    void DrawCard()
    {
        world::Character* c = world::Find(g_cardChar);
        if(!c) { g_cardChar = 0; return; }
        float W = Ww(), H = Hh();
        ImVec2 a(W * 0.5f - U(48), H * 0.08f), b(W * 0.5f + U(48), H * 0.94f);
        DL()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), C(kBlack, 0.5f));
        Panel(a, b, U(2.4f), 0.97f);
        float x0 = a.x + U(3), x1 = b.x - U(3), y = a.y + U(2.5f);
        std::string title = c->nick.empty() ? c->name : c->nick + " (" + c->name + ")";
        Tx(F_HEAD, ImVec2(x0, y), C(kGold), title.c_str());
        if(Button(ImVec2(x1 - U(9), y), ImVec2(x1, y + U(5.5f)), "X", kRed)) { g_cardChar = 0; speech::StopSpeaking(); return; }
        y += U(7.5f);
        const world::Relation& r = c->rel[world::WHO_CJ];
        std::string who = Fmt("%s%s · %d лет%s%s%s", world::FactionName(c->faction), c->story.empty() ? "" : " · сюжетный персонаж", c->age,
                              c->alive ? "" : " · погиб", r.payroll ? " · на зарплате" : "", r.informant ? " · информатор" : "");
        Tx(F_SMALL, ImVec2(x0, y), C(kDim), who.c_str(), x1 - x0); y += U(4.2f);
        std::string rel = Fmt("Доверие %d · уважение %d · симпатия %d · страх %d", r.trust, r.respect, r.liking, r.fear);
        Tx(F_SMALL, ImVec2(x0, y), C(kInk), rel.c_str()); y += U(4.2f);
        if(!c->summary.empty()) { Tx(F_SMALL, ImVec2(x0, y), C(kDim), c->summary.c_str(), x1 - x0); y += TS(F_SMALL, c->summary.c_str(), x1 - x0).y + U(1); }
        if(!c->mem.empty())
        {
            const std::string& m = c->mem.back().text;
            std::string line = "Помнит: " + m;
            Tx(F_SMALL, ImVec2(x0, y), C(kDim), line.c_str(), x1 - x0); y += TS(F_SMALL, line.c_str(), x1 - x0).y + U(1);
        }
        y += U(1.5f);
        Tx(F_SMALL, ImVec2(x0, y), C(kGold), "ГОЛОС"); y += U(4);
        if(speech::Voices().empty()) { Tx(F_SMALL, ImVec2(x0, y), C(kDim), "Голосовой пакет не установлен (Настройки > AI).", x1 - x0); return; }
        if(int d = Stepper(y, x0, x1, "Голос", VoiceTitle(g_cardVoice))) g_cardVoice = NextVoice(g_cardVoice, d);
        if(int d = Stepper(y, x0, x1, "Высота", Fmt("%.2f", g_cardPitch))) g_cardPitch = fminf(1.3f, fmaxf(0.7f, g_cardPitch + d * 0.03f));
        if(int d = Stepper(y, x0, x1, "Скорость", Fmt("%.2f", g_cardRate))) g_cardRate = fminf(1.4f, fmaxf(0.7f, g_cardRate + d * 0.03f));
        float bw = (x1 - x0 - U(2)) / 3;
        if(Button(ImVec2(x0, y), ImVec2(x0 + bw, y + U(6.5f)), "Послушать", kBlue, speech::TtsReady()))
        {
            speech::Voice v; float p = g_cardPitch, rt = g_cardRate;
            if(speech::FindVoice(g_cardVoice, v)) { p *= v.pitch; rt *= v.speed; }
            speech::StopSpeaking();
            speech::Speak(c->faction == world::F_POLICE ? "Полиц+ия! Р+уки, чтоб я в+идел!" : "Ну здор+ово. Чег+о хот+ел?", g_cardVoice, p, rt, "preview");
        }
        if(Button(ImVec2(x0 + bw + U(1), y), ImVec2(x0 + 2 * bw + U(1), y + U(6.5f)), "Сохранить", kGrove))
        {
            c->voice = g_cardVoice; c->vpitch = g_cardPitch; c->vrate = g_cardRate;
            if(!c->story.empty()) { world::StoryVoice sv; sv.voice = c->voice; sv.pitch = c->vpitch; sv.rate = c->vrate; world::SetStoryVoice(c->story, sv); }
            Feed(c->story.empty() ? "Голос сохранён (для этого сохранения)" : "Голос сохранён (во всех сохранениях)");
            LOG_INFO("Голос: #%d %s — назначен «%s» (%.2f, %.2f)%s", c->id, c->name.c_str(), c->voice.c_str(), c->vpitch, c->vrate, c->story.empty() ? "" : ", сюжетный — во всех сохранениях");
            g_cardChar = 0;
        }
        if(Button(ImVec2(x0 + 2 * bw + U(2), y), ImVec2(x1, y + U(6.5f)), "Вернуть как было", kRed))
        {
            if(!c->story.empty()) world::ClearStoryVoice(c->story);
            c->voice.clear(); talk::EnsureVoice(*c);
            g_cardVoice = c->voice; g_cardPitch = c->vpitch; g_cardRate = c->vrate;
            Feed("Голос — как был по умолчанию");
        }
        y += U(8);
        const char* hint = c->story.empty() ? "Голос этого человека действует в этом сохранении." : "Это сюжетный персонаж: его голос одинаков во всех сохранениях.";
        Tx(F_SMALL, ImVec2(x0, y), C(kDim), hint, x1 - x0);
    }

    // ---------- телефон ----------
    void DrawKeypad()
    {
        float W = Ww(), H = Hh();
        ImVec2 a(W * 0.5f - U(24), H * 0.12f), b(W * 0.5f + U(24), H * 0.92f);
        Panel(a, b);
        Tx(F_BODY, ImVec2(a.x + U(3), a.y + U(2)), C(kDim), "Сколько дать?");
        std::string shown = "$" + (g_amount.empty() ? std::string("0") : g_amount);
        Tx(F_MONO, ImVec2(a.x + U(3), a.y + U(7)), C(kGold), shown.c_str());
        Tx(F_SMALL, ImVec2(a.x + U(3), a.y + U(13)), C(kDim), Fmt("у тебя $%d", game::Money()).c_str());
        static const char* keys[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "<", "0", "OK" };
        float kw = U(13), kh = U(9), gx = a.x + U(3), gy = a.y + U(18);
        for(int i = 0; i < 12; ++i)
        {
            ImVec2 ka(gx + (i % 3) * (kw + U(1.5f)), gy + (i / 3) * (kh + U(1.5f))), kb(ka.x + kw, ka.y + kh);
            if(Button(ka, kb, keys[i], i == 11 ? kGrove : kLine))
            {
                if(i == 9) { if(!g_amount.empty()) g_amount.pop_back(); }
                else if(i == 11) { int v = atoi(g_amount.c_str()); g_keypad = false; if(v > 0) talk::OfferMoney(v); g_amount.clear(); }
                else if(g_amount.size() < 7) g_amount += keys[i];
            }
        }
        if(Button(ImVec2(a.x + U(3), b.y - U(8)), ImVec2(b.x - U(3), b.y - U(2)), "Отмена", kRed)) { g_keypad = false; g_amount.clear(); }
    }

    void DrawPhone()
    {
        float W = Ww(), H = Hh();
        ImVec2 a(W - U(62), U(8)), b(W - U(4), H - U(6));
        Panel(a, b, U(3), 0.96f);
        float x0 = a.x + U(3), y = a.y + U(2.5f);
        Tx(F_HEAD, ImVec2(x0, y), C(kGrove), "ТЕЛЕФОН");
        if(Button(ImVec2(b.x - U(12), y), ImVec2(b.x - U(3), y + U(5.5f)), "X", kRed)) { g_phoneOpen = false; return; }
        y += U(8);
        static const char* tabs[] = { "Контакты", "Новости", "Люди" };
        float tw = (b.x - a.x - U(6) - U(2)) / 3;
        for(int i = 0; i < 3; ++i)
        {
            ImVec2 ta(x0 + i * (tw + U(1)), y), tb(ta.x + tw, y + U(5.5f));
            bool on = g_phoneTab == i;
            DL()->AddRectFilled(ta, tb, C(on ? kGold : kPanel2), U(1));
            ImVec2 ts = TS(F_SMALL, tabs[i]);
            Tx(F_SMALL, ImVec2((ta.x + tb.x - ts.x) * 0.5f, (ta.y + tb.y - ts.y) * 0.5f), C(on ? kNight : kInk), tabs[i]);
            if(Clicked(ta, tb)) g_phoneTab = i;
        }
        y += U(8);
        float wrap = b.x - x0 - U(3);
        DL()->PushClipRect(ImVec2(a.x, y), ImVec2(b.x, b.y - U(2)), true);
        auto& W2 = world::W();
        if(g_phoneTab == 0)
        {
            int n = 0;
            for(auto& c : W2.chars)
            {
                const world::Relation& r = c.rel[world::WHO_CJ];
                if(!r.contact || !c.alive) continue;
                ImVec2 ra(x0, y), rb(b.x - U(3), y + U(9));
                DL()->AddRectFilled(ra, rb, C(kPanel2), U(1));
                std::string title = c.nick.empty() ? c.name : c.nick + " (" + c.name + ")";
                Tx(F_BODY, ImVec2(ra.x + U(1.5f), ra.y + U(0.8f)), C(kInk), title.c_str());
                std::string sub = Fmt("%s%s%s", world::FactionName(c.faction), r.payroll ? " · на зарплате" : "", r.informant ? " · информатор" : "");
                Tx(F_SMALL, ImVec2(ra.x + U(1.5f), ra.y + U(5)), C(kDim), sub.c_str());
                if(Clicked(ra, rb)) { talk::StartPhoneCall(c.id); g_phoneOpen = false; }
                y += U(10); ++n;
            }
            if(!n) Tx(F_SMALL, ImVec2(x0, y), C(kDim), "Пока никто не дал номер. Знакомься, договаривайся — люди сами дадут номер.", wrap);
        }
        else if(g_phoneTab == 1)
        {
            for(auto it = W2.news.rbegin(); it != W2.news.rend(); ++it)
            {
                ImVec2 ts = TS(F_SMALL, it->c_str(), wrap);
                Tx(F_SMALL, ImVec2(x0, y), C(kInk), it->c_str(), wrap);
                y += ts.y + U(1.5f);
            }
            if(W2.news.empty()) Tx(F_SMALL, ImVec2(x0, y), C(kDim), "Новостей нет.", wrap);
        }
        else
        {
            for(auto& c : W2.chars)
            {
                const world::Relation& r = c.rel[world::WHO_CJ];
                if(!r.met) continue;
                std::string line = Fmt("%s — %s%s. Доверие %d, уважение %d, симпатия %d, страх %d", c.nick.empty() ? c.name.c_str() : c.nick.c_str(),
                                       world::FactionName(c.faction), c.alive ? "" : " (погиб)", r.trust, r.respect, r.liking, r.fear);
                ImVec2 ts = TS(F_SMALL, line.c_str(), wrap);
                ImVec2 ra(x0 - U(0.8f), y - U(0.4f)), rb(b.x - U(3), y + ts.y + U(0.4f));
                if(HeldFrom(ra, rb)) DL()->AddRectFilled(ra, rb, C(kLine), U(0.8f));
                Tx(F_SMALL, ImVec2(x0, y), C(c.alive ? kInk : kDim), line.c_str(), wrap);
                if(Clicked(ra, rb)) { OpenCard(c.id); g_phoneOpen = false; }
                y += ts.y + U(1.2f);
            }
            Tx(F_SMALL, ImVec2(x0, y + U(0.5f)), C(kGold), "Нажми на человека — карточка и выбор голоса"); y += U(4.5f);
            std::string fs = "Банды: " + world::FactionSummary(world::WHO_CJ);
            Tx(F_SMALL, ImVec2(x0, y + U(1)), C(kDim), fs.c_str(), wrap);
        }
        DL()->PopClipRect();
    }

    // ---------- сцены полиции ----------
    void DrawScene()
    {
        const police::Scene& sc = police::CurrentScene();
        float W = Ww(), H = Hh();
        DL()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), C(kBlack, 0.88f));
        float x0 = W * 0.15f, wrap = W * 0.7f, y = H * 0.12f;
        Tx(F_HEAD, ImVec2(x0, y), C(kGold), sc.title.c_str());
        y += U(9);
        Tx(F_BODY, ImVec2(x0, y), C(kInk), sc.waitingAi ? "…" : sc.text.c_str(), wrap);
        float by = H * 0.82f;
        if(sc.canLawyer && Button(ImVec2(x0, by), ImVec2(x0 + U(38), by + U(7)), "Позвонить адвокату / своему", kBlue)) police::SceneCallHelp();
        if(!sc.waitingAi && Button(ImVec2(W * 0.85f - U(24), by), ImVec2(W * 0.85f, by + U(7)), "Дальше", kGrove)) police::SceneContinue();
    }

    // ---------- слой поверх игры ----------
    void DrawTalkButton(const settings::Data& s)
    {
        float r = U(7.5f) * s.talkS;
        ImVec2 c(s.talkX * Ww(), s.talkY * Hh());
        ImVec2 a(c.x - r, c.y - r), b(c.x + r, c.y + r);
        bool held = HeldFrom(a, b);
        g_talkDrawn = true;
        if(held != g_talkHeld) { g_talkHeld = held; if(held) talk::PttDown(); else talk::PttUp(); }
        bool toFriend = talk::TalkingToFriend();                       // кооп: говоришь другу (голосовой чат), людям не распознаём
        bool listening = (talk::Listening() && !talk::Capturing()) || toFriend, thinking = !toFriend && (talk::Thinking() || (talk::Recognizing() && !talk::Capturing()));
        Rgb ring = listening ? kRed : (thinking ? kGold : kBlue);
        DL()->AddCircleFilled(ImVec2(c.x, c.y + U(0.6f)), r, C(kBlack, 0.45f), 40);
        DL()->AddCircleFilled(c, r - U(0.4f), C(kPanel, 0.9f), 40);
        float t = (float)g_now;
        if(listening) DL()->AddCircle(c, r * (1.05f + 0.25f * fmodf(t * 1.6f, 1.0f)), C(kRed, 1.0f - fmodf(t * 1.6f, 1.0f)), 40, U(0.5f));
        if(thinking) for(int i = 0; i < 3; ++i) DL()->AddCircleFilled(ImVec2(c.x - U(2.2f) + i * U(2.2f), c.y + U(0.3f) * sinf(t * 6 + i)), U(0.7f), C(kGold), 12);
        DL()->AddCircle(c, r - U(0.5f), C(ring), 40, U(0.6f));
        // облачко речи
        if(!thinking)
        {
            ImVec2 ba(c.x - r * 0.45f, c.y - r * 0.35f), bb(c.x + r * 0.45f, c.y + r * 0.2f);
            DL()->AddRectFilled(ba, bb, C(kInk), U(1));
            DL()->AddTriangleFilled(ImVec2(c.x - r * 0.2f, bb.y), ImVec2(c.x, bb.y), ImVec2(c.x - r * 0.3f, bb.y + r * 0.25f), C(kInk));
        }
        const char* lbl = toFriend ? "ДРУГУ" : listening ? "ГОВОРИ" : (talk::PhoneMode() ? "ТРУБКА" : "ГОВОРИТЬ");
        ImVec2 ts = TS(F_SMALL, lbl);
        TxShadow(F_SMALL, ImVec2(c.x - ts.x * 0.5f, b.y + U(0.5f)), C(listening ? kRed : kInk), lbl);
        // кому говорим
        if(world::Character* ch = talk::Current())
        {
            std::string who = (talk::PhoneMode() ? "Звонок: " : "") + (ch->nick.empty() ? ch->name : ch->nick);
            ImVec2 ws = TS(F_SMALL, who.c_str());
            TxShadow(F_SMALL, ImVec2(c.x - ws.x * 0.5f, a.y - ws.y - U(0.5f)), C(kGold), who.c_str());
        }
    }

    void DrawPhoneButton(const settings::Data& s)
    {
        float w = U(11) * s.phoneS, h = U(6) * s.phoneS;
        ImVec2 c(s.phoneX * Ww(), s.phoneY * Hh());
        ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
        if(Button(a, b, "Телефон", kGrove)) g_phoneOpen = !g_phoneOpen;
        // «Метка»: заметил странное — нажми, в подробный лог ляжет отметка и полный снимок всего вокруг
        float mw = TS(F_SMALL, "Метка").x + U(2.4f), mh = U(4.6f) * s.phoneS;
        ImVec2 ma(a.x - mw - U(1.5f), c.y - mh * 0.5f), mb(a.x - U(1.5f), c.y + mh * 0.5f);
        if(Button(ma, mb, "Метка", kRed)) { trace::Mark("кнопка «Метка»"); talk::Notify("Метка записана в подробный лог"); }
    }

    void DrawContextButtons()
    {
        float W = Ww(), H = Hh();
        std::vector<std::pair<std::string, int>> btn;   // текст, действие
        if(talk::InConversation())
        {
            btn.push_back({ "Дать денег", 1 });
            if(int d = talk::PendingDemand()) btn.push_back({ Fmt("Заплатить $%d", d), 2 });
            btn.push_back({ talk::PhoneMode() ? "Положить трубку" : "Уйти", 3 });
            btn.push_back({ "Карточка", 5 });
        }
        if(game::Wanted() > 0 && !police::Arresting()) btn.push_back({ "Сдаться", 4 });
        float bw = U(20), bh = U(6), y = H * 0.86f, x = W * 0.5f - (btn.size() * (bw + U(1))) * 0.5f;
        for(auto& b : btn)
        {
            ImVec2 a(x, y), e(x + bw, y + bh);
            Rgb col = b.second == 4 ? kRed : (b.second == 2 ? kGold : kBlue);
            if(Button(a, e, b.first.c_str(), col))
            {
                if(b.second == 1) { g_keypad = true; g_amount.clear(); }
                else if(b.second == 2) talk::PayDemand();
                else if(b.second == 3) talk::EndConversation();
                else if(b.second == 4) police::Surrender();
                else if(b.second == 5) { if(world::Character* ch = talk::Current()) OpenCard(ch->id); }
            }
            x += bw + U(1);
        }
    }

    void DrawSubtitles()
    {
        float W = Ww(), H = Hh();
        float y = H * 0.80f, wrap = W * 0.6f;
        if((talk::Listening() || talk::Recognizing()) && !talk::Capturing())
        {
            std::string s = talk::Recognizing() ? "Распознаю…" : (talk::Partial().empty() ? "Слушаю…" : "Ты: " + talk::Partial() + "…");
            ImVec2 ts = TS(F_BODY, s.c_str(), wrap);
            TxShadow(F_BODY, ImVec2((W - ts.x) * 0.5f, y - ts.y), C(kDim), s.c_str(), wrap);
            y -= ts.y + U(1);
        }
        const auto& subs = talk::Subs();
        double nowS = talk::Now();
        for(auto it = subs.rbegin(); it != subs.rend(); ++it)
        {
            if(it->from > nowS) continue;          // эта часть длинной речи ещё впереди
            std::string s = it->who + ": " + it->text;
            ImVec2 ts = TS(F_BODY, s.c_str(), wrap);
            y -= ts.y;
            DL()->AddRectFilled(ImVec2((W - ts.x) * 0.5f - U(1), y - U(0.4f)), ImVec2((W + ts.x) * 0.5f + U(1), y + ts.y + U(0.4f)), C(kBlack, 0.45f), U(0.8f));
            TxShadow(F_BODY, ImVec2((W - ts.x) * 0.5f, y), C(it->player ? kGrove : kInk), s.c_str(), wrap);
            y -= U(1.2f);
            if(y < H * 0.45f) break;
        }
        // имя над головой собеседника
        if(uintptr_t ped = talk::CurrentPed())
        {
            game::Vec p = game::Pos(ped); p.z += 1.15f;
            float sx, sy;
            if(world::Character* ch = talk::Current())
                if(game::WorldToScreen(p, sx, sy))
                {
                    std::string n = ch->nick.empty() ? ch->name : ch->nick;
                    ImVec2 ts = TS(F_SMALL, n.c_str());
                    TxShadow(F_SMALL, ImVec2(sx - ts.x * 0.5f, sy - ts.y), C(kGold), n.c_str());
                }
        }
    }

    void DrawFeed()
    {
        std::string s;
        while(talk::PopNotify(s)) Feed(s);
        float W = Ww(), y = U(3);
        for(auto it = g_feed.begin(); it != g_feed.end();)
        {
            float age = (float)(g_now - it->second);
            if(age > 6) { it = g_feed.erase(it); continue; }
            float a = age < 0.3f ? age / 0.3f : (age > 5 ? 6 - age : 1);
            ImVec2 ts = TS(F_SMALL, it->first.c_str(), W * 0.5f);
            ImVec2 pa((W - ts.x) * 0.5f - U(2), y), pb((W + ts.x) * 0.5f + U(2), y + ts.y + U(2));
            DL()->AddRectFilled(pa, pb, C(kPanel, 0.9f * a), U(1.2f));
            DL()->AddRect(pa, pb, C(kGold, 0.8f * a), U(1.2f), 0, U(0.2f));
            Tx(F_SMALL, ImVec2(pa.x + U(2), pa.y + U(1)), C(kInk, a), it->first.c_str(), W * 0.5f);
            y = pb.y + U(1);
            ++it;
        }
        const char* st = police::ArrestStage();
        if(st && *st)
        {
            ImVec2 ts = TS(F_BODY, st);
            TxShadow(F_BODY, ImVec2((W - ts.x) * 0.5f, Hh() * 0.18f), C(kRed), st);
        }
    }

    // ---------- Джарвис: кнопка и плашка ----------
    bool g_jarvisHeld = false;
    void DrawJarvis(const settings::Data& s)
    {
        if(!s.jarvis) return;
        jarvis::View v = jarvis::GetView();
        if(s.jarvisButton)
        {
            float r = U(6.2f) * s.jarvisS;
            ImVec2 c(s.jarvisX * Ww(), s.jarvisY * Hh());
            ImVec2 a(c.x - r, c.y - r), b(c.x + r, c.y + r);
            bool held = HeldFrom(a, b);
            if(held != g_jarvisHeld) { g_jarvisHeld = held; if(held) jarvis::ButtonDown(); else jarvis::ButtonUp(); }
            bool active = v.visible && v.state != jarvis::IDLE && v.state != jarvis::SPEAKING;
            float t = (float)g_now;
            DL()->AddCircleFilled(ImVec2(c.x, c.y + U(0.5f)), r, C(kBlack, 0.45f), 40);
            DL()->AddCircleFilled(c, r - U(0.4f), C(kPanel, 0.9f), 40);
            DL()->AddCircle(c, r - U(0.5f), C(held ? kRed : kGold), 40, U(0.55f));
            // «реактор»: кольцо из сегментов
            for(int i = 0; i < 8; ++i)
            {
                float ang = i * 0.7854f + (active ? t * 2.0f : 0);
                ImVec2 p(c.x + cosf(ang) * r * 0.48f, c.y + sinf(ang) * r * 0.48f);
                DL()->AddCircleFilled(p, r * 0.09f, C(kGold, active ? 1.0f : 0.75f), 10);
            }
            DL()->AddCircleFilled(c, r * 0.18f, C(held ? kRed : kGold), 16);
            ImVec2 ts = TS(F_SMALL, "ДЖАРВИС");
            TxShadow(F_SMALL, ImVec2(c.x - ts.x * 0.5f, b.y + U(0.4f)), C(kGold), "ДЖАРВИС");
        }
        if(!v.visible) return;
        float W = Ww(), wrap = W * 0.46f;
        const char* stt = v.state == jarvis::LISTENING ? (v.listenLeft > 0 ? "Слушаю… (коснись, когда договоришь)" : "Слушаю…")
                        : v.state == jarvis::RECOGNIZING ? "Распознаю…" : v.state == jarvis::THINKING ? "Секунду…" : "";
        std::string body;
        if(!v.heard.empty()) body += "Ты: " + v.heard + "\n";
        if(!v.reply.empty()) body += v.reply;
        float h = U(5.5f);
        ImVec2 bs = body.empty() ? ImVec2(0, 0) : TS(F_SMALL, body.c_str(), wrap);
        h += bs.y + (body.empty() ? 0 : U(1));
        h += v.done.size() * U(3.4f) + (v.done.empty() ? 0 : U(0.6f));
        ImVec2 a((W - wrap) * 0.5f - U(2), U(12)), b((W + wrap) * 0.5f + U(2), U(12) + h + U(1.5f));
        DL()->AddRectFilled(a, b, C(kPanel, 0.93f), U(1.6f));
        DL()->AddRect(a, b, C(kGold, 0.9f), U(1.6f), 0, U(0.3f));
        float y = a.y + U(1.2f);
        Tx(F_SMALL, ImVec2(a.x + U(2), y), C(kGold), "ДЖАРВИС");
        if(*stt) { ImVec2 ts = TS(F_SMALL, stt); Tx(F_SMALL, ImVec2(b.x - U(2) - ts.x, y), C(v.state == jarvis::LISTENING ? kRed : kDim), stt); }
        y += U(4.2f);
        if(!body.empty()) { Tx(F_SMALL, ImVec2(a.x + U(2), y), C(kInk), body.c_str(), wrap); y += bs.y + U(1); }
        for(auto& d : v.done) { Tx(F_SMALL, ImVec2(a.x + U(2), y), C(kGrove), ("· " + d).c_str(), wrap); y += U(3.4f); }
        if(Clicked(a, b)) jarvis::TapPlate();
    }

    bool Frame(bool fromMenu)
    {
        uint64_t nowUs = (uint64_t)(NowSec() * 1e6);
        float dt = g_lastUs ? (float)(nowUs - g_lastUs) / 1e6f : 1.0f / 60.0f;
        if(dt > 0.1f) dt = 0.1f;
        g_lastUs = nowUs; g_now = NowSec();
        imgui_rw::NewFrame(dt);
        if(!g_fontsBuilt) { float h = ImGui::GetIO().DisplaySize.y; if(h < 100) return false; BuildFonts(h); if(!g_fontsBuilt) return false; }
        if(!imgui_rw::EnsureDeviceObjects()) return false;
        PollTouches();
        ImGui::NewFrame();
        if(fromMenu)
        {
            if(g_menuOpen) DrawMenu();
        }
        else if(settings::Get().enabled && game::Player() && !game::CutsceneRunning())
        {
            const police::Scene& sc = police::CurrentScene();
            if(sc.kind != police::SCENE_NONE)
            {
                DrawScene();
                bool speak = sc.canSpeak;
                talk::SetSpeechSink(speak ? [](const std::string& t) { police::SceneSpeak(t); } : std::function<void(const std::string&)>());
                if(speak) DrawTalkButton(settings::Get());
            }
            else
            {
                talk::SetSpeechSink(nullptr);
                if(g_cardChar) DrawCard();
                else if(g_keypad) DrawKeypad();
                else
                {
                    DrawJarvis(settings::Get());
                    DrawSubtitles();
                    DrawContextButtons();
                    DrawTalkButton(settings::Get());
                    DrawPhoneButton(settings::Get());
                    if(g_phoneOpen) DrawPhone();
                }
            }
            DrawFeed();
        }
        // Кнопку перестали показывать, пока её держали (катсцена, меню, смерть) — отпустить: иначе микрофон
        // (в коопе — голос другу) так и остался бы включённым
        if(!fromMenu)
        {
            if(!g_talkDrawn && g_talkHeld) { g_talkHeld = false; talk::PttUp(); }
            g_talkDrawn = false;
        }
        imgui_rw::Render();
        return true;
    }

    void (*g_frameFn)() = nullptr;
}

// ---------- перехваты ----------
SDECL_HOOKv(Render2dStuffH)
{
    Render2dStuffH();
    WHERE("экран: кнопки и субтитры");
    Frame(false);
    WHERE("игра (не мод)");
}

SDECL_HOOKv(MobileMenuRenderH, uintptr_t self)
{
    MobileMenuRenderH(self);
    if(g_menuOpen) { WHERE("меню AI"); Frame(true); WHERE("игра (не мод)"); }
}

SDECL_HOOKv(MobileMenuUpdateH, uintptr_t self)
{
    double now = NowSec();
    if(g_menuOpen) { g_blockMenuUntil = now + 0.3; return; }
    if(now < g_blockMenuUntil || (g_blockMenuUntil > 0 && AnyDown())) { if(AnyDown()) g_blockMenuUntil = now + 0.15; return; }
    g_blockMenuUntil = 0;
    MobileMenuUpdateH(self);
}

static void OnMenuButton(void*) { OpenMenu(); }

void OpenMenu()
{
    g_menuOpen = true;
    g_confirmReset = false;
    for(auto& f : g_f) f = Finger();
}

bool Init(void* h)
{
    if(!imgui_rw::Init(h)) { LOG_ERROR("Интерфейс: нет функций RenderWare"); return false; }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr; io.DisplaySize = ImVec2(1920, 1080);
    g_pointsOrig = (GamePoint*)aml->GetSym(h, "Points");
    uintptr_t base = aml->GetLib("libGTASA.so");
    if(base && g_pointsOrig)
    {
        GamePoint** got = (GamePoint**)(base + kPointsGot);
        if(*got == g_pointsOrig || (*got && (uintptr_t)*got != base)) g_pointsGot = got;
    }
    void* r2d = (void*)aml->GetSym(h, "_Z13Render2dStuffv");
    void* render = (void*)aml->GetSym(h, "_ZN10MobileMenu6RenderEv");
    void* update = (void*)aml->GetSym(h, "_ZN10MobileMenu6UpdateEv");
    if(!r2d || !render || !update || !g_pointsOrig) { LOG_ERROR("Интерфейс: не найдены функции игры"); return false; }
    aml->Hook(r2d, (void*)HookOf_Render2dStuffH, (void**)&Render2dStuffH);
    aml->Hook(render, (void*)HookOf_MobileMenuRenderH, (void**)&MobileMenuRenderH);
    aml->Hook(update, (void*)HookOf_MobileMenuUpdateH, (void**)&MobileMenuUpdateH);
    LOG_INFO("Интерфейс готов");
    return true;
}

#ifdef SALIVING_PREVIEW
void PreviewFrame(bool menu, int tab) { g_menuOpen = menu; g_tab = tab; Frame(menu); }
void PreviewCard(int id) { OpenCard(id); }
void PreviewPhone(bool open, int tab) { g_phoneOpen = open; g_phoneTab = tab; }
#endif

void AttachSAUtils(ISAUtils* sau)
{
    sau->AddSettingsTabButton("AI", OnMenuButton, STB_Settings, "menu_mainsettings", nullptr);
    LOG_INFO("Плитка «AI» добавлена в настройки игры");
}

} // namespace hud
