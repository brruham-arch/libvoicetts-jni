/**
 * voicetts.cpp - AML TTS Mod untuk SA-MP Android
 * Google TTS -> synthesizeToFile -> WAV PCM -> BASS push stream (audio game)
 * v2.6 - PCM masuk BASS stream game, bisa di-capture TikTok/streaming
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
#define WAV_FILE  "/storage/emulated/0/tts_out.wav"
#define VERSION   "v2.6"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
static void logf_impl(const char* msg) {
    FILE* f = fopen(LOGFILE, "a"); if (f) { fprintf(f,"%s\n",msg); fclose(f); }
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

static HSTREAM (*pBASSStreamCreate)(DWORD,DWORD,DWORD,STREAMPROC,void*)  = nullptr;
static DWORD   (*pBASSStreamPutData)(HSTREAM,const void*,DWORD)           = nullptr;
static BOOL    (*pBASSChannelPlay)(DWORD,BOOL)                             = nullptr;
static BOOL    (*pBASSChannelStop)(DWORD)                                  = nullptr;
static BOOL    (*pBASSChannelSetAttribute)(DWORD,DWORD,float)              = nullptr;

// ============================================================
// WAV header parser
// ============================================================
struct WavHeader {
    char     riff[4];       // "RIFF"
    uint32_t chunkSize;
    char     wave[4];       // "WAVE"
    char     fmt[4];        // "fmt "
    uint32_t fmtSize;
    uint16_t audioFormat;   // 1 = PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char     data[4];       // "data"
    uint32_t dataSize;
};

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
static jmethodID g_synth_mid = nullptr;  // synthesizeToFile
static jmethodID g_pitch_mid = nullptr;
static jmethodID g_rate_mid  = nullptr;
static jmethodID g_lang_mid  = nullptr;
static volatile int g_tts_ready  = 0;
static volatile int g_is_playing = 0;

// Queue sederhana: satu teks pending
static char g_pending[512] = {};
static pthread_mutex_t g_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond    = PTHREAD_COND_INITIALIZER;

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

static jclass findAppClass(JNIEnv* env, const char* name) {
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    jmethodID curMid = env->GetStaticMethodID(atCls,
        "currentApplication", "()Landroid/app/Application;");
    jobject app = env->CallStaticObjectMethod(atCls, curMid);
    if (!app || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    jclass ctxCls   = env->FindClass("android/content/Context");
    jmethodID clMid = env->GetMethodID(ctxCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject cl      = env->CallObjectMethod(app, clMid);
    if (!cl || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    jclass clCls    = env->FindClass("java/lang/ClassLoader");
    jmethodID ldMid = env->GetMethodID(clCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    char dotName[256]; strncpy(dotName, name, sizeof(dotName));
    for (char* p = dotName; *p; p++) if (*p == '/') *p = '.';
    jstring jn  = env->NewStringUTF(dotName);
    jobject cls = env->CallObjectMethod(cl, ldMid, jn);
    env->DeleteLocalRef(jn);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return (jclass)env->NewGlobalRef(cls);
}

// ============================================================
// BASS stream — lazy create
// ============================================================
static void ensure_stream(uint32_t sampleRate, uint16_t channels) {
    // Kalau sample rate berbeda, recreate
    if (g_stream) {
        if (pBASSChannelStop) pBASSChannelStop(g_stream);
        g_stream = 0;
    }
    if (!pBASSStreamCreate || !pBASSChannelPlay) return;
    g_stream = pBASSStreamCreate(sampleRate, channels, 0, STREAMPROC_PUSH, nullptr);
    if (!g_stream) { LOGF("[TTS] BASS_StreamCreate failed sr=%u ch=%u", sampleRate, channels); return; }
    if (pBASSChannelSetAttribute)
        pBASSChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, g_volume / 100.0f);
    pBASSChannelPlay(g_stream, 0);
    LOGF("[TTS] " VERSION " BASS stream: %u sr=%u ch=%u", g_stream, sampleRate, channels);
}

// ============================================================
// Push WAV file ke BASS stream
// ============================================================
static void push_wav_to_bass(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { LOGF("[TTS] push_wav: cannot open %s", path); return; }

    WavHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) < sizeof(hdr)) {
        LOGF("[TTS] push_wav: header read failed"); fclose(f); return;
    }

    // Dump 16 byte pertama untuk debug
    LOGF("[TTS] header[0..15]: %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x",
        (unsigned char)((char*)&hdr)[0],  (unsigned char)((char*)&hdr)[1],
        (unsigned char)((char*)&hdr)[2],  (unsigned char)((char*)&hdr)[3],
        (unsigned char)((char*)&hdr)[4],  (unsigned char)((char*)&hdr)[5],
        (unsigned char)((char*)&hdr)[6],  (unsigned char)((char*)&hdr)[7],
        (unsigned char)((char*)&hdr)[8],  (unsigned char)((char*)&hdr)[9],
        (unsigned char)((char*)&hdr)[10], (unsigned char)((char*)&hdr)[11],
        (unsigned char)((char*)&hdr)[12], (unsigned char)((char*)&hdr)[13],
        (unsigned char)((char*)&hdr)[14], (unsigned char)((char*)&hdr)[15]);
    LOGF("[TTS] riff='%.4s' wave='%.4s'", hdr.riff, hdr.wave);

    // Validasi
    if (strncmp(hdr.riff, "RIFF", 4) || strncmp(hdr.wave, "WAVE", 4)) {
        LOGF("[TTS] push_wav: bukan file WAV valid"); fclose(f); return;
    }

    LOGF("[TTS] WAV: sr=%u ch=%u bits=%u dataSize=%u",
         hdr.sampleRate, hdr.numChannels, hdr.bitsPerSample, hdr.dataSize);

    // Cari "data" chunk (skip extra chunks jika ada)
    if (strncmp(hdr.data, "data", 4) != 0) {
        // Scan manual
        fseek(f, 12, SEEK_SET);
        char tag[4]; uint32_t sz;
        while (fread(tag, 1, 4, f) == 4) {
            fread(&sz, 1, 4, f);
            if (strncmp(tag, "data", 4) == 0) {
                hdr.dataSize    = sz;
                hdr.sampleRate  = hdr.sampleRate;
                break;
            }
            fseek(f, sz, SEEK_CUR);
        }
    }

    ensure_stream(hdr.sampleRate, hdr.numChannels);
    if (!g_stream || !pBASSStreamPutData) { fclose(f); return; }

    // Feed PCM ke BASS dalam chunk
    const int CHUNK = 8192;
    char buf[CHUNK];
    uint32_t total = 0;
    while (total < hdr.dataSize) {
        int toRead = (hdr.dataSize - total) > CHUNK ? CHUNK : (hdr.dataSize - total);
        int got = (int)fread(buf, 1, toRead, f);
        if (got <= 0) break;
        pBASSStreamPutData(g_stream, buf, (DWORD)got);
        total += got;
    }
    fclose(f);
    LOGF("[TTS] push_wav: pushed %u bytes to BASS stream", total);
}

// ============================================================
// synthesizeToFile worker thread
// ============================================================
struct SpeakArgs { char text[512]; };

static void* speak_thread(void* arg) {
    SpeakArgs* a = (SpeakArgs*)arg;
    char text[512]; strncpy(text, a->text, sizeof(text)); free(a);

    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env || !g_tts_obj) {
        LOGF("[TTS] speak_thread: no env/tts"); g_is_playing = 0; return nullptr;
    }

    // Update pitch & rate
    if (g_pitch_mid) { env->CallIntMethod(g_tts_obj, g_pitch_mid, (jfloat)g_pitch); env->ExceptionClear(); }
    if (g_rate_mid)  { env->CallIntMethod(g_tts_obj, g_rate_mid,  (jfloat)g_rate);  env->ExceptionClear(); }

    remove(WAV_FILE);

    jstring jtext = env->NewStringUTF(text);
    jstring juid  = env->NewStringUTF("tts_synth");

    // Buat java.io.File object untuk path WAV
    jclass fileCls    = env->FindClass("java/io/File");
    jmethodID fileMid = env->GetMethodID(fileCls, "<init>", "(Ljava/lang/String;)V");
    jstring jpath     = env->NewStringUTF(WAV_FILE);
    jobject fileObj   = env->NewObject(fileCls, fileMid, jpath);
    env->DeleteLocalRef(jpath);
    env->ExceptionClear();

    int ret = -1;
    if (g_synth_mid && fileObj) {
        // API 21+: synthesizeToFile(CharSequence, Bundle, File, String)
        ret = env->CallIntMethod(g_tts_obj, g_synth_mid,
            jtext, (jobject)nullptr, fileObj, juid);
        env->ExceptionClear();
    } else if (g_synth_mid) {
        // fallback legacy: synthesizeToFile(String, HashMap, String)
        jstring jpath2 = env->NewStringUTF(WAV_FILE);
        ret = env->CallIntMethod(g_tts_obj, g_synth_mid,
            jtext, (jobject)nullptr, jpath2);
        env->DeleteLocalRef(jpath2);
        env->ExceptionClear();
    }
    env->DeleteLocalRef(jtext);
    env->DeleteLocalRef(juid);
    if (fileObj) env->DeleteLocalRef(fileObj);
    LOGF("[TTS] synthesizeToFile ret=%d file=%s", ret, WAV_FILE);

    if (ret != 0) { // SUCCESS = 0
        LOGF("[TTS] synthesizeToFile ERROR ret=%d", ret);
        g_is_playing = 0;
        if (attached) g_jvm->DetachCurrentThread();
        return nullptr;
    }

    // Poll tunggu file selesai ditulis (max 5 detik)
    int waited = 0;
    while (waited < 50) {
        usleep(100000); // 100ms
        FILE* f = fopen(WAV_FILE, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f); fclose(f);
            if (sz > 44) {
                LOGF("[TTS] WAV file ready: %ld bytes (waited %dms)", sz, waited*100);
                break;
            }
        }
        waited++;
    }

    if (waited >= 50) {
        LOGF("[TTS] ERROR: WAV file timeout"); g_is_playing = 0;
        if (attached) g_jvm->DetachCurrentThread();
        return nullptr;
    }

    // Push ke BASS
    push_wav_to_bass(WAV_FILE);
    remove(WAV_FILE);

    g_is_playing = 0;
    LOGF("[TTS] speak done: %s", text);
    if (attached) g_jvm->DetachCurrentThread();
    return nullptr;
}

// ============================================================
// Init thread
// ============================================================
static void* tts_init_thread(void*) {
    sleep(3);

    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) { LOGF("[TTS] init: getEnv failed"); return nullptr; }

    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls || env->ExceptionCheck()) {
        env->ExceptionClear(); LOGF("[TTS] ERROR: ActivityThread"); return nullptr;
    }
    jmethodID curMid = env->GetStaticMethodID(atCls,
        "currentApplication","()Landroid/app/Application;");
    jobject ctx = env->CallStaticObjectMethod(atCls, curMid);
    if (!ctx || env->ExceptionCheck()) {
        env->ExceptionClear(); LOGF("[TTS] ERROR: no context"); return nullptr;
    }
    LOGF("[TTS] context ok");

    jclass ttsCls = findAppClass(env, "android/speech/tts/TextToSpeech");
    if (!ttsCls || env->ExceptionCheck()) {
        env->ExceptionClear(); LOGF("[TTS] ERROR: TTS class"); return nullptr;
    }
    LOGF("[TTS] TTS class ok");

    jmethodID initMid = env->GetMethodID(ttsCls, "<init>",
        "(Landroid/content/Context;Landroid/speech/tts/TextToSpeech$OnInitListener;)V");
    jobject ttsObj = env->NewObject(ttsCls, initMid, ctx, (jobject)nullptr);
    if (!ttsObj || env->ExceptionCheck()) {
        env->ExceptionClear(); LOGF("[TTS] ERROR: TTS new"); return nullptr;
    }
    g_tts_obj = env->NewGlobalRef(ttsObj);
    g_tts_cls = (jclass)env->NewGlobalRef(ttsCls);
    LOGF("[TTS] TTS object ok");

    // synthesizeToFile(CharSequence, Bundle, File, String) — API 21+
    g_synth_mid = env->GetMethodID(ttsCls, "synthesizeToFile",
        "(Ljava/lang/CharSequence;Landroid/os/Bundle;Ljava/io/File;Ljava/lang/String;)I");
    if (!g_synth_mid || env->ExceptionCheck()) {
        env->ExceptionClear();
        // fallback: synthesizeToFile(String, HashMap, String) — API < 21
        g_synth_mid = env->GetMethodID(ttsCls, "synthesizeToFile",
            "(Ljava/lang/String;Ljava/util/HashMap;Ljava/lang/String;)I");
        env->ExceptionClear();
        LOGF("[TTS] synthesizeToFile: using legacy API");
    } else {
        LOGF("[TTS] synthesizeToFile: using API 21+");
    }

    g_pitch_mid = env->GetMethodID(ttsCls, "setPitch", "(F)I"); env->ExceptionClear();
    g_rate_mid  = env->GetMethodID(ttsCls, "setSpeechRate", "(F)I"); env->ExceptionClear();
    g_lang_mid  = env->GetMethodID(ttsCls, "setLanguage", "(Ljava/util/Locale;)I"); env->ExceptionClear();

    // Set language awal
    jclass loCls  = env->FindClass("java/util/Locale");
    jmethodID lMid = env->GetMethodID(loCls, "<init>", "(Ljava/lang/String;)V");
    jstring langJs = env->NewStringUTF(g_lang);
    jobject locale = env->NewObject(loCls, lMid, langJs);
    if (g_lang_mid && locale) { env->CallIntMethod(g_tts_obj, g_lang_mid, locale); env->ExceptionClear(); }

    if (g_pitch_mid) { env->CallIntMethod(g_tts_obj, g_pitch_mid, (jfloat)g_pitch); env->ExceptionClear(); }
    if (g_rate_mid)  { env->CallIntMethod(g_tts_obj, g_rate_mid,  (jfloat)g_rate);  env->ExceptionClear(); }

    sleep(1);
    g_tts_ready = 1;
    LOGF("[TTS] " VERSION " ready! lang=%s synth_mid=%p", g_lang, g_synth_mid);

    if (attached) g_jvm->DetachCurrentThread();
    return nullptr;
}

// ============================================================
// TTS API
// ============================================================
static void _tts_speak(const char* text) {
    if (!text || !g_enabled || !g_tts_ready || !g_tts_obj) return;
    if (g_is_playing) {
        LOGF("[TTS] busy, skip: %s", text); return;
    }
    g_is_playing = 1;

    SpeakArgs* a = (SpeakArgs*)malloc(sizeof(SpeakArgs));
    strncpy(a->text, text, sizeof(a->text));
    pthread_t thr;
    pthread_create(&thr, nullptr, speak_thread, a);
    pthread_detach(thr);
}

static void _tts_set_lang(const char* lang) {
    if (!lang) return;
    snprintf(g_lang, sizeof(g_lang), "%s", lang);
    if (!g_tts_ready || !g_tts_obj) return;
    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) return;
    jclass loCls   = env->FindClass("java/util/Locale");
    jmethodID lMid = env->GetMethodID(loCls, "<init>", "(Ljava/lang/String;)V");
    jstring langJs = env->NewStringUTF(lang);
    jobject locale = env->NewObject(loCls, lMid, langJs);
    if (g_lang_mid && locale) { env->CallIntMethod(g_tts_obj, g_lang_mid, locale); env->ExceptionClear(); }
    LOGF("[TTS] lang=%s", lang);
    if (attached) g_jvm->DetachCurrentThread();
}

static void  _tts_enable(void)       { g_enabled = 1; }
static void  _tts_disable(void)      { g_enabled = 0; }
static int   _tts_is_enabled(void)   { return g_enabled; }
static void  _tts_set_pitch(float v) { g_pitch = v < 0.1f ? 0.1f : (v > 3.0f ? 3.0f : v); }
static void  _tts_set_rate(float v)  { g_rate  = v < 0.1f ? 0.1f : (v > 3.0f ? 3.0f : v); }
static float _tts_get_pitch(void)    { return g_pitch; }
static float _tts_get_rate(void)     { return g_rate; }
static void  _tts_set_volume(int v)  {
    g_volume = v < 0 ? 0 : (v > 100 ? 100 : v);
    if (g_stream && pBASSChannelSetAttribute)
        pBASSChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, g_volume / 100.0f);
}
static int _tts_is_ready(void)       { return g_tts_ready; }

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
    static const char* info = "libvoicetts|2.4|VoiceTTS Google TTS->WAV->BASS|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE); remove(ADDR_FILE); remove(WAV_FILE);
    LOGF("[TTS] OnModPreLoad " VERSION);
}

EXPORT void OnModLoad() {
    LOGF("[TTS] OnModLoad " VERSION " start");

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

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { LOGF("[TTS] ERROR: libBASS not found"); return; }
    pBASSStreamCreate        = (HSTREAM(*)(DWORD,DWORD,DWORD,STREAMPROC,void*))dlsym(hBASS,"BASS_StreamCreate");
    pBASSStreamPutData       = (DWORD(*)(HSTREAM,const void*,DWORD))dlsym(hBASS,"BASS_StreamPutData");
    pBASSChannelPlay         = (BOOL(*)(DWORD,BOOL))dlsym(hBASS,"BASS_ChannelPlay");
    pBASSChannelStop         = (BOOL(*)(DWORD))dlsym(hBASS,"BASS_ChannelStop");
    pBASSChannelSetAttribute = (BOOL(*)(DWORD,DWORD,float))dlsym(hBASS,"BASS_ChannelSetAttribute");
    LOGF("[TTS] BASS: StreamCreate=%p PutData=%p", pBASSStreamCreate, pBASSStreamPutData);

    FILE* af = fopen(ADDR_FILE, "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&tts_api); fclose(af); }

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
