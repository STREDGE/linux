#include "overlay.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>
#include <imgui/imgui.h>
#include <sautils/isautils.h>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "coop/coop.h"
#include "platform/android.h"
#include "settings.h"
#include "ui/fonts_data.h"
#include "ui/imgui_rw.h"

namespace overlay
{
namespace
{
    // ---------- палитра макета (docs/design/coop-menu.html) ----------
    struct Rgb { uint8_t r, g, b; };
    constexpr Rgb kNight{ 7, 9, 13 }, kPanel{ 15, 20, 27 }, kPanel2{ 22, 29, 39 }, kLine{ 38, 50, 67 };
    constexpr Rgb kInk{ 238, 241, 230 }, kDim{ 138, 150, 166 }, kGrove{ 79, 209, 90 }, kGroveDeep{ 29, 107, 42 };
    constexpr Rgb kGold{ 242, 193, 78 }, kRed{ 255, 77, 77 }, kBlack{ 0, 0, 0 }, kWhite{ 255, 255, 255 };

    float g_alpha = 1.0f; // общая прозрачность (анимация открытия/закрытия)

    ImU32 C(Rgb c, float a = 1.0f)
    {
        float aa = a * g_alpha;
        if(aa < 0) aa = 0; if(aa > 1) aa = 1;
        return IM_COL32(c.r, c.g, c.b, (int)(aa * 255.0f));
    }

    // ---------- шрифты ----------
    enum FontId { F_TITLE, F_HEAD, F_BODY, F_SMALL, F_MONO, F_KEY, F_COUNT };
    ImFont* g_fonts[F_COUNT] = {};
    bool    g_fontsBuilt = false;

    const ImWchar kRangesLatin[] = { 0x0020, 0x007E, 0 };
    const ImWchar kRangesText[]  = { 0x0020, 0x00FF, 0x0400, 0x045F, 0x2010, 0x2026, 0x2116, 0x2116,
                                     0x2190, 0x2193, 0x2713, 0x2715, 0 };

    void BuildFonts(float H)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        io.Fonts->TexDesiredWidth = 2048;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH = 1; cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        auto add = [&](const unsigned char* data, size_t size, float px, const ImWchar* ranges) {
            return io.Fonts->AddFontFromMemoryTTF((void*)data, (int)size, px, &cfg, ranges);
        };
        g_fonts[F_TITLE] = add(kFontRussoOne,    kFontRussoOneSize,    H * 0.110f, kRangesLatin);
        g_fonts[F_HEAD]  = add(kFontRussoOne,    kFontRussoOneSize,    H * 0.052f, kRangesText);
        g_fonts[F_BODY]  = add(kFontRubikMedium, kFontRubikMediumSize, H * 0.034f, kRangesText);
        g_fonts[F_SMALL] = add(kFontRubikMedium, kFontRubikMediumSize, H * 0.027f, kRangesText);
        g_fonts[F_MONO]  = add(kFontMonoBold,    kFontMonoBoldSize,    H * 0.068f, kRangesText);
        g_fonts[F_KEY]   = add(kFontMonoBold,    kFontMonoBoldSize,    H * 0.045f, kRangesText);
        g_fontsBuilt = io.Fonts->Build();
    }

    // ---------- касания ----------
    // Читаем опросом из массива игры Points (2 пальца по 0x1C байт: x, y, состояние 2 = нажат),
    // а НЕ перехватом AND_TouchEvent: другие моды (например, helibrain) проверяют код касаний
    // и выключаются, если он изменён.
    struct GamePoint { int32_t x, y, state; uint8_t rest[0x1C - 12]; };
    static_assert(sizeof(GamePoint) == 0x1C, "Points");
    // Массив берём через ячейку GOT, как сама игра (AND_TouchEvent): мод Touch x4
    // подменяет её на свой массив на 4 пальца, и тогда исходный Points не обновляется.
    constexpr uintptr_t kPointsGot = 0x851D38;   // GOT-ячейка «Points» в libGTASA 2.10 arm64
    GamePoint*  g_pointsOrig = nullptr;
    GamePoint** g_pointsGot  = nullptr;
    GamePoint*  g_points     = nullptr;          // текущий массив (обновляется каждый кадр)
    int         g_pointsN    = 2;

    void RefreshPoints()
    {
        GamePoint* p = g_pointsGot ? *g_pointsGot : nullptr;
        g_points  = p ? p : g_pointsOrig;
        g_pointsN = (g_points && g_points != g_pointsOrig) ? 4 : 2; // чужой массив — Touch x4, 4 пальца
    }

    struct Pointer
    {
        bool  down = false;
        int   finger = -1;
        ImVec2 pos{ -1, -1 }, downPos{ -1, -1 };
        bool  pressed = false, released = false; // в этом кадре
    } g_ptr;

    void PollTouches()
    {
        g_ptr.pressed = g_ptr.released = false;
        RefreshPoints();
        if(!g_points) return;
        if(g_ptr.down)
        {
            const GamePoint& p = g_points[g_ptr.finger];
            if(p.state == 2) { if(p.x >= 0) g_ptr.pos = ImVec2((float)p.x, (float)p.y); }
            else { g_ptr.down = false; g_ptr.released = true; g_ptr.finger = -1; }
            return;
        }
        for(int f = 0; f < g_pointsN; ++f)
        {
            const GamePoint& p = g_points[f];
            if(p.state == 2 && p.x >= 0)
            {
                g_ptr.down = true; g_ptr.finger = f; g_ptr.pressed = true;
                g_ptr.pos = g_ptr.downPos = ImVec2((float)p.x, (float)p.y);
                break;
            }
        }
    }

    bool AnyFingerDown()
    {
        RefreshPoints();
        if(!g_points) return false;
        for(int f = 0; f < g_pointsN; ++f) if(g_points[f].state == 2) return true;
        return false;
    }

    bool In(const ImVec2& p, const ImVec2& a, const ImVec2& b) { return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y; }
    // Кнопка: нажата сейчас (палец на ней с момента касания)
    bool Held(ImVec2 a, ImVec2 b)    { return g_ptr.down && In(g_ptr.downPos, a, b) && In(g_ptr.pos, a, b); }
    // Клик: палец отпущен на той же кнопке, где коснулся
    bool Clicked(ImVec2 a, ImVec2 b) { return g_ptr.released && In(g_ptr.downPos, a, b) && In(g_ptr.pos, a, b); }

    // ---------- состояние меню ----------
    FLogger*  g_log = nullptr;
    bool      g_open = false, g_closing = false;
    double    g_openedAt = 0, g_closeAt = 0;
    double    g_now = 0;
    uint64_t  g_lastUs = 0;

    net::Role g_sel = net::Role::Off;   // выбранный в меню режим (применяется кнопкой)
    double    g_selAt = 0;              // время выбора — для блика
    std::string g_ip;                   // ввод адреса хоста
    double    g_ipShakeAt = -10;
    settings::Data g_settings;
    float     g_ffKnob = 0;             // 0..1 положение тумблера

    char      g_toast[96] = "";
    double    g_toastAt = -10;

    char      g_myIp[32] = "";
    bool      g_myIpOk = false;
    double    g_myIpAt = -10;

    struct Ripple { ImVec2 c; double t; ImVec2 a, b; };
    std::vector<Ripple> g_ripples;

    double NowSec()
    {
        timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
    }
    float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    float EaseOut(float t) { t = Clamp01(t); return 1 - powf(1 - t, 3); }
    float EaseBack(float t) { t = Clamp01(t); const float c1 = 1.70158f, c3 = c1 + 1; return 1 + c3 * powf(t - 1, 3) + c1 * powf(t - 1, 2); }

    void Toast(const char* s) { snprintf(g_toast, sizeof(g_toast), "%s", s); g_toastAt = g_now; }

    bool ValidIp(const std::string& s)
    {
        int parts = 0, val = -1;
        for(size_t i = 0; i <= s.size(); ++i)
        {
            if(i == s.size() || s[i] == '.')
            {
                if(val < 0 || val > 255) return false;
                ++parts; val = -1;
            }
            else val = (val < 0 ? 0 : val * 10) + (s[i] - '0');
        }
        return parts == 4;
    }

    // ---------- рисование ----------
    ImDrawList* DL() { return ImGui::GetForegroundDrawList(); }
    float H() { return ImGui::GetIO().DisplaySize.y; }
    float W() { return ImGui::GetIO().DisplaySize.x; }
    float U(float v)  { return v * H() / 100.0f; }   // доля высоты экрана, как cqh в макете
    float CW(float v) { return v * W() / 100.0f; }   // доля ширины, как cqw

    ImVec2 TextSize(FontId f, const char* s, float wrap = -1.0f)
    {
        return g_fonts[f]->CalcTextSizeA(g_fonts[f]->FontSize, FLT_MAX, wrap > 0 ? wrap : 0.0f, s);
    }
    void Text(FontId f, ImVec2 p, ImU32 col, const char* s, float wrap = 0.0f)
    {
        DL()->AddText(g_fonts[f], g_fonts[f]->FontSize, p, col, s, nullptr, wrap);
    }
    // Текст с чёрной обводкой и тенью — как заголовки GTA
    void TextOutlined(FontId f, ImVec2 p, Rgb col, const char* s, float stroke)
    {
        DL()->AddText(g_fonts[f], g_fonts[f]->FontSize, ImVec2(p.x, p.y + stroke * 2.2f), C(kBlack, 0.9f), s);
        for(int i = 0; i < 8; ++i)
        {
            float a = i * (float)M_PI / 4;
            DL()->AddText(g_fonts[f], g_fonts[f]->FontSize, ImVec2(p.x + cosf(a) * stroke, p.y + sinf(a) * stroke), C(kBlack), s);
        }
        DL()->AddText(g_fonts[f], g_fonts[f]->FontSize, p, C(col), s);
    }

    void Glow(ImVec2 a, ImVec2 b, float r, Rgb col, float strength)
    {
        for(int i = 1; i <= 5; ++i)
        {
            float e = U(0.6f) * i;
            DL()->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e), C(col, strength * (0.22f - i * 0.035f)), r + e, 0, U(0.6f));
        }
    }

    // Панель с лёгким вертикальным градиентом (скругление без градиента: сверху блик)
    void Panel(ImVec2 a, ImVec2 b, float r, Rgb fill, float alpha = 0.96f)
    {
        DL()->AddRectFilled(a, b, C(fill, alpha), r);
        DL()->AddRectFilledMultiColor(ImVec2(a.x + r, a.y + U(0.3f)), ImVec2(b.x - r, a.y + (b.y - a.y) * 0.5f),
                                      C(kWhite, 0.035f), C(kWhite, 0.035f), C(kWhite, 0), C(kWhite, 0));
        DL()->AddRect(a, b, C(kLine), r, 0, U(0.3f));
    }

    // ---------- иконки режимов ----------
    void Icon(net::Role m, ImVec2 c, float s, ImU32 col)
    {
        ImDrawList* d = DL();
        float t = s * 0.09f;
        switch(m)
        {
            case net::Role::Off: // кнопка питания
                d->PathArcTo(c, s * 0.36f, -M_PI * 0.5f + 0.75f, -M_PI * 0.5f + 2 * M_PI - 0.75f, 24);
                d->PathStroke(col, 0, t);
                d->AddLine(ImVec2(c.x, c.y - s * 0.46f), ImVec2(c.x, c.y - s * 0.06f), col, t);
                break;
            case net::Role::Echo: // ты и пунктирный двойник
                d->AddCircle(ImVec2(c.x - s * 0.14f, c.y - s * 0.2f), s * 0.14f, col, 16, t);
                d->PathArcTo(ImVec2(c.x - s * 0.14f, c.y + s * 0.38f), s * 0.28f, (float)M_PI, 2 * (float)M_PI, 16);
                d->PathStroke(col, 0, t);
                for(int i = 0; i < 4; ++i)
                {
                    float a0 = -M_PI * 0.5f + i * 0.55f;
                    d->PathArcTo(ImVec2(c.x + s * 0.18f, c.y), s * 0.22f, a0, a0 + 0.3f, 4);
                    d->PathStroke(col, 0, t);
                }
                break;
            case net::Role::Host: // дом
            {
                ImVec2 pts[] = { ImVec2(c.x - s * 0.4f, c.y - s * 0.02f), ImVec2(c.x, c.y - s * 0.38f), ImVec2(c.x + s * 0.4f, c.y - s * 0.02f) };
                d->AddPolyline(pts, 3, col, 0, t);
                ImVec2 body[] = { ImVec2(c.x - s * 0.3f, c.y - s * 0.08f), ImVec2(c.x - s * 0.3f, c.y + s * 0.38f),
                                  ImVec2(c.x + s * 0.3f, c.y + s * 0.38f), ImVec2(c.x + s * 0.3f, c.y - s * 0.08f) };
                d->AddPolyline(body, 4, col, 0, t);
                d->AddRect(ImVec2(c.x - s * 0.08f, c.y + s * 0.12f), ImVec2(c.x + s * 0.08f, c.y + s * 0.38f), col, 0, 0, t);
                break;
            }
            case net::Role::Client: // звенья цепи
                d->AddCircle(ImVec2(c.x - s * 0.16f, c.y + s * 0.12f), s * 0.17f, col, 16, t);
                d->AddCircle(ImVec2(c.x + s * 0.16f, c.y - s * 0.12f), s * 0.17f, col, 16, t);
                d->AddLine(ImVec2(c.x - s * 0.08f, c.y + s * 0.06f), ImVec2(c.x + s * 0.08f, c.y - s * 0.06f), col, t);
                break;
        }
    }

    // ---------- части меню ----------
    struct ModeInfo { net::Role role; const char* title; const char* sub; };
    const ModeInfo kModes[] = {
        { net::Role::Off,    "Выключено",       "Обычная игра" },
        { net::Role::Echo,   "Тест в одиночку", "Двойник повторяет тебя" },
        { net::Role::Host,   "Хост",            "Друг подключается к тебе" },
        { net::Role::Client, "Подключиться",    "К игре друга" },
    };

    void StatusPill(ImVec2 rightTop, float h)
    {
        coop::Status st = coop::GetStatus();
        char text[96]; Rgb dot = kDim; bool pulse = false;
        if(st.pending)                         { snprintf(text, sizeof(text), "Включится при загрузке игры"); dot = kGold; pulse = true; }
        else switch(st.role)
        {
            case net::Role::Off:  snprintf(text, sizeof(text), "Кооп выключен"); break;
            case net::Role::Echo: snprintf(text, sizeof(text), st.remoteSpawned ? "Двойник рядом" : "Загрузи сохранение");
                                  dot = st.remoteSpawned ? kGrove : kGold; pulse = true; break;
            case net::Role::Host:
                if(!st.running)        { snprintf(text, sizeof(text), "Ошибка сети"); dot = kRed; }
                else if(st.connected)  { snprintf(text, sizeof(text), "Друг подключён"); dot = kGrove; pulse = true; }
                else                   { snprintf(text, sizeof(text), "Жду друга…"); dot = kGold; pulse = true; }
                break;
            case net::Role::Client:
                if(!st.running)        { snprintf(text, sizeof(text), "Неверный адрес"); dot = kRed; }
                else if(st.connected)  { if(st.pingMs >= 0) snprintf(text, sizeof(text), "Подключено · %d мс", st.pingMs);
                                         else snprintf(text, sizeof(text), "Подключено"); dot = kGrove; pulse = true; }
                else                   { snprintf(text, sizeof(text), "Подключаюсь…"); dot = kGold; pulse = true; }
                break;
        }
        ImVec2 ts = TextSize(F_SMALL, text);
        float padX = U(2.4f), dotR = U(1.0f);
        ImVec2 b(rightTop.x, rightTop.y + h);
        ImVec2 a(b.x - (padX * 2 + dotR * 2 + U(1.2f) + ts.x), rightTop.y);
        DL()->AddRectFilled(a, b, C(kPanel, 0.85f), h * 0.5f);
        DL()->AddRect(a, b, C(kLine), h * 0.5f, 0, U(0.25f));
        ImVec2 dc(a.x + padX + dotR, (a.y + b.y) * 0.5f);
        if(pulse)
        {
            float t = fmodf((float)g_now, 1.4f) / 1.4f;
            DL()->AddCircleFilled(dc, dotR * (1 + 1.6f * t), C(dot, 0.6f * (1 - t)), 20);
        }
        DL()->AddCircleFilled(dc, dotR, C(dot), 20);
        Text(F_SMALL, ImVec2(dc.x + dotR + U(1.2f), dc.y - ts.y * 0.5f), C(kInk), text);
    }

    void Switch(ImVec2 a, float h, bool on, float knob)
    {
        float w = h * 2.1f;
        ImVec2 b(a.x + w, a.y + h);
        Rgb bg = on ? Rgb{ 120, 22, 22 } : kNight;
        DL()->AddRectFilled(a, b, C(bg), h * 0.5f);
        if(on) Glow(a, b, h * 0.5f, kRed, 1.0f);
        DL()->AddRect(a, b, C(on ? kRed : kLine), h * 0.5f, 0, U(0.3f));
        const char* lbl = on ? "ВКЛ" : "ВЫКЛ";
        ImVec2 ls = TextSize(F_SMALL, lbl);
        float lx = on ? a.x + h * 0.32f : b.x - h * 0.32f - ls.x;
        Text(F_SMALL, ImVec2(lx, a.y + (h - ls.y) * 0.5f), C(on ? Rgb{ 255, 214, 214 } : kDim), lbl);
        float r = h * 0.36f;
        ImVec2 kc(a.x + h * 0.5f + (w - h) * knob, a.y + h * 0.5f);
        DL()->AddCircleFilled(ImVec2(kc.x, kc.y + U(0.5f)), r, C(kBlack, 0.5f), 24);
        DL()->AddCircleFilled(kc, r, C(Rgb{ 185, 194, 204 }), 24);
        DL()->AddCircleFilled(ImVec2(kc.x - r * 0.25f, kc.y - r * 0.3f), r * 0.55f, C(kWhite, 0.85f), 20);
    }

    // Строка «Огонь по своим» с тумблером. Возвращает её нижний край.
    float FriendlyFireRow(ImVec2 a, float width)
    {
        float h = U(11);
        ImVec2 b(a.x + width, a.y + h);
        DL()->AddRectFilled(a, b, C(kPanel2), U(1.6f));
        DL()->AddRect(a, b, C(kLine), U(1.6f), 0, U(0.3f));
        Text(F_HEAD, ImVec2(a.x + U(2.4f), a.y + U(1.3f)), C(kInk), "Огонь по своим");
        Text(F_SMALL, ImVec2(a.x + U(2.4f), a.y + U(6.8f)), C(kDim), "Можно ли ранить друга");
        float sh = U(7.6f);
        ImVec2 sa(b.x - U(2.4f) - sh * 2.1f, a.y + (h - sh) * 0.5f);
        Switch(sa, sh, g_settings.friendlyFire, g_ffKnob);
        if(Clicked(a, b))
        {
            g_settings.friendlyFire = !g_settings.friendlyFire;
            settings::Save(g_settings);
            Toast(g_settings.friendlyFire ? "Огонь по своим: ВКЛ" : "Огонь по своим: ВЫКЛ");
        }
        return b.y;
    }

    void Chip(ImVec2 a, ImVec2 b, const char* label, bool* clicked)
    {
        bool held = Held(a, b);
        float s = held ? 0.95f : 1.0f;
        ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), hs((b.x - a.x) * 0.5f * s, (b.y - a.y) * 0.5f * s);
        ImVec2 aa(c.x - hs.x, c.y - hs.y), bb(c.x + hs.x, c.y + hs.y);
        DL()->AddRectFilled(aa, bb, C(held ? kLine : kPanel2), U(1.2f));
        DL()->AddRect(aa, bb, C(held ? kGrove : kLine), U(1.2f), 0, U(0.3f));
        ImVec2 ts = TextSize(F_SMALL, label);
        Text(F_SMALL, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), C(kInk), label);
        if(clicked) *clicked = Clicked(a, b);
    }
    float ChipWidth(const char* label) { return TextSize(F_SMALL, label).x + U(4.4f); }

    // Крупный адрес (светящиеся цифры)
    void IpReadout(ImVec2 a, ImVec2 b, const char* text, bool caret, bool bad)
    {
        float shake = 0;
        float st = (float)(g_now - g_ipShakeAt);
        if(st < 0.3f) shake = sinf(st * 60.0f) * U(1.2f) * (1 - st / 0.3f);
        a.x += shake; b.x += shake;
        DL()->AddRectFilled(a, b, C(Rgb{ 5, 7, 10 }), U(1.6f));
        DL()->AddRect(a, b, C(bad || st < 0.3f ? kRed : kLine), U(1.6f), 0, U(0.3f));
        ImVec2 ts = TextSize(F_MONO, text[0] ? text : "0");
        ImVec2 p(a.x + U(2.4f), (a.y + b.y - ts.y) * 0.5f);
        // мягкое зелёное свечение под цифрами
        if(text[0])
        {
            ImVec2 tsz = TextSize(F_MONO, text);
            for(int i = 3; i >= 1; --i)
            {
                float e = U(0.9f) * i;
                DL()->AddRectFilled(ImVec2(p.x - e, p.y + tsz.y * 0.2f - e), ImVec2(p.x + tsz.x + e, p.y + tsz.y * 0.8f + e),
                                    C(kGrove, 0.035f), U(1.5f) + e);
            }
        }
        Text(F_MONO, p, C(kGrove), text);
        if(caret && fmodf((float)g_now, 1.0f) < 0.5f)
        {
            float cx = p.x + (text[0] ? TextSize(F_MONO, text).x : 0) + U(0.4f);
            DL()->AddRectFilled(ImVec2(cx, p.y + ts.y * 0.12f), ImVec2(cx + U(0.6f), p.y + ts.y * 0.9f), C(kGrove));
        }
    }

    void Keypad(ImVec2 a, float width)
    {
        static const char* keys[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "←" };
        float gap = U(1.2f), kw = (width - gap * 2) / 3, kh = U(9);
        for(int i = 0; i < 12; ++i)
        {
            int r = i / 3, c = i % 3;
            ImVec2 ka(a.x + c * (kw + gap), a.y + r * (kh + gap)), kb(ka.x + kw, ka.y + kh);
            bool held = Held(ka, kb);
            float dy = held ? U(0.4f) : 0;
            ImVec2 pa(ka.x, ka.y + dy), pb(kb.x, kb.y + dy);
            DL()->AddRectFilled(ImVec2(pa.x, pa.y + U(0.6f)), ImVec2(pb.x, pb.y + U(0.6f) - dy), C(kBlack, 0.4f), U(1.4f));
            DL()->AddRectFilled(pa, pb, C(Rgb{ 27, 36, 49 }), U(1.4f));
            DL()->AddRectFilled(pa, ImVec2(pb.x, pa.y + kh * 0.5f), C(kWhite, 0.03f), U(1.4f), ImDrawFlags_RoundCornersTop);
            DL()->AddRect(pa, pb, C(held ? kGrove : kLine), U(1.4f), 0, U(0.3f));
            bool fn = (i == 9 || i == 11);
            FontId f = F_KEY;
            ImVec2 ts = TextSize(f, keys[i]);
            Text(f, ImVec2((pa.x + pb.x - ts.x) * 0.5f, (pa.y + pb.y - ts.y) * 0.5f), C(fn ? kGold : kInk), keys[i]);
            if(g_ptr.pressed && In(g_ptr.downPos, ka, kb)) g_ripples.push_back({ g_ptr.downPos, g_now, ka, kb });
            if(Clicked(ka, kb))
            {
                std::string k = keys[i];
                if(i == 11) { if(!g_ip.empty()) g_ip.pop_back(); }
                else if(i == 9)
                {
                    int dots = 0; for(char ch : g_ip) dots += ch == '.';
                    if(!g_ip.empty() && g_ip.back() != '.' && dots < 3) g_ip += '.';
                    else g_ipShakeAt = g_now;
                }
                else
                {
                    size_t dot = g_ip.rfind('.');
                    std::string last = dot == std::string::npos ? g_ip : g_ip.substr(dot + 1);
                    if(last.size() < 3 && atoi((last + k).c_str()) <= 255) g_ip += k;
                    else g_ipShakeAt = g_now;
                }
            }
        }
        // волны от нажатий (не выходят за клавишу)
        for(size_t i = 0; i < g_ripples.size();)
        {
            float t = (float)(g_now - g_ripples[i].t) / 0.5f;
            if(t >= 1) { g_ripples.erase(g_ripples.begin() + i); continue; }
            ImVec2 ka = g_ripples[i].a, kb = g_ripples[i].b;
            float maxR = fminf(kb.x - ka.x, kb.y - ka.y) * 0.5f;
            ImVec2 c(fminf(fmaxf(g_ripples[i].c.x, ka.x + maxR), kb.x - maxR), fminf(fmaxf(g_ripples[i].c.y, ka.y + maxR), kb.y - maxR));
            DL()->AddCircleFilled(c, maxR * EaseOut(t), C(kGrove, 0.35f * (1 - t)), 24);
            ++i;
        }
    }

    void BigButton(ImVec2 a, ImVec2 b, const char* label, bool enabled, bool* clicked)
    {
        bool held = enabled && Held(a, b);
        float dy = held ? U(0.6f) : 0;
        float r = U(1.6f);
        Rgb top = enabled ? Rgb{ 124, 240, 132 } : Rgb{ 70, 78, 88 };
        Rgb mid = enabled ? kGrove : Rgb{ 55, 62, 70 };
        Rgb bot = enabled ? kGroveDeep : Rgb{ 35, 40, 46 };
        if(enabled) Glow(a, b, r, kGrove, 1.2f);
        DL()->AddRectFilled(ImVec2(a.x, a.y + U(0.8f)), ImVec2(b.x, b.y + U(0.8f)), C(enabled ? Rgb{ 14, 58, 23 } : kBlack), r);
        ImVec2 pa(a.x, a.y + dy), pb(b.x, b.y + dy);
        DL()->AddRectFilled(pa, pb, C(mid), r);
        float hh = pb.y - pa.y;
        // верх светлее, низ темнее — плавно (внутри скруглений, чтобы не вылезало за углы)
        DL()->AddRectFilled(pa, ImVec2(pb.x, pa.y + r), C(top, 0.9f), r, ImDrawFlags_RoundCornersTop);
        DL()->AddRectFilledMultiColor(ImVec2(pa.x, pa.y + r), ImVec2(pb.x, pa.y + hh * 0.55f), C(top, 0.9f), C(top, 0.9f), C(top, 0), C(top, 0));
        DL()->AddRectFilledMultiColor(ImVec2(pa.x, pa.y + hh * 0.6f), ImVec2(pb.x, pb.y - r), C(bot, 0), C(bot, 0), C(bot, 0.85f), C(bot, 0.85f));
        DL()->AddRectFilled(ImVec2(pa.x, pb.y - r), pb, C(bot, 0.85f), r, ImDrawFlags_RoundCornersBottom);
        if(enabled)
        {   // бегущий блик (обрезан по краям кнопки вручную)
            float t = fmodf((float)g_now, 2.8f) / 1.1f;
            if(t < 1)
            {
                float w = (pb.x - pa.x), x = pa.x - w * 0.3f + (w * 1.6f) * t, bw = w * 0.18f;
                float x0 = fmaxf(x, pa.x + r), x1 = fminf(x + bw, pb.x - r);
                if(x1 > x0)
                {
                    float m = (x0 + x1) * 0.5f;
                    DL()->AddRectFilledMultiColor(ImVec2(x0, pa.y), ImVec2(m, pb.y), C(kWhite, 0), C(kWhite, 0.45f), C(kWhite, 0.45f), C(kWhite, 0));
                    DL()->AddRectFilledMultiColor(ImVec2(m, pa.y), ImVec2(x1, pb.y), C(kWhite, 0.45f), C(kWhite, 0), C(kWhite, 0), C(kWhite, 0.45f));
                }
            }
        }
        ImVec2 ts = TextSize(F_HEAD, label);
        Text(F_HEAD, ImVec2((pa.x + pb.x - ts.x) * 0.5f, (pa.y + pb.y - ts.y) * 0.5f), C(enabled ? Rgb{ 6, 19, 10 } : kDim), label);
        if(clicked) *clicked = enabled && Clicked(a, b);
    }

    void Apply()
    {
        g_settings.net.role = g_sel;
        if(g_sel == net::Role::Client) snprintf(g_settings.net.hostAddress, sizeof(g_settings.net.hostAddress), "%s", g_ip.c_str());
        settings::Save(g_settings);
        coop::SetConfig(g_settings.net);
        if(g_log) g_log->Info("Меню: режим %s, адрес хоста %s", settings::RoleToString(g_sel), g_settings.net.hostAddress);
    }

    void Close() { if(!g_closing) { g_closing = true; g_closeAt = g_now; } }

    // ---------- кадр меню ----------
    void DrawMenu()
    {
        float openT  = EaseOut((float)(g_now - g_openedAt) / 0.5f);
        float closeT = g_closing ? Clamp01((float)(g_now - g_closeAt) / 0.25f) : 0.0f;
        if(g_closing && closeT >= 1) { g_open = false; g_closing = false; return; }
        g_alpha = openT * (1 - closeT);

        const float Wd = W(), Hd = H();
        // затемнение игры
        DL()->AddRectFilled(ImVec2(0, 0), ImVec2(Wd, Hd), C(Rgb{ 4, 6, 10 }, 0.62f));

        const float x0 = CW(3), x1 = Wd - CW(3), y0 = U(4), y1 = Hd - U(4);
        const float headH = U(12), barH = U(11), gapY = U(2.4f), gapX = CW(2.4f);
        const float railW = CW(34);
        const float bodyY0 = y0 + headH + gapY, bodyY1 = y1 - barH - gapY;
        auto anim = [&](float delay) { return EaseBack((float)(g_now - g_openedAt - delay) / 0.55f); };

        // --- шапка ---
        {
            float k = anim(0);
            float dy = -(1 - k) * headH * 0.6f;
            TextOutlined(F_TITLE, ImVec2(x0, y0 + dy - U(1)), kGold, "CO-OP", U(0.35f));
            ImVec2 ts = TextSize(F_TITLE, "CO-OP");
            Text(F_SMALL, ImVec2(x0 + ts.x + CW(1.6f), y0 + dy + U(6.2f)), C(kDim), "SAN ANDREAS · ВДВОЁМ");

            // закрыть
            float cr = U(4.5f);
            ImVec2 cc(x1 - cr, y0 + dy + headH * 0.5f);
            ImVec2 ca(cc.x - cr, cc.y - cr), cb(cc.x + cr, cc.y + cr);
            bool held = Held(ca, cb);
            DL()->AddCircleFilled(cc, cr, C(held ? kLine : kPanel), 32);
            DL()->AddCircle(cc, cr, C(kLine), 32, U(0.3f));
            float xs = cr * 0.38f;
            DL()->AddLine(ImVec2(cc.x - xs, cc.y - xs), ImVec2(cc.x + xs, cc.y + xs), C(kInk), U(0.5f));
            DL()->AddLine(ImVec2(cc.x - xs, cc.y + xs), ImVec2(cc.x + xs, cc.y - xs), C(kInk), U(0.5f));
            if(Clicked(ca, cb)) Close();

            StatusPill(ImVec2(ca.x - U(2), cc.y - U(3.2f)), U(6.4f));
        }

        // --- режимы слева ---
        {
            float gap = U(1.6f);
            float ch = (bodyY1 - bodyY0 - gap * 3) / 4;
            for(int i = 0; i < 4; ++i)
            {
                float k = anim(0.06f * i);
                const ModeInfo& m = kModes[i];
                bool sel = g_sel == m.role;
                float shift = (sel ? CW(1.2f) : 0) - (1 - k) * railW * 0.4f;
                ImVec2 a(x0 + shift, bodyY0 + i * (ch + gap)), b(x0 + railW + shift, a.y + ch);
                bool held = Held(a, b);
                if(held) { float sh = (b.x - a.x) * 0.015f; a.x += sh; a.y += sh; b.x -= sh; b.y -= sh; }
                if(sel) Glow(a, b, U(1.6f), kGrove, 1.0f);
                DL()->AddRectFilledMultiColor(ImVec2(a.x + U(1.6f), a.y), ImVec2(b.x - U(1.6f), b.y),
                                              C(kPanel2, 0.95f), C(kPanel, 0.88f), C(kPanel, 0.88f), C(kPanel2, 0.95f));
                DL()->AddRectFilled(a, ImVec2(a.x + U(1.7f), b.y), C(kPanel2, 0.95f), U(1.6f), ImDrawFlags_RoundCornersLeft);
                DL()->AddRectFilled(ImVec2(b.x - U(1.7f), a.y), b, C(kPanel, 0.88f), U(1.6f), ImDrawFlags_RoundCornersRight);
                DL()->AddRect(a, b, C(sel ? kGrove : kLine), U(1.6f), 0, U(0.3f));
                if(sel)
                {   // блик при выборе
                    float t = (float)(g_now - g_selAt) / 0.7f;
                    if(t < 1)
                    {
                        float w = b.x - a.x, x = a.x - w * 0.4f + w * 1.4f * EaseOut(t), bw = w * 0.4f;
                        float xa = fmaxf(x, a.x + U(1.6f)), xb = fminf(x + bw, b.x - U(1.6f));
                        if(xb > xa)
                        {
                            float mx = (xa + xb) * 0.5f;
                            DL()->AddRectFilledMultiColor(ImVec2(xa, a.y), ImVec2(mx, b.y), C(kGrove, 0), C(kGrove, 0.25f), C(kGrove, 0.25f), C(kGrove, 0));
                            DL()->AddRectFilledMultiColor(ImVec2(mx, a.y), ImVec2(xb, b.y), C(kGrove, 0.25f), C(kGrove, 0), C(kGrove, 0), C(kGrove, 0.25f));
                        }
                    }
                }
                float ib = ch - U(3.2f);
                if(ib > U(8)) ib = U(8);
                ImVec2 ia(a.x + U(2.4f), (a.y + b.y - ib) * 0.5f), ibb(ia.x + ib, ia.y + ib);
                DL()->AddRectFilled(ia, ibb, C(Rgb{ 11, 15, 21 }), U(1.4f));
                Icon(m.role, ImVec2((ia.x + ibb.x) * 0.5f, (ia.y + ibb.y) * 0.5f), ib * 0.62f, C(sel ? kGrove : kDim));
                float tx = ibb.x + U(2);
                ImVec2 t1 = TextSize(F_HEAD, m.title);
                float scale = 0.78f;
                float total = t1.y * scale + U(3.2f);
                float ty = (a.y + b.y - total) * 0.5f;
                DL()->AddText(g_fonts[F_HEAD], g_fonts[F_HEAD]->FontSize * scale, ImVec2(tx, ty), C(kInk), m.title);
                Text(F_SMALL, ImVec2(tx, ty + t1.y * scale + U(0.2f)), C(kDim), m.sub);
                if(Clicked(a, b) && g_sel != m.role) { g_sel = m.role; g_selAt = g_now; }
            }
        }

        // --- правая панель ---
        {
            float k = anim(0.1f);
            float dx = (1 - k) * CW(8);
            ImVec2 a(x0 + railW + gapX + dx, bodyY0), b(x1 + dx, bodyY1);
            Panel(a, b, U(2), kPanel);
            DL()->AddRectFilled(ImVec2(b.x - U(14), a.y), ImVec2(b.x - U(2), a.y + U(0.6f)), C(kGold)); // золотая риска
            float px = a.x + CW(3), pw = (b.x - a.x) - CW(6);
            float y = a.y + U(3);

            if(g_sel == net::Role::Off)
            {
                Text(F_SMALL, ImVec2(px, y), C(kGold), "РЕЖИМ"); y += U(3.6f);
                Text(F_HEAD, ImVec2(px, y), C(kInk), "Кооп выключен"); y += U(7);
                const char* p = "Игра идёт как обычно. Выбери «Хост», чтобы позвать друга, или «Подключиться», чтобы зайти к нему.";
                Text(F_BODY, ImVec2(px, y), C(kDim), p, pw); y += TextSize(F_BODY, p, pw).y + U(2.4f);
                FriendlyFireRow(ImVec2(px, y), pw);
            }
            else if(g_sel == net::Role::Echo)
            {
                Text(F_SMALL, ImVec2(px, y), C(kGold), "ПРОВЕРКА БЕЗ ДРУГА"); y += U(3.6f);
                Text(F_HEAD, ImVec2(px, y), C(kInk), "Двойник рядом с тобой"); y += U(7);
                const char* p = "Второй CJ повторяет твои движения через полсекунды, на полтора метра восточнее. Так проверяется мод, когда друга нет онлайн.";
                Text(F_BODY, ImVec2(px, y), C(kDim), p, pw); y += TextSize(F_BODY, p, pw).y + U(2.4f);
                const char* rows[2][2] = { { "Задержка двойника", "0,5 с" }, { "Сдвиг", "1,5 м на восток" } };
                for(auto& r : rows)
                {
                    ImVec2 ra(px, y), rb(px + pw, y + U(8.5f));
                    DL()->AddRectFilled(ra, rb, C(kPanel2), U(1.6f));
                    DL()->AddRect(ra, rb, C(kLine), U(1.6f), 0, U(0.3f));
                    ImVec2 t1 = TextSize(F_BODY, r[0]), t2 = TextSize(F_KEY, r[1]);
                    Text(F_BODY, ImVec2(ra.x + U(2.4f), (ra.y + rb.y - t1.y) * 0.5f), C(kInk), r[0]);
                    Text(F_KEY, ImVec2(rb.x - U(2.4f) - t2.x, (ra.y + rb.y - t2.y) * 0.5f), C(kGrove), r[1]);
                    y = rb.y + U(1.6f);
                }
            }
            else if(g_sel == net::Role::Host)
            {
                if(g_now - g_myIpAt > 1.0) { g_myIpOk = platform::FindTailscaleIPv4(g_myIp, sizeof(g_myIp)); g_myIpAt = g_now; }
                Text(F_SMALL, ImVec2(px, y), C(kGold), "ТВОЙ АДРЕС В TAILSCALE"); y += U(3.8f);
                const char* copyLbl = "Копировать";
                float cw = ChipWidth(copyLbl);
                ImVec2 ra(px, y), rb(px + pw, y + U(11));
                IpReadout(ra, ImVec2(rb.x - cw - U(2.4f), rb.y), g_myIpOk ? g_myIp : "Tailscale выкл.", false, !g_myIpOk);
                bool copy = false;
                Chip(ImVec2(rb.x - cw, y + U(2.2f)), ImVec2(rb.x, rb.y - U(2.2f)), copyLbl, &copy);
                if(copy)
                {
                    if(!g_myIpOk) Toast("Включи Tailscale — адреса пока нет");
                    else if(platform::ClipboardSetText(g_myIp)) { char t[96]; snprintf(t, sizeof(t), "Адрес %s скопирован", g_myIp); Toast(t); }
                    else Toast("Не удалось скопировать");
                }
                y = rb.y + U(2.4f);
                const char* p = g_myIpOk ? "Отправь этот адрес другу. Когда он подключится, точка вверху станет зелёной."
                                         : "Включи Tailscale на телефоне — здесь появится твой адрес для друга.";
                Text(F_BODY, ImVec2(px, y), C(kDim), p, pw); y += TextSize(F_BODY, p, pw).y + U(2.4f);
                FriendlyFireRow(ImVec2(px, y), pw);
            }
            else // Client
            {
                float keysW = U(34);
                float lw = pw - keysW - U(3);
                Text(F_SMALL, ImVec2(px, y), C(kGold), "АДРЕС ХОСТА"); float ly = y + U(3.8f);
                IpReadout(ImVec2(px, ly), ImVec2(px + lw, ly + U(11)), g_ip.c_str(), true, false);
                ly += U(11) + U(2.4f);
                bool paste = false, clear = false;
                const char* l1 = "Вставить"; const char* l2 = "Стереть";
                float w1 = ChipWidth(l1), w2 = ChipWidth(l2);
                Chip(ImVec2(px, ly), ImVec2(px + w1, ly + U(6.4f)), l1, &paste);
                Chip(ImVec2(px + w1 + U(1.6f), ly), ImVec2(px + w1 + U(1.6f) + w2, ly + U(6.4f)), l2, &clear);
                ly += U(6.4f) + U(2.4f);
                Text(F_BODY, ImVec2(px, ly), C(kDim), "Попроси друга нажать «Копировать» в его меню и прислать адрес. Он начинается на 100.", lw);
                Keypad(ImVec2(px + pw - keysW, y), keysW);
                if(paste)
                {
                    char text[256], ip[32];
                    if(platform::ClipboardGetText(text, sizeof(text)) && platform::ExtractIPv4(text, ip, sizeof(ip)))
                    { g_ip = ip; char t[96]; snprintf(t, sizeof(t), "Вставлено: %s", ip); Toast(t); }
                    else Toast("В буфере нет адреса");
                }
                if(clear) g_ip.clear();
            }
        }

        // --- нижняя полоса ---
        {
            float k = anim(0.2f);
            float dy = (1 - k) * barH;
            const char* label = g_sel == net::Role::Off ? "Применить" : g_sel == net::Role::Echo ? "Запустить"
                              : g_sel == net::Role::Host ? "Открыть мир" : "Подключиться";
            bool enabled = g_sel != net::Role::Client || ValidIp(g_ip);
            float bw = TextSize(F_HEAD, label).x + U(12);
            ImVec2 ba(x1 - bw, y1 - barH + U(1) + dy), bb(x1, y1 + dy);
            const char* hint = enabled ? "Выбери режим и нажми кнопку справа" : "Введи адрес хоста полностью, например 100.87.12.40";
            ImVec2 hs = TextSize(F_SMALL, hint);
            Text(F_SMALL, ImVec2(x0, (ba.y + bb.y - hs.y) * 0.5f), C(kDim), hint);
            bool go = false;
            BigButton(ba, bb, label, enabled, &go);
            if(go)
            {
                Apply();
                Toast(g_sel == net::Role::Off ? "Кооп выключен" : g_sel == net::Role::Echo ? "Двойник появится в игре"
                    : g_sel == net::Role::Host ? "Мир открыт — жду друга" : "Подключаюсь…");
            }
        }

        // --- всплывашка ---
        float tt = (float)(g_now - g_toastAt);
        if(tt < 2.2f && g_toast[0])
        {
            float k = EaseBack(tt / 0.35f) * (tt > 1.9f ? 1 - (tt - 1.9f) / 0.3f : 1);
            ImVec2 ts = TextSize(F_BODY, g_toast);
            float w = ts.x + U(6), h = ts.y + U(2.8f);
            ImVec2 a(Wd * 0.5f - w * 0.5f, y1 - barH - U(4) - h + (1 - k) * U(4)), b(a.x + w, a.y + h);
            float sa = g_alpha; g_alpha *= Clamp01(k);
            DL()->AddRectFilled(a, b, C(Rgb{ 5, 7, 10 }), h * 0.5f);
            DL()->AddRect(a, b, C(kGrove), h * 0.5f, 0, U(0.3f));
            Text(F_BODY, ImVec2(a.x + U(3), a.y + U(1.4f)), C(kInk), g_toast);
            g_alpha = sa;
        }

        // тумблер плавно догоняет состояние
        float target = g_settings.friendlyFire ? 1.0f : 0.0f;
        g_ffKnob += (target - g_ffKnob) * Clamp01(ImGui::GetIO().DeltaTime * 14.0f);
    }

    void Frame()
    {
        uint64_t nowUs = (uint64_t)(NowSec() * 1e6);
        float dt = g_lastUs ? (float)(nowUs - g_lastUs) / 1e6f : 1.0f / 60.0f;
        if(dt > 0.1f) dt = 0.1f;
        g_lastUs = nowUs;
        g_now = NowSec();

        imgui_rw::NewFrame(dt);
        if(!g_fontsBuilt)
        {
            float h = ImGui::GetIO().DisplaySize.y;
            if(h < 100) return;
            BuildFonts(h);
            if(!g_fontsBuilt) { logger->Error("Меню: шрифты не собрались"); return; }
        }
        if(!imgui_rw::EnsureDeviceObjects()) return;

        PollTouches();
        ImGui::NewFrame();
        DrawMenu();
        imgui_rw::Render();
    }
}

// ---------- перехваты ----------

DECL_HOOKv(MobileMenuRender, uintptr_t self)
{
    MobileMenuRender(self);
    coop::KeepWorldRunningInMenu();   // в коопе меню не ставит игру на паузу
    if(g_open) Frame();
}

// Пока открыто наше меню (и пока не отпущен палец после закрытия), меню игры
// не обновляется — иначе нажатия попадали бы и в него. Меню игры читает касания
// опросом, поэтому накопленных «призрачных» нажатий потом не будет.
static double g_blockGameMenuUntil = 0;
DECL_HOOKv(MobileMenuUpdate, uintptr_t self)
{
    double now = NowSec();
    if(g_open) { g_blockGameMenuUntil = now + 0.3; return; }
    if(now < g_blockGameMenuUntil || (g_blockGameMenuUntil > 0 && AnyFingerDown()))
    {
        if(AnyFingerDown()) g_blockGameMenuUntil = now + 0.15;
        return;
    }
    g_blockGameMenuUntil = 0;
    MobileMenuUpdate(self);
}

// ---------- открытие ----------

static void OnCoopButton(void*) { Open(); }

void Open()
{
    if(g_open && !g_closing) return;
    g_settings = settings::Load();
    g_sel = coop::GetConfig().role;
    g_ip = coop::GetConfig().hostAddress;
    if(!ValidIp(g_ip)) g_ip = "100.";
    g_ffKnob = g_settings.friendlyFire ? 1.0f : 0.0f;
    g_now = NowSec();
    g_openedAt = g_now; g_selAt = -10; g_toastAt = -10;
    g_ptr = Pointer();
    g_closing = false;
    g_open = true;
    if(g_log) g_log->Info("Меню Co-op открыто");
}

bool IsOpen() { return g_open; }

bool Init(void* h, FLogger* log)
{
    g_log = log;
    if(!imgui_rw::Init(h)) { logger->Error("Меню: нет функций RenderWare — своё меню выключено"); return false; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1920, 1080);

    void* render = (void*)aml->GetSym(h, "_ZN10MobileMenu6RenderEv");
    void* update = (void*)aml->GetSym(h, "_ZN10MobileMenu6UpdateEv");
    g_pointsOrig = (GamePoint*)aml->GetSym(h, "Points");
    uintptr_t base = aml->GetLib("libGTASA.so");
    if(base && g_pointsOrig)
    {
        GamePoint** got = (GamePoint**)(base + kPointsGot);
        // Проверяем, что это та самая ячейка: в ней либо исходный Points, либо чужой массив (Touch x4)
        if(*got == g_pointsOrig || (*got && (uintptr_t)*got != base)) g_pointsGot = got;
        if(g_log) g_log->Info(*got == g_pointsOrig ? "Касания: массив игры Points (2 пальца)"
                                                   : "Касания: массив подменён другим модом (Touch x4?) — читаю его");
    }
    if(!render || !update || !g_pointsOrig) { logger->Error("Меню: нет MobileMenu::Render/Update или Points"); return false; }
    aml->Hook(render, (void*)HookOf_MobileMenuRender, (void**)&MobileMenuRender);
    aml->Hook(update, (void*)HookOf_MobileMenuUpdate, (void**)&MobileMenuUpdate);
    logger->Info("Меню Co-op: перехваты стоят");
    return true;
}

void AttachSAUtils(ISAUtils* sau)
{
    sau->AddSettingsTabButton("CO-OP", OnCoopButton, STB_MainMenu, "menu_mainsettings", nullptr);
    sau->AddSettingsTabButton("CO-OP", OnCoopButton, STB_Pause,    "menu_mainsettings", nullptr);
    logger->Info("Кнопки CO-OP добавлены в главное меню и паузу");
}

} // namespace overlay
