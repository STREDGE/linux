#include "bridge.h"
#include "bridge_dex.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <jni.h>
#include <dlfcn.h>

#include <atomic>
#include <deque>
#include <mutex>

namespace bridge
{
namespace
{
    jclass g_cls = nullptr;     // net.saliving.Bridge (глобальная ссылка)
    bool   g_ready = false;
    std::atomic<int>  g_nextHttp{1};
    std::atomic<bool> g_ttsReady{false};

    std::mutex g_mx;
    std::deque<HttpEvent> g_http;
    std::deque<SttEvent>  g_stt;
    std::deque<TtsEvent>  g_tts;
    std::deque<DlEvent>   g_dl;
    std::deque<std::vector<int16_t>> g_mic;

    struct Env
    {
        JavaVM* vm = nullptr; JNIEnv* env = nullptr; bool attached = false;
        Env()
        {
            vm = aml->GetJavaVM();
            if(!vm) return;
            if(vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
            {
                if(vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true; else env = nullptr;
            }
        }
        ~Env() { if(attached) vm->DetachCurrentThread(); }
        bool Fail() { if(env && env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return true; } return false; }
    };

    // UTF-8 <-> UTF-16 (NewStringUTF не понимает 4-байтовые символы)
    jstring ToJ(JNIEnv* env, const std::string& s)
    {
        std::u16string u; u.reserve(s.size());
        for(size_t i = 0; i < s.size();)
        {
            unsigned char c = s[i];
            uint32_t cp; int n;
            if(c < 0x80) { cp = c; n = 1; }
            else if((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
            else if((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
            else if((c >> 3) == 30) { cp = c & 0x07; n = 4; }
            else { ++i; continue; }
            if(i + n > s.size()) break;
            for(int k = 1; k < n; ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
            i += n;
            if(cp >= 0x10000) { cp -= 0x10000; u.push_back((char16_t)(0xD800 + (cp >> 10))); u.push_back((char16_t)(0xDC00 + (cp & 0x3FF))); }
            else u.push_back((char16_t)cp);
        }
        return env->NewString((const jchar*)u.data(), (jsize)u.size());
    }

    std::string FromJ(JNIEnv* env, jstring js)
    {
        std::string out;
        if(!js) return out;
        const jchar* c = env->GetStringChars(js, nullptr);
        jsize n = env->GetStringLength(js);
        for(jsize i = 0; i < n; ++i)
        {
            uint32_t cp = c[i];
            if(cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) { cp = 0x10000 + ((cp - 0xD800) << 10) + (c[i + 1] - 0xDC00); ++i; }
            if(cp < 0x80) out += (char)cp;
            else if(cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
            else if(cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        }
        env->ReleaseStringChars(js, c);
        return out;
    }

    // ---- native-методы, которые вызывает Java ----
    void JNICALL N_HttpLine(JNIEnv* env, jclass, jint id, jstring line)
    {
        HttpEvent e{ id, false, 0, FromJ(env, line) };
        std::lock_guard<std::mutex> l(g_mx); g_http.push_back(std::move(e));
    }
    void JNICALL N_HttpDone(JNIEnv* env, jclass, jint id, jint status, jstring err)
    {
        HttpEvent e{ id, true, status, FromJ(env, err) };
        std::lock_guard<std::mutex> l(g_mx); g_http.push_back(std::move(e));
    }
    void JNICALL N_Stt(JNIEnv* env, jclass, jint kind, jstring text)
    {
        SttEvent e{ kind, FromJ(env, text) };
        std::lock_guard<std::mutex> l(g_mx); g_stt.push_back(std::move(e));
    }
    void JNICALL N_Tts(JNIEnv* env, jclass, jstring id, jint state)
    {
        TtsEvent e{ FromJ(env, id), state };
        std::lock_guard<std::mutex> l(g_mx); g_tts.push_back(std::move(e));
    }
    void JNICALL N_TtsReady(JNIEnv*, jclass, jint ok) { g_ttsReady = ok != 0; }
    void JNICALL N_Dl(JNIEnv* env, jclass, jint state, jlong done, jlong total, jstring msg)
    {
        DlEvent e{ state, (long long)done, (long long)total, FromJ(env, msg) };
        std::lock_guard<std::mutex> l(g_mx); g_dl.push_back(std::move(e));
    }
    void JNICALL N_Mic(JNIEnv* env, jclass, jshortArray arr, jint n)
    {
        std::vector<int16_t> v(n > 0 ? n : 0);
        if(n > 0) env->GetShortArrayRegion(arr, 0, n, (jshort*)v.data());
        std::lock_guard<std::mutex> l(g_mx); g_mic.push_back(std::move(v));
    }

    jmethodID M(JNIEnv* env, const char* name, const char* sig) { return env->GetStaticMethodID(g_cls, name, sig); }
}

bool Init()
{
    if(g_ready) return true;
    Env j; JNIEnv* env = j.env;
    if(!env) { logger->Error("Мост: нет JNIEnv"); return false; }
    jobject ctx = aml->GetAppContextObject();
    if(!ctx) { logger->Error("Мост: нет контекста приложения"); return false; }

    // ByteBuffer с dex → InMemoryDexClassLoader (Android 8+)
    jobject buf = env->NewDirectByteBuffer((void*)kBridgeDex, (jlong)kBridgeDexSize);
    jclass ctxCls = env->GetObjectClass(ctx);
    jmethodID getCl = env->GetMethodID(ctxCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject parent = env->CallObjectMethod(ctx, getCl);
    jclass imcl = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if(j.Fail() || !imcl) { logger->Error("Мост: нет InMemoryDexClassLoader (Android < 8)"); return false; }
    jmethodID ctor = env->GetMethodID(imcl, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jobject loader = env->NewObject(imcl, ctor, buf, parent);
    if(j.Fail() || !loader) { logger->Error("Мост: dex не загрузился"); return false; }
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = env->GetMethodID(clCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("net.saliving.Bridge");
    jclass cls = (jclass)env->CallObjectMethod(loader, loadClass, name);
    if(j.Fail() || !cls) { logger->Error("Мост: класс Bridge не найден"); return false; }
    g_cls = (jclass)env->NewGlobalRef(cls);
    env->NewGlobalRef(loader); // держим загрузчик живым

    JNINativeMethod nm[] = {
        { (char*)"nativeHttpLine", (char*)"(ILjava/lang/String;)V", (void*)N_HttpLine },
        { (char*)"nativeHttpDone", (char*)"(IILjava/lang/String;)V", (void*)N_HttpDone },
        { (char*)"nativeStt",      (char*)"(ILjava/lang/String;)V", (void*)N_Stt },
        { (char*)"nativeTts",      (char*)"(Ljava/lang/String;I)V", (void*)N_Tts },
        { (char*)"nativeTtsReady", (char*)"(I)V",                   (void*)N_TtsReady },
        { (char*)"nativeDl",       (char*)"(IJJLjava/lang/String;)V", (void*)N_Dl },
        { (char*)"nativeMic",      (char*)"([SI)V",                 (void*)N_Mic },
    };
    if(env->RegisterNatives(g_cls, nm, sizeof(nm) / sizeof(nm[0])) != 0 || j.Fail())
    { logger->Error("Мост: RegisterNatives не удался"); return false; }

    env->CallStaticVoidMethod(g_cls, M(env, "init", "(Landroid/content/Context;)V"), ctx);
    if(j.Fail()) { logger->Error("Мост: Bridge.init упал"); return false; }
    g_ready = true;
    logger->Info("Мост к Java готов (dex %u байт)", (unsigned)kBridgeDexSize);
    return true;
}

bool Ready() { return g_ready; }

int HttpPost(const char* url, const std::vector<std::string>& headers, const std::string& body, int timeoutMs)
{
    if(!g_ready) return 0;
    Env j; JNIEnv* env = j.env; if(!env) return 0;
    int id = g_nextHttp++;
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)headers.size(), strCls, nullptr);
    for(size_t i = 0; i < headers.size(); ++i)
    {
        jstring s = ToJ(env, headers[i]); env->SetObjectArrayElement(arr, (jsize)i, s); env->DeleteLocalRef(s);
    }
    jstring ju = ToJ(env, url), jb = ToJ(env, body);
    env->CallStaticVoidMethod(g_cls, M(env, "httpPost", "(ILjava/lang/String;[Ljava/lang/String;Ljava/lang/String;I)V"),
                              (jint)id, ju, arr, jb, (jint)timeoutMs);
    env->DeleteLocalRef(ju); env->DeleteLocalRef(jb); env->DeleteLocalRef(arr); env->DeleteLocalRef(strCls);
    if(j.Fail()) return 0;
    return id;
}

bool PollHttp(HttpEvent& out)
{
    std::lock_guard<std::mutex> l(g_mx);
    if(g_http.empty()) return false;
    out = std::move(g_http.front()); g_http.pop_front(); return true;
}

bool SttAvailable()
{
    if(!g_ready) return false;
    Env j; if(!j.env) return false;
    jboolean r = j.env->CallStaticBooleanMethod(g_cls, M(j.env, "sttAvailable", "()Z"));
    return !j.Fail() && r;
}

void SttStart(const char* lang)
{
    if(!g_ready) return;
    Env j; if(!j.env) return;
    jstring l = ToJ(j.env, lang);
    j.env->CallStaticVoidMethod(g_cls, M(j.env, "sttStart", "(Ljava/lang/String;)V"), l);
    j.env->DeleteLocalRef(l); j.Fail();
}

static void CallVoid(const char* name)
{
    if(!g_ready) return;
    Env j; if(!j.env) return;
    j.env->CallStaticVoidMethod(g_cls, M(j.env, name, "()V")); j.Fail();
}

void SttStop()   { CallVoid("sttStop"); }
void SttCancel() { CallVoid("sttCancel"); }
void TtsStop()   { CallVoid("ttsStop"); }

bool PollStt(SttEvent& out)
{
    std::lock_guard<std::mutex> l(g_mx);
    if(g_stt.empty()) return false;
    out = std::move(g_stt.front()); g_stt.pop_front(); return true;
}

void TtsInit(const char* lang)
{
    if(!g_ready) return;
    Env j; if(!j.env) return;
    jstring l = ToJ(j.env, lang);
    j.env->CallStaticVoidMethod(g_cls, M(j.env, "ttsInit", "(Ljava/lang/String;)V"), l);
    j.env->DeleteLocalRef(l); j.Fail();
}

bool TtsReady() { return g_ttsReady; }

void TtsSpeak(const std::string& text, float pitch, float rate, const std::string& id, bool queue)
{
    if(!g_ready) return;
    Env j; if(!j.env) return;
    jstring t = ToJ(j.env, text), i = ToJ(j.env, id);
    j.env->CallStaticVoidMethod(g_cls, M(j.env, "ttsSpeak", "(Ljava/lang/String;FFLjava/lang/String;Z)V"),
                                t, (jfloat)pitch, (jfloat)rate, i, (jboolean)queue);
    j.env->DeleteLocalRef(t); j.env->DeleteLocalRef(i); j.Fail();
}

bool PollTts(TtsEvent& out)
{
    std::lock_guard<std::mutex> l(g_mx);
    if(g_tts.empty()) return false;
    out = std::move(g_tts.front()); g_tts.pop_front(); return true;
}

void PushTtsEvent(const std::string& id, int state)
{
    std::lock_guard<std::mutex> l(g_mx); g_tts.push_back(TtsEvent{ id, state });
}

void DownloadPack(const std::string& plan)
{
    if(!g_ready) return;
    Env j; if(!j.env) return;
    jstring p = ToJ(j.env, plan);
    j.env->CallStaticVoidMethod(g_cls, M(j.env, "downloadPack", "(Ljava/lang/String;)V"), p);
    j.env->DeleteLocalRef(p); j.Fail();
}

void CancelDownload() { CallVoid("cancelDownload"); }

bool PollDl(DlEvent& out)
{
    std::lock_guard<std::mutex> l(g_mx);
    if(g_dl.empty()) return false;
    out = std::move(g_dl.front()); g_dl.pop_front(); return true;
}

bool MicStart()
{
    if(!g_ready) return false;
    Env j; if(!j.env) return false;
    jboolean r = j.env->CallStaticBooleanMethod(g_cls, M(j.env, "micStart", "()Z"));
    return !j.Fail() && r;
}

void MicStop() { CallVoid("micStop"); }

bool PollMic(std::vector<int16_t>& out)
{
    std::lock_guard<std::mutex> l(g_mx);
    if(g_mic.empty()) return false;
    out = std::move(g_mic.front()); g_mic.pop_front(); return true;
}

void PlayPcm(const float* samples, int n, int sampleRate, float pitch, float speed, const std::string& id)
{
    if(!g_ready || n <= 0) return;
    Env j; if(!j.env) return;
    jfloatArray arr = j.env->NewFloatArray(n);
    if(!arr) { j.Fail(); return; }
    j.env->SetFloatArrayRegion(arr, 0, n, samples);
    jstring jid = ToJ(j.env, id);
    j.env->CallStaticVoidMethod(g_cls, M(j.env, "playPcm", "([FIFFLjava/lang/String;)V"), arr, (jint)sampleRate, (jfloat)pitch, (jfloat)speed, jid);
    j.env->DeleteLocalRef(arr); j.env->DeleteLocalRef(jid); j.Fail();
}

void PlayStop() { CallVoid("playStop"); }

// ---- выбор файла, ссылка ----
static jobject CurrentActivity()
{
    typedef jobject (*GetActivityFn)(void* self);
    static GetActivityFn getActivity = nullptr;
    if(!getActivity)
    {
        void* h = dlopen("libAML.so", RTLD_NOW | RTLD_NOLOAD);
        if(h) getActivity = (GetActivityFn)dlsym(h, "_ZN3AML18GetCurrentActivityEv");
        if(!getActivity) getActivity = (GetActivityFn)dlsym(RTLD_DEFAULT, "_ZN3AML18GetCurrentActivityEv");
    }
    return getActivity ? getActivity(aml) : nullptr;
}

bool PickFile(const std::string& destPath)
{
    if(!g_ready) return false;
    Env j; if(!j.env) return false;
    jobject act = CurrentActivity();
    if(!act) { logger->Error("Выбор файла: AML не отдал Activity"); return false; }
    jmethodID m = M(j.env, "pickFile", "(Landroid/app/Activity;Ljava/lang/String;)V");
    if(j.Fail() || !m) return false;
    jstring d = ToJ(j.env, destPath);
    j.env->CallStaticVoidMethod(g_cls, m, act, d);
    j.env->DeleteLocalRef(d);
    return !j.Fail();
}

int PickState(std::string* result)
{
    if(!g_ready) return 0;
    Env j; if(!j.env) return 0;
    jfieldID fs = j.env->GetStaticFieldID(g_cls, "pickState", "I");
    jfieldID fr = j.env->GetStaticFieldID(g_cls, "pickResult", "Ljava/lang/String;");
    if(j.Fail() || !fs || !fr) return 0;
    int st = j.env->GetStaticIntField(g_cls, fs);
    if(result)
    {
        jstring r = (jstring)j.env->GetStaticObjectField(g_cls, fr);
        *result = r ? FromJ(j.env, r) : std::string();
        if(r) j.env->DeleteLocalRef(r);
    }
    return st;
}

void PickReset()
{
    if(!g_ready) return;
    Env j; if(!j.env) return;
    jfieldID fs = j.env->GetStaticFieldID(g_cls, "pickState", "I");
    if(!j.Fail() && fs) j.env->SetStaticIntField(g_cls, fs, 0);
}

void OpenUrl(const std::string& url)
{
    if(!g_ready) return;
    Env j; if(!j.env) return;
    jstring u = ToJ(j.env, url);
    j.env->CallStaticVoidMethod(g_cls, M(j.env, "openUrl", "(Ljava/lang/String;)V"), u);
    j.env->DeleteLocalRef(u); j.Fail();
}

} // namespace bridge
