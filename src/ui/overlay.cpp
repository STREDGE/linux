#include "overlay.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/flogger.h>
#include <imgui/imgui.h>
#include <sautils/isautils.h>

#include <float.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "coop/coop.h"
#include "platform/android.h"
#include "voice/voice.h"
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

    // Все пальцы (до 4 с Touch x4): каждая кнопка реагирует на любой палец,
    // который на ней начал касание — можно держать джойстик и жать кнопку.
    constexpr int kMaxFingers = 4;
    struct Finger
    {
        bool   down = false, pressed = false, released = false; // pressed/released — в этом кадре
        ImVec2 pos{ -1, -1 }, downPos{ -1, -1 };
    };
    Finger g_f[kMaxFingers];

    double g_lastPoll = 0;
    void PollTouches()
    {
        // Два наших кадра в одном кадре игры (меню + экран настройки) не должны
        // «съедать» друг у друга нажатия: опрос не чаще раза в 4 мс.
        timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double t = ts.tv_sec + ts.tv_nsec / 1e9;
        if(t - g_lastPoll < 0.004) return;
        g_lastPoll = t;
        for(auto& f : g_f) f.pressed = f.released = false;
        RefreshPoints();
        for(int i = 0; i < kMaxFingers; ++i)
        {
            Finger& f = g_f[i];
            bool down = g_points && i < g_pointsN && g_points[i].state == 2 && g_points[i].x >= 0;
            if(down)
            {
                ImVec2 p((float)g_points[i].x, (float)g_points[i].y);
                if(!f.down) { f.down = true; f.pressed = true; f.downPos = p; }
                f.pos = p;
            }
            else if(f.down) { f.down = false; f.released = true; }
        }
    }
    void ResetFingers() { for(auto& f : g_f) f = Finger(); }

    bool AnyFingerDown()
    {
        RefreshPoints();
        if(!g_points) return false;
        for(int f = 0; f < g_pointsN; ++f) if(g_points[f].state == 2) return true;
        return false;
    }

    bool In(const ImVec2& p, const ImVec2& a, const ImVec2& b) { return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y; }
    // Кнопка нажата сейчас: какой-то палец коснулся её и всё ещё на ней
    bool Held(ImVec2 a, ImVec2 b)
    {
        for(auto& f : g_f) if(f.down && In(f.downPos, a, b) && In(f.pos, a, b)) return true;
        return false;
    }
    // Палец коснулся здесь и ещё не отпущен (может уже уйти с кнопки)
    bool HeldFrom(ImVec2 a, ImVec2 b)
    {
        for(auto& f : g_f) if(f.down && In(f.downPos, a, b)) return true;
        return false;
    }
    // Клик: палец отпущен на той же кнопке, где коснулся
    bool Clicked(ImVec2 a, ImVec2 b)
    {
        for(auto& f : g_f) if(f.released && In(f.downPos, a, b) && In(f.pos, a, b)) return true;
        return false;
    }
    // Палец только что коснулся этой области (для волны от нажатия)
    bool PressedIn(ImVec2 a, ImVec2 b, ImVec2* at)
    {
        for(auto& f : g_f) if(f.pressed && In(f.downPos, a, b)) { if(at) *at = f.downPos; return true; }
        return false;
    }

    // ---------- состояние меню ----------
    bool      g_editRequested = false;  // открыть редактор раскладки при возврате в игру
    // «Настройка сенсоров» игры (CAdjustableHUD): наши элементы двигаются там же
    bool      g_inAdjDraw = false;      // сейчас рисуем внутри CAdjustableHUD::Draw
    double    g_adjSeenAt = -10;        // когда экран настройки последний раз был открыт
    bool      g_resumeGame = false;     // закрыть меню паузы (как «Продолжить») в ближайшем обновлении меню
    void      (*Menu_SwitchOffToGame)() = nullptr;
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
    float     g_swKnob = 0;             // тумблер «общий розыск»

    char      g_toast[192] = "";
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

    bool RoomCode(const std::string& s)
    {
        if(s.size() != 6) return false;
        for(char c : s) if(c < '0' || c > '9') return false;
        return true;
    }
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
    float g_s = 1.0f;                                  // масштаб текущего элемента (редактор раскладки)
    float U(float v)  { return v * H() / 100.0f * g_s; } // доля высоты экрана, как cqh в макете
    float CW(float v) { return v * W() / 100.0f; }   // доля ширины, как cqw

    ImVec2 TextSize(FontId f, const char* s, float wrap = -1.0f)
    {
        return g_fonts[f]->CalcTextSizeA(g_fonts[f]->FontSize * g_s, FLT_MAX, wrap > 0 ? wrap : 0.0f, s);
    }
    void Text(FontId f, ImVec2 p, ImU32 col, const char* s, float wrap = 0.0f)
    {
        DL()->AddText(g_fonts[f], g_fonts[f]->FontSize * g_s, p, col, s, nullptr, wrap);
    }
    // Текст с чёрной обводкой и тенью — как заголовки GTA
    void TextOutlined(FontId f, ImVec2 p, Rgb col, const char* s, float stroke)
    {
        float fs = g_fonts[f]->FontSize * g_s;
        DL()->AddText(g_fonts[f], fs, ImVec2(p.x, p.y + stroke * 2.2f), C(kBlack, 0.9f), s);
        for(int i = 0; i < 8; ++i)
        {
            float a = i * (float)M_PI / 4;
            DL()->AddText(g_fonts[f], fs, ImVec2(p.x + cosf(a) * stroke, p.y + sinf(a) * stroke), C(kBlack), s);
        }
        DL()->AddText(g_fonts[f], fs, p, C(col), s);
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

    float StatusPill(ImVec2 rightTop, float h)
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
                if(!st.running)                        { snprintf(text, sizeof(text), "Ошибка сети"); dot = kRed; }
                else if(st.connected)                  { snprintf(text, sizeof(text), "%s в игре", st.peerNick); dot = kGrove; pulse = true; }
                else if(st.link == net::Link::Pending) { snprintf(text, sizeof(text), "Запрос на подключение"); dot = kGold; pulse = true; }
                else                                   { snprintf(text, sizeof(text), "Жду друга…"); dot = kGold; pulse = true; }
                break;
            case net::Role::Client:
                if(!st.running)                        { snprintf(text, sizeof(text), "Неверный адрес"); dot = kRed; }
                else if(st.connected)                  { if(st.pingMs >= 0) snprintf(text, sizeof(text), "У %s · %d мс", st.peerNick, st.pingMs);
                                                         else snprintf(text, sizeof(text), "У %s", st.peerNick); dot = kGrove; pulse = true; }
                else if(st.link == net::Link::Denied)  { snprintf(text, sizeof(text), "Хост отклонил"); dot = kRed; }
                else                                   { snprintf(text, sizeof(text), "Стучусь к хосту…"); dot = kGold; pulse = true; }
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
        return a.x;
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

    // Строка с тумблером. Возвращает true, если нажали.
    bool ToggleRow(ImVec2 a, float width, const char* title, const char* hint, bool value, float knob, float* bottom)
    {
        float h = U(11);
        ImVec2 b(a.x + width, a.y + h);
        DL()->AddRectFilled(a, b, C(kPanel2), U(1.6f));
        DL()->AddRect(a, b, C(kLine), U(1.6f), 0, U(0.3f));
        Text(F_HEAD, ImVec2(a.x + U(2.4f), a.y + U(1.3f)), C(kInk), title);
        Text(F_SMALL, ImVec2(a.x + U(2.4f), a.y + U(6.8f)), C(kDim), hint);
        float sh = U(7.6f);
        ImVec2 sa(b.x - U(2.4f) - sh * 2.1f, a.y + (h - sh) * 0.5f);
        Switch(sa, sh, value, knob);
        if(bottom) *bottom = b.y;
        return Clicked(a, b);
    }

    // «Огонь по своим» и «Общий розыск» (решает хост). Возвращает нижний край.
    float FriendlyFireRow(ImVec2 a, float width)
    {
        float y = a.y;
        if(ToggleRow(ImVec2(a.x, y), width, "Огонь по своим", "Можно ли ранить друга", g_settings.friendlyFire, g_ffKnob, &y))
        {
            g_settings.friendlyFire = !g_settings.friendlyFire;
            settings::Save(g_settings);
            coop::SetFriendlyFire(g_settings.friendlyFire);
            Toast(g_settings.friendlyFire ? "Огонь по своим: ВКЛ" : "Огонь по своим: ВЫКЛ");
        }
        y += U(1.6f);
        if(ToggleRow(ImVec2(a.x, y), width, "Общий розыск", "Звёзды одни на двоих; выкл. — у каждого свои", g_settings.sharedWanted, g_swKnob, &y))
        {
            g_settings.sharedWanted = !g_settings.sharedWanted;
            settings::Save(g_settings);
            coop::SetSharedWanted(g_settings.sharedWanted);
            Toast(g_settings.sharedWanted ? "Розыск: общий на двоих" : "Розыск: у каждого свой");
        }
        return y;
    }

    void Chip(ImVec2 a, ImVec2 b, const char* label, bool* clicked);
    float ChipWidth(const char* label);

    // Строка «Твой ник» с кнопкой «Вставить» (ник из буфера обмена). Возвращает нижний край.
    float NickRow(ImVec2 a, float width)
    {
        float h = U(8.5f);
        ImVec2 b(a.x + width, a.y + h);
        DL()->AddRectFilled(a, b, C(kPanel2), U(1.6f));
        DL()->AddRect(a, b, C(kLine), U(1.6f), 0, U(0.3f));
        ImVec2 ls = TextSize(F_SMALL, "ТВОЙ НИК");
        Text(F_SMALL, ImVec2(a.x + U(2.4f), (a.y + b.y - ls.y) * 0.5f), C(kGold), "ТВОЙ НИК");
        const char* pl = "Вставить";
        float pw = ChipWidth(pl);
        ImVec2 ns = TextSize(F_BODY, g_settings.net.nick);
        Text(F_BODY, ImVec2(a.x + U(2.4f) + ls.x + U(2), (a.y + b.y - ns.y) * 0.5f), C(kInk), g_settings.net.nick);
        bool paste = false;
        Chip(ImVec2(b.x - U(1.2f) - pw, a.y + U(1.2f)), ImVec2(b.x - U(1.2f), b.y - U(1.2f)), pl, &paste);
        if(paste)
        {
            char text[128];
            if(platform::ClipboardGetText(text, sizeof(text)))
            {
                // первая строка, без пробелов по краям, не длиннее ника
                char* p = text; while(*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
                char* e = p; while(*e && *e != '\n' && *e != '\r') ++e; *e = 0;
                while(e > p && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
                if(*p)
                {
                    snprintf(g_settings.net.nick, sizeof(g_settings.net.nick), "%s", p);
                    settings::Save(g_settings);
                    char t[192]; snprintf(t, sizeof(t), "Ник: %s (применится при подключении)", g_settings.net.nick); Toast(t);
                }
                else Toast("В буфере пусто");
            }
            else Toast("В буфере пусто");
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
            ImVec2 at;
            if(PressedIn(ka, kb, &at)) g_ripples.push_back({ at, g_now, ka, kb });
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
        if(g_sel == net::Role::Client) snprintf(g_settings.net.hostAddress, sizeof(g_settings.net.hostAddress), "%s", g_ip.c_str());   // адрес или код комнаты
        settings::Save(g_settings);
        coop::SetConfig(g_settings.net);
        if(g_log) g_log->Info("Меню: режим %s, адрес хоста %s", settings::RoleToString(g_sel), g_settings.net.hostAddress);
    }

    void Close() { if(!g_closing) { g_closing = true; g_closeAt = g_now; } }

    // ---------- экран «Подключение к миру хоста» (у друга) ----------
    void DrawJoin()
    {
        coop::JoinProgress jp = coop::GetJoinProgress();
        if(!jp.active) return;
        if(g_open && !g_closing && jp.percent >= 15) Close();   // своё меню CO-OP закрываем — дальше экран загрузки
        const float Wd = W(), Hd = H();
        DL()->AddRectFilled(ImVec2(0, 0), ImVec2(Wd, Hd), C(Rgb{ 4, 6, 10 }, 0.78f));
        float w = CW(56), h = U(34);
        ImVec2 a(Wd * 0.5f - w * 0.5f, Hd * 0.5f - h * 0.5f), b(a.x + w, a.y + h);
        Panel(a, b, U(2), kPanel);
        DL()->AddRectFilled(ImVec2(a.x, a.y), ImVec2(b.x, a.y + U(0.6f)), C(kGold));
        float px = a.x + CW(2.4f), pw = w - CW(4.8f), y = a.y + U(3);
        Text(F_SMALL, ImVec2(px, y), C(kGold), "CO-OP"); y += U(4);
        Text(F_HEAD, ImVec2(px, y), C(kInk), jp.title); y += U(7.5f);
        Text(F_BODY, ImVec2(px, y), C(kDim), jp.detail, pw); y += U(7);
        // полоса
        ImVec2 ba(px, y), bb(px + pw, y + U(3));
        DL()->AddRectFilled(ba, bb, C(kPanel2), U(1.5f));
        float k = Clamp01(jp.percent / 100.0f);
        if(k > 0) DL()->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * k, bb.y), C(kGrove), U(1.5f));
        char pct[16]; snprintf(pct, sizeof(pct), "%d %%", jp.percent);
        ImVec2 ps = TextSize(F_KEY, pct);
        y = bb.y + U(1.6f);
        Text(F_KEY, ImVec2(px, y), C(kGrove), pct);
        const char* cl = "Отмена";
        float cw = ChipWidth(cl) + U(2);
        bool cancel = false;
        Chip(ImVec2(px + pw - cw, y - U(0.4f)), ImVec2(px + pw, y + ps.y + U(0.4f)), cl, &cancel);
        if(cancel) { coop::CancelJoin(); Toast("Подключение отменено"); }
    }

    // ---------- выдача оружия (только хост) ----------
    struct WeapItem { int id; const char* name; bool melee; };
    const WeapItem kWeaps[] = {
        { 1, "Кастет", true }, { 2, "Клюшка", true }, { 3, "Дубинка", true }, { 4, "Нож", true }, { 5, "Бита", true },
        { 6, "Лопата", true }, { 7, "Кий", true }, { 8, "Катана", true }, { 9, "Бензопила", true },
        { 16, "Гранаты", false }, { 17, "Газ", false }, { 18, "Молотов", false }, { 39, "Взрывчатка", false },
        { 22, "Пистолет", false }, { 23, "Глушитель", false }, { 24, "Deagle", false },
        { 25, "Дробовик", false }, { 26, "Обрез", false }, { 27, "Боевой дроб.", false },
        { 28, "Uzi", false }, { 32, "Tec-9", false }, { 29, "MP5", false }, { 30, "AK-47", false }, { 31, "M4", false },
        { 33, "Винтовка", false }, { 34, "Снайперка", false }, { 35, "РПГ", false }, { 36, "Ракетница", false },
        { 37, "Огнемёт", false }, { 38, "Миниган", false }, { 41, "Баллончик", false }, { 42, "Огнетушитель", false },
        { 43, "Фотоаппарат", false }, { 46, "Парашют", true },
    };
    bool g_weapOpen = false;
    int  g_weapSel = 13;        // индекс в kWeaps (пистолет)
    int  g_weapAmmo = 100;

    void DrawWeapons(float x0, float y0, float x1, float y1)
    {
        Panel(ImVec2(x0, y0), ImVec2(x1, y1), U(2), kPanel);
        float px = x0 + CW(2.4f), pw = (x1 - x0) - CW(4.8f);
        float y = y0 + U(2.4f);
        Text(F_SMALL, ImVec2(px, y), C(kGold), "ВЫДАТЬ ОРУЖИЕ  ·  ТОЛЬКО ХОСТ");
        const char* backL = "Назад";
        float bw = ChipWidth(backL);
        bool back = false;
        Chip(ImVec2(px + pw - bw, y - U(1)), ImVec2(px + pw, y + U(5)), backL, &back);
        if(back) g_weapOpen = false;
        y += U(6);

        const int cols = 8;
        const int n = (int)(sizeof(kWeaps) / sizeof(kWeaps[0]));
        float gap = U(1.1f), cw = (pw - gap * (cols - 1)) / cols, ch = U(6.2f);
        for(int i = 0; i < n; ++i)
        {
            int r = i / cols, c = i % cols;
            ImVec2 a(px + c * (cw + gap), y + r * (ch + gap)), b(a.x + cw, a.y + ch);
            bool sel = i == g_weapSel, held = Held(a, b);
            DL()->AddRectFilled(a, b, C(sel ? Rgb{ 52, 44, 18 } : held ? kLine : kPanel2), U(1.2f));
            DL()->AddRect(a, b, C(sel ? kGold : kLine), U(1.2f), 0, U(sel ? 0.5f : 0.3f));
            ImVec2 ts = TextSize(F_SMALL, kWeaps[i].name);
            Text(F_SMALL, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), C(sel ? kGold : kInk), kWeaps[i].name);
            if(Clicked(a, b)) g_weapSel = i;
        }
        y += ((n + cols - 1) / cols) * (ch + gap) + U(1.2f);

        // Патроны
        const WeapItem& w = kWeaps[g_weapSel];
        float rowH = U(7);
        if(w.melee)
        {
            Text(F_BODY, ImVec2(px, y + U(1.4f)), C(kDim), "Холодное оружие — без патронов");
        }
        else
        {
            Text(F_SMALL, ImVec2(px, y + U(2)), C(kGold), "ПАТРОНЫ");
            float x = px + TextSize(F_SMALL, "ПАТРОНЫ").x + U(2.4f);
            struct Step { const char* l; int d; bool set; };
            const Step steps[] = { { "-100", -100, false }, { "-10", -10, false }, { "+10", 10, false }, { "+100", 100, false },
                                   { "30", 30, true }, { "100", 100, true }, { "500", 500, true }, { "9999", 9999, true } };
            char num[16]; snprintf(num, sizeof(num), "%d", g_weapAmmo);
            ImVec2 ns = TextSize(F_KEY, "99999");
            ImVec2 na(x, y), nb(x + ns.x + U(3), y + rowH);
            DL()->AddRectFilled(na, nb, C(Rgb{ 4, 8, 6 }), U(1.2f));
            DL()->AddRect(na, nb, C(kLine), U(1.2f), 0, U(0.3f));
            ImVec2 ts = TextSize(F_KEY, num);
            Text(F_KEY, ImVec2((na.x + nb.x - ts.x) * 0.5f, (na.y + nb.y - ts.y) * 0.5f), C(kGrove), num);
            x = nb.x + U(1.6f);
            for(const auto& st : steps)
            {
                float sw = ChipWidth(st.l);
                bool c = false;
                Chip(ImVec2(x, y + U(0.4f)), ImVec2(x + sw, y + rowH - U(0.4f)), st.l, &c);
                if(c) g_weapAmmo = st.set ? st.d : g_weapAmmo + st.d;
                x += sw + U(1.1f);
            }
            if(g_weapAmmo < 1) g_weapAmmo = 1;
            if(g_weapAmmo > 9999) g_weapAmmo = 9999;
        }
        y += rowH + U(2);

        // Кому
        bool friendOk = coop::CanGiveToFriend();
        float bh = U(8), half = (pw - U(2)) * 0.5f;
        bool me = false, fr = false;
        BigButton(ImVec2(px, y), ImVec2(px + half, y + bh), "Выдать себе", true, &me);
        BigButton(ImVec2(px + half + U(2), y), ImVec2(px + pw, y + bh), friendOk ? "Выдать другу" : "Друг не на связи", friendOk, &fr);
        int ammo = w.melee ? 1 : g_weapAmmo;
        if(me) { coop::GiveWeapon(false, w.id, ammo); char t[128]; snprintf(t, sizeof(t), "Тебе: %s%s", w.name, w.melee ? "" : " + патроны"); Toast(t); }
        if(fr) { coop::GiveWeapon(true, w.id, ammo); char t[128]; snprintf(t, sizeof(t), "Другу: %s%s", w.name, w.melee ? "" : " + патроны"); Toast(t); }
    }

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

        if(g_weapOpen && g_sel != net::Role::Host) g_weapOpen = false;
        if(g_weapOpen) DrawWeapons(x0, bodyY0, x1, bodyY1);
        else {
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
                bool relay = g_settings.net.relay[0] != 0;
                coop::Status cst = coop::GetStatus();
                bool roomOk = relay && cst.relay && cst.room[0] && cst.role == net::Role::Host;
                Text(F_SMALL, ImVec2(px, y), C(kGold), relay ? "КОД КОМНАТЫ (СЕРВЕР)" : "ТВОЙ АДРЕС В TAILSCALE"); y += U(3.8f);
                const char* copyLbl = "Копировать";
                float cw = ChipWidth(copyLbl);
                ImVec2 ra(px, y), rb(px + pw, y + U(11));
                const char* shown = relay ? (roomOk ? cst.room : "нажми «Открыть мир»") : (g_myIpOk ? g_myIp : "Tailscale выкл.");
                bool okShown = relay ? roomOk : g_myIpOk;
                IpReadout(ra, ImVec2(rb.x - cw - U(2.4f), rb.y), shown, false, !okShown);
                bool copy = false;
                Chip(ImVec2(rb.x - cw, y + U(2.2f)), ImVec2(rb.x, rb.y - U(2.2f)), copyLbl, &copy);
                if(copy)
                {
                    if(!okShown) Toast(relay ? "Сначала нажми «Открыть мир» — появится код" : "Включи Tailscale — адреса пока нет");
                    else if(platform::ClipboardSetText(shown)) { char t[192]; snprintf(t, sizeof(t), "%s %s скопирован", relay ? "Код" : "Адрес", shown); Toast(t); }
                    else Toast("Не удалось скопировать");
                }
                y = rb.y + U(1.6f);
                y = NickRow(ImVec2(px, y), pw) + U(2);
                const char* p = relay ? "Отправь другу код комнаты. Он введёт его в «Подключиться», ты нажмёшь «Принять»."
                              : g_myIpOk ? "Отправь этот адрес другу. Когда он постучится, ты увидишь запрос и нажмёшь «Принять»."
                                         : "Включи Tailscale на телефоне — здесь появится твой адрес для друга.";
                Text(F_BODY, ImVec2(px, y), C(kDim), p, pw); y += TextSize(F_BODY, p, pw).y + U(2.4f);
                FriendlyFireRow(ImVec2(px, y), pw);
            }
            else // Client
            {
                float keysW = U(34);
                float lw = pw - keysW - U(3);
                Text(F_SMALL, ImVec2(px, y), C(kGold), g_settings.net.relay[0] ? "АДРЕС ХОСТА ИЛИ КОД КОМНАТЫ" : "АДРЕС ХОСТА"); float ly = y + U(3.8f);
                IpReadout(ImVec2(px, ly), ImVec2(px + lw, ly + U(11)), g_ip.c_str(), true, false);
                ly += U(11) + U(2.4f);
                bool paste = false, clear = false;
                const char* l1 = "Вставить"; const char* l2 = "Стереть";
                float w1 = ChipWidth(l1), w2 = ChipWidth(l2);
                Chip(ImVec2(px, ly), ImVec2(px + w1, ly + U(6.4f)), l1, &paste);
                Chip(ImVec2(px + w1 + U(1.6f), ly), ImVec2(px + w1 + U(1.6f) + w2, ly + U(6.4f)), l2, &clear);
                ly += U(6.4f) + U(1.6f);
                ly = NickRow(ImVec2(px, ly), lw) + U(1.6f);
                Text(F_SMALL, ImVec2(px, ly), C(kDim), g_settings.net.relay[0] ? "Код комнаты (6 цифр) или адрес Tailscale пришлёт друг." : "Адрес пришлёт друг: «Копировать» в его меню. Он начинается на 100.", lw);
                Keypad(ImVec2(px + pw - keysW, y), keysW);
                if(paste)
                {
                    char text[256], ip[32];
                    if(platform::ClipboardGetText(text, sizeof(text)) && platform::ExtractIPv4(text, ip, sizeof(ip)))
                    { g_ip = ip; char t[192]; snprintf(t, sizeof(t), "Вставлено: %s", ip); Toast(t); }
                    else Toast("В буфере нет адреса");
                }
                if(clear) g_ip.clear();
            }
        }

        } // режимы и панель

        // --- нижняя полоса ---
        {
            float k = anim(0.2f);
            float dy = (1 - k) * barH;
            const char* label = g_sel == net::Role::Off ? "Применить" : g_sel == net::Role::Echo ? "Запустить"
                              : g_sel == net::Role::Host ? "Открыть мир" : "Подключиться";
            bool enabled = g_sel != net::Role::Client || ValidIp(g_ip) || (g_settings.net.relay[0] && RoomCode(g_ip));
            float bw = TextSize(F_HEAD, label).x + U(12);
            ImVec2 ba(x1 - bw, y1 - barH + U(1) + dy), bb(x1, y1 + dy);
            const char* hint = enabled ? "Выбери режим и нажми кнопку справа" : "Введи адрес хоста полностью, например 100.87.12.40";
            ImVec2 hs = TextSize(F_SMALL, hint);
            float hx = x0;
            if(g_sel == net::Role::Client && coop::CanSaveHostWorld())
            {   // друг: сохранить свежий мир хоста себе в ячейку 8 (продолжить потом одному)
                const char* sl = "Сохранить мир хоста себе";
                float sw = ChipWidth(sl) + U(2);
                bool sc = false;
                Chip(ImVec2(x0, ba.y + U(1.2f)), ImVec2(x0 + sw, bb.y - U(1.2f)), sl, &sc);
                if(sc) { coop::SaveHostWorld(); Toast("Сохраняю мир хоста в ячейку 8…"); }
                hx = x0 + sw + U(2.4f);
            }
            if(g_sel == net::Role::Host)
            {   // кнопка «Оружие» (только у хоста)
                const char* wl = g_weapOpen ? "Скрыть оружие" : "Оружие";
                float ww = ChipWidth(wl) + U(2);
                bool wc = false;
                Chip(ImVec2(x0, ba.y + U(1.2f)), ImVec2(x0 + ww, bb.y - U(1.2f)), wl, &wc);
                if(wc) g_weapOpen = !g_weapOpen;
                hx = x0 + ww + U(2.4f);
            }
            if(!g_weapOpen) Text(F_SMALL, ImVec2(hx, (ba.y + bb.y - hs.y) * 0.5f), C(kDim), hint);
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
        float target2 = g_settings.sharedWanted ? 1.0f : 0.0f;
        g_swKnob += (target2 - g_swKnob) * Clamp01(ImGui::GetIO().DeltaTime * 14.0f);
    }


    // ======================= слой поверх игры =======================
    struct FeedItem { char text[192]; Rgb color; double t; };
    std::vector<FeedItem> g_feed;
    void Feed(Rgb color, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Feed(Rgb color, const char* fmt, ...)
    {
        FeedItem f; f.color = color; f.t = g_now;
        va_list a; va_start(a, fmt); vsnprintf(f.text, sizeof(f.text), fmt, a); va_end(a);
        if(g_feed.size() > 4) g_feed.erase(g_feed.begin());
        g_feed.push_back(f);
    }

    void ProcessEvents()
    {
        char note[160];
        while(coop::PopNotice(note, sizeof(note))) Feed(kGold, "%s", note);
        net::Event e;
        while(coop::PopEvent(e))
        {
            switch(e.type)
            {
                case net::EventType::Connected:
                    if(e.pingMs >= 0) Feed(kGrove, "%s в игре · %d мс", e.nick, e.pingMs);
                    else              Feed(kGrove, "%s в игре", e.nick);
                    break;
                case net::EventType::Disconnected:   Feed(kRed,  "%s отключился · потеря связи", e.nick); break;
                case net::EventType::DeniedByHost:   Feed(kRed,  "Хост отклонил запрос"); break;
                case net::EventType::HostBusy:       Feed(kGold, "У хоста уже играет другой игрок"); break;
                case net::EventType::RequestExpired: Feed(kGold, "Запрос %s истёк", e.nick); break;
                case net::EventType::RoomNotFound:   Feed(kRed, "Комнаты с таким кодом нет — проверь код"); break;
                case net::EventType::JoinRequest:    break; // показывается окном запроса
            }
        }
    }

    struct Box { ImVec2 a, b; };

    // Лента уведомлений: anchor — середина нижнего края. Возвращает занятую область.
    Box DrawFeed(ImVec2 anchor, bool sample)
    {
        float y = anchor.y;
        Box box{ ImVec2(anchor.x - U(30), anchor.y - U(6)), ImVec2(anchor.x + U(30), anchor.y) };
        std::vector<FeedItem> demo;
        if(sample) { FeedItem f; snprintf(f.text, sizeof(f.text), "Пример уведомления: друг в игре"); f.color = kGrove; f.t = g_now - 1.0; demo.push_back(f); }
        std::vector<FeedItem>& items = sample ? demo : g_feed;
        for(int i = (int)items.size() - 1; i >= 0; --i)
        {
            float t = (float)(g_now - items[i].t);
            if(!sample && t > 3.6f) { items.erase(items.begin() + i); continue; }
            float kin = sample ? 1 : EaseBack(t / 0.45f), kout = (!sample && t > 3.2f) ? 1 - (t - 3.2f) / 0.4f : 1;
            float sa = g_alpha; g_alpha = Clamp01(kin) * Clamp01(kout);
            ImVec2 ts = TextSize(F_BODY, items[i].text);
            float w = ts.x + U(5.6f), h = ts.y + U(2.8f);
            float dy = (1 - Clamp01(kin)) * U(3) - (1 - kout) * U(2);
            ImVec2 a(anchor.x - w * 0.5f, y - h + dy), b(a.x + w, a.y + h);
            DL()->AddRectFilled(ImVec2(a.x, a.y + U(1)), ImVec2(b.x, b.y + U(1)), C(kBlack, 0.35f), U(1.4f));
            DL()->AddRectFilled(a, b, C(kNight, 0.88f), U(1.4f));
            DL()->AddRectFilled(a, ImVec2(a.x + U(1), b.y), C(items[i].color), U(1.4f), ImDrawFlags_RoundCornersLeft);
            Text(F_BODY, ImVec2(a.x + U(3), a.y + U(1.4f)), C(kInk), items[i].text);
            g_alpha = sa;
            box.a.x = fminf(box.a.x, a.x); box.b.x = fmaxf(box.b.x, b.x); box.a.y = fminf(box.a.y, a.y);
            y = a.y - U(1.2f);
        }
        return box;
    }

    // ---- окно «кто-то подключается» ----
    double g_reqShownAt = -10;
    char   g_reqNick[net::kNickLen] = "";
    void DrawRequest()
    {
        char nick[net::kNickLen]; float left = 0;
        if(!coop::PendingRequest(nick, sizeof(nick), &left)) { g_reqShownAt = -10; return; }
        if(g_reqShownAt < 0 || strcmp(nick, g_reqNick)) { g_reqShownAt = g_now; snprintf(g_reqNick, sizeof(g_reqNick), "%s", nick); }
        float k = EaseBack((float)(g_now - g_reqShownAt) / 0.45f);
        float w = fminf(CW(54), U(128)), h = U(30);
        ImVec2 a(W() * 0.5f - w * 0.5f, U(4) - (1 - k) * (h + U(6))), b(a.x + w, a.y + h);
        Glow(a, b, U(2), kGold, 1.2f);
        DL()->AddRectFilled(a, b, C(kNight, 0.97f), U(2));
        DL()->AddRect(a, b, C(kGold), U(2), 0, U(0.35f));
        // таймер
        float tr = U(5); ImVec2 tc(a.x + U(3) + tr, a.y + U(3) + tr);
        DL()->AddCircle(tc, tr, C(kLine), 40, U(0.7f));
        float frac = left / 15.0f;
        if(frac > 0.001f)
        {
            DL()->PathArcTo(tc, tr, -M_PI * 0.5f, -M_PI * 0.5f + 2 * M_PI * frac, 40);
            DL()->PathStroke(C(kGold), 0, U(0.7f));
        }
        char sec[8]; snprintf(sec, sizeof(sec), "%d", (int)ceilf(left));
        ImVec2 ss = TextSize(F_KEY, sec);
        Text(F_KEY, ImVec2(tc.x - ss.x * 0.5f, tc.y - ss.y * 0.5f), C(kInk), sec);
        float tx = tc.x + tr + U(3);
        Text(F_SMALL, ImVec2(tx, a.y + U(3)), C(kGold), "ПОДКЛЮЧЕНИЕ К ТВОЕМУ МИРУ");
        Text(F_HEAD, ImVec2(tx, a.y + U(6.6f)), C(kInk), nick);
        // кнопки
        float bh = U(8.5f), gap = U(1.6f), bw = (w - U(6) - gap) * 0.5f;
        ImVec2 ya(a.x + U(3), b.y - U(3) - bh), yb(ya.x + bw, ya.y + bh);
        ImVec2 na(yb.x + gap, ya.y), nb(na.x + bw, yb.y);
        bool yes = false, no = false;
        BigButton(ya, yb, "Принять", true, &yes);
        bool held = Held(na, nb);
        DL()->AddRectFilled(ImVec2(na.x, na.y + U(0.6f)), ImVec2(nb.x, nb.y + U(0.6f)), C(Rgb{ 10, 14, 20 }), U(1.6f));
        DL()->AddRectFilled(ImVec2(na.x, na.y + (held ? U(0.5f) : 0)), ImVec2(nb.x, nb.y + (held ? U(0.5f) : 0)), C(Rgb{ 27, 34, 48 }), U(1.6f));
        ImVec2 ns = TextSize(F_HEAD, "Отклонить");
        Text(F_HEAD, ImVec2((na.x + nb.x - ns.x) * 0.5f, (na.y + nb.y - ns.y) * 0.5f + (held ? U(0.5f) : 0)), C(kInk), "Отклонить");
        no = Clicked(na, nb);
        if(yes) { coop::AnswerJoin(true); }
        if(no)  { coop::AnswerJoin(false); Feed(kGold, "Запрос %s отклонён", nick); }
    }

    // ---- панель «Команда» ----
    double g_mateInAt = -10, g_mateGoneAt = -10;
    bool   g_mateWasPresent = false;
    char   g_mateGoneNick[net::kNickLen] = "";

    void Bars(ImVec2 a, float w, float hp, float ar)
    {
        float h = U(0.9f), gap = U(0.5f);
        DL()->AddRectFilled(a, ImVec2(a.x + w, a.y + h), C(kWhite, 0.12f), h);
        DL()->AddRectFilledMultiColor(a, ImVec2(a.x + w * Clamp01(hp / 100.0f), a.y + h),
                                      C(Rgb{ 192, 57, 43 }), C(Rgb{ 255, 92, 92 }), C(Rgb{ 255, 92, 92 }), C(Rgb{ 192, 57, 43 }));
        ImVec2 b(a.x, a.y + h + gap);
        DL()->AddRectFilled(b, ImVec2(b.x + w, b.y + h), C(kWhite, 0.12f), h);
        if(ar > 0.5f)
            DL()->AddRectFilledMultiColor(b, ImVec2(b.x + w * Clamp01(ar / 100.0f), b.y + h),
                                          C(Rgb{ 159, 179, 200 }), C(Rgb{ 232, 238, 245 }), C(Rgb{ 232, 238, 245 }), C(Rgb{ 159, 179, 200 }));
    }

    void MicGlyph(ImVec2 c, float s, ImU32 col)
    {
        float t = s * 0.12f;
        DL()->AddRect(ImVec2(c.x - s * 0.18f, c.y - s * 0.45f), ImVec2(c.x + s * 0.18f, c.y + s * 0.12f), col, s * 0.18f, 0, t);
        DL()->PathArcTo(ImVec2(c.x, c.y - s * 0.02f), s * 0.34f, 0.15f, (float)M_PI - 0.15f, 12);
        DL()->PathStroke(col, 0, t);
        DL()->AddLine(ImVec2(c.x, c.y + s * 0.32f), ImVec2(c.x, c.y + s * 0.48f), col, t);
    }

    float MateRow(ImVec2 a, float w, Rgb stripe, const char* nick, const char* meta, float hp, float ar,
                  bool talking, bool lost, bool showMic)
    {
        float h = U(8.6f);
        ImVec2 b(a.x + w, a.y + h);
        DL()->AddRectFilledMultiColor(a, b, C(kNight, 0.88f), C(kNight, 0.55f), C(kNight, 0.55f), C(kNight, 0.88f));
        DL()->AddRectFilled(a, ImVec2(a.x + U(0.8f), b.y), C(lost ? kRed : stripe));
        if(talking) DL()->AddRect(a, b, C(kGrove, 0.7f), 0, 0, U(0.3f));
        float sa = g_alpha; if(lost) g_alpha *= 0.55f;
        float fs = g_fonts[F_HEAD]->FontSize * 0.62f * g_s;
        DL()->AddText(g_fonts[F_HEAD], fs, ImVec2(a.x + U(2.2f), a.y + U(1.2f) + U(0.3f)), C(kBlack), nick);
        DL()->AddText(g_fonts[F_HEAD], fs, ImVec2(a.x + U(2.2f), a.y + U(1.2f)), C(lost ? kDim : kInk), nick);
        ImVec2 ms = TextSize(F_SMALL, meta);
        float mx = b.x - U(1.6f) - ms.x;
        Text(F_SMALL, ImVec2(mx, a.y + U(1.6f)), C(lost ? kRed : kGrove), meta);
        if(showMic)
        {
            float pulse = talking ? 1 + 0.25f * sinf((float)g_now * 12) : 1;
            MicGlyph(ImVec2(mx - U(2.4f), a.y + U(3)), U(2.8f) * pulse, C(talking ? kGrove : kDim));
        }
        Bars(ImVec2(a.x + U(2.2f), a.y + U(5.4f)), w - U(3.8f), hp, ar);
        g_alpha = sa;
        return b.y;
    }

    // topLeft — левый верхний угол панели. Возвращает занятую область.
    Box DrawTeam(const coop::Team& t, ImVec2 topLeft)
    {
        float x = topLeft.x, y = topLeft.y, w = U(36);
        Box box{ topLeft, ImVec2(x + w, y + U(3.6f) + U(8.6f) * 2 + U(1)) };
        if(!t.show) { g_mateWasPresent = false; return box; }
        TextOutlined(F_SMALL, ImVec2(x, y), kGold, t.mate.present ? "КОМАНДА  2" : "КОМАНДА  1", U(0.2f));
        y += U(3.6f);
        const char* role = t.role == net::Role::Host ? "хост" : t.role == net::Role::Client ? "гость" : "тест";
        char meNick[64]; snprintf(meNick, sizeof(meNick), "%s · ты", t.myNick);
        y = MateRow(ImVec2(x, y), w, kGold, meNick, role, t.myHealth, t.myArmour, false, false, false) + U(1);

        if(t.mate.present && !g_mateWasPresent) g_mateInAt = g_now;
        if(!t.mate.present && g_mateWasPresent) { g_mateGoneAt = g_now; }
        g_mateWasPresent = t.mate.present;
        if(t.mate.present) snprintf(g_mateGoneNick, sizeof(g_mateGoneNick), "%s", t.mate.nick);

        if(t.mate.present)
        {
            float k = EaseBack((float)(g_now - g_mateInAt) / 0.55f);
            char meta[64], ping[16] = "", dist[16] = "";
            if(t.mate.pingMs >= 0) snprintf(ping, sizeof(ping), "%d мс  ", t.mate.pingMs);
            if(t.mate.distM >= 0) snprintf(dist, sizeof(dist), "%.0f м", t.mate.distM);
            snprintf(meta, sizeof(meta), "%s%s", t.mate.lost ? "нет связи  " : ping, dist);
            float sa = g_alpha; g_alpha = Clamp01(k);
            MateRow(ImVec2(x - (1 - Clamp01(k)) * w * 1.1f, y), w, kGrove, t.mate.nick, meta,
                    t.mate.health, t.mate.armour, t.mate.talking, t.mate.lost, true);
            g_alpha = sa;
        }
        else if(g_now - g_mateGoneAt < 0.5)
        {   // уезжает влево
            float k = (float)(g_now - g_mateGoneAt) / 0.5f;
            float sa = g_alpha; g_alpha = 1 - k;
            MateRow(ImVec2(x - EaseOut(k) * w * 1.1f, y), w, kRed, g_mateGoneNick, "", 0, 0, false, true, false);
            g_alpha = sa;
        }
        return box;
    }

    // ---- «друг говорит»: anchor — середина верхнего края ----
    double g_talkSince = -10;
    Box DrawTalking(const coop::Team& t, ImVec2 anchor, bool sample)
    {
        bool on = sample || (t.mate.present && t.mate.talking && !t.mate.lost);
        if(on && g_talkSince < 0) g_talkSince = g_now;
        if(!on) g_talkSince = -10;
        char text[64]; snprintf(text, sizeof(text), "%s говорит", sample ? "Друг" : t.mate.nick);
        ImVec2 ts = TextSize(F_BODY, text);
        float eqW = U(3.6f), w = ts.x + eqW + U(6.5f), h = ts.y + U(2.4f);
        Box box{ ImVec2(anchor.x - w * 0.5f, anchor.y), ImVec2(anchor.x + w * 0.5f, anchor.y + h) };
        if(!on) return box;
        float k = sample ? 1 : EaseBack((float)(g_now - g_talkSince) / 0.35f);
        ImVec2 a(anchor.x - w * 0.5f, anchor.y - (1 - k) * (h + U(4))), b(a.x + w, a.y + h);
        DL()->AddRectFilled(a, b, C(kNight, 0.85f), h * 0.5f);
        DL()->AddRect(a, b, C(kGrove), h * 0.5f, 0, U(0.3f));
        for(int i = 0; i < 3; ++i)
        {
            float lv = (sample || !voice::Available()) ? 0.7f : voice::OutputLevel();
            float v = 0.2f + 0.8f * lv * (0.5f + 0.5f * fabsf(sinf((float)g_now * 9 + i * 1.3f)));
            float bx = a.x + U(2.4f) + i * U(1.2f), bh = U(2.6f) * v;
            DL()->AddRectFilled(ImVec2(bx, (a.y + b.y) * 0.5f + U(1.3f) - bh), ImVec2(bx + U(0.7f), (a.y + b.y) * 0.5f + U(1.3f)), C(kGrove), U(0.3f));
        }
        Text(F_BODY, ImVec2(a.x + U(2.4f) + eqW + U(1.4f), a.y + U(1.2f)), C(kInk), text);
        return box;
    }

    // ---- кнопка микрофона: c — центр ----
    bool   g_micHeld = false;
    double g_micSince = 0;
    bool   g_micHintShown = false;
    bool   g_micAsked = false;          // системный запрос уже показывали в этом запуске
    Box DrawMic(const coop::Team& t, ImVec2 c, bool interactive)
    {
        float r = U(7.2f);
        ImVec2 a(c.x - r, c.y - r), b(c.x + r, c.y + r);
        Box box{ ImVec2(a.x, a.y - U(4)), ImVec2(b.x, b.y + U(5)) };
        bool held = interactive && HeldFrom(a, b);
        if(!t.show && interactive) held = false;
        if(interactive && held != g_micHeld)
        {
            g_micHeld = held; g_micSince = g_now;
            coop::SetTalking(held);
            if(held)
            {
                // Разрешение на микрофон: нет — показываем системный запрос (один раз за запуск),
                // отказали — подсказываем, где включить вручную.
                int mic = platform::HasPermission("android.permission.RECORD_AUDIO");
                if(mic == 0)
                {
                    // Как в обычных приложениях: сначала системное окно с вопросом;
                    // если его показать нельзя (или уже отказывали) — открываем страницу игры
                    // в настройках, там включаешь микрофон и «Назад» возвращает в игру.
                    bool asked = false;
                    if(!g_micAsked) { g_micAsked = true; asked = platform::RequestPermission("android.permission.RECORD_AUDIO"); }
                    if(asked) { Feed(kGold, "Разреши доступ к микрофону в окне Android"); if(g_log) g_log->Info("Микрофон: показан системный запрос"); }
                    else if(platform::OpenAppSettings()) { Feed(kGold, "Разрешения > Микрофон > Разрешить, потом «Назад» — вернёшься в игру"); if(g_log) g_log->Info("Микрофон: открыты настройки приложения"); }
                    else { Feed(kRed, "Включи микрофон: Настройки > Приложения > GTA SA > Разрешения"); if(g_log) g_log->Error("Микрофон: ни запрос, ни настройки открыть не удалось"); }
                }
                else if(mic == 1 && !g_micHintShown)
                {
                    g_micHintShown = true;
                    if(!voice::Available())          Feed(kRed, "Голос не работает на этом телефоне (нужен Android 8+)");
                    else if(t.role == net::Role::Echo) Feed(kGrove, "Говори — через полсекунды услышишь себя");
                    else if(!t.mate.present || t.mate.lost) Feed(kGold, "Друг не подключён — говорить некому");
                }
            }
        }
        float tt = (float)(g_now - g_micSince);
        DL()->AddCircleFilled(ImVec2(c.x, c.y + U(0.8f)), r, C(kBlack, 0.45f), 48);
        if(held)
        {
            float ph = fmodf(tt, 1.1f) / 1.1f;
            DL()->AddCircle(c, r * (1.05f + 0.4f * ph), C(kRed, 0.9f * (1 - ph)), 48, U(0.5f));
        }
        int segs = 24;
        for(int i = 0; i < segs; ++i)
        {
            float a0 = i * 2 * (float)M_PI / segs + (held ? tt * 2.6f : 0), a1 = a0 + 2 * (float)M_PI / segs + 0.02f;
            float v = 0.5f + 0.5f * cosf(a0 * 2);
            Rgb c0 = held ? Rgb{ (uint8_t)(150 + 105 * v), (uint8_t)(20 + 60 * v), (uint8_t)(20 + 60 * v) }
                          : Rgb{ (uint8_t)(155 + 100 * v), (uint8_t)(106 + 118 * v), (uint8_t)(18 + 120 * v) };
            DL()->PathArcTo(c, r - U(0.7f), a0, a1, 3);
            DL()->PathStroke(C(c0), 0, U(1.4f));
        }
        DL()->AddCircleFilled(c, r - U(1.4f), C(held ? Rgb{ 142, 16, 16 } : Rgb{ 11, 15, 21 }), 48);
        DL()->AddCircleFilled(ImVec2(c.x - r * 0.2f, c.y - r * 0.25f), r * 0.45f, C(held ? Rgb{ 255, 107, 107 } : Rgb{ 42, 52, 66 }, 0.55f), 32);
        MicGlyph(c, r * 0.95f, C(held ? kWhite : kGold));
        const char* lbl = held ? "ГОВОРИ" : "ЗАЖМИ";
        ImVec2 ls = TextSize(F_SMALL, lbl);
        TextOutlined(F_SMALL, ImVec2(c.x - ls.x * 0.5f, a.y - ls.y - U(0.8f)), held ? Rgb{ 255, 214, 214 } : kGold, lbl, U(0.2f));
        if(held)
            for(int i = 0; i < 5; ++i)
            {
                float lv = voice::Available() ? voice::InputLevel() : 0.6f;
                float v = 0.15f + 0.85f * lv * (0.55f + 0.45f * fabsf(sinf((float)g_now * 10 + i * 1.7f)));
                float bx = c.x - U(3.2f) + i * U(1.5f), bh = U(3.6f) * v;
                DL()->AddRectFilled(ImVec2(bx, b.y + U(4.6f) - bh), ImVec2(bx + U(0.9f), b.y + U(4.6f)), C(kRed), U(0.4f));
            }
        return box;
    }

    // ======================= раскладка и её редактор =======================
    settings::Layout g_layout;
    bool             g_layoutLoaded = false;
    bool             g_edit = false;
    settings::Layout g_editLayout;
    int              g_editSel = settings::L_MIC;
    Box              g_editBox[settings::L_COUNT];
    struct Drag { int item = -1; ImVec2 offset; };
    Drag             g_drag[kMaxFingers];
    float            g_pinchDist0 = 0, g_pinchScale0 = 1; int g_pinchItem = -1;

    const char* kItemNames[settings::L_COUNT] = { "Микрофон", "Команда", "Уведомления", "Говорит" };

    ImVec2 ItemPos(const settings::Layout& l, int i) { return ImVec2(l.item[i].x * W(), l.item[i].y * H()); }

    // Рисует все элементы по раскладке l. sample — показать примеры (в редакторе).
    void DrawHudItems(const coop::Team& team, const settings::Layout& l, bool sample)
    {
        coop::Team t = team;
        if(sample)
        {
            t.show = true; if(!t.myNick[0]) snprintf(t.myNick, sizeof(t.myNick), "%s", g_settings.net.nick);
            if(t.myHealth <= 0) t.myHealth = 100;
            if(!t.mate.present) { t.mate.present = true; snprintf(t.mate.nick, sizeof(t.mate.nick), "Друг"); t.mate.health = 80; t.mate.armour = 50; t.mate.pingMs = 84; t.mate.distM = 12; }
        }
        g_s = l.item[settings::L_TEAM].s; g_editBox[settings::L_TEAM] = DrawTeam(t, ItemPos(l, settings::L_TEAM));
        g_s = l.item[settings::L_TALK].s; g_editBox[settings::L_TALK] = DrawTalking(t, ItemPos(l, settings::L_TALK), sample);
        if(t.show || sample) { g_s = l.item[settings::L_MIC].s; g_editBox[settings::L_MIC] = DrawMic(t, ItemPos(l, settings::L_MIC), !sample); }
        else if(g_micHeld) { g_micHeld = false; coop::SetTalking(false); }
        g_s = l.item[settings::L_FEED].s; g_editBox[settings::L_FEED] = DrawFeed(ItemPos(l, settings::L_FEED), sample);
        g_s = 1.0f;
    }

    void DashedRect(ImVec2 a, ImVec2 b, ImU32 col, float th)
    {
        float dash = U(1.6f), gap = U(1.0f);
        auto line = [&](ImVec2 p, ImVec2 q) {
            float len = sqrtf((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
            for(float d = 0; d < len; d += dash + gap)
            {
                float e = fminf(d + dash, len);
                DL()->AddLine(ImVec2(p.x + (q.x - p.x) * d / len, p.y + (q.y - p.y) * d / len),
                              ImVec2(p.x + (q.x - p.x) * e / len, p.y + (q.y - p.y) * e / len), col, th);
            }
        };
        line(a, ImVec2(b.x, a.y)); line(ImVec2(b.x, a.y), b); line(b, ImVec2(a.x, b.y)); line(ImVec2(a.x, b.y), a);
    }

    void EnterEdit()
    {
        g_edit = true; g_editLayout = g_layout; g_editSel = settings::L_MIC;
        for(auto& d : g_drag) d = Drag();
        g_pinchItem = -1;
        coop::LockLocalControls(true);
        Feed(kGold, "Тяни элементы пальцем. Размер — щипком двумя пальцами или кнопками - / +");
    }
    void LeaveEdit(bool save)
    {
        if(save) { g_layout = g_editLayout; settings::SaveLayout(g_layout); Feed(kGrove, "Расположение сохранено"); }
        g_edit = false;
        coop::LockLocalControls(false);
    }

    void DrawEditor(const coop::Team& team, bool ownBar = true)
    {
        Box bar{ ImVec2(-1, -1), ImVec2(-1, -1) };
        if(!ownBar)
        {   // внутри «Настройки сенсоров» игры: свои кнопки игры там уже есть (Сохранить / Отмена / По умолчанию)
            DrawHudItems(team, g_editLayout, true);
        }
        else
        {
        // затемнение, чтобы было видно, что это режим настройки
        DL()->AddRectFilled(ImVec2(0, 0), ImVec2(W(), H()), C(Rgb{ 4, 6, 10 }, 0.35f));
        DrawHudItems(team, g_editLayout, true);

        // ---- панель кнопок снизу по центру ----
        float bh = U(8), gap = U(1.2f);
        const char* lbls[5] = { "-", "+", "Сброс", "Отмена", "Готово" };
        float ws[5]; float total = 0;
        for(int i = 0; i < 5; ++i)
        {
            ws[i] = (i == 4 ? TextSize(F_HEAD, lbls[i]).x + U(8) : TextSize(F_SMALL, lbls[i]).x + U(i < 2 ? 6 : 4.4f));
            total += ws[i] + (i ? gap : 0);
        }
        char title[96];
        snprintf(title, sizeof(title), "РАСПОЛОЖЕНИЕ · %s · %d%%", kItemNames[g_editSel], (int)roundf(g_editLayout.item[g_editSel].s * 100));
        ImVec2 tsz = TextSize(F_SMALL, title);
        float pw = fmaxf(total, tsz.x) + U(4), ph = bh + tsz.y + U(5);
        ImVec2 pa(W() * 0.5f - pw * 0.5f, H() - ph - U(3)), pb(pa.x + pw, pa.y + ph);
        bar = Box{ pa, pb };
        DL()->AddRectFilled(pa, pb, C(kNight, 0.92f), U(1.6f));
        DL()->AddRect(pa, pb, C(kGold), U(1.6f), 0, U(0.3f));
        Text(F_SMALL, ImVec2(W() * 0.5f - tsz.x * 0.5f, pa.y + U(1.2f)), C(kGold), title);
        float x = W() * 0.5f - total * 0.5f, y = pa.y + tsz.y + U(2.4f);
        bool click[5] = {};
        for(int i = 0; i < 5; ++i)
        {
            ImVec2 a(x, y), b(x + ws[i], y + bh);
            if(i == 4) BigButton(a, b, lbls[i], true, &click[i]);
            else Chip(a, b, lbls[i], &click[i]);
            x += ws[i] + gap;
        }
        settings::LayoutItem& sel = g_editLayout.item[g_editSel];
        if(click[0]) sel.s = fmaxf(0.5f, sel.s - 0.1f);
        if(click[1]) sel.s = fminf(2.5f, sel.s + 0.1f);
        if(click[2]) { g_editLayout = settings::DefaultLayout(); Feed(kGold, "Расположение по умолчанию"); }
        if(click[3]) { LeaveEdit(false); return; }
        if(click[4]) { LeaveEdit(true); return; }
        }

        // ---- выделение и перетаскивание ----
        for(int i = 0; i < settings::L_COUNT; ++i)
        {
            Box& bx = g_editBox[i];
            ImVec2 a(bx.a.x - U(0.8f), bx.a.y - U(0.8f)), b(bx.b.x + U(0.8f), bx.b.y + U(0.8f));
            bool s = i == g_editSel;
            DashedRect(a, b, C(s ? kGold : kWhite, s ? 1.0f : 0.6f), U(s ? 0.4f : 0.25f));
            float lh = TextSize(F_SMALL, kItemNames[i]).y;
            float ly = a.y - lh - U(0.4f);
            if(ly < U(0.5f)) ly = b.y + U(0.4f);   // у верхнего края — подпись под рамкой
            Text(F_SMALL, ImVec2(a.x, ly), C(s ? kGold : kWhite, 0.9f), kItemNames[i]);
        }
        int fingersOn[settings::L_COUNT] = {}; int fingerA[settings::L_COUNT], fingerB[settings::L_COUNT];
        for(int f = 0; f < kMaxFingers; ++f)
        {
            Finger& F = g_f[f];
            if(F.pressed && !In(F.downPos, bar.a, bar.b))
            {
                for(int i = settings::L_COUNT - 1; i >= 0; --i)
                {
                    Box& bx = g_editBox[i];
                    if(In(F.downPos, ImVec2(bx.a.x - U(2), bx.a.y - U(2)), ImVec2(bx.b.x + U(2), bx.b.y + U(2))))
                    {
                        g_drag[f].item = i; g_editSel = i;
                        ImVec2 p = ItemPos(g_editLayout, i);
                        g_drag[f].offset = ImVec2(p.x - F.downPos.x, p.y - F.downPos.y);
                        break;
                    }
                }
            }
            if(!F.down) g_drag[f].item = -1;
            int it = g_drag[f].item;
            if(it >= 0) { if(fingersOn[it] == 0) fingerA[it] = f; else fingerB[it] = f; fingersOn[it]++; }
        }
        for(int i = 0; i < settings::L_COUNT; ++i)
        {
            settings::LayoutItem& li = g_editLayout.item[i];
            if(fingersOn[i] >= 2)
            {   // щипок двумя пальцами — размер
                ImVec2 p = g_f[fingerA[i]].pos, q = g_f[fingerB[i]].pos;
                float d = sqrtf((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
                if(g_pinchItem != i) { g_pinchItem = i; g_pinchDist0 = d; g_pinchScale0 = li.s; }
                if(g_pinchDist0 > 1) li.s = fminf(2.5f, fmaxf(0.5f, g_pinchScale0 * d / g_pinchDist0));
            }
            else
            {
                if(g_pinchItem == i) g_pinchItem = -1;
                if(fingersOn[i] == 1)
                {
                    Finger& F = g_f[fingerA[i]];
                    ImVec2 p(F.pos.x + g_drag[fingerA[i]].offset.x, F.pos.y + g_drag[fingerA[i]].offset.y);
                    li.x = Clamp01(p.x / W()); li.y = Clamp01(p.y / H());
                }
            }
        }
    }

    void Frame(bool fromMenu)
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
        ProcessEvents();
        if(fromMenu && g_open) DrawMenu();
        g_alpha = 1.0f;
        if(!g_layoutLoaded) { g_layout = settings::LoadLayout(); g_layoutLoaded = true; }
        coop::Team team = coop::GetTeam();
        if(g_editRequested) { g_editRequested = false; EnterEdit(); }
        if(g_inAdjDraw)
        {   // экран «Настройка сенсоров»: наши элементы среди кнопок игры, тянуть пальцем / щипок
            DrawEditor(team, false);
            imgui_rw::Render();
            return;
        }
        if(!fromMenu)
        {
            if(g_edit) DrawEditor(team);
            else       DrawHudItems(team, g_layout, false);
        }
        else if(g_edit)
        {   // редактор открыт из Настройки > Управление — прямо поверх меню игры
            DrawEditor(team);
        }
        else
        {   // поверх меню игры — только уведомления (по раскладке)
            g_s = g_layout.item[settings::L_FEED].s;
            DrawFeed(ItemPos(g_layout, settings::L_FEED), false);
            g_s = 1.0f;
        }
        if(!g_edit) DrawRequest();
        DrawJoin();
        imgui_rw::Render();
    }
}

// ---------- перехваты ----------

DECL_HOOKv(MobileMenuRender, uintptr_t self)
{
    MobileMenuRender(self);
    coop::MenuTick();                 // сеть в главном меню (подключение к хосту до загрузки игры)
    coop::KeepWorldRunningInMenu();   // в коопе меню не ставит игру на паузу
    Frame(true);                      // меню CO-OP (если открыто) + запросы и уведомления
}

// ---- «Настройка сенсоров» самой игры ----
static void AdjBegin()
{
    double now = NowSec();
    if(now - g_adjSeenAt > 1.0)
    {   // экран только что открылся: начинаем с сохранённой раскладки
        if(!g_layoutLoaded) { g_layout = settings::LoadLayout(); g_layoutLoaded = true; }
        g_editLayout = g_layout; g_editSel = settings::L_MIC;
        for(auto& d : g_drag) d = Drag();
        g_pinchItem = -1;
        if(g_log) g_log->Info("Настройка сенсоров открыта: кнопки CO-OP можно двигать там же");
    }
    g_adjSeenAt = now;
}

DECL_HOOKv(AdjustableHudDraw, uintptr_t self)
{
    AdjustableHudDraw(self);
    AdjBegin();
    g_inAdjDraw = true;
    Frame(true);
    g_inAdjDraw = false;
}

// Кнопки самого экрана: self+0x64 — нажатая (2 сохранить, 3 отмена),
// после обработки становится -1; self+0x52E == 1 — экран закрывается (так же делает helibrain).
DECL_HOOKv(AdjustableHudUpdate, uintptr_t self)
{
    int before = self ? *(int*)(self + 0x64) : -1;
    AdjustableHudUpdate(self);
    if(!self) return;
    AdjBegin();
    int after = *(int*)(self + 0x64);
    if(before >= 2 && before <= 4 && after == -1)
    {
        bool closing = *(uint8_t*)(self + 0x52E) == 1;
        // Значение 4 приходит не только от «По умолчанию», но и при перетаскивании кнопок
        // игры — по нему наши элементы сбрасывались на каждом движении. Его не трогаем.
        if(before == 2 && closing) { g_layout = g_editLayout; settings::SaveLayout(g_layout); if(g_log) g_log->Info("Настройка сенсоров: сохранено — кнопки CO-OP тоже"); }
        else if(before == 3 && closing) { g_editLayout = g_layout; if(g_log) g_log->Info("Настройка сенсоров: отмена — кнопки CO-OP на прежних местах"); }
        if(closing) g_adjSeenAt = -10;
    }
}

// Пока открыт редактор раскладки, камера не обновляется: иначе пальцы,
// которые двигают элементы, заодно крутили бы камеру.
DECL_HOOKv(CameraProcess, uintptr_t self)
{
    if(g_edit) return;
    CameraProcess(self);
}

// Слой поверх игры: панель «Команда», кнопка микрофона, запросы, уведомления
DECL_HOOKv(Render2dStuff)
{
    // Пока игра рисует свой интерфейс, второго CJ нет в слоте игрока 2 —
    // иначе она рисует вторую иконку оружия («дубль кулака»).
    uintptr_t token = coop::HideRemoteBegin();
    Render2dStuff();
    coop::HideRemoteEnd(token);
    Frame(false);
}

// Пока открыто наше меню (и пока не отпущен палец после закрытия), меню игры
// не обновляется — иначе нажатия попадали бы и в него. Меню игры читает касания
// опросом, поэтому накопленных «призрачных» нажатий потом не будет.
static double g_blockGameMenuUntil = 0;
DECL_HOOKv(MobileMenuUpdate, uintptr_t self)
{
    if(coop::MenuUpdateTick()) return; // мир хоста получен в главном меню — загрузка (как «Продолжить»), меню уже закрыто
    double now = NowSec();
    if(g_resumeGame && !g_open && Menu_SwitchOffToGame)
    {   // «Экран»: сразу в игру, там откроется редактор
        g_resumeGame = false;
        Menu_SwitchOffToGame();
        return;
    }
    char rn[net::kNickLen]; float rl;
    if(g_open || g_edit || coop::PendingRequest(rn, sizeof(rn), &rl)) { g_blockGameMenuUntil = now + 0.3; return; }
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
    if(!ValidIp(g_ip) && !RoomCode(g_ip)) g_ip = g_settings.net.relay[0] ? "" : "100.";
    g_ffKnob = g_settings.friendlyFire ? 1.0f : 0.0f;
    g_swKnob = g_settings.sharedWanted ? 1.0f : 0.0f;
    g_now = NowSec();
    g_openedAt = g_now; g_selAt = -10; g_toastAt = -10;
    ResetFingers();
    g_closing = false;
    g_open = true;
    if(g_log) g_log->Info("Меню Co-op открыто");
}

bool IsOpen() { return g_open; }

#ifdef SACOOP_PREVIEW
void DebugRequestEdit() { g_editRequested = true; }   // только для превью на компьютере
#endif

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
    Menu_SwitchOffToGame = (void(*)())aml->GetSym(h, "_Z20Menu_SwitchOffToGamev");
    void* adjDraw = (void*)aml->GetSym(h, "_ZN14CAdjustableHUD4DrawEv");
    void* adjUpd  = (void*)aml->GetSym(h, "_ZN14CAdjustableHUD6UpdateEv");
    if(adjDraw && adjUpd)
    {
        aml->Hook(adjDraw, (void*)HookOf_AdjustableHudDraw,   (void**)&AdjustableHudDraw);
        aml->Hook(adjUpd,  (void*)HookOf_AdjustableHudUpdate, (void**)&AdjustableHudUpdate);
    }
    else logger->Error("Нет CAdjustableHUD — кнопки CO-OP не появятся в «Настройке сенсоров»");
    void* camProcess = (void*)aml->GetSym(h, "_ZN7CCamera7ProcessEv");
    if(camProcess) aml->Hook(camProcess, (void*)HookOf_CameraProcess, (void**)&CameraProcess);
    void* r2d = (void*)aml->GetSym(h, "_Z13Render2dStuffv");
    if(r2d) aml->Hook(r2d, (void*)HookOf_Render2dStuff, (void**)&Render2dStuff);
    else logger->Error("Меню: нет Render2dStuff — панели поверх игры не будет");
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
