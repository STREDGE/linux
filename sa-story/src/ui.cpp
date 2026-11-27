#include "ui.h"

#include <mod/amlmod.h>
#include <imgui/imgui.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "fonts_data.h"
#include "game.h"
#include "imgui_rw.h"
#include "log.h"

namespace ui
{
namespace
{
    enum FontId { F_BIG, F_HEAD, F_BODY, F_COUNT };
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
        g_fonts[F_BIG]  = io.Fonts->AddFontFromMemoryTTF((void*)kFontRussoOne, (int)kFontRussoOneSize, H * 0.085f, &cfg, kRanges);
        g_fonts[F_HEAD] = io.Fonts->AddFontFromMemoryTTF((void*)kFontRussoOne, (int)kFontRussoOneSize, H * 0.048f, &cfg, kRanges);
        g_fonts[F_BODY] = io.Fonts->AddFontFromMemoryTTF((void*)kFontRubikMedium, (int)kFontRubikMediumSize, H * 0.036f, &cfg, kRanges);
        g_fontsBuilt = io.Fonts->Build();
    }

    // Текст с чёрной обводкой (как у игры), по центру x; wrap — ширина переноса
    void Text(FontId f, float cx, float y, ImU32 col, float alpha, const std::string& s, float wrap)
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImFont* font = g_fonts[f];
        float size = font->FontSize;
        ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, wrap, s.c_str());
        float x = cx - sz.x * 0.5f;
        ImU32 shadow = IM_COL32(0, 0, 0, (int)(230 * alpha));
        float o = size * 0.06f; if(o < 1.5f) o = 1.5f;
        for(int dx = -1; dx <= 1; ++dx) for(int dy = -1; dy <= 1; ++dy)
            if(dx || dy) dl->AddText(font, size, ImVec2(x + dx * o, y + dy * o), shadow, s.c_str(), nullptr, wrap);
        dl->AddText(font, size, ImVec2(x, y), col, s.c_str(), nullptr, wrap);
    }

    void Draw()
    {
        double now = NowSec();
        ImGuiIO& io = ImGui::GetIO();
        float W = io.DisplaySize.x, H = io.DisplaySize.y;
        bool cut = game::CutsceneRunning();

        if(float a = Alpha(g_result, now))
        {
            Text(F_BIG, W * 0.5f, H * 0.30f, Col(g_resultPassed ? 0xF2C14E : 0xFF4D4D, a), a, g_result.a, W * 0.9f);
            if(!g_result.b.empty()) Text(F_HEAD, W * 0.5f, H * 0.30f + H * 0.10f, Col(0xFFFFFF, a), a, g_result.b, W * 0.8f);
        }
        else if(float a = Alpha(g_title, now))
            Text(F_BIG, W * 0.5f, H * 0.34f, Col(0xF2C14E, a), a, g_title.a, W * 0.9f);

        if(float a = Alpha(g_notice, now)) Text(F_BODY, W * 0.5f, H * 0.08f, Col(0x4FD15A, a), a, g_notice.a, W * 0.8f);

        if(!cut)
        {
            if(g_timer >= 0)
            {
                char b[32]; snprintf(b, sizeof(b), "ВРЕМЯ %d:%02d", g_timer / 60, g_timer % 60);
                ImU32 c = g_timer <= 10 ? Col(0xFF4D4D, 1) : Col(0xFFFFFF, 1);
                Text(F_HEAD, W * 0.84f, H * 0.20f, c, 1, b, W * 0.3f);
            }
            if(g_counter >= 0)
            {
                char b[96]; snprintf(b, sizeof(b), "%s: %d", g_counterLabel.c_str(), g_counter);
                Text(F_HEAD, W * 0.84f, H * 0.20f + (g_timer >= 0 ? H * 0.06f : 0), Col(0xFFFFFF, 1), 1, b, W * 0.3f);
            }
        }

        // реплика героя важнее цели: цель — когда реплик нет
        if(float a = Alpha(g_sub, now))
        {
            std::string line = g_sub.a.empty() ? g_sub.b : g_sub.a + ": " + g_sub.b;
            Text(F_BODY, W * 0.5f, H * 0.80f, Col(g_sub.rgb, a), a, line, W * 0.80f);
        }
        else if(float a = Alpha(g_obj, now))
            Text(F_BODY, W * 0.5f, H * 0.80f, Col(0xFFFFFF, a), a, g_obj.a, W * 0.80f);
    }
}

SDECL_HOOKv(Render2dStuffH)
{
    Render2dStuffH();
    WHERE("экран: текст миссии");
    uint64_t nowUs = (uint64_t)(NowSec() * 1e6);
    float dt = g_lastUs ? (float)(nowUs - g_lastUs) / 1e6f : 1.0f / 60.0f;
    if(dt > 0.1f) dt = 0.1f;
    g_lastUs = nowUs;
    if(game::Player())
    {
        imgui_rw::NewFrame(dt);
        bool ok = true;
        if(!g_fontsBuilt) { float h = ImGui::GetIO().DisplaySize.y; if(h < 100) ok = false; else BuildFonts(h); ok = ok && g_fontsBuilt; }
        if(ok && imgui_rw::EnsureDeviceObjects())
        {
            ImGui::NewFrame();
            Draw();
            imgui_rw::Render();
        }
    }
    WHERE("игра (не мод)");
}

bool Init(void* h)
{
    if(!imgui_rw::Init(h)) { LOG("ОШИБКА", "Экран: нет функций RenderWare — текст миссий не покажется"); return false; }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr; io.DisplaySize = ImVec2(1920, 1080);
    void* r2d = (void*)aml->GetSym(h, "_Z13Render2dStuffv");
    if(!r2d) { LOG("ОШИБКА", "Экран: нет Render2dStuff"); return false; }
    aml->Hook(r2d, (void*)HookOf_Render2dStuffH, (void**)&Render2dStuffH);
    return true;
}

void Title(const std::string& t, float sec) { g_title = { t, "", NowSec() + sec, NowSec() }; LOG("ЭКРАН", "название: %s", t.c_str()); }
void Objective(const std::string& t, float sec) { g_obj = { t, "", NowSec() + sec, NowSec() }; LOG("ЭКРАН", "цель: %s", t.c_str()); }
void Subtitle(const std::string& who, const std::string& t, float sec, unsigned rgb)
{
    g_sub = { who, t, NowSec() + sec, NowSec(), rgb };
    LOG("РЕПЛИКА", "%s: %s", who.c_str(), t.c_str());
}
void ClearSubtitle() { g_sub.until = 0; }
void Timer(int s) { g_timer = s; }
void Counter(const std::string& label, int v) { g_counterLabel = label; g_counter = v; }
void Result(bool passed, const std::string& t, const std::string& sub)
{
    g_resultPassed = passed; g_result = { t, sub, NowSec() + 5.0, NowSec() };
    LOG("ЭКРАН", "%s %s", t.c_str(), sub.c_str());
}
void Notice(const std::string& t, float sec) { g_notice = { t, "", NowSec() + sec, NowSec() }; LOG("ЭКРАН", "сообщение: %s", t.c_str()); }
void Clear() { g_title.until = g_obj.until = g_sub.until = 0; g_timer = -1; g_counter = -1; }
}
