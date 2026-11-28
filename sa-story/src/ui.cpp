#include "ui.h"

#include <mod/amlmod.h>
#include <imgui/imgui.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "coop_role.h"
#include "fonts_data.h"
#include "game.h"
#include "imgui_rw.h"
#include "log.h"

namespace ui
{
namespace
{
    enum FontId { F_BIG, F_HEAD, F_BODY, F_SMALL, F_COUNT };
    ImFont* g_fonts[F_COUNT] = {};
    bool g_fontsBuilt = false;
    const ImWchar kRanges[] = { 0x0020, 0x00FF, 0x0400, 0x045F, 0x2010, 0x2026, 0x2116, 0x2116, 0 };

    double NowSec() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

    struct Timed { std::string a, b; double until = 0, start = 0; unsigned rgb = 0xFFFFFF; };
    Timed g_title, g_obj, g_sub, g_result, g_notice;
    bool g_resultPassed = false;
    int g_timer = -1;
    std::string g_counterLabel; int g_counter = -1;
    uint64_t g_lastUs = 0;

    // ---- касания: опрос Points через GOT (Touch x4 подменяет ячейку на свой массив на 4 пальца) ----
    struct GamePoint { int32_t x, y, state; uint8_t rest[0x1C - 12]; };
    constexpr uintptr_t kPointsGot = 0x851D38;
    GamePoint* g_pointsOrig = nullptr; GamePoint** g_pointsGot = nullptr;
    struct Finger { bool down = false, pressed = false, released = false; ImVec2 pos{ -1, -1 }, downPos{ -1, -1 }; };
    Finger g_f[4];
    double g_lastPoll = 0;
    void PollTouches()
    {
        double t = NowSec();
        if(t - g_lastPoll < 0.004) return;
        g_lastPoll = t;
        for(auto& f : g_f) f.pressed = f.released = false;
        GamePoint* p = g_pointsGot ? *g_pointsGot : nullptr;
        GamePoint* pts = p ? p : g_pointsOrig;
        int n = (pts && pts != g_pointsOrig) ? 4 : 2;
        for(int i = 0; i < 4; ++i)
        {
            Finger& f = g_f[i];
            bool down = pts && i < n && pts[i].state == 2 && pts[i].x >= 0;
            if(down) { ImVec2 q((float)pts[i].x, (float)pts[i].y); if(!f.down) { f.down = true; f.pressed = true; f.downPos = q; } f.pos = q; }
            else if(f.down) { f.down = false; f.released = true; }
        }
    }
    bool AnyDown() { for(auto& f : g_f) if(f.down) return true; return false; }
    bool In(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y; }
    bool HeldFrom(ImVec2 a, ImVec2 b) { for(auto& f : g_f) if(f.down && In(f.downPos, a, b)) return true; return false; }
    bool Clicked(ImVec2 a, ImVec2 b) { for(auto& f : g_f) if(f.released && In(f.downPos, a, b) && In(f.pos, a, b)) return true; return false; }

    ImU32 Col(unsigned rgb, float a) { if(a < 0) a = 0; if(a > 1) a = 1; return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (int)(a * 255)); }
    float Alpha(const Timed& t, double now)
    {
        if(now >= t.until) return 0;
        float in = (float)((now - t.start) / 0.25), out = (float)((t.until - now) / 0.4);
        float a = in < out ? in : out;
        return a > 1 ? 1 : a;
    }
    void BuildFonts(float H)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear(); io.Fonts->TexDesiredWidth = 2048;
        ImFontConfig cfg; cfg.FontDataOwnedByAtlas = false; cfg.OversampleH = 1; cfg.OversampleV = 1; cfg.PixelSnapH = true;
        g_fonts[F_BIG]   = io.Fonts->AddFontFromMemoryTTF((void*)kFontRussoOne, (int)kFontRussoOneSize, H * 0.085f, &cfg, kRanges);
        g_fonts[F_HEAD]  = io.Fonts->AddFontFromMemoryTTF((void*)kFontRussoOne, (int)kFontRussoOneSize, H * 0.048f, &cfg, kRanges);
        g_fonts[F_BODY]  = io.Fonts->AddFontFromMemoryTTF((void*)kFontRubikMedium, (int)kFontRubikMediumSize, H * 0.036f, &cfg, kRanges);
        g_fonts[F_SMALL] = io.Fonts->AddFontFromMemoryTTF((void*)kFontRubikMedium, (int)kFontRubikMediumSize, H * 0.028f, &cfg, kRanges);
        g_fontsBuilt = io.Fonts->Build();
    }
    ImDrawList* DL() { return ImGui::GetForegroundDrawList(); }
    float Hh() { return ImGui::GetIO().DisplaySize.y; }
    float Ww() { return ImGui::GetIO().DisplaySize.x; }
    float U(float v) { return v * Hh() / 100.0f; }
    ImVec2 TS(FontId f, const std::string& s, float wrap = 0) { return g_fonts[f]->CalcTextSizeA(g_fonts[f]->FontSize, FLT_MAX, wrap, s.c_str()); }

    // Текст с чёрной обводкой (как у игры), по центру x
    void Text(FontId f, float cx, float y, ImU32 col, float alpha, const std::string& s, float wrap)
    {
        ImFont* font = g_fonts[f];
        float size = font->FontSize;
        ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, wrap, s.c_str());
        float x = cx - sz.x * 0.5f;
        ImU32 shadow = IM_COL32(0, 0, 0, (int)(230 * alpha));
        float o = size * 0.06f; if(o < 1.5f) o = 1.5f;
        for(int dx = -1; dx <= 1; ++dx) for(int dy = -1; dy <= 1; ++dy)
            if(dx || dy) DL()->AddText(font, size, ImVec2(x + dx * o, y + dy * o), shadow, s.c_str(), nullptr, wrap);
        DL()->AddText(font, size, ImVec2(x, y), col, s.c_str(), nullptr, wrap);
    }
    void TextL(FontId f, ImVec2 p, ImU32 col, const std::string& s, float wrap = 0) { DL()->AddText(g_fonts[f], g_fonts[f]->FontSize, p, col, s.c_str(), nullptr, wrap); }

    constexpr unsigned kPanel = 0x12161C, kPanel2 = 0x1E242C, kGold = 0xF2C14E, kInk = 0xF0F2F5, kNavy = 0x2E5AAC;
    bool Button(ImVec2 a, ImVec2 b, const std::string& label, unsigned accent, FontId f = F_BODY)
    {
        bool held = HeldFrom(a, b);
        DL()->AddRectFilled(a, b, Col(held ? accent : kPanel2, 0.94f), U(1.4f));
        DL()->AddRect(a, b, Col(accent, 0.95f), U(1.4f), 0, U(0.35f));
        ImVec2 ts = TS(f, label, b.x - a.x - U(2));
        TextL(f, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), Col(held ? 0x0B0E12 : kInk, 1), label, b.x - a.x - U(2));
        bool c = Clicked(a, b);
        if(c) LOG("КНОПКА", "«%s»", label.c_str());
        return c;
    }

    // ---------- окно выбора режима ----------
    std::function<void(int)> g_newGameFn;
    bool g_chooser = false; int g_pendingChoice = -1; void* g_newGameSelf = nullptr; bool g_passNewGame = false;
    double g_blockMenuUntil = 0;
    void (*NewGameCheckOrig)(void*) = nullptr;

    void DrawChooser()
    {
        float W = Ww(), H = Hh();
        DL()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Col(0x05070A, 0.82f));
        Text(F_HEAD, W * 0.5f, H * 0.10f, Col(kGold, 1), 1, "НОВАЯ ИГРА", W);
        float cw = W * 0.34f, ch = H * 0.50f, gap = W * 0.04f, top = H * 0.22f;
        ImVec2 a1(W * 0.5f - gap * 0.5f - cw, top), b1(a1.x + cw, top + ch);
        ImVec2 a2(W * 0.5f + gap * 0.5f, top), b2(a2.x + cw, top + ch);
        struct Card { ImVec2 a, b; const char* title; const char* text; unsigned accent; int choice; };
        Card cards[2] = {
            { a1, b1, "ОБЫЧНАЯ GTA", "Родной сюжет San Andreas, мод ничего не меняет", 0x6E7B8C, 0 },
            { a2, b2, "S.W.A.T.", "Артём, база, отряд. Начало — на вертолёте над городом", kNavy, 1 },
        };
        for(auto& c : cards)
        {
            bool held = HeldFrom(c.a, c.b);
            DL()->AddRectFilled(c.a, c.b, Col(held ? c.accent : kPanel, 0.96f), U(2.2f));
            DL()->AddRect(c.a, c.b, Col(c.accent, 1), U(2.2f), 0, U(0.5f));
            Text(F_HEAD, (c.a.x + c.b.x) * 0.5f, c.a.y + ch * 0.18f, Col(held ? 0xFFFFFF : kGold, 1), 1, c.title, cw);
            Text(F_BODY, (c.a.x + c.b.x) * 0.5f, c.a.y + ch * 0.50f, Col(kInk, 1), 1, c.text, cw - U(6));
            if(Clicked(c.a, c.b)) { g_pendingChoice = c.choice; g_chooser = false; LOG("РЕЖИМ", "выбран режим: %s", c.title); }
        }
        ImVec2 ba(W * 0.5f - W * 0.12f, top + ch + H * 0.06f), bb(W * 0.5f + W * 0.12f, top + ch + H * 0.06f + H * 0.09f);
        if(Button(ba, bb, "Назад", 0x6E7B8C)) { g_chooser = false; g_pendingChoice = -1; LOG("РЕЖИМ", "выбор режима закрыт — остаёмся в меню"); }
    }

    // ---------- меню в игре ----------
    struct Menu { bool open = false; std::string title, footer; std::vector<std::string> items; std::function<void(int)> fn; };
    Menu g_menu;
    void DrawMenu()
    {
        float W = Ww(), H = Hh();
        float pw = W * 0.40f, rowH = H * 0.085f;
        float ph = U(12) + rowH * (float)(g_menu.items.size() + 1) + (g_menu.footer.empty() ? 0 : U(6));
        ImVec2 a(W * 0.06f, H * 0.5f - ph * 0.5f), b(a.x + pw, a.y + ph);
        DL()->AddRectFilled(a, b, Col(kPanel, 0.93f), U(2));
        DL()->AddRect(a, b, Col(kGold, 0.9f), U(2), 0, U(0.4f));
        TextL(F_HEAD, ImVec2(a.x + U(2.5f), a.y + U(2)), Col(kGold, 1), g_menu.title);
        float y = a.y + U(10);
        int chosen = -2;
        for(size_t i = 0; i < g_menu.items.size(); ++i)
        {
            if(Button(ImVec2(a.x + U(2), y), ImVec2(b.x - U(2), y + rowH - U(1)), g_menu.items[i], kNavy, F_SMALL)) chosen = (int)i;
            y += rowH;
        }
        if(Button(ImVec2(a.x + U(2), y), ImVec2(b.x - U(2), y + rowH - U(1)), "Закрыть", 0x6E7B8C, F_SMALL)) chosen = -1;
        y += rowH;
        if(!g_menu.footer.empty()) TextL(F_SMALL, ImVec2(a.x + U(2.5f), y), Col(0xB8C0CC, 1), g_menu.footer, pw - U(5));
        if(chosen != -2)
        {
            auto fn = g_menu.fn;
            if(chosen < 0) g_menu.open = false;
            if(fn) fn(chosen);
        }
    }

    // ---------- кнопки действий, звёзды, статус ----------
    struct Act { std::string label; std::function<void()> fn; bool on = false; };
    Act g_act[3];
    struct StarMark { game::Vec at; int stars; bool surr; };
    std::vector<StarMark> g_stars;
    std::string g_status; unsigned g_statusRgb = 0xFFFFFF; bool g_statusOn = false;

    void DrawActs()
    {
        float W = Ww(), H = Hh();
        for(int i = 0; i < 3; ++i)
        {
            Act& a = g_act[i];
            if(!a.on) continue;
            float bw = W * 0.17f, bh = H * 0.10f;
            ImVec2 p0(W * 0.62f - (bw + U(1.5f)) * i, H * 0.62f), p1(p0.x + bw, p0.y + bh);
            if(Button(p0, p1, a.label, kGold, F_SMALL) && a.fn) { auto fn = a.fn; a.on = false; fn(); }
            a.on = false;
        }
    }
    // Звезда розыска (как у игры): пятиугольник + 5 лучей
    void Star(ImVec2 c, float r, ImU32 col)
    {
        ImVec2 o[5], in[5];
        for(int i = 0; i < 5; ++i)
        {
            float a = -1.5708f + i * 1.25664f, b = a + 0.62832f;
            o[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
            in[i] = ImVec2(c.x + cosf(b) * r * 0.42f, c.y + sinf(b) * r * 0.42f);
        }
        DL()->AddConvexPolyFilled(in, 5, col);
        for(int i = 0; i < 5; ++i) DL()->AddTriangleFilled(o[i], in[i], in[(i + 4) % 5], col);
    }
    void DrawStars()
    {
        for(auto& s : g_stars)
        {
            float sx, sy;
            game::Vec head = s.at; head.z += 1.25f;
            if(!game::WorldToScreen(head, sx, sy)) continue;
            if(s.surr) { Text(F_SMALL, sx, sy - U(4), Col(0x9AD08A, 1), 1, "сдался", U(30)); continue; }
            float r = U(1.3f), step = r * 2.3f;
            float x0 = sx - step * (s.stars - 1) * 0.5f;
            for(int i = 0; i < s.stars; ++i) { Star(ImVec2(x0 + step * i, sy - U(2.5f)), r * 1.25f, IM_COL32(0, 0, 0, 200)); Star(ImVec2(x0 + step * i, sy - U(2.5f)), r, Col(kGold, 1)); }
        }
        g_stars.clear();
    }

    void DrawHud()
    {
        double now = NowSec();
        float W = Ww(), H = Hh();
        bool cut = game::CutsceneRunning();
        if(float a = Alpha(g_result, now))
        {
            Text(F_BIG, W * 0.5f, H * 0.30f, Col(g_resultPassed ? kGold : 0xFF4D4D, a), a, g_result.a, W * 0.9f);
            if(!g_result.b.empty()) Text(F_HEAD, W * 0.5f, H * 0.40f, Col(0xFFFFFF, a), a, g_result.b, W * 0.8f);
        }
        else if(float a = Alpha(g_title, now)) Text(F_BIG, W * 0.5f, H * 0.34f, Col(kGold, a), a, g_title.a, W * 0.9f);
        if(float a = Alpha(g_notice, now)) Text(F_BODY, W * 0.5f, H * 0.08f, Col(0x4FD15A, a), a, g_notice.a, W * 0.8f);
        if(!cut)
        {
            if(g_timer >= 0)
            {
                char b[32]; snprintf(b, sizeof(b), "ВРЕМЯ %d:%02d", g_timer / 60, g_timer % 60);
                Text(F_HEAD, W * 0.84f, H * 0.20f, g_timer <= 10 ? Col(0xFF4D4D, 1) : Col(0xFFFFFF, 1), 1, b, W * 0.3f);
            }
            if(g_counter >= 0)
            {
                char b[96]; snprintf(b, sizeof(b), "%s: %d", g_counterLabel.c_str(), g_counter);
                Text(F_HEAD, W * 0.84f, H * 0.20f + (g_timer >= 0 ? H * 0.06f : 0), Col(0xFFFFFF, 1), 1, b, W * 0.3f);
            }
            if(g_statusOn) Text(F_SMALL, W * 0.84f, H * 0.32f, Col(g_statusRgb, 1), 1, g_status, W * 0.3f);
            DrawStars();
        }
        g_statusOn = false;
        if(float a = Alpha(g_sub, now))
        {
            std::string line = g_sub.a.empty() ? g_sub.b : g_sub.a + ": " + g_sub.b;
            Text(F_BODY, W * 0.5f, H * 0.80f, Col(g_sub.rgb, a), a, line, W * 0.80f);
        }
        else if(float a = Alpha(g_obj, now)) Text(F_BODY, W * 0.5f, H * 0.80f, Col(0xFFFFFF, a), a, g_obj.a, W * 0.80f);
        if(!cut)
        {
            if(g_menu.open) DrawMenu();
            DrawActs();
        }
    }

    bool BeginFrame()
    {
        uint64_t nowUs = (uint64_t)(NowSec() * 1e6);
        float dt = g_lastUs ? (float)(nowUs - g_lastUs) / 1e6f : 1.0f / 60.0f;
        if(dt > 0.1f) dt = 0.1f;
        g_lastUs = nowUs;
        imgui_rw::NewFrame(dt);
        if(!g_fontsBuilt) { float h = ImGui::GetIO().DisplaySize.y; if(h < 100) return false; BuildFonts(h); if(!g_fontsBuilt) return false; }
        if(!imgui_rw::EnsureDeviceObjects()) return false;
        PollTouches();
        ImGui::NewFrame();
        return true;
    }
}

SDECL_HOOKv(Render2dStuffH)
{
    Render2dStuffH();
    if(!game::Player()) return;
    WHERE("экран: SA Story");
    if(BeginFrame()) { DrawHud(); imgui_rw::Render(); }
    WHERE("игра (не мод)");
}

SDECL_HOOKv(MenuRenderH, uintptr_t self)
{
    MenuRenderH(self);
    if(!g_chooser) return;
    WHERE("меню: выбор режима");
    if(BeginFrame()) { DrawChooser(); imgui_rw::Render(); }
    WHERE("игра (не мод)");
}

SDECL_HOOKv(MenuUpdateH, uintptr_t self)
{
    double now = NowSec();
    if(g_pendingChoice >= 0 && g_newGameSelf && NewGameCheckOrig)
    {   // выбор сделан — дальше обычный запуск новой игры игрой
        int c = g_pendingChoice; g_pendingChoice = -1;
        if(g_newGameFn) g_newGameFn(c);
        g_passNewGame = true;
        WHERE("меню: новая игра");
        NewGameCheckOrig(g_newGameSelf);
        g_passNewGame = false;
        g_blockMenuUntil = now + 0.4;
        WHERE("игра (не мод)");
        return;
    }
    if(g_chooser) { g_blockMenuUntil = now + 0.3; return; }               // окно открыто — меню игры касания не видит
    if(now < g_blockMenuUntil || (g_blockMenuUntil > 0 && AnyDown())) { if(AnyDown()) g_blockMenuUntil = now + 0.15; return; }
    g_blockMenuUntil = 0;
    MenuUpdateH(self);
}

SDECL_HOOKv(NewGameCheckH, void* self)
{
    if(g_passNewGame || !g_newGameFn || sacoop_role::CoopActive())
    {   // кооп («Новая игра» из лобби SA Coop) — пока всегда обычная GTA
        if(sacoop_role::CoopActive() && g_newGameFn && !g_passNewGame) { g_newGameFn(0); LOG("РЕЖИМ", "«Новая игра» в коопе — обычная GTA (S.W.A.T. в коопе позже)"); }
        NewGameCheckH(self);
        return;
    }
    g_newGameSelf = self;
    g_chooser = true;
    for(auto& f : g_f) f = Finger();
    LOG("РЕЖИМ", "«Новая игра»: окно выбора «Обычная GTA / S.W.A.T.»");
}

bool Init(void* h)
{
    if(!imgui_rw::Init(h)) { LOG("ОШИБКА", "Экран: нет функций RenderWare — текст не покажется"); return false; }
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
    void* mr = (void*)aml->GetSym(h, "_ZN10MobileMenu6RenderEv");
    void* mu = (void*)aml->GetSym(h, "_ZN10MobileMenu6UpdateEv");
    void* ng = (void*)aml->GetSym(h, "_ZN15StartGameScreen14OnNewGameCheckEv");
    if(!r2d) { LOG("ОШИБКА", "Экран: нет Render2dStuff"); return false; }
    aml->Hook(r2d, (void*)HookOf_Render2dStuffH, (void**)&Render2dStuffH);
    if(mr && mu && ng)
    {
        aml->Hook(mr, (void*)HookOf_MenuRenderH, (void**)&MenuRenderH);
        aml->Hook(mu, (void*)HookOf_MenuUpdateH, (void**)&MenuUpdateH);
        aml->Hook(ng, (void*)HookOf_NewGameCheckH, (void**)&NewGameCheckH);
        NewGameCheckOrig = NewGameCheckH;
    }
    else LOG("ОШИБКА", "Экран: нет MobileMenu/StartGameScreen — окна выбора режима не будет (новая игра — обычная)");
    LOG("ЭКРАН", "готов (касания: %s)", g_pointsGot ? "Points через GOT" : (g_pointsOrig ? "Points" : "НЕТ"));
    return true;
}

void Title(const std::string& t, float sec) { g_title = { t, "", NowSec() + sec, NowSec() }; LOG("ЭКРАН", "название: %s", t.c_str()); }
void Objective(const std::string& t, float sec) { g_obj = { t, "", NowSec() + sec, NowSec() }; LOG("ЭКРАН", "цель: %s", t.c_str()); }
void Subtitle(const std::string& who, const std::string& t, float sec, unsigned rgb) { g_sub = { who, t, NowSec() + sec, NowSec(), rgb }; LOG("РЕПЛИКА", "%s: %s", who.c_str(), t.c_str()); }
void ClearSubtitle() { g_sub.until = 0; }
void Timer(int s) { g_timer = s; }
void Counter(const std::string& label, int v) { g_counterLabel = label; g_counter = v; }
void Result(bool passed, const std::string& t, const std::string& sub) { g_resultPassed = passed; g_result = { t, sub, NowSec() + 5.0, NowSec() }; LOG("ЭКРАН", "%s %s", t.c_str(), sub.c_str()); }
void Notice(const std::string& t, float sec) { g_notice = { t, "", NowSec() + sec, NowSec() }; LOG("ЭКРАН", "сообщение: %s", t.c_str()); }
void Clear() { g_title.until = g_obj.until = g_sub.until = 0; g_timer = -1; g_counter = -1; g_menu.open = false; }
void SetNewGameHandler(std::function<void(int)> fn) { g_newGameFn = std::move(fn); }
void OpenMenu(const std::string& title, const std::vector<std::string>& items, std::function<void(int)> fn, const std::string& footer)
{
    g_menu.open = true; g_menu.title = title; g_menu.items = items; g_menu.fn = std::move(fn); g_menu.footer = footer;
    LOG("ЭКРАН", "меню «%s» (%d пунктов)", title.c_str(), (int)items.size());
}
void CloseMenu() { g_menu.open = false; }
bool MenuOpen() { return g_menu.open; }
void ActionButton(int slot, const std::string& label, std::function<void()> fn) { if(slot < 0 || slot > 2) return; g_act[slot].label = label; g_act[slot].fn = std::move(fn); g_act[slot].on = true; }
void Stars(const game::Vec& w, int stars, bool surr) { if(g_stars.size() < 32) g_stars.push_back({ w, stars, surr }); }
void Status(const std::string& t, unsigned rgb) { g_status = t; g_statusRgb = rgb; g_statusOn = true; }
}
