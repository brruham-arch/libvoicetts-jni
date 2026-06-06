/**
 * voicetts.cpp - AML TTS Mod untuk SA-MP Android
 * Google TTS via JNI + BASS push stream output
 * v2.3 - fix FindClass via app ClassLoader + lazy BASS stream
 * Author: brruham
 */

#include <jni.h>
#include <string.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define LOG_TAG   "libvoicetts"
#define LOGFILE   "/storage/emulated/0/voicetts_log.txt"
#define ADDR_FILE "/storage/emulated/0/voicetts_addr.txt"
#define VERSION   "v2.3"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
static void logf_impl(const char* msg) {
    FILE* f = fopen(LOGFILE, "a"); if (f) { fprintf(f, "%s\n", msg); fclose(f); }
    LOGI("%s", msg);
}
#define LOGF(fmt,...) do{char _b[512];snprintf(_b,sizeof(_b),fmt,##__VA_ARGS__);logf_impl(_b);}while(0)

// ============================================================
// BASS
// ============================================================
typedef unsigned int  DWORD;
typedef unsigned int  HSTREAM;
typedef int           BOOL;
typedef DWORD (*STREAMPROC)(HSTREAM,void*,DWORD,void*);
#define STREAMPROC_PUSH  ((STREAMPROC)-1)
#define BASS_ATTRIB_VOL  2

static HSTREAM (*pBASSStreamCreate)(DWORD,DWORD,DWORD,STREAMPROC,void*) = nullptr;
static DWORD   (*pBASSStreamPutData)(HSTREAM,const void*,DWORD)          = nullptr;
static BOOL    (*pBASSChannelPlay)(DWORD,BOOL)                            = nullptr;
static BOOL    (*pBASSChannelStop)(DWORD)                                 = nullptr;
static BOOL    (*pBASSChannelSetAttribute)(DWORD,DWORD,float)             = nullptr;

// ============================================================
// Globals
// ============================================================
static int   g_enabled  = 1;
static float g_pitch    = 1.0f;
static float g_rate     = 1.0f;
static int   g_volume   = 100;
static char  g_lang[16] = "id";

static HSTREAM   g_stream    = 0;
static JavaVM*   g_jvm       = nullptr;
static jobject   g_tts_obj   = nullptr;
static jclass    g_tts_cls   = nullptr;
static jmethodID g_speak_mid = nullptr;
static jmethodID g_pitch_mid = nullptr;
static jmethodID g_rate_mid  = nullptr;
static jmethodID g_lang_mid  = nullptr;
static volatile int g_tts_ready = 0;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================
// JNI helpers
// ============================================================
static JNIEnv* getEnv(bool* attached) {
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr; *attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != 0) return nullptr;
        *attached = true;
    }
    return env;
}

// FindClass yang benar: pakai ClassLoader dari activity
// Harus dipanggil dari thread yang sudah punya class loader context
static jclass findAppClass(JNIEnv* env, const char* name) {
    // Ambil ActivityThread.currentApplication() -> ClassLoader
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls) { env->ExceptionClear(); return nullptr; }

    jmethodID curMid = env->GetStaticMethodID(atCls,
        "currentApplication", "()Landroid/app/Application;");
    if (!curMid) { env->ExceptionClear(); return nullptr; }

    jobject app = env->CallStaticObjectMethod(atCls, curMid);
    if (!app) { env->ExceptionClear(); return nullptr; }

    jclass ctxCls = env->FindClass("android/content/Context");
    jmethodID getCLMid = env->GetMethodID(ctxCls,
        "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject cl = env->CallObjectMethod(app, getCLMid);
    if (!cl) { env->ExceptionClear(); return nullptr; }

    jclass clCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadMid = env->GetMethodID(clCls,
        "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");

    // Ganti / ke .
    char dotName[256];
    strncpy(dotName, name, sizeof(dotName));
    for (char* p = dotName; *p; p++) if (*p == '/') *p = '.';

    jstring jname = env->NewStringUTF(dotName);
    jobject cls = env->CallObjectMethod(cl, loadMid, jname);
    env->DeleteLocalRef(jname);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return (jclass)env->NewGlobalRef(cls);
}

// ============================================================
// BASS stream — lazy create saat pertama speak
// ============================================================
static void ensure_stream() {
    if (g_stream) return;
    if (!pBASSStreamCreate || !pBASSChannelPlay) return;
    // 22050 Hz mono — Android TTS default
    g_stream = pBASSStreamCreate(22050, 1, 0, STREAMPROC_PUSH, nullptr);
    if (!g_stream) { LOGF("[TTS] BASS_StreamCreate failed"); return; }
    if (pBASSChannelSetAttribute)
        pBASSChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, g_volume / 100.0f);
    pBASSChannelPlay(g_stream, 0);
    LOGF("[TTS] " VERSION " BASS stream: %u", g_stream);
}

// ============================================================
// Init thread
// ============================================================
static void* tts_init_thread(void*) {
    // Tunggu game selesai init (BASS, Activity, dll)
    sleep(3);

    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) { LOGF("[TTS] init: getEnv failed"); return nullptr; }

    // Activity context
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGF("[TTS] ERROR: ActivityThread not found");
        if (attached) g_jvm->DetachCurrentThread();
        return nullptr;
    }
    jmethodID curMid = env->GetStaticMethodID(atCls,
        "currentApplication", "()Landroid/app/Application;");
    jobject ctx = env->CallStaticObjectMethod(atCls, curMid);
    if (!ctx || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGF("[TTS] ERROR: no Application context");
        if (attached) g_jvm->DetachCurrentThread();
        return nullptr;
    }
    LOGF("[TTS] context ok");

    // FindClass via ClassLoader
    jclass ttsCls = findAppClass(env, "android/speech/tts/TextToSpeech");
    if (!ttsCls || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGF("[TTS] ERROR: TextToSpeech class not found");
        if (attached) g_jvm->DetachCurrentThread();
        return nullptr;
    }
    LOGF("[TTS] TextToSpeech class found");

    // new TextToSpeech(context, null)
    jmethodID initMid = env->GetMethodID(ttsCls, "<init>",
        "(Landroid/content/Context;Landroid/speech/tts/TextToSpeech$OnInitListener;)V");
    if (!initMid || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGF("[TTS] ERROR: TTS constructor not found");
        if (attached) g_jvm->DetachCurrentThread();
        return nullptr;
    }

    jobject ttsObj = env->NewObject(ttsCls, initMid, ctx, (jobject)nullptr);
    if (!ttsObj || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGF("[TTS] ERROR: TTS new failed");
        if (attached) g_jvm->DetachCurrentThread();
        return nullptr;
    }
    g_tts_obj = env->NewGlobalRef(ttsObj);
    g_tts_cls = (jclass)env->NewGlobalRef(ttsCls);
    LOGF("[TTS] TTS object created");

    // Cache method IDs
    g_speak_mid = env->GetMethodID(ttsCls, "speak",
        "(Ljava/lang/CharSequence;ILandroid/os/Bundle;Ljava/lang/String;)I");
    if (!g_speak_mid || env->ExceptionCheck()) {
        env->ExceptionClear();
        // fallback API < 21
        g_speak_mid = env->GetMethodID(ttsCls, "speak",
            "(Ljava/lang/String;ILjava/util/HashMap;)I");
        env->ExceptionClear();
    }
    g_pitch_mid = env->GetMethodID(ttsCls, "setPitch", "(F)I");
    g_rate_mid  = env->GetMethodID(ttsCls, "setSpeechRate", "(F)I");
    g_lang_mid  = env->GetMethodID(ttsCls, "setLanguage", "(Ljava/util/Locale;)I");
    env->ExceptionClear();

    // Set language
    jclass localeCls = env->FindClass("java/util/Locale");
    jmethodID lMid   = env->GetMethodID(localeCls, "<init>", "(Ljava/lang/String;)V");
    jstring   langJs = env->NewStringUTF(g_lang);
    jobject   locale = env->NewObject(localeCls, lMid, langJs);
    if (g_lang_mid && locale) env->CallIntMethod(g_tts_obj, g_lang_mid, locale);
    env->ExceptionClear();

    // Set pitch & rate
    if (g_pitch_mid) env->CallIntMethod(g_tts_obj, g_pitch_mid, (jfloat)g_pitch);
    if (g_rate_mid)  env->CallIntMethod(g_tts_obj, g_rate_mid,  (jfloat)g_rate);
    env->ExceptionClear();

    // Tunggu engine init
    sleep(1);
    g_tts_ready = 1;
    LOGF("[TTS] " VERSION " ready! lang=%s", g_lang);

    if (attached) g_jvm->DetachCurrentThread();
    return nullptr;
}

// ============================================================
// TTS API
// ============================================================
static void _tts_speak(const char* text) {
    if (!text || !g_enabled || !g_tts_ready || !g_tts_obj) return;

    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) return;

    ensure_stream(); // lazy create BASS stream

    if (g_pitch_mid) env->CallIntMethod(g_tts_obj, g_pitch_mid, (jfloat)g_pitch);
    if (g_rate_mid)  env->CallIntMethod(g_tts_obj, g_rate_mid,  (jfloat)g_rate);
    env->ExceptionClear();

    jstring jtext = env->NewStringUTF(text);
    // API 21+
    jmethodID spk = env->GetMethodID(g_tts_cls, "speak",
        "(Ljava/lang/CharSequence;ILandroid/os/Bundle;Ljava/lang/String;)I");
    env->ExceptionClear();
    if (spk) {
        jstring uid = env->NewStringUTF("tts1");
        env->CallIntMethod(g_tts_obj, spk, jtext, (jint)0, (jobject)nullptr, uid);
        env->DeleteLocalRef(uid);
    } else if (g_speak_mid) {
        env->CallIntMethod(g_tts_obj, g_speak_mid, jtext, (jint)0, (jobject)nullptr);
    }
    env->ExceptionClear();
    env->DeleteLocalRef(jtext);
    LOGF("[TTS] speak: %s", text);

    if (attached) g_jvm->DetachCurrentThread();
}

static void _tts_set_lang(const char* lang) {
    if (!lang) return;
    snprintf(g_lang, sizeof(g_lang), "%s", lang);
    if (!g_tts_ready || !g_tts_obj) return;

    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) return;

    jclass localeCls = env->FindClass("java/util/Locale");
    jmethodID lMid   = env->GetMethodID(localeCls, "<init>", "(Ljava/lang/String;)V");
    jstring langJs   = env->NewStringUTF(lang);
    jobject locale   = env->NewObject(localeCls, lMid, langJs);
    if (g_lang_mid && locale) env->CallIntMethod(g_tts_obj, g_lang_mid, locale);
    env->ExceptionClear();
    LOGF("[TTS] lang=%s", lang);
    if (attached) g_jvm->DetachCurrentThread();
}

static void  _tts_enable(void)       { g_enabled = 1; }
static void  _tts_disable(void)      { g_enabled = 0; }
static int   _tts_is_enabled(void)   { return g_enabled; }
static void  _tts_set_pitch(float v) { g_pitch = v; }
static void  _tts_set_rate(float v)  { g_rate  = v; }
static float _tts_get_pitch(void)    { return g_pitch; }
static float _tts_get_rate(void)     { return g_rate; }
static void  _tts_set_volume(int v)  {
    g_volume = v < 0 ? 0 : (v > 100 ? 100 : v);
    if (g_stream && pBASSChannelSetAttribute)
        pBASSChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, g_volume / 100.0f);
}
static int _tts_is_ready(void) { return g_tts_ready; }

// ============================================================
// Exported API struct
// ============================================================
struct TtsAPI {
    void  (*speak)(const char*);
    void  (*set_pitch)(float);
    void  (*set_rate)(float);
    void  (*set_volume)(int);
    void  (*enable)(void);
    void  (*disable)(void);
    int   (*is_enabled)(void);
    float (*get_pitch)(void);
    float (*get_rate)(void);
    void  (*set_lang)(const char*);
    int   (*is_ready)(void);
};

#define EXPORT __attribute__((visibility("default")))

extern "C" {

EXPORT TtsAPI tts_api = {
    _tts_speak, _tts_set_pitch, _tts_set_rate, _tts_set_volume,
    _tts_enable, _tts_disable, _tts_is_enabled,
    _tts_get_pitch, _tts_get_rate, _tts_set_lang, _tts_is_ready,
};

EXPORT void* __GetModInfo() {
    static const char* info = "libvoicetts|2.3|VoiceTTS Google TTS JNI|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE); remove(ADDR_FILE);
    LOGF("[TTS] OnModPreLoad " VERSION);
}

EXPORT void OnModLoad() {
    LOGF("[TTS] OnModLoad " VERSION " start");

    // Ambil JavaVM
    typedef jint (*GetVMs_t)(JavaVM**, jsize, jsize*);
    GetVMs_t fnGet = nullptr;

    const char* libs[] = {
        "/apex/com.android.art/lib/libnativehelper.so",
        "/apex/com.android.art/lib64/libnativehelper.so",
        "/system/lib/libandroid_runtime.so",
        nullptr
    };
    for (int i = 0; libs[i]; i++) {
        void* h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h) h = dlopen(libs[i], RTLD_LAZY | RTLD_NOLOAD);
        if (h) {
            fnGet = (GetVMs_t)dlsym(h, "JNI_GetCreatedJavaVMs");
            if (fnGet) { LOGF("[TTS] JNI_GetCreatedJavaVMs from %s", libs[i]); break; }
        }
    }
    if (!fnGet) { LOGF("[TTS] ERROR: JNI_GetCreatedJavaVMs not found"); return; }

    jsize vmCount = 0; JavaVM* vms[1] = {};
    fnGet(vms, 1, &vmCount);
    if (vmCount < 1 || !vms[0]) { LOGF("[TTS] ERROR: no JavaVM"); return; }
    g_jvm = vms[0];
    LOGF("[TTS] JavaVM: %p", g_jvm);

    // Load BASS
    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { LOGF("[TTS] ERROR: libBASS not found"); return; }
    pBASSStreamCreate      = (HSTREAM(*)(DWORD,DWORD,DWORD,STREAMPROC,void*))dlsym(hBASS,"BASS_StreamCreate");
    pBASSStreamPutData     = (DWORD(*)(HSTREAM,const void*,DWORD))dlsym(hBASS,"BASS_StreamPutData");
    pBASSChannelPlay       = (BOOL(*)(DWORD,BOOL))dlsym(hBASS,"BASS_ChannelPlay");
    pBASSChannelStop       = (BOOL(*)(DWORD))dlsym(hBASS,"BASS_ChannelStop");
    pBASSChannelSetAttribute = (BOOL(*)(DWORD,DWORD,float))dlsym(hBASS,"BASS_ChannelSetAttribute");
    LOGF("[TTS] BASS: StreamCreate=%p PutData=%p", pBASSStreamCreate, pBASSStreamPutData);

    // Tulis addr API untuk Lua
    FILE* af = fopen(ADDR_FILE, "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&tts_api); fclose(af); }

    // Init TTS di background thread (sleep 3 detik dulu)
    pthread_t thr;
    pthread_create(&thr, nullptr, tts_init_thread, nullptr);
    pthread_detach(thr);

    LOGF("[TTS] OnModLoad done, TTS init in background...");
}

EXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    LOGF("[TTS] JNI_OnLoad vm=%p", vm);
    return JNI_VERSION_1_6;
}

} // extern "C"
