# libvoicetts-jni

TTS mod untuk SA-MP Android (MoNetLoader/AML) menggunakan **Google TTS engine** via JNI.

## Fitur
- Google TTS neural voice — jauh lebih natural dari espeak
- Output audio via BASS push stream (speaker game)
- Tidak butuh Termux, tidak inject mic/SampVoice
- Lua UI: pitch, rate, volume, language slider
- Config tersimpan otomatis (inicfg)

## Alur
```
Lua speak(text)
  → libvoicetts.so
  → JNI android.speech.tts.TextToSpeech
  → Google TTS engine
  → PCM audio
  → BASS_StreamPutData
  → speaker game
```

## Build

### GitHub Actions (otomatis)
Push ke `main` → Actions build → download artifact `libvoicetts-armeabi-v7a`

### Manual (Termux)
```bash
pkg install ndk
cd jni
ndk-build NDK_PROJECT_PATH=.. NDK_APPLICATION_MK=Application.mk
```

## Install
1. Copy `libs/armeabi-v7a/libvoicetts.so` ke folder AML mods
2. Copy `voicetts.lua` ke `/storage/emulated/0/Android/media/com.sampmobilerp.game/monetloader/monet/`
3. Masuk game — tunggu `[TTS] OK — Google TTS ready`

## Commands
| Command | Fungsi |
|---|---|
| `/tts <text>` | Speak teks |
| `/tts on/off` | Toggle TTS |
| `/ttsui` | Panel settings |
| `/ttsinput` | Input window |
| `/ttspitch 1.2` | Set pitch |
| `/ttsrate 1.5` | Set rate |
| `/ttsvol 80` | Set volume |
| `/ttslang en` | Ganti bahasa |

## Requirements
- Android 5.0+ (API 21)
- Google TTS app terinstall (biasanya sudah default)
- SA-MP Mobile + MoNetLoader + AML
