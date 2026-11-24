// Превью меню Co-op на компьютере: те же функции рисования, вывод в картинку.
// Сборка: tools/preview/build.sh
#include <imgui/imgui.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <vector>
#include <string>

#include <mod/amlmod.h>
#include <mod/logger.h>
#include "coop/coop.h"
#include "settings.h"
#include "platform/android.h"
#include "ui/imgui_rw.h"
#include "ui/overlay.h"

int g_fakePoints[14] = {};
static int g_dummy;
uintptr_t IAML::GetSym(void*, const char* n) { return !strcmp(n, "Points") ? (uintptr_t)g_fakePoints : (uintptr_t)&g_dummy; }
IAML    g_aml;    IAML*   aml = &g_aml;
Logger  g_logger; Logger* logger = &g_logger;

static int   W = 2400, H = 1080;
static net::Role g_role = net::Role::Host;
static bool g_connected = false;

// ---- заглушки модулей мода ----
namespace coop {
    void SetConfig(const net::Config& c) { g_role = c.role; }
    net::Config GetConfig() { net::Config c; c.role = g_role; strcpy(c.hostAddress, "100.87.12.40"); return c; }
    Status GetStatus() { Status s{}; s.role = g_role; s.running = true; s.connected = g_connected; s.pingMs = 84; s.remoteSpawned = true; strcpy(s.host, "100.87.12.40");
                         s.link = g_connected ? net::Link::Connected : net::Link::Idle; strcpy(s.peerNick, "HOMIE_BE"); return s; }
    bool g_pending = false, g_talk = false; int g_evStage = 0;
    Team GetTeam() { Team t; t.show = true; t.role = g_role; strcpy(t.myNick, "SM-X710"); t.myHealth = 86; t.myArmour = 40;
                     t.mate.present = g_connected; strcpy(t.mate.nick, "HOMIE_BE"); t.mate.health = 72; t.mate.armour = 65;
                     t.mate.pingMs = 84; t.mate.distM = 12; t.mate.talking = g_talk; return t; }
    bool PopEvent(net::Event& e) { if(g_evStage == 1) { g_evStage = 2; e = { net::EventType::Connected, "HOMIE_BE", 84 }; return true; } return false; }
    bool PendingRequest(char* n, size_t l, float* left) { if(!g_pending) return false; snprintf(n, l, "HOMIE_BE"); *left = 11.3f; return true; }
    void AnswerJoin(bool) {}
    void SetTalking(bool) {}
}
namespace settings {
    Data Load() { Data d; d.net.role = g_role; strcpy(d.net.hostAddress, "100.87.12.40"); strcpy(d.net.nick, "SM-X710"); return d; }
    void Save(const Data&) {}
    const char* RoleToString(net::Role) { return "x"; }
}
namespace platform {
    bool FindTailscaleIPv4(char* o, size_t l) { snprintf(o, l, "100.101.24.7"); return true; }
    bool ClipboardGetText(char* o, size_t l) { snprintf(o, l, "вот адрес 100.87.12.40"); return true; }
    bool ClipboardSetText(const char*) { return true; }
    bool ExtractIPv4(const char*, char* o, size_t l) { snprintf(o, l, "100.87.12.40"); return true; }
    bool DeviceModel(char* o, size_t l) { snprintf(o, l, "SM-X710"); return true; }
}

// ---- программный растеризатор вместо RenderWare ----
static std::vector<float> g_img;           // RGB
static unsigned char* g_atlas = nullptr; static int g_aw = 0, g_ah = 0;

namespace imgui_rw {
    bool Init(void*) { return true; }
    bool Ready() { return g_atlas != nullptr; }
    bool EnsureDeviceObjects()
    {
        if(g_atlas) return true;
        unsigned char* px; ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &g_aw, &g_ah);
        g_atlas = new unsigned char[g_aw * g_ah * 4]; memcpy(g_atlas, px, g_aw * g_ah * 4);
        ImGui::GetIO().Fonts->SetTexID((ImTextureID)1);
        printf("atlas %dx%d\n", g_aw, g_ah);
        return true;
    }
    void NewFrame(float dt) { ImGui::GetIO().DisplaySize = ImVec2((float)W, (float)H); ImGui::GetIO().DeltaTime = dt; }
    void Render()
    {
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();
        // фон «игры»
        for(int y = 0; y < H; ++y) for(int x = 0; x < W; ++x)
        {
            float t = (float)y / H, s = (float)x / W;
            float* p = &g_img[(y * W + x) * 3];
            p[0] = 0.40f - 0.25f * t + 0.1f * s; p[1] = 0.50f - 0.30f * t; p[2] = 0.62f - 0.40f * t;
        }
        for(int n = 0; n < dd->CmdListsCount; ++n)
        {
            const ImDrawList* cl = dd->CmdLists[n];
            for(const ImDrawCmd& cmd : cl->CmdBuffer)
                for(unsigned i = 0; i < cmd.ElemCount; i += 3)
                {
                    const ImDrawVert* v[3];
                    for(int k = 0; k < 3; ++k) v[k] = &cl->VtxBuffer[cmd.VtxOffset + cl->IdxBuffer[cmd.IdxOffset + i + k]];
                    float minx = fminf(v[0]->pos.x, fminf(v[1]->pos.x, v[2]->pos.x)), maxx = fmaxf(v[0]->pos.x, fmaxf(v[1]->pos.x, v[2]->pos.x));
                    float miny = fminf(v[0]->pos.y, fminf(v[1]->pos.y, v[2]->pos.y)), maxy = fmaxf(v[0]->pos.y, fmaxf(v[1]->pos.y, v[2]->pos.y));
                    int x0 = (int)fmaxf(0, floorf(minx)), x1 = (int)fminf(W - 1, ceilf(maxx));
                    int y0 = (int)fmaxf(0, floorf(miny)), y1 = (int)fminf(H - 1, ceilf(maxy));
                    float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) - (v[2]->pos.x - v[0]->pos.x) * (v[1]->pos.y - v[0]->pos.y);
                    if(fabsf(area) < 1e-6f) continue;
                    for(int y = y0; y <= y1; ++y) for(int x = x0; x <= x1; ++x)
                    {
                        float px = x + 0.5f, py = y + 0.5f;
                        float w0 = ((v[1]->pos.x - px) * (v[2]->pos.y - py) - (v[2]->pos.x - px) * (v[1]->pos.y - py)) / area;
                        float w1 = ((v[2]->pos.x - px) * (v[0]->pos.y - py) - (v[0]->pos.x - px) * (v[2]->pos.y - py)) / area;
                        float w2 = 1 - w0 - w1;
                        if(w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float c[4];
                        for(int k = 0; k < 4; ++k)
                            c[k] = (w0 * ((v[0]->col >> (8 * k)) & 255) + w1 * ((v[1]->col >> (8 * k)) & 255) + w2 * ((v[2]->col >> (8 * k)) & 255)) / 255.0f;
                        float u = w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x, vv = w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y;
                        int tx = (int)(u * g_aw), ty = (int)(vv * g_ah);
                        if(tx < 0) tx = 0; if(ty < 0) ty = 0; if(tx >= g_aw) tx = g_aw - 1; if(ty >= g_ah) ty = g_ah - 1;
                        const unsigned char* t = &g_atlas[(ty * g_aw + tx) * 4];
                        float a = c[3] * t[3] / 255.0f;
                        float* p = &g_img[(y * W + x) * 3];
                        for(int k = 0; k < 3; ++k) p[k] = p[k] * (1 - a) + c[k] * (t[k] / 255.0f) * a;
                    }
                }
        }
    }
}

namespace overlay { extern void (*MobileMenuRender)(uintptr_t); void HookOf_MobileMenuRender(uintptr_t);
                    extern void (*Render2dStuff)(); void HookOf_Render2dStuff(); }
static void NoR2d() {}
static void HudFrames(double seconds) { int n = (int)(seconds * 30); for(int i = 0; i < n; ++i) { overlay::HookOf_Render2dStuff(); usleep(33000); } }
namespace coop { void KeepWorldRunningInMenu() {} }
static void NoRender(uintptr_t) {}

static void Frames(double seconds)
{
    int n = (int)(seconds * 30);
    for(int i = 0; i < n; ++i) { overlay::HookOf_MobileMenuRender(0); usleep(33000); }
}
static void Tap(float x, float y)
{
    g_fakePoints[0] = (int)x; g_fakePoints[1] = (int)y; g_fakePoints[2] = 2; Frames(0.1);
    g_fakePoints[2] = 0; Frames(0.1);
}
static void Save(const char* name)
{
    FILE* f = fopen(name, "wb");
    fprintf(f, "P6 %d %d 255\n", W, H);
    for(float v : g_img) { int b = (int)(fminf(fmaxf(v, 0), 1) * 255); fputc(b, f); }
    fclose(f);
    printf("saved %s\n", name);
}

int main(int argc, char** argv)
{
    if(argc > 2) { W = atoi(argv[1]); H = atoi(argv[2]); }
    g_img.resize((size_t)W * H * 3);
    overlay::MobileMenuRender = NoRender;
    overlay::Init(nullptr, nullptr);
    overlay::Open();
    Frames(1.2); Save("host.ppm");

    // карточки режимов: x0=3%W, ширина 34%W; тело от y0+12u+2.4u до H-4u-11u-2.4u
    float u = H / 100.0f, bodyY0 = 4 * u + 12 * u + 2.4f * u, bodyY1 = H - 4 * u - 11 * u - 2.4f * u;
    float ch = (bodyY1 - bodyY0 - 1.6f * u * 3) / 4, cx = W * 0.03f + W * 0.17f;
    auto card = [&](int i) { return bodyY0 + i * (ch + 1.6f * u) + ch * 0.5f; };
    Tap(cx, card(3)); Frames(0.3); Save("join.ppm");
    Tap(cx, card(0)); Frames(0.9); Save("off.ppm");
    Tap(cx, card(1)); Frames(0.9); Save("solo.ppm");
    Tap(cx, card(2)); Frames(0.9); Save("host.ppm");

    // слой поверх игры: закрываем меню крестиком, затем запрос, подключение, разговор
    overlay::Render2dStuff = NoR2d;
    Tap(W - W * 0.03f - 4.5f * u, 4 * u + 6 * u); Frames(0.5);
    coop::g_pending = true; HudFrames(0.8); Save("hud_request.ppm");
    coop::g_pending = false; g_connected = true; coop::g_evStage = 1; HudFrames(0.9);
    coop::g_talk = true;
    g_fakePoints[0] = (int)(W - W * 0.15f); g_fakePoints[1] = (int)(31 * u); g_fakePoints[2] = 2; HudFrames(0.6); Save("hud_team.ppm");
    g_fakePoints[2] = 0;
    return 0;
}
