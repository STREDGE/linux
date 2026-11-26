// Голосовой чат: AAudio (через dlopen — на Android 7 мод просто остаётся без голоса)
// + Opus. Колбэки AAudio только копируют отсчёты в кольцевые буферы; пересчёт
// частоты, кодирование и декодирование — в игровом потоке (Pump).

#include "voice.h"
#include "trace/ctrace.h"

#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <vector>

#include <mod/flogger.h>
#include <opus.h>

namespace voice
{
namespace
{
    // ---------------- AAudio (минимум, что нужно) ----------------
    typedef struct AAudioStreamStruct        AAudioStream;
    typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
    typedef int32_t (*DataCb)(AAudioStream*, void*, void*, int32_t);
    typedef void    (*ErrorCb)(AAudioStream*, void*, int32_t);

    enum : int32_t
    {
        AAUDIO_OK = 0,
        AAUDIO_DIRECTION_OUTPUT = 0, AAUDIO_DIRECTION_INPUT = 1,
        AAUDIO_FORMAT_PCM_I16 = 1,
        AAUDIO_SHARING_MODE_SHARED = 1,
        AAUDIO_PERFORMANCE_MODE_NONE = 10, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12,
        AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION = 7,
        AAUDIO_CALLBACK_RESULT_CONTINUE = 0,
    };

    struct AAudioApi
    {
        int32_t (*createStreamBuilder)(AAudioStreamBuilder**);
        void    (*setDirection)(AAudioStreamBuilder*, int32_t);
        void    (*setSampleRate)(AAudioStreamBuilder*, int32_t);
        void    (*setChannelCount)(AAudioStreamBuilder*, int32_t);
        void    (*setFormat)(AAudioStreamBuilder*, int32_t);
        void    (*setSharingMode)(AAudioStreamBuilder*, int32_t);
        void    (*setPerformanceMode)(AAudioStreamBuilder*, int32_t);
        void    (*setDataCallback)(AAudioStreamBuilder*, DataCb, void*);
        void    (*setErrorCallback)(AAudioStreamBuilder*, ErrorCb, void*);
        void    (*setInputPreset)(AAudioStreamBuilder*, int32_t);       // Android 9+, может не быть
        int32_t (*openStream)(AAudioStreamBuilder*, AAudioStream**);
        int32_t (*builderDelete)(AAudioStreamBuilder*);
        int32_t (*requestStart)(AAudioStream*);
        int32_t (*requestStop)(AAudioStream*);
        int32_t (*close)(AAudioStream*);
        int32_t (*getSampleRate)(AAudioStream*);
        int32_t (*getChannelCount)(AAudioStream*);
        const char* (*resultText)(int32_t);
    };
    AAudioApi A{};
    bool      g_hasAAudio = false;

    bool LoadAAudio()
    {
        void* h = dlopen("libaaudio.so", RTLD_NOW);
        if(!h) return false;
        #define SYM(field, name) *(void**)&A.field = dlsym(h, name)
        SYM(createStreamBuilder, "AAudio_createStreamBuilder");
        SYM(setDirection,        "AAudioStreamBuilder_setDirection");
        SYM(setSampleRate,       "AAudioStreamBuilder_setSampleRate");
        SYM(setChannelCount,     "AAudioStreamBuilder_setChannelCount");
        SYM(setFormat,           "AAudioStreamBuilder_setFormat");
        SYM(setSharingMode,      "AAudioStreamBuilder_setSharingMode");
        SYM(setPerformanceMode,  "AAudioStreamBuilder_setPerformanceMode");
        SYM(setDataCallback,     "AAudioStreamBuilder_setDataCallback");
        SYM(setErrorCallback,    "AAudioStreamBuilder_setErrorCallback");
        SYM(setInputPreset,      "AAudioStreamBuilder_setInputPreset");
        SYM(openStream,          "AAudioStreamBuilder_openStream");
        SYM(builderDelete,       "AAudioStreamBuilder_delete");
        SYM(requestStart,        "AAudioStream_requestStart");
        SYM(requestStop,         "AAudioStream_requestStop");
        SYM(close,               "AAudioStream_close");
        SYM(getSampleRate,       "AAudioStream_getSampleRate");
        SYM(getChannelCount,     "AAudioStream_getChannelCount");
        SYM(resultText,          "AAudio_convertResultToText");
        #undef SYM
        return A.createStreamBuilder && A.setDirection && A.setSampleRate && A.setChannelCount && A.setFormat &&
               A.setSharingMode && A.setPerformanceMode && A.setDataCallback && A.setErrorCallback &&
               A.openStream && A.builderDelete && A.requestStart && A.requestStop && A.close &&
               A.getSampleRate && A.getChannelCount;
    }
    const char* ResText(int32_t r) { return A.resultText ? A.resultText(r) : "?"; }

    // ---------------- кольцевой буфер: один пишет, один читает ----------------
    struct Ring
    {
        static constexpr uint32_t kSize = 1u << 15;     // 32768 отсчётов (~0,7 с при 48 кГц)
        int16_t data[kSize];
        std::atomic<uint32_t> w{0}, r{0};
        uint32_t Fill() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire); }
        void Clear() { r.store(w.load(std::memory_order_acquire), std::memory_order_release); }
        uint32_t Write(const int16_t* src, uint32_t n)
        {
            uint32_t wi = w.load(std::memory_order_relaxed), ri = r.load(std::memory_order_acquire);
            uint32_t space = kSize - (wi - ri);
            if(n > space) n = space;
            for(uint32_t i = 0; i < n; ++i) data[(wi + i) & (kSize - 1)] = src[i];
            w.store(wi + n, std::memory_order_release);
            return n;
        }
        uint32_t Read(int16_t* dst, uint32_t n)
        {
            uint32_t ri = r.load(std::memory_order_relaxed), wi = w.load(std::memory_order_acquire);
            uint32_t avail = wi - ri;
            if(n > avail) n = avail;
            for(uint32_t i = 0; i < n; ++i) dst[i] = data[(ri + i) & (kSize - 1)];
            r.store(ri + n, std::memory_order_release);
            return n;
        }
    };

    // ---------------- линейный пересчёт частоты ----------------
    struct Resampler
    {
        double step = 1.0;   // inRate / outRate
        double frac = 0.0;
        int16_t prev = 0;
        void Set(int inRate, int outRate) { step = (double)inRate / outRate; frac = 0; prev = 0; }
        void Process(const int16_t* in, int n, std::vector<int16_t>& out)
        {
            if(step == 1.0) { out.insert(out.end(), in, in + n); return; }
            for(int i = 0; i < n; ++i)
            {
                int16_t x = in[i];
                while(frac < 1.0) { out.push_back((int16_t)(prev + (x - prev) * frac)); frac += step; }
                frac -= 1.0; prev = x;
            }
        }
    };

    uint64_t NowUs()
    {
        timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
    }
    // Сколько прошло от since до now (0, если since «в будущем» — без переполнения)
    uint64_t Since(uint64_t now, uint64_t since) { return now > since ? now - since : 0; }

    FLogger* g_log = nullptr;
    void Log(bool err, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool err, const char* fmt, ...)
    {
        if(!g_log) return;
        char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        if(err) g_log->Error("[ГОЛОС] %s", buf); else g_log->Info("[ГОЛОС] %s", buf);
        ctrace::Line("ГОЛОС", err ? "ОШИБКА: %s" : "%s", buf);
    }

    // ---------------- потоки AAudio ----------------
    struct Stream
    {
        AAudioStream* s = nullptr;
        int rate = 0, channels = 1;
        bool started = false;
        std::atomic<bool> broken{false};
    };
    Stream g_in, g_out;
    Ring   g_inRing, g_outRing;    // отсчёты на частоте телефона, моно

    int32_t InCallback(AAudioStream*, void*, void* audio, int32_t frames)
    {
        const int16_t* src = (const int16_t*)audio;
        if(g_in.channels == 1) g_inRing.Write(src, (uint32_t)frames);
        else
        {
            int16_t tmp[512];
            for(int32_t done = 0; done < frames; )
            {
                int32_t n = frames - done; if(n > 512) n = 512;
                for(int32_t i = 0; i < n; ++i)
                {
                    int sum = 0; for(int c = 0; c < g_in.channels; ++c) sum += src[(done + i) * g_in.channels + c];
                    tmp[i] = (int16_t)(sum / g_in.channels);
                }
                g_inRing.Write(tmp, (uint32_t)n); done += n;
            }
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    int32_t OutCallback(AAudioStream*, void*, void* audio, int32_t frames)
    {
        int16_t* dst = (int16_t*)audio;
        int ch = g_out.channels;
        int16_t tmp[512];
        for(int32_t done = 0; done < frames; )
        {
            int32_t n = frames - done; if(n > 512) n = 512;
            uint32_t got = g_outRing.Read(tmp, (uint32_t)n);
            for(int32_t i = (int32_t)got; i < n; ++i) tmp[i] = 0;   // пусто — тишина
            for(int32_t i = 0; i < n; ++i) for(int c = 0; c < ch; ++c) dst[(done + i) * ch + c] = tmp[i];
            done += n;
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void ErrCallback(AAudioStream*, void* user, int32_t) { ((Stream*)user)->broken = true; }

    void CloseStream(Stream& st)
    {
        if(!st.s) return;
        if(st.started) A.requestStop(st.s);
        A.close(st.s);
        st.s = nullptr; st.started = false; st.broken = false;
    }

    bool OpenStream(Stream& st, bool input)
    {
        AAudioStreamBuilder* b = nullptr;
        int32_t r = A.createStreamBuilder(&b);
        if(r != AAUDIO_OK || !b) { Log(true, "не создать AAudio: %s", ResText(r)); return false; }
        A.setDirection(b, input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
        A.setSampleRate(b, kRate);
        A.setChannelCount(b, 1);
        A.setFormat(b, AAUDIO_FORMAT_PCM_I16);
        A.setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        A.setPerformanceMode(b, input ? AAUDIO_PERFORMANCE_MODE_NONE : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        // Режим «связь»: телефон сам убирает эхо динамика и шум
        if(input && A.setInputPreset) A.setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
        A.setDataCallback(b, input ? InCallback : OutCallback, nullptr);
        A.setErrorCallback(b, ErrCallback, &st);
        r = A.openStream(b, &st.s);
        A.builderDelete(b);
        if(r != AAUDIO_OK || !st.s)
        {
            st.s = nullptr;
            Log(true, "%s не открылся: %s%s", input ? "микрофон" : "динамик", ResText(r),
                input ? " (нет разрешения на микрофон?)" : "");
            return false;
        }
        st.rate = A.getSampleRate(st.s);
        st.channels = A.getChannelCount(st.s);
        if(st.channels < 1) st.channels = 1;
        st.broken = false; st.started = false;
        Log(false, "%s открыт: %d Гц, каналов %d", input ? "микрофон" : "динамик", st.rate, st.channels);
        return true;
    }

    bool StartStream(Stream& st)
    {
        if(!st.s) return false;
        if(st.started) return true;
        int32_t r = A.requestStart(st.s);
        if(r != AAUDIO_OK) { Log(true, "старт потока: %s", ResText(r)); return false; }
        st.started = true;
        return true;
    }

    void StopStream(Stream& st)
    {
        if(st.s && st.started) { A.requestStop(st.s); st.started = false; }
    }

    // ---------------- кодек ----------------
    OpusEncoder* g_enc = nullptr;
    OpusDecoder* g_dec = nullptr;

    // ---------------- передача ----------------
    constexpr uint64_t kHangoverUs   = 250000;   // после отпускания кнопки дописываем хвост слова
    constexpr uint64_t kOpenRetryUs  = 3000000;
    bool      g_transmitWanted = false;
    bool      g_transmitting = false;
    uint64_t  g_releaseUs = 0;
    uint64_t  g_nextOpenTryUs = 0;
    uint8_t   g_spurt = 0;
    uint16_t  g_txSeq = 0;
    Resampler g_inRs;
    std::vector<int16_t> g_txPcm;      // 48 кГц, ждут кодирования
    struct Encoded { uint16_t seq; uint8_t spurt; uint8_t len; uint8_t data[kMaxPacket]; uint64_t t; };
    std::vector<Encoded> g_txQueue;    // готовые кадры для сети
    float     g_inLevel = 0;

    // ---------------- эхо-проверка ----------------
    bool      g_loopback = false;
    uint64_t  g_loopDelayUs = 500000;
    std::vector<Encoded> g_loopQueue;

    // ---------------- приём (буфер против рывков сети) ----------------
    constexpr int      kSlots        = 32;
    constexpr int      kPrebuffer    = 3;        // кадров до начала воспроизведения (60 мс)
    constexpr int      kTargetMs     = 100;      // держим в динамике ~100 мс звука
    constexpr int      kMaxBuffered  = 12;       // больше — догоняем, выбрасывая старое
    constexpr uint64_t kSpurtEndUs   = 400000;   // тишина дольше — фраза закончилась
    constexpr uint64_t kOutIdleUs    = 10000000; // динамик без звука 10 с — закрываем
    struct Slot { bool valid; uint16_t seq; uint8_t len; uint8_t data[kMaxPacket]; };
    Slot      g_slots[kSlots];
    bool      g_rxActive = false;
    bool      g_rxPrebuffer = true;
    uint8_t   g_rxSpurt = 0;
    uint16_t  g_rxNext = 0;
    uint64_t  g_rxLastUs = 0;
    uint64_t  g_rxFirstUs = 0;
    uint64_t  g_outLastVoiceUs = 0;
    Resampler g_outRs;
    float     g_outLevel = 0;
    uint32_t  g_statFrames = 0, g_statLost = 0, g_statDropped = 0;

    float FrameLevel(const int16_t* p, int n)
    {
        double s = 0; for(int i = 0; i < n; ++i) s += (double)p[i] * p[i];
        float rms = sqrtf((float)(s / (n > 0 ? n : 1))) / 32768.0f;
        float db = 20 * log10f(rms + 1e-6f);               // -60..0 дБ -> 0..1
        float v = (db + 55) / 45; return v < 0 ? 0 : v > 1 ? 1 : v;
    }

    int Buffered()
    {
        int n = 0; for(auto& s : g_slots) if(s.valid) ++n; return n;
    }

    bool LaterBuffered()
    {
        for(auto& s : g_slots) if(s.valid && (int16_t)(s.seq - g_rxNext) > 0) return true;
        return false;
    }

    void ResetRx()
    {
        for(auto& s : g_slots) s.valid = false;
        g_rxActive = false; g_rxPrebuffer = true;
    }

    // Декодированный кадр (48 кГц) — в динамик
    void QueueOut(const int16_t* pcm, int n)
    {
        static std::vector<int16_t> tmp; tmp.clear();
        g_outRs.Process(pcm, n, tmp);
        uint32_t w = g_outRing.Write(tmp.data(), (uint32_t)tmp.size());
        if(w < tmp.size()) ++g_statDropped;
    }

    void EnsureOutput(uint64_t now)
    {
        if(g_out.broken) { Log(true, "динамик отвалился (наушники?) — переоткрываю"); CloseStream(g_out); }
        if(!g_out.s)
        {
            if(now < g_nextOpenTryUs) return;
            if(!OpenStream(g_out, false)) { g_nextOpenTryUs = now + kOpenRetryUs; return; }
            g_outRs.Set(kRate, g_out.rate);
        }
        if(!g_out.started) { g_outRing.Clear(); StartStream(g_out); }
    }

    void PumpPlayback(uint64_t now)
    {
        if(g_rxActive) EnsureOutput(now);
        if(!g_out.s || !g_out.started)
        {
            g_outLevel *= 0.85f;
            return;
        }

        uint32_t target = (uint32_t)(g_out.rate * kTargetMs / 1000);
        int16_t pcm[kFrameSamples];
        while(g_rxActive && g_outRing.Fill() < target)
        {
            if(g_rxPrebuffer)
            {
                if(Buffered() < kPrebuffer && Since(now, g_rxFirstUs) < 120000) break;
                g_rxPrebuffer = false;
            }
            // Слишком отстали (сеть притормозила, потом всё пришло разом) — догоняем
            while(Buffered() > kMaxBuffered)
            {
                Slot& s = g_slots[g_rxNext % kSlots];
                if(s.valid && s.seq == g_rxNext) { s.valid = false; ++g_statDropped; }
                ++g_rxNext;
            }

            Slot& s = g_slots[g_rxNext % kSlots];
            int n = 0;
            if(s.valid && s.seq == g_rxNext)
            {
                n = opus_decode(g_dec, s.data, s.len, pcm, kFrameSamples, 0);
                s.valid = false;
            }
            else if(LaterBuffered())
            {
                // Кадр потерялся: пробуем восстановить из следующего (FEC), иначе — «додумать» (PLC)
                Slot& nx = g_slots[(uint16_t)(g_rxNext + 1) % kSlots];
                if(nx.valid && nx.seq == (uint16_t)(g_rxNext + 1)) n = opus_decode(g_dec, nx.data, nx.len, pcm, kFrameSamples, 1);
                else                                              n = opus_decode(g_dec, nullptr, 0, pcm, kFrameSamples, 0);
                ++g_statLost;
            }
            else
            {
                if(Since(now, g_rxLastUs) > kSpurtEndUs) g_rxActive = false;   // фраза закончилась
                break;
            }
            ++g_rxNext;
            if(n > 0)
            {
                ++g_statFrames;
                QueueOut(pcm, n);
                float lv = FrameLevel(pcm, n);
                g_outLevel = lv > g_outLevel ? lv : g_outLevel * 0.8f + lv * 0.2f;
                g_outLastVoiceUs = now;
            }
        }
        if(!g_rxActive) g_outLevel *= 0.85f;

        // Долго тихо — освобождаем динамик
        if(!g_rxActive && Since(now, g_outLastVoiceUs) > kOutIdleUs && g_outRing.Fill() == 0)
        {
            StopStream(g_out);
            if(g_statFrames)
                Log(false, "друг замолчал: кадров %u, потеряно %u, выброшено %u", g_statFrames, g_statLost, g_statDropped);
            g_statFrames = g_statLost = g_statDropped = 0;
        }
    }

    void Encode(uint64_t now)
    {
        while((int)g_txPcm.size() >= kFrameSamples)
        {
            float lv = FrameLevel(g_txPcm.data(), kFrameSamples);
            g_inLevel = lv > g_inLevel ? lv : g_inLevel * 0.8f + lv * 0.2f;
            Encoded e{};
            int n = opus_encode(g_enc, g_txPcm.data(), kFrameSamples, e.data, kMaxPacket);
            g_txPcm.erase(g_txPcm.begin(), g_txPcm.begin() + kFrameSamples);
            if(n <= 0) { Log(true, "Opus: ошибка кодирования %d", n); continue; }
            e.seq = g_txSeq++; e.spurt = g_spurt; e.len = (uint8_t)n; e.t = now;
            if(g_loopback) g_loopQueue.push_back(e);
            else if(g_txQueue.size() < 50) g_txQueue.push_back(e);
        }
    }

    void PumpCapture(uint64_t now)
    {
        bool want = g_transmitWanted || (g_transmitting && Since(now, g_releaseUs) < kHangoverUs);
        if(g_in.broken) { Log(true, "микрофон отвалился — переоткрываю"); CloseStream(g_in); }

        if(want && !g_transmitting)
        {
            if(!g_in.s)
            {
                if(now < g_nextOpenTryUs) return;
                if(!OpenStream(g_in, true)) { g_nextOpenTryUs = now + kOpenRetryUs; return; }
                g_inRs.Set(g_in.rate, kRate);
            }
            g_inRing.Clear();
            if(!StartStream(g_in)) return;
            g_transmitting = true;
            ++g_spurt;
            g_txPcm.clear();
            opus_encoder_ctl(g_enc, OPUS_RESET_STATE);
        }
        else if(!want && g_transmitting)
        {
            StopStream(g_in);
            g_transmitting = false;
            g_txPcm.clear();
        }

        if(!g_transmitting) { g_inLevel *= 0.85f; return; }

        int16_t buf[1024];
        static std::vector<int16_t> rs;
        for(;;)
        {
            uint32_t n = g_inRing.Read(buf, 1024);
            if(!n) break;
            rs.clear(); g_inRs.Process(buf, (int)n, rs);
            g_txPcm.insert(g_txPcm.end(), rs.begin(), rs.end());
        }
        Encode(now);
    }
}

// ======================= наружу =======================

bool Init(FLogger* log)
{
    g_log = log;
    g_hasAAudio = LoadAAudio();
    if(!g_hasAAudio) { Log(true, "AAudio нет (нужен Android 8+) — голосовой чат выключен"); return false; }

    int err = 0;
    g_enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    if(err != OPUS_OK || !g_enc) { Log(true, "Opus: кодер не создался (%d)", err); g_enc = nullptr; return false; }
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(g_enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(g_enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(g_enc, OPUS_SET_PACKET_LOSS_PERC(5));
    g_dec = opus_decoder_create(kRate, 1, &err);
    if(err != OPUS_OK || !g_dec) { Log(true, "Opus: декодер не создался (%d)", err); g_dec = nullptr; return false; }
    g_txPcm.reserve(kFrameSamples * 4);
    Log(false, "готов: AAudio + Opus %s, 48 кГц, 24 кбит/с", opus_get_version_string());
    return true;
}

bool Available() { return g_hasAAudio && g_enc && g_dec; }

void SetTransmit(bool on)
{
    if(!Available()) return;
    if(on == g_transmitWanted) return;
    g_transmitWanted = on;
    if(!on) g_releaseUs = NowUs();
}

bool Transmitting() { return g_transmitting; }

int PopEncodedFrame(uint8_t* out, int maxLen, uint16_t* seq, uint8_t* spurt)
{
    if(g_txQueue.empty()) return 0;
    const Encoded& e = g_txQueue.front();
    int n = e.len <= maxLen ? e.len : 0;
    if(n) { memcpy(out, e.data, n); *seq = e.seq; *spurt = e.spurt; }
    g_txQueue.erase(g_txQueue.begin());
    return n;
}

void PushReceivedFrame(uint16_t seq, uint8_t spurt, const uint8_t* data, int len)
{
    if(!Available() || len <= 0 || len > kMaxPacket) return;
    uint64_t now = NowUs();
    if(!g_rxActive || spurt != g_rxSpurt)
    {
        // Новая фраза: начинаем с этого кадра
        ResetRx();
        g_rxActive = true; g_rxSpurt = spurt; g_rxNext = seq; g_rxFirstUs = now;
        opus_decoder_ctl(g_dec, OPUS_RESET_STATE);
    }
    int16_t ahead = (int16_t)(seq - g_rxNext);
    if(ahead < 0) return;                      // опоздал — уже проиграли
    if(ahead >= kSlots) { ++g_statDropped; return; }
    Slot& s = g_slots[seq % kSlots];
    s.valid = true; s.seq = seq; s.len = (uint8_t)len; memcpy(s.data, data, len);
    g_rxLastUs = now;
}

void SetLoopback(bool on, int delayMs)
{
    g_loopback = on;
    g_loopDelayUs = (uint64_t)delayMs * 1000;
    if(!on) g_loopQueue.clear();
}

void Pump()
{
    if(!Available()) return;
    uint64_t now = NowUs();
    PumpCapture(now);
    if(g_loopback)
    {
        size_t i = 0;
        for(; i < g_loopQueue.size() && Since(now, g_loopQueue[i].t) >= g_loopDelayUs; ++i)
            PushReceivedFrame(g_loopQueue[i].seq, g_loopQueue[i].spurt, g_loopQueue[i].data, g_loopQueue[i].len);
        if(i) g_loopQueue.erase(g_loopQueue.begin(), g_loopQueue.begin() + i);
    }
    PumpPlayback(NowUs());
}

void Stop()
{
    if(!g_hasAAudio) return;
    CloseStream(g_in);
    CloseStream(g_out);
    g_transmitWanted = g_transmitting = false;
    g_txPcm.clear(); g_txQueue.clear(); g_loopQueue.clear();
    ResetRx();
    g_inRing.Clear(); g_outRing.Clear();
    g_inLevel = g_outLevel = 0;
}

float InputLevel()  { return g_inLevel; }
float OutputLevel() { return g_outLevel; }
bool  Playing()     { return g_rxActive && !g_rxPrebuffer; }

} // namespace voice
