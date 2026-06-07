/**
 * voicetts.cpp - AML TTS Mod untuk SA-MP Android
 * Google TTS -> synthesizeToFile -> WAV PCM ->
 *   [1] BASS push stream (speaker game / TikTok capture)
 *   [2] ring buffer mic -> BASS_ChannelGetData hook -> SampVoice inject
 * v2.8 - gabungan speaker output + mic inject
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
#define VERSION   "v2.8"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
static void logf_impl(const char* msg) {
    FILE* f = fopen(LOGFILE, "a"); if (f) { fprintf(f,"%s\n",msg); fclose(f); }
    LOGI("%s", msg);
}
#define LOGF(fmt,...) do{char _b[512];snprintf(_b,sizeof(_b),fmt,##__VA_ARGS__);logf_impl(_b);}while(0)

// ============================================================
// BASS types
// ============================================================
typedef unsigned int  DWORD;
typedef unsigned int  HSTREAM;
typedef unsigned int  HRECORD;
typedef unsigned int  HDSP;
typedef int           BOOL;
typedef DWORD (*STREAMPROC)(HSTREAM,void*,DWORD,void*);
typedef void  (*DSPPROC)(HDSP,DWORD,void*,DWORD,void*);
#define STREAMPROC_PUSH  ((STREAMPROC)-1)
#define BASS_ATTRIB_VOL  2

// BASS function pointers
static HSTREAM (*pBASSStreamCreate)(DWORD,DWORD,DWORD,STREAMPROC,void*)  = nullptr;
static DWORD   (*pBASSStreamPutData)(HSTREAM,const void*,DWORD)           = nullptr;
static BOOL    (*pBASSChannelPlay)(DWORD,BOOL)                             = nullptr;
static BOOL    (*pBASSChannelStop)(DWORD)                                  = nullptr;
static BOOL    (*pBASSChannelSetAttribute)(DWORD,DWORD,float)              = nullptr;
static HDSP    (*pBASSChannelSetDSP)(DWORD,DSPPROC,void*,int)             = nullptr;

// Dobby hooks
static void* (*pDobbyHook)(void*,void*,void**)                            = nullptr;
static void* (*pDobbySymbolResolver)(const char*,const char*)             = nullptr;

// Original functions (before hook)
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*)    = nullptr;
static DWORD   (*orig_BASSChannelGetData)(DWORD,void*,DWORD)              = nullptr;
static DWORD   (*orig_BASSChannelIsActive)(DWORD)                         = nullptr;
static BOOL    (*orig_BASSChannelPause)(DWORD)                            = nullptr;

// ============================================================
// PCM ring buffer untuk mic inject (48000 Hz mono)
// ============================================================
#define PCM_BUF_SIZE (48000 * 4)
static short           g_pcm_buf[PCM_BUF_SIZE];
static int             g_pcm_write = 0;
static int             g_pcm_read  = 0;
static int             g_pcm_avail = 0;
static pthread_mutex_t g_pcm_mutex = PTHREAD_MUTEX_INITIALIZER;

#define ESPEAK_RATE  24000  // Google TTS output rate
#define RECORD_RATE  48000  // BASS record rate

static HRECORD g_hrecord   = 0;
static HSTREAM g_stream    = 0;  // local speaker stream

// ============================================================
// WAV header
// ============================================================
struct WavHeader {
    char     riff[4];
    uint32_t chunkSize;
    char     wave[4];
    char     fmt[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char     data[4];
    uint32_t dataSize;
};

// ============================================================
// Globals
// ============================================================
static int   g_enabled     = 1;
static int   g_mic_inject  = 1;  // inject ke mic SampVoice
static int   g_play_local  = 1;  // output ke speaker game
static float g_pitch       = 1.0f;
static float g_rate        = 1.0f;
static int   g_volume      = 100;
static char  g_lang[16]    = "id";

static JavaVM*   g_jvm       = nullptr;
static jobject   g_tts_obj   = nullptr;
static jclass    g_tts_cls   = nullptr;
static jmethodID g_synth_mid = nullptr;
static jmethodID g_pitch_mid = nullptr;
static jmethodID g_rate_mid  = nullptr;
static jmethodID g_lang_mid  = nullptr;
static volatile int g_tts_ready  = 0;
static volatile int g_is_playing = 0;

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
    if (!atCls||env->ExceptionCheck()){env->ExceptionClear();return nullptr;}
    jmethodID curMid = env->GetStaticMethodID(atCls,
        "currentApplication","()Landroid/app/Application;");
    jobject app = env->CallStaticObjectMethod(atCls, curMid);
    if (!app||env->ExceptionCheck()){env->ExceptionClear();return nullptr;}
    jclass ctxCls   = env->FindClass("android/content/Context");
    jmethodID clMid = env->GetMethodID(ctxCls,"getClassLoader","()Ljava/lang/ClassLoader;");
    jobject cl      = env->CallObjectMethod(app, clMid);
    if (!cl||env->ExceptionCheck()){env->ExceptionClear();return nullptr;}
    jclass clCls    = env->FindClass("java/lang/ClassLoader");
    jmethodID ldMid = env->GetMethodID(clCls,"loadClass","(Ljava/lang/String;)Ljava/lang/Class;");
    char dotName[256]; strncpy(dotName,name,sizeof(dotName));
    for (char* p=dotName;*p;p++) if(*p=='/') *p='.';
    jstring jn  = env->NewStringUTF(dotName);
    jobject cls = env->CallObjectMethod(cl, ldMid, jn);
    env->DeleteLocalRef(jn);
    if (env->ExceptionCheck()){env->ExceptionClear();return nullptr;}
    return (jclass)env->NewGlobalRef(cls);
}

// ============================================================
// BASS local stream
// ============================================================
static void ensure_stream(uint32_t sr, uint16_t ch) {
    if (g_stream) { if (pBASSChannelStop) pBASSChannelStop(g_stream); g_stream=0; }
    if (!pBASSStreamCreate||!pBASSChannelPlay) return;
    g_stream = pBASSStreamCreate(sr, ch, 0, STREAMPROC_PUSH, nullptr);
    if (!g_stream){LOGF("[TTS] BASS_StreamCreate failed sr=%u ch=%u",sr,ch);return;}
    if (pBASSChannelSetAttribute)
        pBASSChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, g_volume/100.0f);
    pBASSChannelPlay(g_stream, 0);
    LOGF("[TTS] " VERSION " BASS stream: %u sr=%u ch=%u", g_stream, sr, ch);
}

// ============================================================
// BASS hooks untuk mic inject
// ============================================================
static void tts_dsp_proc(HDSP, DWORD, void*, DWORD, void*) {}

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    LOGF("[TTS] BASSRecordStart freq=%u chans=%u handle=%u", freq, chans, handle);
    if (handle && pBASSChannelSetDSP) {
        g_hrecord = handle;
        pBASSChannelSetDSP(handle, tts_dsp_proc, nullptr, 0);
        LOGF("[TTS] DSP installed on HRECORD %u", handle);
    }
    return handle;
}

static DWORD hook_BASSChannelGetData(DWORD handle, void* buf, DWORD len) {
    DWORD ret = orig_BASSChannelGetData(handle, buf, len);
    if (handle != g_hrecord) return ret;
    if (ret==0||ret==(DWORD)-1||ret==(DWORD)-2) return ret;
    if (len & 0x40000000) return ret;
    if (!buf||!g_enabled||!g_mic_inject) return ret;

    short* pcm     = (short*)buf;
    int    samples = (int)(ret / sizeof(short));

    pthread_mutex_lock(&g_pcm_mutex);
    int avail = g_pcm_avail;
    if (avail > 0) {
        int inject = avail < samples ? avail : samples;
        for (int i = 0; i < inject; i++) {
            pcm[i] = g_pcm_buf[g_pcm_read];
            g_pcm_read = (g_pcm_read + 1) % PCM_BUF_SIZE;
            g_pcm_avail--;
        }
        for (int i = inject; i < samples; i++) pcm[i] = 0;
    } else {
        memset(buf, 0, ret);
    }
    pthread_mutex_unlock(&g_pcm_mutex);
    return ret;
}

static DWORD hook_BASSChannelIsActive(DWORD handle) {
    if (handle == g_hrecord) {
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail > 0) return 1;
    }
    return orig_BASSChannelIsActive(handle);
}

static BOOL hook_BASSChannelPause(DWORD handle) {
    if (handle == g_hrecord) {
        pthread_mutex_lock(&g_pcm_mutex);
        int avail = g_pcm_avail;
        pthread_mutex_unlock(&g_pcm_mutex);
        if (avail > 0) return 1;
    }
    if (handle == g_stream) return 1;
    if (orig_BASSChannelPause) return orig_BASSChannelPause(handle);
    return 0;
}

// ============================================================
// Resample + push ke ring buffer mic (24000 -> 48000)
// ============================================================
static void feed_mic_buffer(const short* samples, int count, uint32_t srcRate) {
    if (!g_mic_inject) return;
    int outCount = (int)(((long long)count * RECORD_RATE + srcRate - 1) / srcRate);
    pthread_mutex_lock(&g_pcm_mutex);
    for (int i = 0; i < outCount && g_pcm_avail < PCM_BUF_SIZE; i++) {
        float pos  = (float)i * srcRate / RECORD_RATE;
        int   idx  = (int)pos;
        float frac = pos - idx;
        short s0   = samples[idx];
        short s1   = (idx + 1 < count) ? samples[idx + 1] : s0;
        g_pcm_buf[g_pcm_write] = (short)(s0 + frac * (s1 - s0));
        g_pcm_write = (g_pcm_write + 1) % PCM_BUF_SIZE;
        g_pcm_avail++;
    }
    pthread_mutex_unlock(&g_pcm_mutex);
}

// ============================================================
// Push WAV ke BASS stream + mic buffer
// ============================================================
static void push_wav(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { LOGF("[TTS] push_wav: cannot open %s", path); return; }

    WavHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) < sizeof(hdr)) {
        LOGF("[TTS] push_wav: header read failed"); fclose(f); return;
    }

    if (strncmp(hdr.riff,"RIFF",4)||strncmp(hdr.wave,"WAVE",4)) {
        LOGF("[TTS] push_wav: invalid WAV"); fclose(f); return;
    }

    // Cari data chunk jika ada extra chunks
    if (strncmp(hdr.data,"data",4) != 0) {
        fseek(f, 12, SEEK_SET);
        char tag[4]; uint32_t sz;
        while (fread(tag,1,4,f)==4) {
            fread(&sz,1,4,f);
            if (strncmp(tag,"data",4)==0) { hdr.dataSize=sz; break; }
            fseek(f, sz, SEEK_CUR);
        }
    }

    LOGF("[TTS] WAV: sr=%u ch=%u bits=%u dataSize=%u",
         hdr.sampleRate, hdr.numChannels, hdr.bitsPerSample, hdr.dataSize);

    // Baca semua PCM
    short* pcmData = (short*)malloc(hdr.dataSize);
    if (!pcmData) { fclose(f); return; }
    uint32_t got = (uint32_t)fread(pcmData, 1, hdr.dataSize, f);
    fclose(f);

    int sampleCount = (int)(got / sizeof(short));

    // [1] Push ke BASS speaker stream
    if (g_play_local) {
        ensure_stream(hdr.sampleRate, hdr.numChannels);
        if (g_stream && pBASSStreamPutData) {
            pBASSStreamPutData(g_stream, pcmData, got);
            LOGF("[TTS] push_wav: %u bytes -> BASS stream", got);
        }
    }

    // [2] Feed ke mic ring buffer (resample ke 48000 jika perlu)
    if (g_mic_inject && g_hrecord) {
        feed_mic_buffer(pcmData, sampleCount, hdr.sampleRate);
        LOGF("[TTS] push_wav: %d samples -> mic buffer (resample %u->%u)",
             sampleCount, hdr.sampleRate, RECORD_RATE);
    }

    free(pcmData);
}

// ============================================================
// speak worker thread
// ============================================================
struct SpeakArgs { char text[512]; };

static void* speak_thread(void* arg) {
    SpeakArgs* a = (SpeakArgs*)arg;
    char text[512]; strncpy(text, a->text, sizeof(text)); free(a);

    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env || !g_tts_obj) {
        LOGF("[TTS] speak_thread: no env/tts"); g_is_playing=0; return nullptr;
    }

    if (g_pitch_mid) { env->CallIntMethod(g_tts_obj, g_pitch_mid, (jfloat)g_pitch); env->ExceptionClear(); }
    if (g_rate_mid)  { env->CallIntMethod(g_tts_obj, g_rate_mid,  (jfloat)g_rate);  env->ExceptionClear(); }

    remove(WAV_FILE);

    jstring jtext   = env->NewStringUTF(text);
    jstring juid    = env->NewStringUTF("tts_synth");
    jclass  fileCls = env->FindClass("java/io/File");
    jmethodID fMid  = env->GetMethodID(fileCls, "<init>", "(Ljava/lang/String;)V");
    jstring jpath   = env->NewStringUTF(WAV_FILE);
    jobject fileObj = env->NewObject(fileCls, fMid, jpath);
    env->DeleteLocalRef(jpath);
    env->ExceptionClear();

    int ret = -1;
    if (g_synth_mid && fileObj) {
        ret = env->CallIntMethod(g_tts_obj, g_synth_mid,
            jtext, (jobject)nullptr, fileObj, juid);
        env->ExceptionClear();
    }
    env->DeleteLocalRef(jtext);
    env->DeleteLocalRef(juid);
    if (fileObj) env->DeleteLocalRef(fileObj);
    LOGF("[TTS] synthesizeToFile ret=%d", ret);

    if (ret != 0) {
        LOGF("[TTS] synthesizeToFile ERROR"); g_is_playing=0;
        if (attached) g_jvm->DetachCurrentThread();
        return nullptr;
    }

    // Poll tunggu RIFF header valid (max 8 detik)
    bool wav_ready = false;
    for (int i = 0; i < 80; i++) {
        usleep(100000);
        FILE* f = fopen(WAV_FILE, "rb");
        if (f) {
            char magic[4]={};
            fread(magic, 1, 4, f); fclose(f);
            if (strncmp(magic, "RIFF", 4) == 0) {
                usleep(100000);
                wav_ready = true;
                LOGF("[TTS] WAV ready (waited %dms)", (i+1)*100);
                break;
            }
        }
    }

    if (!wav_ready) {
        LOGF("[TTS] ERROR: WAV timeout"); g_is_playing=0;
        if (attached) g_jvm->DetachCurrentThread();
        return nullptr;
    }

    push_wav(WAV_FILE);
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
    if (!atCls||env->ExceptionCheck()){env->ExceptionClear();LOGF("[TTS] ERROR: AT");return nullptr;}
    jmethodID curMid = env->GetStaticMethodID(atCls,
        "currentApplication","()Landroid/app/Application;");
    jobject ctx = env->CallStaticObjectMethod(atCls, curMid);
    if (!ctx||env->ExceptionCheck()){env->ExceptionClear();LOGF("[TTS] ERROR: ctx");return nullptr;}
    LOGF("[TTS] context ok");

    jclass ttsCls = findAppClass(env, "android/speech/tts/TextToSpeech");
    if (!ttsCls||env->ExceptionCheck()){env->ExceptionClear();LOGF("[TTS] ERROR: TTS cls");return nullptr;}
    LOGF("[TTS] TTS class ok");

    jmethodID initMid = env->GetMethodID(ttsCls, "<init>",
        "(Landroid/content/Context;Landroid/speech/tts/TextToSpeech$OnInitListener;)V");
    jobject ttsObj = env->NewObject(ttsCls, initMid, ctx, (jobject)nullptr);
    if (!ttsObj||env->ExceptionCheck()){env->ExceptionClear();LOGF("[TTS] ERROR: TTS new");return nullptr;}
    g_tts_obj = env->NewGlobalRef(ttsObj);
    g_tts_cls = (jclass)env->NewGlobalRef(ttsCls);
    LOGF("[TTS] TTS object ok");

    // synthesizeToFile API 21+
    g_synth_mid = env->GetMethodID(ttsCls, "synthesizeToFile",
        "(Ljava/lang/CharSequence;Landroid/os/Bundle;Ljava/io/File;Ljava/lang/String;)I");
    if (!g_synth_mid||env->ExceptionCheck()) {
        env->ExceptionClear();
        g_synth_mid = env->GetMethodID(ttsCls, "synthesizeToFile",
            "(Ljava/lang/String;Ljava/util/HashMap;Ljava/lang/String;)I");
        env->ExceptionClear();
        LOGF("[TTS] synthesizeToFile: legacy API");
    } else {
        LOGF("[TTS] synthesizeToFile: API 21+");
    }

    g_pitch_mid = env->GetMethodID(ttsCls,"setPitch","(F)I");           env->ExceptionClear();
    g_rate_mid  = env->GetMethodID(ttsCls,"setSpeechRate","(F)I");      env->ExceptionClear();
    g_lang_mid  = env->GetMethodID(ttsCls,"setLanguage","(Ljava/util/Locale;)I"); env->ExceptionClear();

    // Set lang awal
    jclass loCls  = env->FindClass("java/util/Locale");
    jmethodID lMid = env->GetMethodID(loCls,"<init>","(Ljava/lang/String;)V");
    jstring langJs = env->NewStringUTF(g_lang);
    jobject locale = env->NewObject(loCls, lMid, langJs);
    if (g_lang_mid&&locale){env->CallIntMethod(g_tts_obj,g_lang_mid,locale);env->ExceptionClear();}
    if (g_pitch_mid){env->CallIntMethod(g_tts_obj,g_pitch_mid,(jfloat)g_pitch);env->ExceptionClear();}
    if (g_rate_mid) {env->CallIntMethod(g_tts_obj,g_rate_mid, (jfloat)g_rate); env->ExceptionClear();}

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
    if (!text||!g_enabled||!g_tts_ready||!g_tts_obj) return;
    if (g_is_playing) { LOGF("[TTS] busy, skip"); return; }
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
    if (!g_tts_ready||!g_tts_obj) return;
    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) return;
    jclass loCls   = env->FindClass("java/util/Locale");
    jmethodID lMid = env->GetMethodID(loCls,"<init>","(Ljava/lang/String;)V");
    jstring langJs = env->NewStringUTF(lang);
    jobject locale = env->NewObject(loCls, lMid, langJs);
    if (g_lang_mid&&locale){env->CallIntMethod(g_tts_obj,g_lang_mid,locale);env->ExceptionClear();}
    LOGF("[TTS] lang=%s", lang);
    if (attached) g_jvm->DetachCurrentThread();
}

static void  _tts_enable(void)            { g_enabled    = 1; }
static void  _tts_disable(void)           { g_enabled    = 0; }
static int   _tts_is_enabled(void)        { return g_enabled; }
static void  _tts_set_pitch(float v)      { g_pitch = v<0.1f?0.1f:(v>3.0f?3.0f:v); }
static void  _tts_set_rate(float v)       { g_rate  = v<0.1f?0.1f:(v>3.0f?3.0f:v); }
static float _tts_get_pitch(void)         { return g_pitch; }
static float _tts_get_rate(void)          { return g_rate; }
static void  _tts_set_mic_inject(int v)   { g_mic_inject = v; LOGF("[TTS] mic_inject=%d",v); }
static int   _tts_get_mic_inject(void)    { return g_mic_inject; }
static void  _tts_set_play_local(int v)   { g_play_local = v; LOGF("[TTS] play_local=%d",v); }
static int   _tts_get_play_local(void)    { return g_play_local; }
static void  _tts_set_volume(int v) {
    g_volume = v<0?0:(v>100?100:v);
    if (g_stream&&pBASSChannelSetAttribute)
        pBASSChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, g_volume/100.0f);
}
static int   _tts_is_ready(void)          { return g_tts_ready; }
static void  _tts_notify_mic_on(unsigned int handle) {
    if (handle && handle != g_hrecord) {
        g_hrecord = handle;
        if (pBASSChannelSetDSP)
            pBASSChannelSetDSP(handle, tts_dsp_proc, nullptr, 0);
        LOGF("[TTS] notify_mic_on handle=%u", handle);
    }
}
static int _tts_pcm_avail(void) {
    pthread_mutex_lock(&g_pcm_mutex);
    int a = g_pcm_avail;
    pthread_mutex_unlock(&g_pcm_mutex);
    return a;
}

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
    void  (*set_mic_inject)(int);
    int   (*get_mic_inject)(void);
    void  (*set_play_local)(int);
    int   (*get_play_local)(void);
    void  (*notify_mic_on)(unsigned int);
    int   (*pcm_avail)(void);
};

#define EXPORT __attribute__((visibility("default")))

extern "C" {

EXPORT TtsAPI tts_api = {
    _tts_speak, _tts_set_pitch, _tts_set_rate, _tts_set_volume,
    _tts_enable, _tts_disable, _tts_is_enabled,
    _tts_get_pitch, _tts_get_rate, _tts_set_lang, _tts_is_ready,
    _tts_set_mic_inject, _tts_get_mic_inject,
    _tts_set_play_local, _tts_get_play_local,
    _tts_notify_mic_on, _tts_pcm_avail,
};

EXPORT void* __GetModInfo() {
    static const char* info = "libvoicetts|2.8|VoiceTTS Google TTS JNI+MicInject|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE); remove(ADDR_FILE); remove(WAV_FILE);
    LOGF("[TTS] OnModPreLoad " VERSION);
}

EXPORT void OnModLoad() {
    LOGF("[TTS] OnModLoad " VERSION " start");

    // JavaVM
    typedef jint (*GetVMs_t)(JavaVM**, jsize, jsize*);
    GetVMs_t fnGet = nullptr;
    const char* libs[] = {
        "/apex/com.android.art/lib/libnativehelper.so",
        "/apex/com.android.art/lib64/libnativehelper.so",
        "/system/lib/libandroid_runtime.so",
        nullptr
    };
    for (int i = 0; libs[i]; i++) {
        void* h = dlopen(libs[i], RTLD_NOW|RTLD_GLOBAL);
        if (!h) h = dlopen(libs[i], RTLD_LAZY|RTLD_NOLOAD);
        if (h) { fnGet=(GetVMs_t)dlsym(h,"JNI_GetCreatedJavaVMs"); if(fnGet){LOGF("[TTS] JVM from %s",libs[i]);break;} }
    }
    if (!fnGet) { LOGF("[TTS] ERROR: no JNI_GetCreatedJavaVMs"); return; }
    jsize vmCount=0; JavaVM* vms[1]={};
    fnGet(vms, 1, &vmCount);
    if (vmCount<1||!vms[0]) { LOGF("[TTS] ERROR: no JavaVM"); return; }
    g_jvm = vms[0];
    LOGF("[TTS] JavaVM: %p", g_jvm);

    // BASS
    void* hBASS = dlopen("libBASS.so", RTLD_NOW|RTLD_GLOBAL);
    if (!hBASS) { LOGF("[TTS] ERROR: libBASS"); return; }
    pBASSStreamCreate        = (HSTREAM(*)(DWORD,DWORD,DWORD,STREAMPROC,void*))dlsym(hBASS,"BASS_StreamCreate");
    pBASSStreamPutData       = (DWORD(*)(HSTREAM,const void*,DWORD))dlsym(hBASS,"BASS_StreamPutData");
    pBASSChannelPlay         = (BOOL(*)(DWORD,BOOL))dlsym(hBASS,"BASS_ChannelPlay");
    pBASSChannelStop         = (BOOL(*)(DWORD))dlsym(hBASS,"BASS_ChannelStop");
    pBASSChannelSetAttribute = (BOOL(*)(DWORD,DWORD,float))dlsym(hBASS,"BASS_ChannelSetAttribute");
    pBASSChannelSetDSP       = (HDSP(*)(DWORD,DSPPROC,void*,int))dlsym(hBASS,"BASS_ChannelSetDSP");
    LOGF("[TTS] BASS: StreamCreate=%p PutData=%p", pBASSStreamCreate, pBASSStreamPutData);

    // Dobby untuk hooks
    void* hDobby = dlopen("libdobby.so", RTLD_NOW|RTLD_GLOBAL);
    if (!hDobby) { LOGF("[TTS] WARNING: libdobby not found, mic inject disabled"); }
    else {
        pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby,"DobbySymbolResolver");
        pDobbyHook           = (void*(*)(void*,void*,void**))dlsym(hDobby,"DobbyHook");

        if (pDobbySymbolResolver && pDobbyHook) {
            // Hook BASS_RecordStart
            void* addrRec = pDobbySymbolResolver("libBASS.so","BASS_RecordStart");
            if (addrRec) {
                pDobbyHook(addrRec,(void*)hook_BASSRecordStart,(void**)&orig_BASSRecordStart);
                LOGF("[TTS] BASS_RecordStart hooked");
            }
            // Hook BASS_ChannelGetData
            void* addrGet = pDobbySymbolResolver("libBASS.so","BASS_ChannelGetData");
            if (addrGet) {
                pDobbyHook(addrGet,(void*)hook_BASSChannelGetData,(void**)&orig_BASSChannelGetData);
                LOGF("[TTS] BASS_ChannelGetData hooked");
            }
            // Hook BASS_ChannelIsActive
            void* addrActive = dlsym(hBASS,"BASS_ChannelIsActive");
            if (addrActive) {
                pDobbyHook(addrActive,(void*)hook_BASSChannelIsActive,(void**)&orig_BASSChannelIsActive);
                LOGF("[TTS] BASS_ChannelIsActive hooked");
            }
            // Hook BASS_ChannelPause
            void* addrPause = pDobbySymbolResolver("libBASS.so","BASS_ChannelPause");
            if (addrPause) {
                pDobbyHook(addrPause,(void*)hook_BASSChannelPause,(void**)&orig_BASSChannelPause);
                LOGF("[TTS] BASS_ChannelPause hooked");
            }
        } else {
            LOGF("[TTS] WARNING: Dobby symbols not found, mic inject disabled");
        }
    }

    // Tulis addr API untuk Lua
    FILE* af = fopen(ADDR_FILE,"w");
    if (af) { fprintf(af,"%lu\n",(unsigned long)&tts_api); fclose(af); }

    pthread_t thr;
    pthread_create(&thr, nullptr, tts_init_thread, nullptr);
    pthread_detach(thr);

    LOGF("[TTS] OnModLoad done");
}

EXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    LOGF("[TTS] JNI_OnLoad vm=%p", vm);
    return JNI_VERSION_1_6;
}

} // extern "C"
