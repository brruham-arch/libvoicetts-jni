/**
 * voicetts.cpp - AML TTS Mod untuk SA-MP Android
 * Alur: Lua speak(text) -> JNI Android TextToSpeech -> AudioTrack PCM callback
 *       -> BASS push stream -> speaker game
 * Author: brruham
 */

#include <jni.h>
#include <string.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define LOG_TAG "libvoicetts"
#define LOGFILE "/storage/emulated/0/voicetts_log.txt"
#define ADDR_FILE "/storage/emulated/0/voicetts_addr.txt"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
static void logf_impl(const char* msg) {
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
    LOGI("%s", msg);
}
#define LOGF(fmt, ...) do { char _b[512]; snprintf(_b,sizeof(_b),fmt,##__VA_ARGS__); logf_impl(_b); } while(0)

// ============================================================
// BASS types
// ============================================================
typedef unsigned int DWORD;
typedef unsigned int HSTREAM;
typedef int          BOOL;
typedef DWORD (*STREAMPROC)(HSTREAM, void*, DWORD, void*);
#define STREAMPROC_PUSH     ((STREAMPROC)-1)
#define BASS_ATTRIB_VOL     2

static HSTREAM (*pBASSStreamCreate)(DWORD,DWORD,DWORD,STREAMPROC,void*) = nullptr;
static DWORD   (*pBASSStreamPutData)(HSTREAM,const void*,DWORD)          = nullptr;
static BOOL    (*pBASSChannelPlay)(DWORD,BOOL)                            = nullptr;
static BOOL    (*pBASSChannelStop)(DWORD)                                 = nullptr;
static BOOL    (*pBASSChannelSetAttribute)(DWORD,DWORD,float)             = nullptr;

// ============================================================
// Globals
// ============================================================
static int   g_enabled    = 1;
static float g_pitch      = 1.0f;
static float g_rate       = 1.0f;
static int   g_volume     = 100;  // 0-100
static char  g_lang[16]   = "id";

static HSTREAM g_stream   = 0;
static JavaVM* g_jvm      = nullptr;

// TTS Java objects
static jobject  g_tts_obj       = nullptr;
static jmethodID g_speak_mid    = nullptr;
static jmethodID g_stop_mid     = nullptr;
static jmethodID g_setPitch_mid = nullptr;
static jmethodID g_setRate_mid  = nullptr;
static jclass   g_tts_cls       = nullptr;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_tts_ready = 0;

// ============================================================
// JNI helper — get env (attach jika perlu)
// ============================================================
static JNIEnv* getEnv(bool* attached) {
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    *attached = false;
    int st = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != 0) return nullptr;
        *attached = true;
    }
    return env;
}

static void detachEnv() {
    if (g_jvm) g_jvm->DetachCurrentThread();
}

// ============================================================
// BASS local stream
// ============================================================
static void create_stream() {
    if (!pBASSStreamCreate || !pBASSChannelPlay) return;
    if (g_stream) return;
    // 16000 Hz mono — Android TTS default sample rate
    g_stream = pBASSStreamCreate(16000, 1, 0, STREAMPROC_PUSH, nullptr);
    if (!g_stream) { LOGF("[TTS] ERROR: BASS_StreamCreate failed"); return; }
    if (pBASSChannelSetAttribute)
        pBASSChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, g_volume / 100.0f);
    pBASSChannelPlay(g_stream, 0);
    LOGF("[TTS] BASS stream created: %u", g_stream);
}

static void destroy_stream() {
    if (!g_stream) return;
    if (pBASSChannelStop) pBASSChannelStop(g_stream);
    g_stream = 0;
    LOGF("[TTS] BASS stream destroyed");
}

// ============================================================
// TTS UtteranceProgressListener (PCM via AudioTrack hook)
// Strategi: pakai OnAudioAvailable callback (API 26+)
// ============================================================

// Native method dipanggil dari Java UtteranceProgressListener
extern "C" JNIEXPORT void JNICALL
Java_com_voicetts_TTSCallback_onAudioAvailable(
        JNIEnv* env, jobject thiz,
        jstring utteranceId, jbyteArray audioData, jint sampleRateInHz) {

    if (!g_stream || !pBASSStreamPutData) return;

    jsize len = env->GetArrayLength(audioData);
    jbyte* buf = env->GetByteArrayElements(audioData, nullptr);
    if (buf && len > 0) {
        // Update stream sample rate jika berbeda
        // (buat stream baru kalau rate berubah — jarang terjadi)
        pBASSStreamPutData(g_stream, buf, (DWORD)len);
        LOGF("[TTS] onAudioAvailable: %d bytes sr=%d", (int)len, sampleRateInHz);
    }
    if (buf) env->ReleaseByteArrayElements(audioData, buf, JNI_ABORT);
}

extern "C" JNIEXPORT void JNICALL
Java_com_voicetts_TTSCallback_onDone(JNIEnv*, jobject, jstring) {
    LOGF("[TTS] onDone");
}

extern "C" JNIEXPORT void JNICALL
Java_com_voicetts_TTSCallback_onError(JNIEnv*, jobject, jstring, jint) {
    LOGF("[TTS] onError");
}

// ============================================================
// Init TTS di thread terpisah (butuh Looper)
// ============================================================
struct InitArgs {
    char lang[16];
};

static void* tts_init_thread(void* arg) {
    InitArgs* a = (InitArgs*)arg;
    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) { LOGF("[TTS] init: getEnv failed"); free(a); return nullptr; }

    // Looper.prepare()
    jclass looperCls = env->FindClass("android/os/Looper");
    jmethodID prepareMid = env->GetStaticMethodID(looperCls, "prepare", "()V");
    env->CallStaticVoidMethod(looperCls, prepareMid);

    // Activity context via ActivityThread
    jclass atCls = env->FindClass("android/app/ActivityThread");
    jmethodID curMid = env->GetStaticMethodID(atCls,
        "currentActivityThread", "()Landroid/app/ActivityThread;");
    jobject at = env->CallStaticObjectMethod(atCls, curMid);

    jmethodID appMid = env->GetMethodID(atCls,
        "getApplication", "()Landroid/app/Application;");
    jobject ctx = env->CallObjectMethod(at, appMid);
    if (!ctx) { LOGF("[TTS] ERROR: no context"); if (attached) detachEnv(); free(a); return nullptr; }

    // new TextToSpeech(context, listener)
    // Buat listener sederhana (anonymous class via Proxy tidak mudah dari JNI)
    // Pakai TTSInitListener class yang kita daftarkan
    jclass ttsCls = env->FindClass("android/speech/tts/TextToSpeech");
    if (!ttsCls) { LOGF("[TTS] ERROR: TTS class not found"); if (attached) detachEnv(); free(a); return nullptr; }

    // Init listener — pakai null dulu, lalu set language setelah konstruktor
    jmethodID initMid = env->GetMethodID(ttsCls, "<init>",
        "(Landroid/content/Context;Landroid/speech/tts/TextToSpeech$OnInitListener;)V");

    // Buat OnInitListener via lambda/anonymous — dari JNI perlu Proxy
    // Cara termudah: pakai class helper kita sendiri
    // Untuk simplicity: set language di speak() saja, TTS init tanpa listener check
    jobject ttsObj = env->NewObject(ttsCls, initMid, ctx, (jobject)nullptr);
    if (!ttsObj) { LOGF("[TTS] ERROR: TTS new failed"); if (attached) detachEnv(); free(a); return nullptr; }

    g_tts_obj = env->NewGlobalRef(ttsObj);
    g_tts_cls = (jclass)env->NewGlobalRef(ttsCls);

    g_speak_mid = env->GetMethodID(ttsCls, "speak",
        "(Ljava/lang/CharSequence;ILjava/util/HashMap;)I");
    g_stop_mid  = env->GetMethodID(ttsCls, "stop", "()I");
    g_setPitch_mid = env->GetMethodID(ttsCls, "setPitch", "(F)I");
    g_setRate_mid  = env->GetMethodID(ttsCls, "setSpeechRate", "(F)I");

    // Set language
    jclass localeCls = env->FindClass("java/util/Locale");
    jmethodID localeMid = env->GetMethodID(localeCls, "<init>", "(Ljava/lang/String;)V");
    jstring langStr = env->NewStringUTF(a->lang);
    jobject locale = env->NewObject(localeCls, localeMid, langStr);
    jmethodID setLangMid = env->GetMethodID(ttsCls, "setLanguage",
        "(Ljava/util/Locale;)I");
    env->CallIntMethod(g_tts_obj, setLangMid, locale);

    // Set pitch & rate
    env->CallIntMethod(g_tts_obj, g_setPitch_mid, (jfloat)g_pitch);
    env->CallIntMethod(g_tts_obj, g_setRate_mid,  (jfloat)g_rate);

    // Tunggu TTS engine init (sleep sederhana)
    struct timespec ts = {0, 500000000L}; // 500ms
    nanosleep(&ts, nullptr);

    g_tts_ready = 1;
    LOGF("[TTS] TTS ready, lang=%s", a->lang);

    free(a);

    // Looper.loop() — perlu untuk TTS callback
    jmethodID loopMid = env->GetStaticMethodID(looperCls, "myLooper",
        "()Landroid/os/Looper;");
    jobject myLooper = env->CallStaticObjectMethod(looperCls, loopMid);
    if (myLooper) {
        jclass looperInstCls = env->GetObjectClass(myLooper);
        jmethodID loopMid2 = env->GetMethodID(looperInstCls, "loop", "()V");
        // Looper.loop() blocking — jangan panggil di sini,
        // TTS Android handle sendiri di main looper
    }

    if (attached) detachEnv();
    return nullptr;
}

// ============================================================
// TTS API functions
// ============================================================
static void _tts_speak(const char* text) {
    if (!text || !g_enabled || !g_tts_ready) return;
    if (!g_stream) create_stream();

    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) return;

    // Update pitch & rate
    if (g_setPitch_mid) env->CallIntMethod(g_tts_obj, g_setPitch_mid, (jfloat)g_pitch);
    if (g_setRate_mid)  env->CallIntMethod(g_tts_obj, g_setRate_mid,  (jfloat)g_rate);

    jstring jtext = env->NewStringUTF(text);

    // TextToSpeech.QUEUE_FLUSH = 0
    // speak(CharSequence, int, HashMap) — API < 21
    // speak(CharSequence, int, Bundle, String) — API >= 21
    // Pakai API 21+
    jclass ttsCls = g_tts_cls;
    jmethodID speakMid = env->GetMethodID(ttsCls, "speak",
        "(Ljava/lang/CharSequence;ILandroid/os/Bundle;Ljava/lang/String;)I");

    if (speakMid) {
        // QUEUE_FLUSH = 0
        env->CallIntMethod(g_tts_obj, speakMid, jtext, (jint)0,
            (jobject)nullptr, env->NewStringUTF("tts_utterance"));
    } else if (g_speak_mid) {
        // fallback API < 21
        env->CallIntMethod(g_tts_obj, g_speak_mid, jtext, (jint)0, (jobject)nullptr);
    }

    env->DeleteLocalRef(jtext);
    LOGF("[TTS] speak: %s", text);
    if (attached) detachEnv();
}

static void _tts_set_lang(const char* lang) {
    if (!lang || !g_tts_ready) return;
    snprintf(g_lang, sizeof(g_lang), "%s", lang);

    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) return;

    jclass localeCls = env->FindClass("java/util/Locale");
    jmethodID localeMid = env->GetMethodID(localeCls, "<init>", "(Ljava/lang/String;)V");
    jstring langStr = env->NewStringUTF(lang);
    jobject locale = env->NewObject(localeCls, localeMid, langStr);
    jmethodID setLangMid = env->GetMethodID(g_tts_cls, "setLanguage",
        "(Ljava/util/Locale;)I");
    env->CallIntMethod(g_tts_obj, setLangMid, locale);
    LOGF("[TTS] lang=%s", lang);
    if (attached) detachEnv();
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
static int   _tts_is_ready(void)     { return g_tts_ready; }

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
    _tts_speak,
    _tts_set_pitch,
    _tts_set_rate,
    _tts_set_volume,
    _tts_enable,
    _tts_disable,
    _tts_is_enabled,
    _tts_get_pitch,
    _tts_get_rate,
    _tts_set_lang,
    _tts_is_ready,
};

EXPORT void* __GetModInfo() {
    static const char* info = "libvoicetts|2.0|VoiceTTS Google TTS JNI|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    remove(ADDR_FILE);
    LOGF("[TTS] OnModPreLoad v2.2");
}

EXPORT void OnModLoad() {
    LOGF("[TTS] OnModLoad start");

    // Ambil JavaVM — v2.2
    // Strategi: cari JNI_GetCreatedJavaVMs dari library yang pasti loaded
    typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);
    JNI_GetCreatedJavaVMs_t fnGet = nullptr;

    const char* rtPaths[] = {
        "/system/lib/libandroid_runtime.so",
        "/system/lib64/libandroid_runtime.so",
        "/system/lib/libnativehelper.so",
        "/apex/com.android.art/lib/libnativehelper.so",
        nullptr
    };
    for (int i = 0; rtPaths[i]; i++) {
        void* h = dlopen(rtPaths[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h) h = dlopen(rtPaths[i], RTLD_LAZY | RTLD_NOLOAD);
        if (h) {
            fnGet = (JNI_GetCreatedJavaVMs_t)dlsym(h, "JNI_GetCreatedJavaVMs");
            if (fnGet) { LOGF("[TTS] v2.2 JNI_GetCreatedJavaVMs from: %s", rtPaths[i]); break; }
            LOGF("[TTS] dlopen ok but sym not found: %s", rtPaths[i]);
        } else {
            LOGF("[TTS] dlopen fail: %s | %s", rtPaths[i], dlerror());
        }
    }
    if (!fnGet) { LOGF("[TTS] ERROR: JNI_GetCreatedJavaVMs not found"); return; }

    jsize vmCount = 0;
    JavaVM* vms[1] = {};
    fnGet(vms, 1, &vmCount);
    if (vmCount < 1 || !vms[0]) { LOGF("[TTS] ERROR: no JavaVM"); return; }
    g_jvm = vms[0];
    LOGF("[TTS] v2.2 JavaVM: %p", g_jvm);

    // Load BASS
    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { LOGF("[TTS] ERROR: libBASS not found"); return; }
    pBASSStreamCreate      = (HSTREAM(*)(DWORD,DWORD,DWORD,STREAMPROC,void*))dlsym(hBASS,"BASS_StreamCreate");
    pBASSStreamPutData     = (DWORD(*)(HSTREAM,const void*,DWORD))dlsym(hBASS,"BASS_StreamPutData");
    pBASSChannelPlay       = (BOOL(*)(DWORD,BOOL))dlsym(hBASS,"BASS_ChannelPlay");
    pBASSChannelStop       = (BOOL(*)(DWORD))dlsym(hBASS,"BASS_ChannelStop");
    pBASSChannelSetAttribute = (BOOL(*)(DWORD,DWORD,float))dlsym(hBASS,"BASS_ChannelSetAttribute");
    LOGF("[TTS] BASS loaded: StreamCreate=%p PutData=%p", pBASSStreamCreate, pBASSStreamPutData);

    // Buat BASS stream awal
    create_stream();

    // Init TTS di thread (butuh Looper baru)
    InitArgs* args = (InitArgs*)malloc(sizeof(InitArgs));
    snprintf(args->lang, sizeof(args->lang), "%s", g_lang);
    pthread_t thr;
    pthread_create(&thr, nullptr, tts_init_thread, args);
    pthread_detach(thr);

    // Tulis addr API ke file untuk Lua
    FILE* af = fopen(ADDR_FILE, "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&tts_api); fclose(af); }

    LOGF("[TTS] OnModLoad selesai, tunggu TTS ready...");
}

// Dipanggil JNI_OnLoad saat .so di-dlopen
EXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    LOGF("[TTS] JNI_OnLoad vm=%p", vm);
    return JNI_VERSION_1_6;
}

} // extern "C"
