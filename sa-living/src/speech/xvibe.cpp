#include "xvibe.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <random>
#include <set>
#include <unordered_map>

#include "speech/ort/onnxruntime_c_api.h"
#include "util/json.h"

namespace xvibe
{
namespace
{
    constexpr uint32_t kOrtApiVersion = 24;      // onnxruntime 1.24 (в пакете sherpa-onnx 1.12.40)
    constexpr int   kLatentDim = 32, kCondDim = 1024, kSampleRate = 24000;
    constexpr float kFrameRate = 12.5f, kTokensPerSecond = 3.0f, kGenPadding = 2.0f;
    constexpr int   kMaxTokensPerChunk = 50;

    const OrtApi* A = nullptr;
    OrtEnv* g_env = nullptr;
    OrtSessionOptions* g_so = nullptr;
    OrtMemoryInfo* g_mem = nullptr;
    OrtAllocator* g_alloc = nullptr;
    OrtSession *g_enc = nullptr, *g_tc = nullptr, *g_main = nullptr, *g_flow = nullptr, *g_dec = nullptr;
    std::string g_tcIn = "token_ids", g_tcOut, g_flowOut, g_decOut;
    Options g_opt;
    bool g_loaded = false;
    std::mt19937 g_rng{ 12345 };

    struct StateEntry { std::string in, out, dtype, fill; std::vector<int64_t> shape; int index; };
    std::vector<StateEntry> g_flowState, g_mimiState;

    // ---- токенизатор (SentencePiece BPE: nmt_nfkc, add_dummy_prefix) ----
    std::unordered_map<std::string, int> g_vocab;
    std::unordered_map<std::string, float> g_score;
    std::vector<std::string> g_id2piece;
    int g_unk = 0;
    std::set<int> g_eosTokens, g_fallbackTokens;

    struct VoiceState { std::vector<OrtValue*> state; uint64_t used = 0; };
    std::map<std::string, VoiceState> g_voices;
    uint64_t g_tick = 0;

    bool Check(OrtStatus* s, std::string& err)
    {
        if(!s) return true;
        err = A->GetErrorMessage(s);
        A->ReleaseStatus(s);
        return false;
    }
    #define CHK(x) do { if(!Check((x), err)) return false; } while(0)

    size_t ElemSize(ONNXTensorElementDataType t)
    {
        switch(t) { case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return 8; case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL: return 1; default: return 4; }
    }

    ONNXTensorElementDataType TypeOf(const std::string& d)
    {
        if(d == "int64") return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
        if(d == "bool") return ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
        return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    }

    bool MakeTensor(const std::vector<int64_t>& shape, ONNXTensorElementDataType t, OrtValue** out, std::string& err)
    {
        CHK(A->CreateTensorAsOrtValue(g_alloc, shape.data(), shape.size(), t, out));
        return true;
    }

    // Тензор из готовых данных (копия)
    bool FromData(const void* data, size_t bytes, const std::vector<int64_t>& shape, ONNXTensorElementDataType t, OrtValue** out, std::string& err)
    {
        if(!MakeTensor(shape, t, out, err)) return false;
        if(bytes) { void* p = nullptr; CHK(A->GetTensorMutableData(*out, &p)); memcpy(p, data, bytes); }
        return true;
    }

    bool Info(OrtValue* v, std::vector<int64_t>& shape, ONNXTensorElementDataType& t, size_t& count, std::string& err)
    {
        OrtTensorTypeAndShapeInfo* info = nullptr;
        CHK(A->GetTensorTypeAndShape(v, &info));
        size_t nd = 0; A->GetDimensionsCount(info, &nd);
        shape.resize(nd); if(nd) A->GetDimensions(info, shape.data(), nd);
        A->GetTensorElementType(info, &t);
        A->GetTensorShapeElementCount(info, &count);
        A->ReleaseTensorTypeAndShapeInfo(info);
        return true;
    }

    bool Clone(OrtValue* src, OrtValue** dst, std::string& err)
    {
        std::vector<int64_t> shape; ONNXTensorElementDataType t; size_t n;
        if(!Info(src, shape, t, n, err)) return false;
        void* p = nullptr;
        if(n) CHK(A->GetTensorMutableData(src, &p));
        return FromData(p, n * ElemSize(t), shape, t, dst, err);
    }

    void ReleaseAll(std::vector<OrtValue*>& v) { for(auto* x : v) if(x) A->ReleaseValue(x); v.clear(); }

    bool InitState(const std::vector<StateEntry>& man, std::vector<OrtValue*>& st, std::string& err)
    {
        ReleaseAll(st);
        for(auto& e : man)
        {
            OrtValue* v = nullptr;
            auto t = TypeOf(e.dtype);
            if(!MakeTensor(e.shape, t, &v, err)) return false;
            size_t n = 1; for(auto d : e.shape) n *= (size_t)d;
            if(n)
            {
                void* p = nullptr; CHK(A->GetTensorMutableData(v, &p));
                memset(p, 0, n * ElemSize(t));
                if(e.fill == "nan" && t == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) { float* f = (float*)p; for(size_t i = 0; i < n; ++i) f[i] = NAN; }
                else if(e.fill == "ones")
                {
                    if(t == ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL) memset(p, 1, n);
                    else if(t == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) { int64_t* f = (int64_t*)p; for(size_t i = 0; i < n; ++i) f[i] = 1; }
                    else { float* f = (float*)p; for(size_t i = 0; i < n; ++i) f[i] = 1.0f; }
                }
            }
            st.push_back(v);
        }
        return true;
    }

    std::string OutName(OrtSession* s, size_t i)
    {
        char* name = nullptr;
        if(A->SessionGetOutputName(s, i, g_alloc, &name)) return "";
        std::string r = name; g_alloc->Free(g_alloc, name);
        return r;
    }

    // Запуск: входы (имя, значение) — значения НЕ освобождаются; выходы — новые OrtValue
    bool Run(OrtSession* s, const std::vector<const char*>& inNames, const std::vector<OrtValue*>& in,
             const std::vector<const char*>& outNames, std::vector<OrtValue*>& out, std::string& err)
    {
        out.assign(outNames.size(), nullptr);
        CHK(A->Run(s, nullptr, inNames.data(), (const OrtValue* const*)in.data(), in.size(), outNames.data(), outNames.size(), out.data()));
        return true;
    }

    // ---------- UTF-8 ----------
    std::vector<std::string> Chars(const std::string& s)
    {
        std::vector<std::string> r;
        for(size_t i = 0; i < s.size();)
        {
            unsigned char c = s[i];
            size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            r.push_back(s.substr(i, n)); i += n;
        }
        return r;
    }
    uint32_t Cp(const std::string& ch)
    {
        const unsigned char* p = (const unsigned char*)ch.data();
        if(ch.size() == 1) return p[0];
        if(ch.size() == 2) return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        if(ch.size() == 3) return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    std::string Utf8(uint32_t c)
    {
        std::string o;
        if(c < 0x80) o += (char)c;
        else if(c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        return o;
    }
    bool IsCyr(uint32_t c) { return (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451; }
    bool IsVowel(uint32_t c)
    {
        static const uint32_t v[] = { 0x430, 0x435, 0x451, 0x438, 0x43E, 0x443, 0x44B, 0x44D, 0x44E, 0x44F,
                                      0x410, 0x415, 0x401, 0x418, 0x41E, 0x423, 0x42B, 0x42D, 0x42E, 0x42F };
        for(uint32_t x : v) if(x == c) return true;
        return false;
    }
    bool IsAlnum(uint32_t c) { return IsCyr(c) || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    uint32_t Upper(uint32_t c)
    {
        if(c >= 0x430 && c <= 0x44F) return c - 0x20;
        if(c == 0x451) return 0x401;
        if(c >= 'a' && c <= 'z') return c - 32;
        return c;
    }

    std::vector<int> Bpe(const std::string& text)
    {
        // nmt_nfkc: пробелы схлопнуты, по краям обрезаны; dummy prefix «▁»
        std::string t;
        bool sp = true;
        for(auto& ch : Chars(text))
        {
            uint32_t c = Cp(ch);
            if(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0) { if(!sp) { t += ' '; sp = true; } continue; }
            t += ch; sp = false;
        }
        while(!t.empty() && t.back() == ' ') t.pop_back();
        std::string u = "\xE2\x96\x81";
        for(char c : t) { if(c == ' ') u += "\xE2\x96\x81"; else u += c; }
        std::vector<std::string> sym = Chars(u);
        while(sym.size() > 1)
        {
            float best = -1e30f; int bi = -1;
            for(size_t i = 0; i + 1 < sym.size(); ++i)
            {
                auto it = g_score.find(sym[i] + sym[i + 1]);
                if(it != g_score.end() && it->second > best) { best = it->second; bi = (int)i; }
            }
            if(bi < 0) break;
            sym[bi] += sym[bi + 1];
            sym.erase(sym.begin() + bi + 1);
        }
        std::vector<int> ids;
        for(auto& s : sym)
        {
            auto it = g_vocab.find(s);
            int id = it == g_vocab.end() ? g_unk : it->second;
            if(id == g_unk && !ids.empty() && ids.back() == g_unk) continue;   // подряд неизвестные — один <unk>
            ids.push_back(id);
        }
        return ids;
    }

    std::string Decode(const std::vector<int>& ids, size_t a, size_t b)
    {
        std::string s;
        for(size_t i = a; i < b; ++i) if(ids[i] >= 0 && ids[i] < (int)g_id2piece.size()) s += g_id2piece[ids[i]];
        std::string r;
        for(size_t i = 0; i < s.size();)
        {
            if(s.compare(i, 3, "\xE2\x96\x81") == 0) { r += ' '; i += 3; } else r += s[i++];
        }
        size_t x = r.find_first_not_of(' '), y = r.find_last_not_of(' ');
        return x == std::string::npos ? "" : r.substr(x, y - x + 1);
    }

    // _prepare_text_prompt: заглавная первая буква, точка в конце; сколько кадров после «конца»
    std::string PromptText(const std::string& in, int& framesAfterEos)
    {
        std::string t = in;
        int words = 0; bool inw = false;
        for(char c : t) { if(c != ' ') { if(!inw) words++; inw = true; } else inw = false; }
        framesAfterEos = (words <= 4 ? 3 : 1) + 2;
        auto ch = Chars(t);
        if(!ch.empty()) { ch[0] = Utf8(Upper(Cp(ch[0]))); }
        std::string r; for(auto& c : ch) r += c;
        if(!ch.empty() && IsAlnum(Cp(ch.back()))) r += ".";
        return r;
    }

    std::vector<std::string> SplitChunks(const std::string& text)
    {
        int dummy;
        std::string prepared = PromptText(text, dummy);
        std::vector<int> toks = Bpe(prepared);
        auto bounds = [](const std::vector<int>& tk, const std::set<int>& b) {
            std::vector<size_t> idx{ 0 }; bool prev = false;
            for(size_t i = 0; i < tk.size(); ++i)
            {
                if(b.count(tk[i])) prev = true;
                else { if(prev) idx.push_back(i); prev = false; }
            }
            idx.push_back(tk.size());
            return idx;
        };
        std::vector<std::pair<int, std::string>> segs;
        auto bi = bounds(toks, g_eosTokens);
        for(size_t i = 0; i + 1 < bi.size(); ++i)
        {
            int cnt = (int)(bi[i + 1] - bi[i]);
            std::string st = Decode(toks, bi[i], bi[i + 1]);
            if(cnt <= kMaxTokensPerChunk) { segs.push_back({ cnt, st }); continue; }
            std::vector<int> sub = Bpe(st);
            auto sb = bounds(sub, g_fallbackTokens);
            if(sb.size() > 2) for(size_t k = 0; k + 1 < sb.size(); ++k) segs.push_back({ (int)(sb[k + 1] - sb[k]), Decode(sub, sb[k], sb[k + 1]) });
            else segs.push_back({ cnt, st });
        }
        std::vector<std::string> chunks; std::string cur; int cc = 0;
        for(auto& s : segs)
        {
            if(cur.empty()) { cur = s.second; cc = s.first; continue; }
            if(cc + s.first > kMaxTokensPerChunk) { chunks.push_back(cur); cur = s.second; cc = s.first; }
            else { cur += " " + s.second; cc += s.first; }
        }
        if(!cur.empty()) chunks.push_back(cur);
        return chunks;
    }

    bool LoadManifest(const json::Value& arr, std::vector<StateEntry>& out)
    {
        out.clear();
        for(auto& e : arr.arr())
        {
            StateEntry s; s.in = e["input_name"].str(); s.out = e["output_name"].str(); s.dtype = e["dtype"].str();
            s.fill = e["fill"].str(); s.index = e["index"].i();
            for(auto& d : e["shape"].arr()) s.shape.push_back((int64_t)d.num());
            out.push_back(s);
        }
        return !out.empty();
    }

    bool ReadFile(const std::string& p, std::string& out)
    {
        FILE* f = fopen(p.c_str(), "rb"); if(!f) return false;
        char buf[65536]; size_t n; out.clear();
        while((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f); return true;
    }

    // Прогон FlowLM: последовательность + эмбеддинги, состояние обновляется; cond/eos — по желанию
    bool RunMain(OrtValue* seq, OrtValue* emb, std::vector<OrtValue*>& state, OrtValue** cond, float* eos, std::string& err)
    {
        std::vector<const char*> in{ "sequence", "text_embeddings" }, out{ "conditioning", "eos_logit" };
        std::vector<OrtValue*> vals{ seq, emb };
        for(size_t i = 0; i < g_flowState.size(); ++i) { in.push_back(g_flowState[i].in.c_str()); vals.push_back(state[i]); }
        for(auto& e : g_flowState) out.push_back(e.out.c_str());
        std::vector<OrtValue*> res;
        if(!Run(g_main, in, vals, out, res, err)) return false;
        for(size_t i = 0; i < g_flowState.size(); ++i) { A->ReleaseValue(state[i]); state[i] = res[2 + g_flowState[i].index]; }
        if(eos) { float* p = nullptr; A->GetTensorMutableData(res[1], (void**)&p); *eos = p[0]; }
        if(cond) *cond = res[0]; else A->ReleaseValue(res[0]);
        A->ReleaseValue(res[1]);
        return true;
    }

    bool Decode(OrtValue* latent, std::vector<OrtValue*>& mstate, const std::function<bool(const float*, int)>& cb, bool& go, std::string& err)
    {
        std::vector<const char*> in{ "latent" }, out{ g_decOut.c_str() };
        std::vector<OrtValue*> vals{ latent };
        for(size_t i = 0; i < g_mimiState.size(); ++i) { in.push_back(g_mimiState[i].in.c_str()); vals.push_back(mstate[i]); }
        for(auto& e : g_mimiState) out.push_back(e.out.c_str());
        std::vector<OrtValue*> res;
        if(!Run(g_dec, in, vals, out, res, err)) return false;
        for(size_t i = 0; i < g_mimiState.size(); ++i) { A->ReleaseValue(mstate[i]); mstate[i] = res[1 + g_mimiState[i].index]; }
        std::vector<int64_t> sh; ONNXTensorElementDataType t; size_t n = 0;
        if(Info(res[0], sh, t, n, err) && n && go)
        {
            float* p = nullptr; A->GetTensorMutableData(res[0], (void**)&p);
            go = cb(p, (int)n);
        }
        A->ReleaseValue(res[0]);
        return true;
    }

    std::vector<float> Resample(const float* x, int n, int from, int to)
    {
        if(from == to) return std::vector<float>(x, x + n);
        int m = (int)((int64_t)n * to / from);
        std::vector<float> y(m);
        double step = (double)from / to;
        for(int i = 0; i < m; ++i)
        {
            double p = i * step; int k = (int)p; double f = p - k;
            float a = x[k < n ? k : n - 1], b = x[k + 1 < n ? k + 1 : n - 1];
            y[i] = (float)(a + (b - a) * f);
        }
        return y;
    }
}

bool Loaded() { return g_loaded; }
int  SampleRate() { return kSampleRate; }

std::string PrepareText(const std::string& in)
{
    // «+гласная» → гласная + U+0301; ё без знака; только буквы, цифры, пробел и знаки препинания
    std::string out;
    auto ch = Chars(in);
    for(size_t i = 0; i < ch.size(); ++i)
    {
        uint32_t c = Cp(ch[i]);
        if(c == '+')
        {
            if(i + 1 < ch.size() && IsVowel(Cp(ch[i + 1])))
            {
                uint32_t v = Cp(ch[i + 1]);
                out += ch[i + 1];
                if(v != 0x451 && v != 0x401) out += "\xCC\x81";
                ++i;
            }
            continue;
        }
        if(c == 0x301)
        {
            if(!out.empty()) out += ch[i];
            continue;
        }
        if(c == 0x2014 || c == 0x2013) { out += " \xE2\x80\x94 "; continue; }        // тире
        if(c == 0x2026) { out += "..."; continue; }
        if(c == 0xAB || c == 0xBB || c == '"' || c == 0x201C || c == 0x201D) continue;  // кавычки — без них
        if(IsAlnum(c) || c == ' ' || c == ',' || c == '.' || c == '!' || c == '?' || c == ':' || c == ';' || c == '-' || c == '\'') { out += ch[i]; continue; }
        if(c == '\n' || c == '\t') { out += ' '; continue; }
        // остальное — выбрасываем
    }
    std::string r; bool sp = false;
    for(char c : out) { if(c == ' ') { if(!sp && !r.empty()) r += c; sp = true; } else { r += c; sp = false; } }
    while(!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

std::vector<int> Tokenize(const std::string& text) { return Bpe(text); }

bool Load(const std::string& dir, const std::string& ortLib, const Options& opt, std::string& err)
{
    if(g_loaded) return true;
    g_opt = opt;
    void* h = ortLib.empty() ? RTLD_DEFAULT : dlopen(ortLib.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if(!h) { err = std::string("dlopen onnxruntime: ") + dlerror(); return false; }
    typedef const OrtApiBase* (*GetBase_t)();
    GetBase_t gb = (GetBase_t)dlsym(h, "OrtGetApiBase");
    if(!gb) { err = "нет OrtGetApiBase"; return false; }
    A = gb()->GetApi(kOrtApiVersion);
    if(!A) { err = "onnxruntime старее 1.24"; return false; }

    CHK(A->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "xvibe", &g_env));
    CHK(A->CreateSessionOptions(&g_so));
    CHK(A->SetIntraOpNumThreads(g_so, opt.threads));
    CHK(A->SetInterOpNumThreads(g_so, 1));
    CHK(A->SetSessionGraphOptimizationLevel(g_so, ORT_ENABLE_ALL));
    CHK(A->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_mem));
    CHK(A->GetAllocatorWithDefaultOptions(&g_alloc));

    std::string s;
    json::Value bundle;
    if(!ReadFile(dir + "/bundle.json", s) || !json::Parse(s, bundle)) { err = "нет bundle.json"; return false; }
    if(!LoadManifest(bundle["flow_lm_state_manifest"], g_flowState) || !LoadManifest(bundle["mimi_state_manifest"], g_mimiState))
    { err = "в bundle.json нет описания состояний"; return false; }

    json::Value voc, sc;
    if(!ReadFile(dir + "/vocab.json", s) || !json::Parse(s, voc)) { err = "нет vocab.json"; return false; }
    if(!ReadFile(dir + "/token_scores.json", s) || !json::Parse(s, sc)) { err = "нет token_scores.json"; return false; }
    g_vocab.clear(); g_score.clear(); g_id2piece.assign(voc.size(), "");
    for(auto& kv : voc.items())
    {
        int id = kv.second.i();
        g_vocab[kv.first] = id;
        if(id >= 0 && id < (int)g_id2piece.size()) g_id2piece[id] = kv.first;
    }
    for(auto& kv : sc.items()) g_score[kv.first] = (float)kv.second.num();
    if(g_vocab.count("<unk>")) g_unk = g_vocab["<unk>"];

    auto open = [&](const char* name, OrtSession** sess) -> bool {
        std::string p = dir + "/" + name;
        CHK(A->CreateSession(g_env, p.c_str(), g_so, sess));
        return true;
    };
    if(!open("mimi_encoder.onnx", &g_enc) || !open("text_conditioner.onnx", &g_tc) || !open("flow_lm_main_int8.onnx", &g_main) ||
       !open("flow_lm_flow_int8.onnx", &g_flow) || !open("mimi_decoder_int8.onnx", &g_dec)) return false;
    g_tcOut = OutName(g_tc, 0); g_flowOut = OutName(g_flow, 0); g_decOut = OutName(g_dec, 0);

    // границы предложений и запасные границы — как в эталоне: Encode(".!...?")[1:], Encode(",;:")[1:]
    auto e = Bpe(".!...?"); g_eosTokens = std::set<int>(e.begin() + (e.empty() ? 0 : 1), e.end());
    auto f = Bpe(",;:");    g_fallbackTokens = std::set<int>(f.begin() + (f.empty() ? 0 : 1), f.end());
    g_loaded = true;
    return true;
}

bool PrepareVoice(const std::string& key, const float* audio, int n, int sr, std::string& err)
{
    if(!g_loaded) { err = "модель не загружена"; return false; }
    if(g_voices.count(key)) { g_voices[key].used = ++g_tick; return true; }
    std::vector<float> a = Resample(audio, n, sr, kSampleRate);
    if(a.size() > (size_t)kSampleRate * 12) a.resize(kSampleRate * 12);   // хватит 12 секунд
    OrtValue* in = nullptr;
    if(!FromData(a.data(), a.size() * 4, { 1, 1, (int64_t)a.size() }, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in, err)) return false;
    std::vector<const char*> inN{ "audio" }, outN{ "latents" };
    std::vector<OrtValue*> res;
    bool ok = Run(g_enc, inN, { in }, outN, res, err);
    A->ReleaseValue(in);
    if(!ok) return false;
    VoiceState vs;
    if(!InitState(g_flowState, vs.state, err)) { A->ReleaseValue(res[0]); return false; }
    OrtValue* empty = nullptr;
    if(!MakeTensor({ 1, 0, kLatentDim }, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &empty, err)) { A->ReleaseValue(res[0]); return false; }
    ok = RunMain(empty, res[0], vs.state, nullptr, nullptr, err);   // BOS уже внутри mimi_encoder
    A->ReleaseValue(empty); A->ReleaseValue(res[0]);
    if(!ok) return false;
    // кэш голосов: выкидываем самый давний
    while((int)g_voices.size() >= g_opt.voiceCache)
    {
        auto old = g_voices.begin();
        for(auto it = g_voices.begin(); it != g_voices.end(); ++it) if(it->second.used < old->second.used) old = it;
        ReleaseAll(old->second.state); g_voices.erase(old);
    }
    vs.used = ++g_tick;
    g_voices[key] = std::move(vs);
    return true;
}

bool LoadVoiceState(const std::string& key, const std::string& path, std::string& err)
{
    if(!g_loaded) { err = "модель не загружена"; return false; }
    if(g_voices.count(key)) { g_voices[key].used = ++g_tick; return true; }
    std::string d;
    if(!ReadFile(path, d) || d.size() < 8 || d.compare(0, 4, "XVST") != 0) { err = "нет файла голоса " + path; return false; }
    uint32_t cnt; memcpy(&cnt, d.data() + 4, 4);
    if(cnt != g_flowState.size()) { err = "файл голоса не от этой модели"; return false; }
    size_t pos = 8;
    VoiceState vs;
    for(uint32_t i = 0; i < cnt; ++i)
    {
        if(pos + 2 > d.size()) { err = "файл голоса обрезан"; ReleaseAll(vs.state); return false; }
        uint8_t code = d[pos], nd = d[pos + 1]; pos += 2;
        std::vector<int64_t> shape(nd);
        memcpy(shape.data(), d.data() + pos, nd * 8); pos += nd * 8;
        size_t n = 1; for(auto x : shape) n *= (size_t)x;
        ONNXTensorElementDataType t = code == 1 ? ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 : code == 2 ? ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
        size_t bytes = n * ElemSize(t);
        if(pos + bytes > d.size()) { err = "файл голоса обрезан"; ReleaseAll(vs.state); return false; }
        OrtValue* v = nullptr;
        if(!FromData(d.data() + pos, bytes, shape, t, &v, err)) { ReleaseAll(vs.state); return false; }
        pos += bytes;
        vs.state.push_back(v);
    }
    while((int)g_voices.size() >= g_opt.voiceCache)
    {
        auto old = g_voices.begin();
        for(auto it = g_voices.begin(); it != g_voices.end(); ++it) if(it->second.used < old->second.used) old = it;
        ReleaseAll(old->second.state); g_voices.erase(old);
    }
    vs.used = ++g_tick;
    g_voices[key] = std::move(vs);
    return true;
}

bool Generate(const std::string& voiceKey, const std::string& rawText, const std::function<bool(const float*, int)>& onAudio, std::string& err)
{
    if(!g_loaded) { err = "модель не загружена"; return false; }
    auto vit = g_voices.find(voiceKey);
    if(vit == g_voices.end()) { err = "голос не подготовлен"; return false; }
    vit->second.used = ++g_tick;
    std::string text = PrepareText(rawText);
    if(text.empty()) return true;

    std::vector<OrtValue*> mstate;
    if(!InitState(g_mimiState, mstate, err)) return false;
    OrtValue *emptySeq = nullptr, *emptyText = nullptr;
    if(!MakeTensor({ 1, 0, kLatentDim }, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &emptySeq, err) ||
       !MakeTensor({ 1, 0, kCondDim }, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &emptyText, err)) return false;

    std::normal_distribution<float> nd(0.0f, sqrtf(g_opt.temperature > 0 ? g_opt.temperature : 1e-6f));
    std::vector<float> pending;                  // латенты, ещё не отданные декодеру
    bool go = true, ok = true;
    float dt = 1.0f / g_opt.lsdSteps;

    auto flush = [&](bool all) -> bool {
        int frames = (int)(pending.size() / kLatentDim);
        while(frames >= g_opt.decodeChunk || (all && frames > 0))
        {
            int take = frames >= g_opt.decodeChunk ? g_opt.decodeChunk : frames;
            OrtValue* lat = nullptr;
            if(!FromData(pending.data(), (size_t)take * kLatentDim * 4, { 1, take, kLatentDim }, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &lat, err)) return false;
            bool r = Decode(lat, mstate, onAudio, go, err);
            A->ReleaseValue(lat);
            if(!r) return false;
            pending.erase(pending.begin(), pending.begin() + (size_t)take * kLatentDim);
            frames -= take;
            if(!go) return true;
        }
        return true;
    };

    for(const std::string& chunk : SplitChunks(text))
    {
        if(!go) break;
        int framesAfterEos = 3;
        std::string prompt = PromptText(chunk, framesAfterEos);
        std::vector<int> ids = Bpe(prompt);
        if(ids.empty()) continue;
        int maxFrames = (int)ceilf((ids.size() / kTokensPerSecond + kGenPadding) * kFrameRate);

        // копия состояния голоса
        std::vector<OrtValue*> st;
        for(auto* v : vit->second.state) { OrtValue* c = nullptr; if(!Clone(v, &c, err)) { ReleaseAll(st); ok = false; break; } st.push_back(c); }
        if(!ok) break;

        // текст
        std::vector<int64_t> id64(ids.begin(), ids.end());
        OrtValue* tid = nullptr; std::vector<OrtValue*> temb;
        ok = FromData(id64.data(), id64.size() * 8, { 1, (int64_t)id64.size() }, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &tid, err) &&
             Run(g_tc, { g_tcIn.c_str() }, { tid }, { g_tcOut.c_str() }, temb, err);
        if(tid) A->ReleaseValue(tid);
        if(ok) { ok = RunMain(emptySeq, temb[0], st, nullptr, nullptr, err); A->ReleaseValue(temb[0]); }
        if(!ok) { ReleaseAll(st); break; }

        std::vector<float> cur(kLatentDim, NAN);
        int eosStep = -1;
        for(int step = 0; step < maxFrames && go; ++step)
        {
            OrtValue* seq = nullptr; OrtValue* cond = nullptr; float eos = -100;
            if(!FromData(cur.data(), kLatentDim * 4, { 1, 1, kLatentDim }, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &seq, err)) { ok = false; break; }
            ok = RunMain(seq, emptyText, st, &cond, &eos, err);
            A->ReleaseValue(seq);
            if(!ok) break;
            if(eos > -4.0f && eosStep < 0) eosStep = step;
            if(eosStep >= 0 && step >= eosStep + framesAfterEos) { A->ReleaseValue(cond); break; }

            std::vector<float> x(kLatentDim);
            for(auto& v : x) v = g_opt.temperature > 0 ? nd(g_rng) : 0.0f;
            for(int j = 0; j < g_opt.lsdSteps && ok; ++j)
            {
                float s = j * dt, t = s + dt;
                OrtValue *xs = nullptr, *ss = nullptr, *ts = nullptr; std::vector<OrtValue*> fo;
                ok = FromData(x.data(), kLatentDim * 4, { 1, kLatentDim }, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &xs, err) &&
                     FromData(&s, 4, { 1, 1 }, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &ss, err) &&
                     FromData(&t, 4, { 1, 1 }, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &ts, err) &&
                     Run(g_flow, { "c", "s", "t", "x" }, { cond, ss, ts, xs }, { g_flowOut.c_str() }, fo, err);
                if(ok) { float* f = nullptr; A->GetTensorMutableData(fo[0], (void**)&f); for(int k = 0; k < kLatentDim; ++k) x[k] += f[k] * dt; A->ReleaseValue(fo[0]); }
                if(xs) A->ReleaseValue(xs); if(ss) A->ReleaseValue(ss); if(ts) A->ReleaseValue(ts);
            }
            A->ReleaseValue(cond);
            if(!ok) break;
            cur = x;
            pending.insert(pending.end(), x.begin(), x.end());
            if(!flush(false)) { ok = false; break; }
        }
        ReleaseAll(st);
        if(!ok) break;
    }
    if(ok && go) ok = flush(true);
    A->ReleaseValue(emptySeq); A->ReleaseValue(emptyText);
    ReleaseAll(mstate);
    return ok;
}

} // namespace xvibe
