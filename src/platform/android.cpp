#include "android.h"

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <jni.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dlfcn.h>

namespace platform
{

bool FindTailscaleIPv4(char* out, size_t len)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if(s < 0) return false;

    char    buf[4096];
    ifconf  ifc{};
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    bool found = false;
    if(ioctl(s, SIOCGIFCONF, &ifc) == 0)
    {
        for(char* p = buf; p + sizeof(ifreq) <= buf + ifc.ifc_len; p += sizeof(ifreq))
        {
            ifreq* r = (ifreq*)p;
            if(r->ifr_addr.sa_family != AF_INET) continue;
            in_addr a = ((sockaddr_in*)&r->ifr_addr)->sin_addr;
            uint32_t ip = ntohl(a.s_addr);
            if((ip & 0xFFC00000u) == 0x64400000u) // 100.64.0.0/10 — диапазон Tailscale
            {
                inet_ntop(AF_INET, &a, out, (socklen_t)len);
                found = true;
                break;
            }
        }
    }
    close(s);
    return found;
}

bool ExtractIPv4(const char* text, char* out, size_t len)
{
    if(!text) return false;
    for(const char* p = text; *p; ++p)
    {
        if(!isdigit((unsigned char)*p) || (p > text && (isdigit((unsigned char)p[-1]) || p[-1] == '.'))) continue;
        unsigned a, b, c, d;
        int n = 0;
        if(sscanf(p, "%3u.%3u.%3u.%3u%n", &a, &b, &c, &d, &n) == 4 && a <= 255 && b <= 255 && c <= 255 && d <= 255)
        {
            snprintf(out, len, "%u.%u.%u.%u", a, b, c, d);
            return true;
        }
    }
    return false;
}

// ---------- JNI ----------

namespace
{
    // JNIEnv для текущего потока; если поток не был подключён к Java — подключаем
    // и отключаем обратно в деструкторе.
    struct JniScope
    {
        JavaVM* vm = nullptr;
        JNIEnv* env = nullptr;
        bool    attached = false;

        JniScope()
        {
            vm = aml->GetJavaVM();
            if(!vm) return;
            if(vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
            {
                if(vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
                else env = nullptr;
            }
        }
        ~JniScope() { if(attached) vm->DetachCurrentThread(); }

        bool Failed()
        {
            if(env->ExceptionCheck()) { env->ExceptionClear(); return true; }
            return false;
        }
    };

    jobject GetClipboardManager(JniScope& j)
    {
        JNIEnv* env = j.env;
        jobject ctx = aml->GetAppContextObject();
        if(!ctx) return nullptr;
        jclass ctxCls = env->GetObjectClass(ctx);
        jmethodID getService = env->GetMethodID(ctxCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jstring name = env->NewStringUTF("clipboard");
        jobject cm = getService ? env->CallObjectMethod(ctx, getService, name) : nullptr;
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(ctxCls);
        if(j.Failed()) return nullptr;
        return cm;
    }
}

bool ClipboardGetText(char* out, size_t len)
{
    JniScope j;
    if(!j.env) return false;
    JNIEnv* env = j.env;
    out[0] = 0;

    jobject cm = GetClipboardManager(j);
    if(!cm) return false;

    bool ok = false;
    jclass cmCls = env->GetObjectClass(cm);
    jmethodID getClip = env->GetMethodID(cmCls, "getPrimaryClip", "()Landroid/content/ClipData;");
    jobject clip = getClip ? env->CallObjectMethod(cm, getClip) : nullptr;
    if(!j.Failed() && clip)
    {
        jclass clipCls = env->GetObjectClass(clip);
        jmethodID count = env->GetMethodID(clipCls, "getItemCount", "()I");
        jmethodID itemAt = env->GetMethodID(clipCls, "getItemAt", "(I)Landroid/content/ClipData$Item;");
        if(count && itemAt && env->CallIntMethod(clip, count) > 0 && !j.Failed())
        {
            jobject item = env->CallObjectMethod(clip, itemAt, 0);
            if(!j.Failed() && item)
            {
                jclass itemCls = env->GetObjectClass(item);
                jmethodID getText = env->GetMethodID(itemCls, "getText", "()Ljava/lang/CharSequence;");
                jobject text = getText ? env->CallObjectMethod(item, getText) : nullptr;
                if(!j.Failed() && text)
                {
                    jclass objCls = env->FindClass("java/lang/Object");
                    jmethodID toString = env->GetMethodID(objCls, "toString", "()Ljava/lang/String;");
                    jstring str = (jstring)env->CallObjectMethod(text, toString);
                    if(!j.Failed() && str)
                    {
                        const char* utf = env->GetStringUTFChars(str, nullptr);
                        if(utf) { strncpy(out, utf, len - 1); out[len - 1] = 0; ok = true; env->ReleaseStringUTFChars(str, utf); }
                        env->DeleteLocalRef(str);
                    }
                    env->DeleteLocalRef(objCls);
                    env->DeleteLocalRef(text);
                }
                env->DeleteLocalRef(itemCls);
                env->DeleteLocalRef(item);
            }
        }
        env->DeleteLocalRef(clipCls);
        env->DeleteLocalRef(clip);
    }
    env->DeleteLocalRef(cmCls);
    env->DeleteLocalRef(cm);
    return ok;
}

int HasPermission(const char* permission)
{
    JniScope j;
    if(!j.env) return -1;
    JNIEnv* env = j.env;
    jobject ctx = aml->GetAppContextObject();
    if(!ctx) return -1;
    int result = -1;
    jclass ctxCls = env->GetObjectClass(ctx);
    jmethodID check = env->GetMethodID(ctxCls, "checkSelfPermission", "(Ljava/lang/String;)I");
    if(!j.Failed() && check)
    {
        jstring name = env->NewStringUTF(permission);
        jint r = env->CallIntMethod(ctx, check, name);
        if(!j.Failed()) result = (r == 0) ? 1 : 0;   // PackageManager.PERMISSION_GRANTED == 0
        env->DeleteLocalRef(name);
    }
    env->DeleteLocalRef(ctxCls);
    return result;
}

bool RequestPermission(const char* permission)
{
    // Текущий экран игры берём у AML напрямую по имени функции (не через таблицу
    // виртуальных методов — так не зависим от того, какая версия AML стоит).
    // libAML загружен игрой не «глобально», поэтому ищем функцию в нём самом, а не везде.
    typedef jobject (*GetActivityFn)(void* self);
    static GetActivityFn getActivity = nullptr;
    if(!getActivity)
    {
        void* h = dlopen("libAML.so", RTLD_NOW | RTLD_NOLOAD);
        if(h) getActivity = (GetActivityFn)dlsym(h, "_ZN3AML18GetCurrentActivityEv");
        if(!getActivity) getActivity = (GetActivityFn)dlsym(RTLD_DEFAULT, "_ZN3AML18GetCurrentActivityEv");
    }
    if(!getActivity) { logger->Error("Нет AML::GetCurrentActivity — запросить разрешение нельзя"); return false; }

    JniScope j;
    if(!j.env) return false;
    JNIEnv* env = j.env;
    jobject activity = getActivity(aml);
    if(!activity) { logger->Error("AML не отдал Activity"); return false; }

    bool ok = false;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID req = env->GetMethodID(actCls, "requestPermissions", "([Ljava/lang/String;I)V");
    if(!j.Failed() && req)
    {
        jclass strCls = env->FindClass("java/lang/String");
        jobjectArray arr = env->NewObjectArray(1, strCls, nullptr);
        jstring name = env->NewStringUTF(permission);
        env->SetObjectArrayElement(arr, 0, name);
        env->CallVoidMethod(activity, req, arr, (jint)4242);
        ok = !j.Failed();
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(arr);
        env->DeleteLocalRef(strCls);
    }
    env->DeleteLocalRef(actCls);
    return ok;
}

bool OpenAppSettings()
{
    JniScope j;
    if(!j.env) return false;
    JNIEnv* env = j.env;
    jobject ctx = aml->GetAppContextObject();
    if(!ctx) return false;
    bool ok = false;

    jclass ctxCls = env->GetObjectClass(ctx);
    jmethodID getPkg = env->GetMethodID(ctxCls, "getPackageName", "()Ljava/lang/String;");
    jstring pkg = getPkg ? (jstring)env->CallObjectMethod(ctx, getPkg) : nullptr;
    if(!j.Failed() && pkg)
    {
        const char* p = env->GetStringUTFChars(pkg, nullptr);
        char uriText[256]; snprintf(uriText, sizeof(uriText), "package:%s", p ? p : "");
        if(p) env->ReleaseStringUTFChars(pkg, p);

        jclass uriCls = env->FindClass("android/net/Uri");
        jmethodID parse = uriCls ? env->GetStaticMethodID(uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;") : nullptr;
        jstring uriStr = env->NewStringUTF(uriText);
        jobject uri = parse ? env->CallStaticObjectMethod(uriCls, parse, uriStr) : nullptr;

        jclass intentCls = env->FindClass("android/content/Intent");
        jmethodID ctor = intentCls ? env->GetMethodID(intentCls, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V") : nullptr;
        jmethodID addFlags = intentCls ? env->GetMethodID(intentCls, "addFlags", "(I)Landroid/content/Intent;") : nullptr;
        jstring action = env->NewStringUTF("android.settings.APPLICATION_DETAILS_SETTINGS");
        if(!j.Failed() && uri && ctor && addFlags)
        {
            jobject intent = env->NewObject(intentCls, ctor, action, uri);
            if(!j.Failed() && intent)
            {
                jobject r = env->CallObjectMethod(intent, addFlags, (jint)0x10000000); // FLAG_ACTIVITY_NEW_TASK
                if(r) env->DeleteLocalRef(r);
                jmethodID start = env->GetMethodID(ctxCls, "startActivity", "(Landroid/content/Intent;)V");
                if(start) { env->CallVoidMethod(ctx, start, intent); ok = !j.Failed(); }
                env->DeleteLocalRef(intent);
            }
        }
        j.Failed();
        env->DeleteLocalRef(action);
        if(intentCls) env->DeleteLocalRef(intentCls);
        if(uri) env->DeleteLocalRef(uri);
        env->DeleteLocalRef(uriStr);
        if(uriCls) env->DeleteLocalRef(uriCls);
        env->DeleteLocalRef(pkg);
    }
    env->DeleteLocalRef(ctxCls);
    return ok;
}

bool DeviceModel(char* out, size_t len)
{
    JniScope j;
    if(!j.env) return false;
    JNIEnv* env = j.env;
    bool ok = false;
    jclass build = env->FindClass("android/os/Build");
    if(!j.Failed() && build)
    {
        jfieldID fid = env->GetStaticFieldID(build, "MODEL", "Ljava/lang/String;");
        jstring str = fid ? (jstring)env->GetStaticObjectField(build, fid) : nullptr;
        if(!j.Failed() && str)
        {
            const char* utf = env->GetStringUTFChars(str, nullptr);
            if(utf) { snprintf(out, len, "%s", utf); ok = out[0] != 0; env->ReleaseStringUTFChars(str, utf); }
            env->DeleteLocalRef(str);
        }
        env->DeleteLocalRef(build);
    }
    return ok;
}

bool ClipboardSetText(const char* text)
{
    JniScope j;
    if(!j.env) return false;
    JNIEnv* env = j.env;

    jobject cm = GetClipboardManager(j);
    if(!cm) return false;

    bool ok = false;
    jclass clipCls = env->FindClass("android/content/ClipData");
    jmethodID newPlain = clipCls ? env->GetStaticMethodID(clipCls, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;") : nullptr;
    if(!j.Failed() && newPlain)
    {
        jstring label = env->NewStringUTF("SA Coop");
        jstring value = env->NewStringUTF(text);
        jobject clip = env->CallStaticObjectMethod(clipCls, newPlain, label, value);
        if(!j.Failed() && clip)
        {
            jclass cmCls = env->GetObjectClass(cm);
            jmethodID setClip = env->GetMethodID(cmCls, "setPrimaryClip", "(Landroid/content/ClipData;)V");
            if(setClip) { env->CallVoidMethod(cm, setClip, clip); ok = !j.Failed(); }
            env->DeleteLocalRef(cmCls);
            env->DeleteLocalRef(clip);
        }
        env->DeleteLocalRef(label);
        env->DeleteLocalRef(value);
    }
    if(clipCls) env->DeleteLocalRef(clipCls);
    env->DeleteLocalRef(cm);
    return ok;
}

} // namespace platform
