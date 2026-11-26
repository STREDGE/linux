// Превью интерфейса SA Living на компьютере: то же рисование (src/ui/hud.cpp), вывод в картинки .ppm.
// Сборка и запуск: tools/preview/build.sh (картинки — в текущей папке).
#include <imgui/imgui.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <string>

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <sautils/isautils.h>
#include "ai/claude.h"
#include "game/game.h"
#include "jarvis/jarvis.h"
#include "net/link.h"
#include "platform/android.h"
#include "platform/bridge.h"
#include "settings.h"
#include "sim/police.h"
#include "speech/speech.h"
#include "talk/talk.h"
#include "ui/hud.h"
#include "ui/imgui_rw.h"
#include "util/log.h"
#include "world/world.h"

int g_fakePoints[14] = {};
static int g_dummy;
uintptr_t IAML::GetSym(void*, const char* n) { return !strcmp(n, "Points") ? (uintptr_t)g_fakePoints : (uintptr_t)&g_dummy; }
IAML g_aml; IAML* aml = &g_aml;
Logger g_logger; Logger* logger = &g_logger;
void LogInfo(const char*, ...) {}
void LogError(const char*, ...) {}
void SALivingEnsureSpeech() {}
static int W = 2400, H = 1080;

// ---- заглушки модулей ----
static int g_scenario = 0;      // 0 всё готово, 1 ничего не установлено, 2 идёт скачивание
namespace claude {
    static Settings g_s; void Configure(const Settings& s) { g_s = s; } const Settings& Get() { return g_s; }
    bool HasKey() { return g_scenario != 1; } std::string MaskedKey() { return "sk-ant-…a1b2"; }
    int Send(const Request&, TextFn, DoneFn) { return 0; } void Cancel(int) {} void Pump() {} int InFlight() { return 0; }
    void HandleProxyRequest(const json::Value&) {} void HandleProxyResult(const json::Value&) {}
    bool ViaHost() { return false; } bool Available() { return g_scenario != 1; }
    Usage Today() { Usage u; u.requests = 37; return u; } Usage Total() { Usage u; u.requests = 412; return u; }
    double CostUsd(const Usage& u) { return u.requests * 0.0021; } void SetDailyLimit(int) {} bool LimitReached() { return false; }
    void LoadUsage(const json::Value&) {} json::Value SaveUsage() { return json::Value(); }
}
namespace game {
    float Dist(const Vec&, const Vec&) { return 3; } float Dist2D(const Vec&, const Vec&) { return 3; }
    uintptr_t Player() { return 1; } int Money() { return 12500; } void AddMoney(int) {} int Wanted() { return 0; } void SetWanted(int) {}
    bool PlayerInVehicle() { return false; } uintptr_t PlayerVehicle() { return 0; }
    Vec Pos(uintptr_t) { return Vec{ 2495, -1668, 13 }; } bool CutsceneRunning() { return false; }
    bool WorldToScreen(const Vec&, float& sx, float& sy) { sx = W * 0.42f; sy = H * 0.36f; return true; }
}
namespace coopnet { Role GetRole() { return NONE; } bool Connected() { return false; } bool Send(const json::Value&) { return false; } const char* PeerNick() { return ""; } void Init() {} void Frame() {} }
namespace platform {
    bool ClipboardGetText(char*, size_t) { return false; } bool ClipboardSetText(const char*) { return true; }
    int HasPermission(const char*) { return g_scenario == 1 ? 0 : 1; } bool RequestPermission(const char*) { return true; } bool OpenAppSettings() { return true; }
}
namespace bridge { bool Ready() { return true; } void TtsInit(const char*) {} bool TtsReady() { return false; } }
namespace settings { static Data g_d; Data& Get() { return g_d; } void Load() {} void Save() {} }
namespace police {
    static Scene g_sc; const Scene& CurrentScene() { return g_sc; } void SceneContinue() {} void SceneSpeak(const std::string&) {} void SceneCallHelp() {}
    bool Arresting() { return false; } void Surrender() {} const char* ArrestStage() { return ""; }
}
namespace speech {
    Status Get()
    {
        Status s;
        if(g_scenario == 0) { s.asrFiles = s.asrReady = s.ttsFiles = s.ttsReady = true; s.voices = 5; s.lastRtf = 0.62f; s.lastAsrSec = 0.8f; }
        if(g_scenario == 2) { s.busy = true; s.progress = 0.56f; s.busyText = "model.int8.onnx: 120 из 214 МБ"; s.ttsFiles = s.ttsReady = true; s.voices = 4; s.lastRtf = 0.62f; }
        return s;
    }
    void Frame() {} void DownloadAsr() {} bool ImportVoicePackAuto() { return true; } bool PickVoicePack() { return true; } void CancelBusy() {}
    bool TtsReady() { return g_scenario != 1; } void Speak(const std::string&, const std::string&, float, float, const std::string&) {} void StopSpeaking() {} bool Speaking() { return false; }
    std::vector<Voice> Voices()
    {
        std::vector<Voice> v; if(g_scenario == 1) return v;
        const char* t[] = { "Мужской 1 (живой)", "Мужской 2 (спокойный)", "Мужской 3 (низкий)", "Женский 1", "Мой голос 5" };
        const char* k[] = { "m01", "m02", "m_deep", "f01", "user_1" };
        for(int i = 0; i < (g_scenario == 0 ? 5 : 4); ++i) { Voice x; x.key = k[i]; x.title = t[i]; x.female = i == 3; x.builtin = i < 4; v.push_back(x); }
        return v;
    }
    bool FindVoice(const std::string& key, Voice& out) { for(auto& v : Voices()) if(v.key == key) { out = v; return true; } return false; }
    bool RecordVoiceStart() { return true; } void RecordVoiceFinish(const std::string&, bool) {} bool RecordingVoice() { return false; } float RecordSeconds() { return 0; }
    bool AddVoiceFromFile(const std::string&, bool) { return true; } void DeleteVoice(const std::string&) {} bool PopNotice(std::string&) { return false; }
}
namespace jarvis {
    static View g_v; View GetView() { return g_v; } void ButtonDown() {} void ButtonUp() {} void TapPlate() {} void Preview() {}
    bool LibraryFilePresent() { return g_scenario == 0; } void DownloadLibrary() {}
}
namespace world {
    static World g_w; World& W() { return g_w; } void Reset() {} int g_forgetDays = 7;
    const std::string& MemoryKey() { static std::string k = "GTASAsf3"; return k; }
    Character* Find(int id) { for(auto& c : g_w.chars) if(c.id == id) return &c; return nullptr; }
    const char* FactionName(int f) { static const char* n[] = { "—", "Полиция", "Grove Street", "Ballas", "Vagos", "Aztecas", "Rifa", "Da Nang", "Триады", "Мафия", "Горожане" }; return n[f]; }
    std::string FactionSummary(int) { return "Ballas — вражда 55, Vagos — вражда 45"; }
    void SetStoryVoice(const std::string&, const StoryVoice&) {} void ClearStoryVoice(const std::string&) {}
    std::string Fmt(const char* fmt, ...) { char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a); return b; }
}
namespace trace { void Line(const char*, const char*, ...) {} void Mark(const std::string&) {} }
namespace talk {
    static std::deque<Sub> g_subs; static bool g_conv = false;
    void Frame() {} bool PopNotify(std::string&) { return false; } void Notify(const std::string&) {}
    double Now() { return 0; }
    bool Capturing() { return false; } bool Recognizing() { return false; } void CaptureDown(std::function<void(const std::string&)>) {} void CaptureUp() {}
    void EnsureVoice(world::Character& c, const std::vector<std::string>&) { if(c.voice.empty()) c.voice = "m01"; }
    bool Listening() { return false; } bool Thinking() { return false; } bool PhoneMode() { return false; } const std::string& Partial() { static std::string e; return e; }
    world::Character* Current() { return g_conv ? world::Find(1) : nullptr; } uintptr_t CurrentPed() { return g_conv ? 5 : 0; }
    bool InConversation() { return g_conv; } int PendingDemand() { return 0; } void PayDemand() {} void EndConversation() {} void OfferMoney(int) {}
    void PttDown() {} void PttUp() {} void StartPhoneCall(int) {} const std::deque<Sub>& Subs() { return g_subs; }
    void SetSpeechSink(std::function<void(const std::string&)>) {}
}

// ---- программный растеризатор вместо RenderWare (как в превью SA Coop) ----
static std::vector<float> g_img; static unsigned char* g_atlas = nullptr; static int g_aw = 0, g_ah = 0;
namespace imgui_rw {
    bool Init(void*) { return true; } bool Ready() { return g_atlas != nullptr; }
    bool EnsureDeviceObjects()
    {
        if(g_atlas) return true;
        unsigned char* px; ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &g_aw, &g_ah);
        g_atlas = new unsigned char[g_aw * g_ah * 4]; memcpy(g_atlas, px, g_aw * g_ah * 4);
        ImGui::GetIO().Fonts->SetTexID((ImTextureID)1);
        return true;
    }
    void NewFrame(float dt) { ImGui::GetIO().DisplaySize = ImVec2((float)W, (float)H); ImGui::GetIO().DeltaTime = dt; }
    void Render()
    {
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();
        for(int y = 0; y < H; ++y) for(int x = 0; x < W; ++x) { float t = (float)y / H, s = (float)x / W; float* p = &g_img[(y * W + x) * 3]; p[0] = 0.40f - 0.25f * t + 0.1f * s; p[1] = 0.50f - 0.30f * t; p[2] = 0.62f - 0.40f * t; }
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
                    int x0 = (int)fmaxf(fmaxf(0, cmd.ClipRect.x), floorf(minx)), x1 = (int)fminf(fminf(W - 1, cmd.ClipRect.z), ceilf(maxx));
                    int y0 = (int)fmaxf(fmaxf(0, cmd.ClipRect.y), floorf(miny)), y1 = (int)fminf(fminf(H - 1, cmd.ClipRect.w), ceilf(maxy));
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
                        for(int k = 0; k < 4; ++k) c[k] = (w0 * ((v[0]->col >> (8 * k)) & 255) + w1 * ((v[1]->col >> (8 * k)) & 255) + w2 * ((v[2]->col >> (8 * k)) & 255)) / 255.0f;
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

namespace hud { void PreviewFrame(bool menu, int tab); void PreviewCard(int id); void PreviewPhone(bool open, int tab); }
static void Frames(bool menu, int tab, int n = 3) { for(int i = 0; i < n; ++i) { hud::PreviewFrame(menu, tab); usleep(20000); } }
static void Save(const char* name)
{
    FILE* f = fopen(name, "wb"); fprintf(f, "P6 %d %d 255\n", W, H);
    for(float v : g_img) { int b = (int)(fminf(fmaxf(v, 0), 1) * 255); fputc(b, f); }
    fclose(f); printf("saved %s\n", name);
}

int main(int argc, char** argv)
{
    if(argc > 2) { W = atoi(argv[1]); H = atoi(argv[2]); }
    g_img.resize((size_t)W * H * 3);
    hud::Init(nullptr);
    // мир для карточки и телефона
    world::Character c; c.id = 1; c.name = "Тайрон Уокер"; c.nick = "Шрам"; c.faction = world::F_BALLAS; c.age = 27; c.voice = "m01"; c.vpitch = 0.97f; c.vrate = 1.03f;
    c.rel[0].met = true; c.rel[0].trust = -12; c.rel[0].respect = 8; c.rel[0].liking = -20; c.rel[0].fear = 15; c.summary = "Боец Ballas с Айдлвуда. Считает, что CJ ему должен за брата.";
    c.mem.push_back({ 3, "CJ предлагал пятьсот баксов за наводку на склад" });
    world::W().chars.push_back(c);
    world::Character s; s.id = 2; s.name = "Свит Джонсон"; s.story = "sweet"; s.faction = world::F_GROVE; s.age = 30; s.voice = "m_deep"; s.rel[0].met = true; s.rel[0].trust = 60; s.rel[0].respect = 40; s.rel[0].liking = 55; s.rel[0].contact = true;
    world::W().chars.push_back(s);

    Frames(true, 0); Save("menu_main.ppm");
    g_scenario = 1; Frames(true, 0); Save("menu_main_empty.ppm");
    g_scenario = 2; Frames(true, 0); Save("menu_main_download.ppm");
    g_scenario = 0;
    Frames(true, 1); Save("menu_voices.ppm");
    Frames(true, 2); Save("menu_jarvis.ppm");
    Frames(true, 3); Save("menu_settings.ppm");

    // в игре: Джарвис отвечает
    jarvis::g_v.visible = true; jarvis::g_v.state = jarvis::SPEAKING; jarvis::g_v.heard = "Дай миниган и тысячу патронов, и сними розыск";
    jarvis::g_v.reply = "Готово, сэр. Миниган, тысяча патронов. Розыск снят."; jarvis::g_v.done = { "Выдано: Миниган, 1000 патр.", "Розыск снят" };
    talk::g_conv = true;
    talk::Sub a; a.who = "CJ"; a.text = "Слышь, кто тут ночью стрелял?"; a.player = true; a.until = 1e12; talk::g_subs.push_back(a);
    talk::Sub b; b.who = "Шрам"; b.text = "А тебе какое дело, зелёный? Вали, пока цел."; b.player = false; b.charId = 1; b.until = 1e12; talk::g_subs.push_back(b);
    Frames(false, 0); Save("game_jarvis.ppm");
    jarvis::g_v.state = jarvis::LISTENING; jarvis::g_v.heard.clear(); jarvis::g_v.reply.clear(); jarvis::g_v.done.clear(); jarvis::g_v.listenLeft = 4;
    Frames(false, 0); Save("game_jarvis_listen.ppm");
    jarvis::g_v.visible = false;
    hud::PreviewCard(1); Frames(false, 0); Save("card.ppm");
    hud::PreviewCard(2); Frames(false, 0); Save("card_story.ppm");
    return 0;
}
