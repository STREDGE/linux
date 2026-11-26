#include "speech.h"

#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "platform/bridge.h"
#include "speech/sherpa-onnx-c-api.h"
#include "speech/xvibe.h"
#include "util/json.h"
#include "util/log.h"

namespace speech
{
namespace
{
    const char* kAsrModelUrl  = "https://huggingface.co/csukuangfj/sherpa-onnx-nemo-ctc-punct-giga-am-v3-russian-2025-12-16/resolve/main/model.int8.onnx";
    const char* kAsrTokensUrl = "https://huggingface.co/csukuangfj/sherpa-onnx-nemo-ctc-punct-giga-am-v3-russian-2025-12-16/resolve/main/tokens.txt";
    const char* kAarUrl       = "https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.12.40/sherpa-onnx-1.12.40.aar";
    const char* kTtsFiles[] = { "bundle.json", "vocab.json", "token_scores.json", "mimi_encoder.onnx", "text_conditioner.onnx",
                                "flow_lm_main_int8.onnx", "flow_lm_flow_int8.onnx", "mimi_decoder_int8.onnx" };

    std::string g_base, g_lib;
    std::vector<std::string> g_searchDirs;

    std::mutex g_mx;                       // состояние, голоса, очереди результатов
    Status g_st;
    std::vector<Voice> g_voices;
    std::deque<Heard> g_heard;
    std::deque<std::string> g_spoken;
    std::deque<std::string> g_notices;

    double NowSec() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
    bool Exists(const std::string& p, long minSize = 1) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= minSize; }
    void MkDirs(const std::string& p) { for(size_t i = 1; i <= p.size(); ++i) if(i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0777); }
    void Notice(const std::string& s) { std::lock_guard<std::mutex> l(g_mx); g_notices.push_back(s); if(g_notices.size() > 8) g_notices.pop_front(); }
    void SetError(const std::string& s) { { std::lock_guard<std::mutex> l(g_mx); g_st.error = s; } LOG_ERROR("Речь: %s", s.c_str()); }

    bool CopyFile(const std::string& from, const std::string& to)
    {
        FILE* a = fopen(from.c_str(), "rb"); if(!a) return false;
        std::string tmp = to + ".tmp";
        FILE* b = fopen(tmp.c_str(), "wb"); if(!b) { fclose(a); return false; }
        char buf[1 << 16]; size_t n; bool ok = true;
        while((n = fread(buf, 1, sizeof(buf), a)) > 0) if(fwrite(buf, 1, n, b) != n) { ok = false; break; }
        fclose(a); fclose(b);
        if(ok) { remove(to.c_str()); ok = rename(tmp.c_str(), to.c_str()) == 0; chmod(to.c_str(), 0700); }
        else remove(tmp.c_str());
        return ok;
    }

    bool ReadText(const std::string& p, std::string& out)
    {
        FILE* f = fopen(p.c_str(), "rb"); if(!f) return false;
        char buf[1 << 15]; size_t n; out.clear();
        while((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f); return true;
    }
    bool WriteText(const std::string& p, const std::string& s)
    {
        std::string tmp = p + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb"); if(!f) return false;
        bool ok = fwrite(s.data(), 1, s.size(), f) == s.size(); fclose(f);
        if(ok) { remove(p.c_str()); ok = rename(tmp.c_str(), p.c_str()) == 0; }
        return ok;
    }

    std::string OrtPath()    { return g_lib + "/libonnxruntime.so"; }
    std::string SherpaPath() { return g_lib + "/libsherpa-onnx-c-api.so"; }

    bool AsrFilesOk() { return Exists(g_base + "/asr/model.int8.onnx", 50 << 20) && Exists(g_base + "/asr/tokens.txt") && Exists(OrtPath(), 1 << 20) && Exists(SherpaPath(), 1 << 20); }
    bool TtsFilesOk()
    {
        for(const char* f : kTtsFiles) if(!Exists(g_base + "/tts/" + f)) return false;
        return Exists(g_base + "/tts/voices/voices.json") && Exists(OrtPath(), 1 << 20);
    }

    // ---------- голоса ----------
    void LoadVoiceList(const std::string& dir, bool builtin, std::vector<Voice>& out)
    {
        std::string s; json::Value root;
        if(!ReadText(dir + "/voices.json", s) || !json::Parse(s, root)) return;
        for(auto& v : root["voices"].arr())
        {
            Voice x; x.key = v["key"].str(); x.title = v["title"].s(x.key.c_str()); x.female = v["female"].b();
            x.file = dir + "/" + v["file"].s((x.key + ".xvst").c_str()); x.builtin = builtin;
            x.pitch = (float)v["pitch"].num(1); x.speed = (float)v["speed"].num(1);
            if(!x.key.empty() && Exists(x.file)) out.push_back(x);
        }
    }
    void ReloadVoices()
    {
        std::vector<Voice> v;
        LoadVoiceList(g_base + "/tts/voices", true, v);
        LoadVoiceList(g_base + "/uservoices", false, v);
        std::lock_guard<std::mutex> l(g_mx);
        g_voices.swap(v); g_st.voices = (int)g_voices.size();
    }
    void SaveUserVoices()
    {
        json::Value arr = json::Value::Array();
        {
            std::lock_guard<std::mutex> l(g_mx);
            for(auto& v : g_voices)
            {
                if(v.builtin) continue;
                json::Value o = json::Value::Object();
                o["key"] = v.key; o["title"] = v.title; o["female"] = v.female; o["pitch"] = v.pitch; o["speed"] = v.speed;
                size_t sl = v.file.rfind('/'); o["file"] = sl == std::string::npos ? v.file : v.file.substr(sl + 1);
                arr.push(o);
            }
        }
        json::Value root = json::Value::Object(); root["voices"] = arr;
        MkDirs(g_base + "/uservoices");
        WriteText(g_base + "/uservoices/voices.json", root.dump());
    }

    // ---------- распознавание: sherpa-onnx через dlopen ----------
    struct Sherpa
    {
        void* lib = nullptr;
        const SherpaOnnxOfflineRecognizer* (*Create)(const SherpaOnnxOfflineRecognizerConfig*) = nullptr;
        const SherpaOnnxOfflineStream* (*CreateStream)(const SherpaOnnxOfflineRecognizer*) = nullptr;
        void (*Accept)(const SherpaOnnxOfflineStream*, int32_t, const float*, int32_t) = nullptr;
        void (*Decode)(const SherpaOnnxOfflineRecognizer*, const SherpaOnnxOfflineStream*) = nullptr;
        const SherpaOnnxOfflineRecognizerResult* (*Result)(const SherpaOnnxOfflineStream*) = nullptr;
        void (*FreeResult)(const SherpaOnnxOfflineRecognizerResult*) = nullptr;
        void (*FreeStream)(const SherpaOnnxOfflineStream*) = nullptr;
        const SherpaOnnxOfflineRecognizer* rec = nullptr;
    } g_sh;
    void* g_ortHandle = nullptr;

    std::mutex g_ortMx;
    bool LoadOrtLib(std::string& err)
    {
        std::lock_guard<std::mutex> l(g_ortMx);
        if(g_ortHandle) return true;
        g_ortHandle = dlopen(OrtPath().c_str(), RTLD_NOW | RTLD_GLOBAL);
        if(!g_ortHandle) { const char* e = dlerror(); err = std::string("не загрузился движок (onnxruntime): ") + (e ? e : "?"); return false; }
        return true;
    }

    bool AsrLoad(std::string& err)
    {
        if(g_sh.rec) return true;
        if(!LoadOrtLib(err)) return false;
        if(!g_sh.lib) g_sh.lib = dlopen(SherpaPath().c_str(), RTLD_NOW | RTLD_GLOBAL);
        if(!g_sh.lib) { const char* e = dlerror(); err = std::string("не загрузилось распознавание (sherpa): ") + (e ? e : "?"); return false; }
        #define SYM(field, name) g_sh.field = (decltype(g_sh.field))dlsym(g_sh.lib, name)
        SYM(Create, "SherpaOnnxCreateOfflineRecognizer"); SYM(CreateStream, "SherpaOnnxCreateOfflineStream");
        SYM(Accept, "SherpaOnnxAcceptWaveformOffline"); SYM(Decode, "SherpaOnnxDecodeOfflineStream");
        SYM(Result, "SherpaOnnxGetOfflineStreamResult"); SYM(FreeResult, "SherpaOnnxDestroyOfflineRecognizerResult");
        SYM(FreeStream, "SherpaOnnxDestroyOfflineStream");
        #undef SYM
        if(!g_sh.Create || !g_sh.CreateStream || !g_sh.Accept || !g_sh.Decode || !g_sh.Result || !g_sh.FreeResult || !g_sh.FreeStream)
        { err = "в sherpa-onnx нет нужных функций (другая версия?)"; return false; }
        std::string model = g_base + "/asr/model.int8.onnx", tokens = g_base + "/asr/tokens.txt";
        SherpaOnnxOfflineRecognizerConfig cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.feat_config.sample_rate = 16000; cfg.feat_config.feature_dim = 80;
        cfg.model_config.nemo_ctc.model = model.c_str();
        cfg.model_config.tokens = tokens.c_str();
        cfg.model_config.num_threads = 4;
        cfg.model_config.provider = "cpu";
        cfg.decoding_method = "greedy_search";
        g_sh.rec = g_sh.Create(&cfg);
        if(!g_sh.rec) { err = "модель распознавания не открылась (файл повреждён? скачай заново)"; return false; }
        return true;
    }

    bool AsrRun(const std::vector<float>& pcm, std::string& text, std::string& err)
    {
        if(!g_sh.rec) { err = "распознавание не загружено"; return false; }
        const SherpaOnnxOfflineStream* st = g_sh.CreateStream(g_sh.rec);
        if(!st) { err = "не создать поток распознавания"; return false; }
        g_sh.Accept(st, 16000, pcm.data(), (int32_t)pcm.size());
        g_sh.Decode(g_sh.rec, st);
        const SherpaOnnxOfflineRecognizerResult* r = g_sh.Result(st);
        text = r && r->text ? r->text : "";
        if(r) g_sh.FreeResult(r);
        g_sh.FreeStream(st);
        while(!text.empty() && (text[0] == ' ' || text[0] == '\n')) text.erase(0, 1);
        while(!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
        return true;
    }

    // ---------- рабочие потоки ----------
    struct AsrJob { int kind; std::vector<float> pcm; };          // 0 загрузить, 1 распознать
    struct TtsJob
    {
        int kind = 0;                                              // 0 загрузить, 1 сказать, 2 новый голос из записи
        uint64_t seq = 0; int gen = 0;
        std::string text, voice, id, title; float pitch = 1, speed = 1; bool female = false;
        std::vector<float> pcm; int sampleRate = 16000;
    };
    std::mutex g_asrMx, g_ttsMx;
    std::condition_variable g_asrCv, g_ttsCv;
    std::deque<AsrJob> g_asrQ;
    std::deque<TtsJob> g_ttsQ;
    std::thread g_asrThread, g_ttsThread;
    bool g_threads = false;
    std::mutex g_playMx;                    // запуск звука и «замолчи» — по очереди (берётся раньше g_ttsMx и g_mx)
    std::atomic<int> g_loud{ 2 };           // громкость голосов 0..3
    std::atomic<int> g_gen{ 0 };            // растёт при StopSpeaking — старые фразы бросаем
    std::atomic<int> g_pending{ 0 };        // фраз в очереди / звучит
    std::atomic<uint64_t> g_seq{ 0 };
    std::atomic<bool> g_asrBusy{ false };
    std::map<std::string, std::pair<int, std::string>> g_playing;   // id моста → (gen, id вызывающего); под g_mx

    void AsrThread()
    {
        for(;;)
        {
            AsrJob j;
            { std::unique_lock<std::mutex> l(g_asrMx); g_asrCv.wait(l, [] { return !g_asrQ.empty(); }); j = std::move(g_asrQ.front()); g_asrQ.pop_front(); }
            if(j.kind == 0)
            {
                std::string err; double t0 = NowSec();
                bool ok = AsrLoad(err);
                { std::lock_guard<std::mutex> l(g_mx); g_st.asrReady = ok; if(!g_st.ttsFiles || g_st.ttsReady || !g_threads) g_st.loading = false; }
                if(ok) LOG_INFO("Речь: распознавание GigaAM готово (%.1f с)", NowSec() - t0); else SetError(err);
            }
            else
            {
                Heard h; double t0 = NowSec();
                h.ok = AsrRun(j.pcm, h.text, h.error);
                double dt = NowSec() - t0;
                LOG_INFO("Речь: распознано за %.2f с (%.1f с звука): «%s»%s", dt, j.pcm.size() / 16000.0, h.text.c_str(), h.ok ? "" : (" ОШИБКА " + h.error).c_str());
                std::lock_guard<std::mutex> l(g_mx);
                g_st.lastAsrSec = (float)dt;
                g_heard.push_back(std::move(h));
                g_asrBusy = false;
            }
        }
    }

    bool EnsureVoice(const std::string& key, std::string& err)
    {
        if(xvibe::HasVoice(key)) return true;
        Voice v;
        if(!FindVoice(key, v)) { err = "нет голоса " + key; return false; }
        double t0 = NowSec();
        bool ok = xvibe::LoadVoiceState(key, v.file, err);
        if(ok) LOG_INFO("Речь: голос «%s» загружен (%.2f с)", v.title.c_str(), NowSec() - t0);
        return ok;
    }

    bool TtsLoad(std::string& err)
    {
        if(xvibe::Loaded()) return true;
        if(!LoadOrtLib(err)) return false;
        xvibe::Options o; o.threads = 4; o.voiceCache = 4; o.decodeChunk = 12;
        return xvibe::Load(g_base + "/tts", OrtPath(), o, err);
    }

    void Finished(const std::string& id) { std::lock_guard<std::mutex> l(g_mx); g_spoken.push_back(id); }

    void TtsThread()
    {
        for(;;)
        {
            TtsJob j;
            { std::unique_lock<std::mutex> l(g_ttsMx); g_ttsCv.wait(l, [] { return !g_ttsQ.empty(); }); j = std::move(g_ttsQ.front()); g_ttsQ.pop_front(); }
            if(j.kind == 0)
            {
                std::string err; double t0 = NowSec();
                bool ok = TtsLoad(err);
                { std::lock_guard<std::mutex> l(g_mx); g_st.ttsReady = ok; g_st.loading = false; }
                if(ok) LOG_INFO("Речь: голосовой движок xVibe готов (%.1f с)", NowSec() - t0); else SetError("голоса: " + err);
            }
            else if(j.kind == 1)
            {
                if(j.gen != g_gen.load()) continue;                       // уже отменили
                std::string err; std::vector<float> all;
                double t0 = NowSec();
                bool ok = xvibe::Loaded() && EnsureVoice(j.voice, err);
                if(ok) ok = xvibe::Generate(j.voice, j.text, [&](const float* p, int n) { all.insert(all.end(), p, p + n); return j.gen == g_gen.load(); }, err);
                double dt = NowSec() - t0, len = all.size() / 24000.0;
                std::lock_guard<std::mutex> pl(g_playMx);                 // «замолчи» не должно вклиниться между проверкой и запуском звука
                if(j.gen != g_gen.load()) continue;
                if(!ok || all.empty())
                {
                    if(!ok) SetError("озвучка: " + err);
                    if(g_pending > 0) g_pending--;
                    Finished(j.id);
                    continue;
                }
                { std::lock_guard<std::mutex> l(g_mx); g_st.lastRtf = len > 0 ? (float)(dt / len) : 0; g_playing["x" + std::to_string(j.seq)] = { j.gen, j.id }; }
                LOG_INFO("Речь: озвучено %.1f с звука за %.2f с (x%.2f), голос %s", len, dt, len > 0 ? dt / len : 0.0, j.voice.c_str());
                // Громкость: голос xVibe выходит тихим (пик ~0,3). Выравниваем пик и мягко поджимаем (tanh) —
                // тихие слоги становятся громче, без хрипа. Уровень — из настроек («Громкость голосов»).
                {
                    float peak = 1e-4f; for(float v : all) peak = std::max(peak, fabsf(v));
                    static const float kDrive[] = { 1.0f, 1.6f, 2.4f, 3.4f };
                    float drive = kDrive[std::min(3, std::max(0, g_loud.load()))];
                    float norm = 0.95f / peak, k = tanhf(drive);
                    for(float& v : all) v = tanhf(v * norm * drive) / k * (g_loud.load() == 0 ? 0.7f : 0.98f);
                }
                // мягкий вход и выход, чтобы не щёлкало
                int fade = 24000 / 100;
                for(int i = 0; i < fade && i < (int)all.size(); ++i) { all[i] *= (float)i / fade; all[all.size() - 1 - i] *= (float)i / fade; }
                bridge::PlayPcm(all.data(), (int)all.size(), 24000, j.pitch, j.speed, "x" + std::to_string(j.seq));
            }
            else if(j.kind == 2)
            {
                std::string err;
                std::string key = "user_" + std::to_string((long long)time(nullptr));
                bool ok = xvibe::Loaded();
                if(!ok) err = "голосовой движок ещё не загружен";
                if(ok) ok = xvibe::PrepareVoice(key, j.pcm.data(), (int)j.pcm.size(), j.sampleRate, err);
                MkDirs(g_base + "/uservoices");
                std::string file = g_base + "/uservoices/" + key + ".xvst";
                if(ok) ok = xvibe::SaveVoiceState(key, file, err);
                if(!ok) { Notice("Голос не добавлен: " + err); SetError("новый голос: " + err); continue; }
                Voice v; v.key = key; v.title = j.title.empty() ? "Мой голос" : j.title; v.female = j.female; v.file = file; v.builtin = false;
                { std::lock_guard<std::mutex> l(g_mx); g_voices.push_back(v); g_st.voices = (int)g_voices.size(); }
                SaveUserVoices();
                Notice("Голос «" + v.title + "» добавлен");
                LOG_INFO("Речь: добавлен голос %s «%s» (%.1f с образца)", key.c_str(), v.title.c_str(), j.pcm.size() / (double)j.sampleRate);
            }
        }
    }

    void StartThreads()
    {
        if(g_threads) return;
        g_threads = true;
        g_asrThread = std::thread(AsrThread); g_asrThread.detach();
        g_ttsThread = std::thread(TtsThread); g_ttsThread.detach();
    }

    void RefreshFilesAndLoad()
    {
        bool asr = AsrFilesOk(), tts = TtsFilesOk();
        bool needAsr, needTts;
        {
            std::lock_guard<std::mutex> l(g_mx);
            g_st.asrFiles = asr; g_st.ttsFiles = tts;
            needAsr = asr && !g_st.asrReady; needTts = tts && !g_st.ttsReady;
            if(needAsr || needTts) g_st.loading = true;
        }
        if(tts) ReloadVoices();
        if(!needAsr && !needTts) return;
        StartThreads();
        if(needTts) { TtsJob j; j.kind = 0; { std::lock_guard<std::mutex> l(g_ttsMx); g_ttsQ.push_back(std::move(j)); } g_ttsCv.notify_one(); }
        if(needAsr) { AsrJob j; j.kind = 0; { std::lock_guard<std::mutex> l(g_asrMx); g_asrQ.push_back(std::move(j)); } g_asrCv.notify_one(); }
    }

    // ---------- установка ----------
    enum { BUSY_NONE, BUSY_ASR, BUSY_PACK, BUSY_EXTRA };
    std::string g_extraLabel;
    int g_busyKind = BUSY_NONE;
    enum { PICK_NONE, PICK_PACK, PICK_WAV };
    int g_pickKind = PICK_NONE;
    std::string g_pickTitle; bool g_pickFemale = false;

    void SetBusy(int kind, const std::string& text)
    {
        g_busyKind = kind;
        std::lock_guard<std::mutex> l(g_mx);
        g_st.busy = kind != BUSY_NONE; g_st.busyText = text; g_st.progress = -1; if(kind != BUSY_NONE) g_st.error.clear();
    }

    void StartImport(const std::string& zip)
    {
        LOG_INFO("Речь: импорт голосового пакета %s", zip.c_str());
        MkDirs(g_base);
        SetBusy(BUSY_PACK, "Распаковка голосового пакета…");
        bridge::DownloadPack("unzipall\t" + zip + "\t" + g_base);
    }

    // после распаковки: движки из <base>/lib — в личную папку игры (оттуда можно грузить код)
    void InstallLibsFromPack()
    {
        MkDirs(g_lib);
        for(const char* n : { "libonnxruntime.so", "libsherpa-onnx-c-api.so" })
        {
            std::string from = g_base + "/lib/" + n, to = g_lib + "/" + n;
            if(Exists(from, 1 << 20)) { if(CopyFile(from, to)) remove(from.c_str()); else SetError(std::string("не скопировать ") + n + " в папку игры"); }
        }
    }

    // ---------- запись ----------
    enum { MIC_NONE, MIC_HEAR, MIC_VOICE };
    int g_micMode = MIC_NONE;
    // внешний звук (SA Coop): друг слышит живой голос, а мы распознаём его копию
    bool g_ext = false, g_extReady = false;
    std::vector<int16_t> g_extPcm;                 // 16 кГц
    int g_extSum = 0, g_extCnt = 0; double g_extPos = 0;
    bool g_micDrop = false;
    double g_micSince = 0;
    std::string g_recTitle; bool g_recFemale = false;

    bool ParseWav(const std::string& path, std::vector<float>& out, int& sr)
    {
        std::string d;
        if(!ReadText(path, d) || d.size() < 44 || d.compare(0, 4, "RIFF") != 0 || d.compare(8, 4, "WAVE") != 0) return false;
        size_t pos = 12; int fmt = 0, ch = 0, bits = 0; sr = 0;
        while(pos + 8 <= d.size())
        {
            uint32_t len; memcpy(&len, d.data() + pos + 4, 4);
            std::string id = d.substr(pos, 4); pos += 8;
            if(id == "fmt " && pos + 16 <= d.size())
            {
                uint16_t f, c, b; uint32_t r; memcpy(&f, d.data() + pos, 2); memcpy(&c, d.data() + pos + 2, 2); memcpy(&r, d.data() + pos + 4, 4); memcpy(&b, d.data() + pos + 14, 2);
                fmt = f; ch = c; sr = (int)r; bits = b;
            }
            else if(id == "data")
            {
                if(!ch || !sr) return false;
                size_t n = std::min<size_t>(len, d.size() - pos);
                int bps = bits / 8; if(bps <= 0) return false;
                size_t frames = n / (bps * ch);
                out.resize(frames);
                for(size_t i = 0; i < frames; ++i)
                {
                    double acc = 0;
                    for(int c = 0; c < ch; ++c)
                    {
                        const char* p = d.data() + pos + (i * ch + c) * bps;
                        if(fmt == 3 && bps == 4) { float v; memcpy(&v, p, 4); acc += v; }
                        else if(bps == 2) { int16_t v; memcpy(&v, p, 2); acc += v / 32768.0; }
                        else if(bps == 3) { int32_t v = (uint8_t)p[0] | ((uint8_t)p[1] << 8) | ((int8_t)p[2] << 16); acc += v / 8388608.0; }
                        else if(bps == 4) { int32_t v; memcpy(&v, p, 4); acc += v / 2147483648.0; }
                        else if(bps == 1) { acc += ((uint8_t)*p - 128) / 128.0; }
                    }
                    out[i] = (float)(acc / ch);
                }
                return !out.empty();
            }
            pos += len + (len & 1);
        }
        return false;
    }

    void QueueNewVoice(std::vector<float>&& pcm, int sr, const std::string& title, bool female)
    {
        if(pcm.size() < (size_t)sr * 4) { Notice("Образец слишком короткий — нужно 10–20 секунд спокойной речи"); return; }
        // громкость образца — к норме
        float peak = 0; for(float v : pcm) peak = std::max(peak, fabsf(v));
        if(peak < 0.02f) { Notice("Образец слишком тихий — запиши ближе к микрофону"); return; }
        float k = 0.9f / peak; for(float& v : pcm) v *= k;
        TtsJob j; j.kind = 2; j.pcm = std::move(pcm); j.sampleRate = sr; j.title = title; j.female = female;
        StartThreads();
        { std::lock_guard<std::mutex> l(g_ttsMx); g_ttsQ.push_back(std::move(j)); }
        g_ttsCv.notify_one();
        Notice("Готовлю голос — это займёт несколько секунд…");
    }

    // ---------- числа словами ----------
    std::string Triple(int n, bool fem)
    {
        static const char* ones[] = { "", "один", "два", "три", "четыре", "пять", "шесть", "семь", "восемь", "девять", "десять", "одиннадцать",
                                      "двенадцать", "тринадцать", "четырнадцать", "пятнадцать", "шестнадцать", "семнадцать", "восемнадцать", "девятнадцать" };
        static const char* tens[] = { "", "", "двадцать", "тридцать", "сорок", "пятьдесят", "шестьдесят", "семьдесят", "восемьдесят", "девяносто" };
        static const char* hund[] = { "", "сто", "двести", "триста", "четыреста", "пятьсот", "шестьсот", "семьсот", "восемьсот", "девятьсот" };
        std::string s;
        auto add = [&](const char* w) { if(*w) { if(!s.empty()) s += ' '; s += w; } };
        add(hund[n / 100]); n %= 100;
        if(n >= 20) { add(tens[n / 10]); n %= 10; }
        if(n > 0) add(fem && n == 1 ? "одна" : fem && n == 2 ? "две" : ones[n]);
        return s;
    }
    const char* Plural(int n, const char* one, const char* few, const char* many)
    {
        n %= 100; if(n >= 11 && n <= 19) return many;
        n %= 10; return n == 1 ? one : (n >= 2 && n <= 4 ? few : many);
    }
    std::string NumberWords(long long v)
    {
        if(v == 0) return "ноль";
        if(v < 0 || v >= 1000000000000LL) return "";
        std::string s;
        auto add = [&](const std::string& w) { if(!w.empty()) { if(!s.empty()) s += ' '; s += w; } };
        int bil = (int)(v / 1000000000), mil = (int)(v / 1000000 % 1000), th = (int)(v / 1000 % 1000), r = (int)(v % 1000);
        if(bil) { add(Triple(bil, false)); add(Plural(bil, "миллиард", "миллиарда", "миллиардов")); }
        if(mil) { add(Triple(mil, false)); add(Plural(mil, "миллион", "миллиона", "миллионов")); }
        if(th)  { if(th != 1 || bil || mil) add(Triple(th, true)); add(Plural(th, "тысяча", "тысячи", "тысяч")); }
        if(r) add(Triple(r, false));
        return s;
    }
}

std::string Normalize(const std::string& in)
{
    std::string out; out.reserve(in.size() + 32);
    for(size_t i = 0; i < in.size();)
    {
        unsigned char c = in[i];
        if(c == '$' && i + 1 < in.size() && isdigit((unsigned char)in[i + 1]))
        {   // $500 → пятьсот долларов
            size_t j = i + 1; long long v = 0; int digits = 0;
            while(j < in.size() && (isdigit((unsigned char)in[j]) || ((in[j] == ' ' || in[j] == ',') && j + 1 < in.size() && isdigit((unsigned char)in[j + 1]) && digits > 0)))
            { if(isdigit((unsigned char)in[j])) { v = v * 10 + (in[j] - '0'); ++digits; } ++j; }
            std::string w = digits <= 12 ? NumberWords(v) : "";
            if(!w.empty()) { out += w; out += ' '; out += Plural((int)(v % 100), "доллар", "доллара", "долларов"); i = j; continue; }
        }
        if(isdigit(c))
        {
            size_t j = i; long long v = 0; int digits = 0;
            while(j < in.size() && isdigit((unsigned char)in[j])) { v = v * 10 + (in[j] - '0'); ++digits; ++j; }
            std::string w = digits <= 12 ? NumberWords(v) : "";
            if(!w.empty())
            {
                out += w; i = j;
                if(i < in.size() && in[i] == '%') { out += ' '; out += Plural((int)(v % 100), "процент", "процента", "процентов"); ++i; }
                else if(i < in.size() && in[i] == '$') { out += ' '; out += Plural((int)(v % 100), "доллар", "доллара", "долларов"); ++i; }
                continue;
            }
        }
        if(c == '$') { out += "долларов"; ++i; continue; }
        if(c == '%') { out += " процентов"; ++i; continue; }
        if(c == '&') { out += " и "; ++i; continue; }
        if(c == '*' || c == '_' || c == '#' || c == '"' || c == '[' || c == ']' || c == '(' || c == ')') { ++i; continue; }
        out += (char)c; ++i;
    }
    return out;
}

// ================= API =================
void Init(const std::string& baseDir, const std::string& libDir)
{
    g_base = baseDir; g_lib = libDir;
    MkDirs(g_base); MkDirs(g_lib);
    InstallLibsFromPack();
    RefreshFilesAndLoad();
    Status s = Get();
    LOG_INFO("Речь: распознавание %s, голоса %s (%d шт.), папка %s", s.asrFiles ? "установлено" : "не установлено",
             s.ttsFiles ? "установлены" : "не установлены", s.voices, g_base.c_str());
}

void SetSearchDirs(const std::vector<std::string>& dirs) { g_searchDirs = dirs; }

Status Get() { std::lock_guard<std::mutex> l(g_mx); return g_st; }

void DownloadAsr()
{
    if(g_busyKind != BUSY_NONE) return;
    MkDirs(g_base + "/asr"); MkDirs(g_base + "/dl"); MkDirs(g_lib);
    std::string plan;
    plan += std::string(kAsrTokensUrl) + "\t" + g_base + "/asr/tokens.txt\n";
    plan += std::string(kAsrModelUrl) + "\t" + g_base + "/asr/model.int8.onnx\n";
    if(!Exists(OrtPath(), 1 << 20) || !Exists(SherpaPath(), 1 << 20))
    {
        std::string aar = g_base + "/dl/sherpa.aar";
        plan += std::string(kAarUrl) + "\t" + aar + "\n";
        plan += "unzip\t" + aar + "\tjni/arm64-v8a/libonnxruntime.so\t" + OrtPath() + "\n";
        plan += "unzip\t" + aar + "\tjni/arm64-v8a/libsherpa-onnx-c-api.so\t" + SherpaPath() + "\n";
    }
    SetBusy(BUSY_ASR, "Скачиваю распознавание речи…");
    LOG_INFO("Речь: скачивание распознавания (GigaAM v3)");
    bridge::DownloadPack(plan);
}

static bool LooksLikePack(const char* name)
{
    size_t n = strlen(name);
    if(n < 5 || strcasecmp(name + n - 4, ".zip")) return false;
    std::string low; for(size_t i = 0; i < n; ++i) low += (char)tolower((unsigned char)name[i]);
    return low.find("saliving") != std::string::npos && low.find("voice") != std::string::npos;
}

bool ImportVoicePackAuto()
{
    if(g_busyKind != BUSY_NONE) return true;
    std::string best; time_t bestT = 0;
    for(const std::string& dir : g_searchDirs)
    {
        DIR* d = opendir(dir.c_str());
        if(!d) continue;
        while(dirent* e = readdir(d))
        {
            if(!LooksLikePack(e->d_name)) continue;
            std::string p = dir + "/" + e->d_name; struct stat st;
            if(stat(p.c_str(), &st) == 0 && st.st_size > (20 << 20) && st.st_mtime >= bestT) { best = p; bestT = st.st_mtime; }
        }
        closedir(d);
    }
    if(best.empty()) return false;
    StartImport(best);
    return true;
}

bool PickVoicePack()
{
    if(g_busyKind != BUSY_NONE) return false;
    MkDirs(g_base + "/dl");
    if(!bridge::PickFile(g_base + "/dl/pack.zip")) return false;
    g_pickKind = PICK_PACK;
    return true;
}

void CancelBusy() { if(g_busyKind != BUSY_NONE) bridge::CancelDownload(); }

bool DownloadExtra(const std::string& url, const std::string& path, const std::string& label)
{
    if(g_busyKind != BUSY_NONE) return false;
    size_t sl = path.rfind('/'); if(sl != std::string::npos) MkDirs(path.substr(0, sl));
    remove((path + ".ok").c_str());
    g_extraLabel = label;
    SetBusy(BUSY_EXTRA, "Скачиваю: " + label + "…");
    bridge::DownloadPack(url + "\t" + path + "\n");
    return true;
}

void RemoveAll()
{
    for(const char* f : kTtsFiles) remove((g_base + "/tts/" + f).c_str());
    remove((g_base + "/asr/model.int8.onnx").c_str()); remove((g_base + "/asr/model.int8.onnx.ok").c_str());
    remove((g_base + "/asr/tokens.txt").c_str()); remove((g_base + "/asr/tokens.txt.ok").c_str());
    Notice("Файлы речи удалены. Движки выгрузятся после перезапуска игры.");
    std::lock_guard<std::mutex> l(g_mx); g_st.asrFiles = g_st.ttsFiles = false;
}

bool AsrReady() { std::lock_guard<std::mutex> l(g_mx); return g_st.asrReady; }
bool TtsReady() { std::lock_guard<std::mutex> l(g_mx); return g_st.ttsReady; }

bool Listen()
{
    if(!AsrReady() || g_micMode != MIC_NONE || g_asrBusy) return false;     // прошлая фраза ещё распознаётся — не начинаем новую запись
    if(!bridge::MicStart()) return false;
    g_micMode = MIC_HEAR; g_micDrop = false; g_micSince = NowSec();
    return true;
}
void StopListen()
{
    if(g_micMode != MIC_HEAR) return;
    if(g_ext) { g_ext = false; g_extReady = true; g_asrBusy = true; return; }   // запись готова — распознаем в Frame
    bridge::MicStop(); g_asrBusy = true;
}
void CancelListen()
{
    if(g_micMode != MIC_HEAR) return;
    if(g_ext) { g_ext = false; g_extPcm.clear(); g_micMode = MIC_NONE; return; }
    g_micDrop = true; bridge::MicStop();
}
bool ListenExternal()
{
    if(!AsrReady() || g_micMode != MIC_NONE || g_asrBusy) return false;
    g_extPcm.clear(); g_extReady = false; g_extSum = 0; g_extCnt = 0; g_extPos = 0;
    g_ext = true; g_micMode = MIC_HEAR; g_micDrop = false; g_micSince = NowSec();
    return true;
}
void FeedExternal(const int16_t* pcm, int n, int rate)
{
    if(!g_ext || g_micMode != MIC_HEAR || !pcm || n <= 0 || rate <= 0) return;
    if(g_extPcm.size() > 16000u * 30) return;                         // не дольше 30 с, как у своего микрофона
    if(rate == 16000) { g_extPcm.insert(g_extPcm.end(), pcm, pcm + n); return; }
    if(rate == 48000)
    {   // 48 → 16 кГц: среднее по три отсчёта (заодно срезает верх, который распознаванию не нужен)
        for(int i = 0; i < n; ++i) { g_extSum += pcm[i]; if(++g_extCnt == 3) { g_extPcm.push_back((int16_t)(g_extSum / 3)); g_extSum = 0; g_extCnt = 0; } }
        return;
    }
    double step = rate / 16000.0;                                     // другая частота — простое прореживание
    for(; g_extPos < n; g_extPos += step) g_extPcm.push_back(pcm[(int)g_extPos]);
    g_extPos -= n;
}
bool Listening() { return g_micMode == MIC_HEAR && !g_asrBusy; }
bool Recognizing() { return g_asrBusy; }
bool PollHeard(Heard& out) { std::lock_guard<std::mutex> l(g_mx); if(g_heard.empty()) return false; out = std::move(g_heard.front()); g_heard.pop_front(); return true; }

void Speak(const std::string& text, const std::string& voiceKey, float pitch, float speed, const std::string& id)
{
    std::string t = Normalize(text);
    if(t.empty() || !TtsReady()) { Finished(id); return; }
    TtsJob j; j.kind = 1; j.text = t; j.voice = voiceKey; j.pitch = pitch; j.speed = speed; j.id = id; j.gen = g_gen.load(); j.seq = ++g_seq;
    g_pending++;
    { std::lock_guard<std::mutex> l(g_ttsMx); g_ttsQ.push_back(std::move(j)); }
    g_ttsCv.notify_one();
}

void StopSpeaking()
{
    std::lock_guard<std::mutex> pl(g_playMx);
    g_gen++;
    {
        std::lock_guard<std::mutex> l(g_ttsMx);
        for(auto it = g_ttsQ.begin(); it != g_ttsQ.end();) { if(it->kind == 1) it = g_ttsQ.erase(it); else ++it; }
    }
    g_pending = 0;
    { std::lock_guard<std::mutex> l(g_mx); g_playing.clear(); }
    bridge::PlayStop();
}

void SetLoudness(int level) { g_loud = level; }

bool Speaking() { return g_pending.load() > 0; }
bool PollSpoken(std::string& id) { std::lock_guard<std::mutex> l(g_mx); if(g_spoken.empty()) return false; id = std::move(g_spoken.front()); g_spoken.pop_front(); return true; }

std::vector<Voice> Voices() { std::lock_guard<std::mutex> l(g_mx); return g_voices; }
bool FindVoice(const std::string& key, Voice& out)
{
    std::lock_guard<std::mutex> l(g_mx);
    for(auto& v : g_voices) if(v.key == key) { out = v; return true; }
    return false;
}

std::string PickVoice(bool female, unsigned seed, const std::vector<std::string>& avoid)
{
    std::lock_guard<std::mutex> l(g_mx);
    std::vector<const Voice*> c;
    for(auto& v : g_voices) if(v.female == female) c.push_back(&v);
    if(c.empty()) for(auto& v : g_voices) c.push_back(&v);
    if(c.empty()) return "";
    std::vector<const Voice*> fresh;
    for(auto* v : c) { bool used = false; for(auto& a : avoid) if(a == v->key) used = true; if(!used) fresh.push_back(v); }
    if(!fresh.empty()) c.swap(fresh);
    return c[seed % c.size()]->key;
}

bool RecordVoiceStart()
{
    if(g_micMode != MIC_NONE) return false;
    if(!bridge::MicStart()) return false;
    g_micMode = MIC_VOICE; g_micDrop = false; g_micSince = NowSec();
    return true;
}
void RecordVoiceFinish(const std::string& title, bool female) { if(g_micMode == MIC_VOICE) { g_recTitle = title; g_recFemale = female; bridge::MicStop(); } }
bool RecordingVoice() { return g_micMode == MIC_VOICE; }
float RecordSeconds() { return g_micMode == MIC_NONE ? 0 : (float)(NowSec() - g_micSince); }

bool AddVoiceFromFile(const std::string& title, bool female)
{
    MkDirs(g_base + "/uservoices");
    if(!bridge::PickFile(g_base + "/uservoices/import.wav")) return false;
    g_pickKind = PICK_WAV; g_pickTitle = title; g_pickFemale = female;
    return true;
}

void DeleteVoice(const std::string& key)
{
    std::string file;
    {
        std::lock_guard<std::mutex> l(g_mx);
        for(auto it = g_voices.begin(); it != g_voices.end(); ++it)
            if(it->key == key && !it->builtin) { file = it->file; g_voices.erase(it); break; }
        g_st.voices = (int)g_voices.size();
    }
    if(file.empty()) return;
    remove(file.c_str());
    SaveUserVoices();
}

bool PopNotice(std::string& out) { std::lock_guard<std::mutex> l(g_mx); if(g_notices.empty()) return false; out = std::move(g_notices.front()); g_notices.pop_front(); return true; }

void Frame()
{
    // скачивание / распаковка
    bridge::DlEvent d;
    while(bridge::PollDl(d))
    {
        if(d.state == 0)
        {
            std::lock_guard<std::mutex> l(g_mx);
            g_st.progress = d.total > 0 ? (float)d.done / (float)d.total : -1;
            char buf[256];
            if(d.total > 0) snprintf(buf, sizeof(buf), "%s: %lld из %lld МБ", d.msg.c_str(), d.done >> 10, d.total >> 10);
            else snprintf(buf, sizeof(buf), "%s…", d.msg.c_str());
            g_st.busyText = buf;
        }
        else if(d.state == 1)
        {
            int kind = g_busyKind;
            SetBusy(BUSY_NONE, "");
            if(kind == BUSY_ASR) { remove((g_base + "/dl/sherpa.aar").c_str()); remove((g_base + "/dl/sherpa.aar.ok").c_str()); chmod(OrtPath().c_str(), 0700); chmod(SherpaPath().c_str(), 0700); }
            if(kind == BUSY_PACK) { InstallLibsFromPack(); remove((g_base + "/dl/pack.zip").c_str()); }
            RefreshFilesAndLoad();
            Status s = Get();
            if(kind == BUSY_ASR) Notice(s.asrFiles ? "Распознавание речи скачано — загружаю…" : "Распознавание скачалось не полностью — попробуй ещё раз");
            if(kind == BUSY_PACK) Notice(s.ttsFiles ? "Голосовой пакет установлен — загружаю голоса…" : "В архиве нет голосового пакета SA Living");
            if(kind == BUSY_EXTRA) Notice("Скачано: " + g_extraLabel);
            LOG_INFO("Речь: %s завершено (распознавание %d, голоса %d)", kind == BUSY_ASR ? "скачивание" : "импорт", (int)s.asrFiles, (int)s.ttsFiles);
        }
        else
        {
            int kind = g_busyKind;
            SetBusy(BUSY_NONE, "");
            SetError((kind == BUSY_PACK ? "импорт не удался: " : "скачивание не удалось: ") + d.msg);
            Notice((kind == BUSY_PACK ? "Импорт не удался: " : "Скачивание не удалось: ") + d.msg);
        }
    }

    // выбор файла
    if(g_pickKind != PICK_NONE)
    {
        std::string res; int st = bridge::PickState(&res);
        if(st == 2)
        {
            int kind = g_pickKind; g_pickKind = PICK_NONE; bridge::PickReset();
            if(kind == PICK_PACK) StartImport(res);
            else
            {
                std::vector<float> pcm; int sr = 0;
                if(ParseWav(res, pcm, sr)) QueueNewVoice(std::move(pcm), sr, g_pickTitle, g_pickFemale);
                else Notice("Это не WAV-файл. Нужен обычный .wav (запись голоса 10–20 с)");
                remove(res.c_str());
            }
        }
        else if(st < 0)
        {
            g_pickKind = PICK_NONE; bridge::PickReset();
            if(st == -2) Notice("Файл не взят: " + res);
        }
    }

    // микрофон (свой — через мост Android, или внешний — копия звука SA Coop)
    std::vector<int16_t> rec;
    bool extRec = false;
    if(g_extReady) { g_extReady = false; rec.swap(g_extPcm); extRec = true; }
    while(extRec || bridge::PollMic(rec))
    {
        extRec = false;
        int mode = g_micMode; g_micMode = MIC_NONE;
        if(g_micDrop) { g_micDrop = false; g_asrBusy = false; continue; }
        std::vector<float> pcm(rec.size());
        for(size_t i = 0; i < rec.size(); ++i) pcm[i] = rec[i] / 32768.0f;
        if(mode == MIC_HEAR)
        {
            float peak = 0; for(float v : pcm) peak = std::max(peak, fabsf(v));
            if(pcm.size() < 16000 / 4 || peak < 0.01f)
            {
                g_asrBusy = false;
                Heard h; h.ok = true; h.text = "";
                LOG_INFO("Речь: запись пустая (%.2f с, громкость %.3f)", pcm.size() / 16000.0, peak);
                std::lock_guard<std::mutex> l(g_mx); g_heard.push_back(h);
                continue;
            }
            g_asrBusy = true;
            AsrJob j; j.kind = 1; j.pcm = std::move(pcm);
            { std::lock_guard<std::mutex> l(g_asrMx); g_asrQ.push_back(std::move(j)); }
            g_asrCv.notify_one();
        }
        else if(mode == MIC_VOICE) QueueNewVoice(std::move(pcm), 16000, g_recTitle, g_recFemale);
    }
    // защита: запись не дольше 30 с (мост сам останавливает), запись голоса — не дольше 25 с
    if(g_micMode == MIC_VOICE && NowSec() - g_micSince > 25) bridge::MicStop();

    // конец воспроизведения
    bridge::TtsEvent te;
    while(bridge::PollTts(te))
    {
        if(te.state == 0) continue;
        std::lock_guard<std::mutex> l(g_mx);
        auto it = g_playing.find(te.id);
        if(it != g_playing.end())
        {
            if(it->second.first == g_gen.load()) { g_spoken.push_back(it->second.second); if(g_pending > 0) g_pending--; }
            g_playing.erase(it);
        }
        else g_spoken.push_back(te.id);      // голос Android (Google)
    }
}

// ---------- для проверки на компьютере ----------
bool TestLoad(std::string& err)
{
    for(int i = 0; i < 1200; ++i)
    {
        Status s = Get();
        if(s.asrReady && s.ttsReady) return true;
        if(!s.error.empty()) { err = s.error; return false; }
        if(!s.asrFiles || !s.ttsFiles) { err = "файлы речи не найдены"; return false; }
        usleep(100000);
    }
    err = "движки не загрузились за 2 минуты";
    return false;
}
bool TestRecognize(const std::vector<float>& pcm, std::string& text, std::string& err) { return AsrRun(pcm, text, err); }
bool TestSynthesize(const std::string& voiceKey, const std::string& text, std::vector<float>& pcm, std::string& err)
{
    if(!EnsureVoice(voiceKey, err)) return false;
    return xvibe::Generate(voiceKey, Normalize(text), [&](const float* p, int n) { pcm.insert(pcm.end(), p, p + n); return true; }, err);
}

} // namespace speech
