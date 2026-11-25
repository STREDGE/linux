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
#include "sim/police.h"
#include "talk/talk.h"
#include "ui/fonts_data.h"
#include "ui/imgui_rw.h"
#include "util/log.h"
#include "world/world.h"

using world::Fmt;

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
    bool Clicked(ImVec2 a, ImVec2 b) { for(auto& f : g_f) if(f.released && In(f.downPos, a, b) && In(f.pos, a, b)) return true; return false; }

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
        return enabled && Clicked(a, b);
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

    void DrawMenu()
    {
        settings::Data& s = settings::Get();
        float W = Ww(), H = Hh();
        DL()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), C(kNight, 0.93f));
        float x0 = W * 0.12f, x1 = W * 0.88f;
        float y = U(4);
        Tx(F_HEAD, ImVec2(x0, y), C(kGold), "ИИ — ЖИВЫЕ ЛЮДИ");
        ImVec2 cb(x1 - U(14), y), ce(x1, y + U(6));
        if(Button(cb, ce, "Закрыть", kGrove)) { settings::Save(); g_menuOpen = false; g_blockMenuUntil = g_now + 0.4; return; }
        y += U(9);

        // ключ
        Panel(ImVec2(x0 - U(2), y - U(1.5f)), ImVec2(x1 + U(2), y + U(15)));
        std::string masked = claude::HasKey() ? claude::MaskedKey() : std::string("не вставлен");
        Row(y, x0, x1, "Ключ Anthropic (свой, в лог не пишется)", masked.c_str()); y += U(5.5f);
        float bw = (x1 - x0 - U(2)) / 3;
        if(Button(ImVec2(x0, y), ImVec2(x0 + bw, y + U(6)), "Вставить из буфера", kGold))
        {
            char buf[512] = "";
            if(platform::ClipboardGetText(buf, sizeof(buf)))
            {
                std::string k = buf; while(!k.empty() && (k.back() == ' ' || k.back() == '\n' || k.back() == '\r')) k.pop_back();
                while(!k.empty() && (k[0] == ' ' || k[0] == '\n')) k.erase(0, 1);
                if(k.rfind("sk-", 0) == 0 && k.size() > 20) { s.key = k; claude::Settings cs = claude::Get(); cs.key = k; claude::Configure(cs); settings::Save(); MenuToast("Ключ вставлен — нажми «Проверить»"); }
                else MenuToast("В буфере нет ключа (должен начинаться с sk-)");
            }
            else MenuToast("Буфер обмена пуст");
        }
        if(Button(ImVec2(x0 + bw + U(1), y), ImVec2(x0 + 2 * bw + U(1), y + U(6)), g_checkReq ? "Проверяю…" : "Проверить", kGrove, claude::HasKey() && !g_checkReq)) CheckKey();
        if(Button(ImVec2(x0 + 2 * bw + U(2), y), ImVec2(x1, y + U(6)), "Удалить", kRed, claude::HasKey()))
        { s.key.clear(); claude::Settings cs = claude::Get(); cs.key.clear(); claude::Configure(cs); settings::Save(); MenuToast("Ключ удалён"); }
        y += U(10);

        // настройки
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
        int nl = Chips(y, x0, x1, "Лимит запросов в день", limits, 3, li);
        if(nl != li) { static const int lv[] = { 100, 400, 1500 }; s.dailyLimit = lv[nl]; claude::SetDailyLimit(s.dailyLimit); }
        Toggle(y, x0, x1, "Голоса педов", s.voices);
        Toggle(y, x0, x1, "Субтитры", s.subtitles);
        Toggle(y, x0, x1, "Задержание по шагам (наручники, участок, суд)", s.realArrest);
        Toggle(y, x0, x1, "Розыск только по свидетелям", s.witnesses);
        Toggle(y, x0, x1, "Режиссёр города (планы банд, слухи)", s.director);
        Toggle(y, x0, x1, "Кооп: друг говорит через мой ключ", s.shareKeyWithFriend);

        // расход
        claude::Usage t = claude::Today(), all = claude::Total();
        std::string us = Fmt("сегодня %d запр. ≈ $%.3f · всего %d ≈ $%.2f", t.requests, claude::CostUsd(t), all.requests, claude::CostUsd(all));
        Row(y, x0, x1, "Расход", us.c_str()); y += U(5.5f);
        const char* mode = coopnet::GetRole() == coopnet::HOST ? "кооп: ты хост (общий мир)" : coopnet::GetRole() == coopnet::CLIENT ? "кооп: ИИ через хоста" : "одиночная игра";
        Row(y, x0, x1, "Режим", mode); y += U(6);

        if(!g_confirmReset) { if(Button(ImVec2(x0, y), ImVec2(x0 + U(30), y + U(6)), "Новый мир (забыть всех)", kRed)) g_confirmReset = true; }
        else
        {
            Tx(F_SMALL, ImVec2(x0, y + U(1.5f)), C(kRed), "Точно? Все люди, связи и слухи будут забыты.");
            if(Button(ImVec2(x1 - U(40), y), ImVec2(x1 - U(21), y + U(6)), "Да, забыть", kRed)) { world::Reset(); world::Save(world::DefaultPath()); g_confirmReset = false; MenuToast("Мир начат заново"); }
            if(Button(ImVec2(x1 - U(19), y), ImVec2(x1, y + U(6)), "Нет", kGrove)) g_confirmReset = false;
        }
        if(g_now - g_menuToastAt < 4)
        {
            ImVec2 ts = TS(F_BODY, g_menuToast.c_str(), W * 0.7f);
            ImVec2 a((W - ts.x) * 0.5f - U(2), H - ts.y - U(6)), b((W + ts.x) * 0.5f + U(2), H - U(2));
            Panel(a, b); Tx(F_BODY, ImVec2(a.x + U(2), a.y + U(2)), C(kGold), g_menuToast.c_str(), W * 0.7f);
        }
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
                Tx(F_SMALL, ImVec2(x0, y), C(c.alive ? kInk : kDim), line.c_str(), wrap);
                y += ts.y + U(1.2f);
            }
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
        if(held != g_talkHeld) { g_talkHeld = held; if(held) talk::PttDown(); else talk::PttUp(); }
        bool listening = talk::Listening(), thinking = talk::Thinking();
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
        const char* lbl = listening ? "ГОВОРИ" : (talk::PhoneMode() ? "ТРУБКА" : "ГОВОРИТЬ");
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
        }
        if(game::Wanted() > 0 && !police::Arresting()) btn.push_back({ "Сдаться", 4 });
        float bw = U(20), bh = U(6), y = H * 0.70f, x = W * 0.5f - (btn.size() * (bw + U(1))) * 0.5f;
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
            }
            x += bw + U(1);
        }
    }

    void DrawSubtitles()
    {
        float W = Ww(), H = Hh();
        float y = H * 0.80f, wrap = W * 0.6f;
        if(talk::Listening() && !talk::Partial().empty())
        {
            std::string s = "Ты: " + talk::Partial() + "…";
            ImVec2 ts = TS(F_BODY, s.c_str(), wrap);
            TxShadow(F_BODY, ImVec2((W - ts.x) * 0.5f, y - ts.y), C(kDim), s.c_str(), wrap);
            y -= ts.y + U(1);
        }
        const auto& subs = talk::Subs();
        for(auto it = subs.rbegin(); it != subs.rend(); ++it)
        {
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
                if(g_keypad) DrawKeypad();
                else
                {
                    DrawSubtitles();
                    DrawContextButtons();
                    DrawTalkButton(settings::Get());
                    DrawPhoneButton(settings::Get());
                    if(g_phoneOpen) DrawPhone();
                }
            }
            DrawFeed();
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
    Frame(false);
}

SDECL_HOOKv(MobileMenuRenderH, uintptr_t self)
{
    MobileMenuRenderH(self);
    if(g_menuOpen) Frame(true);
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

void AttachSAUtils(ISAUtils* sau)
{
    sau->AddSettingsTabButton("AI", OnMenuButton, STB_Settings, "menu_mainsettings", nullptr);
    LOG_INFO("Плитка «AI» добавлена в настройки игры");
}

} // namespace hud
