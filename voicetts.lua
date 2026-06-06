-- voicetts.lua v2.0 by brruham
-- Google TTS via JNI (libvoicetts.so) + BASS speaker output
-- Tidak butuh Termux, tidak inject mic

local ffi    = require("ffi")
local imgui  = require("mimgui")
local inicfg = require("inicfg")
local new    = imgui.new

ffi.cdef[[
    typedef struct {
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
    } TtsAPI;
]]

-- ============================================================
-- Config
-- ============================================================
local CFG_FILE  = "voicetts"
local ADDR_FILE = "/storage/emulated/0/voicetts_addr.txt"

local ini = inicfg.load({
    settings = {
        enabled  = true,
        pitch    = 1.0,
        rate     = 1.0,
        volume   = 100,
        lang     = "id",
    }
}, CFG_FILE)

local function saveConfig()
    inicfg.save(ini, CFG_FILE)
end

-- ============================================================
-- State
-- ============================================================
local tts          = nil
local showSettings = new.bool(false)
local showInput    = new.bool(false)
local inputBuf     = new.char[512]()
local chkEnabled   = new.bool(ini.settings.enabled ~= false)
local sliderPitch  = new.float(ini.settings.pitch  or 1.0)
local sliderRate   = new.float(ini.settings.rate   or 1.0)
local sliderVol    = new.int(ini.settings.volume   or 100)

local LANGS     = { "id", "en", "ms", "jv", "su", "en-US", "en-GB" }
local langItems = ffi.new("const char*[?]", #LANGS)
for i, v in ipairs(LANGS) do langItems[i-1] = v end

local langIdx = new.int(0)
for i, v in ipairs(LANGS) do
    if v == (ini.settings.lang or "id") then langIdx[0] = i - 1; break end
end

local sizeX, sizeY = getScreenResolution()

-- ============================================================
-- Speak
-- ============================================================
local function speak(text)
    if not tts or not chkEnabled[0] then return end
    if not tts.is_ready() then
        sampAddChatMessage("[TTS] Belum ready, tunggu sebentar...", 0xFFAA00)
        return
    end
    text = text:match("^%s*(.-)%s*$")
    if #text == 0 then return end
    tts.speak(text)
    sampAddChatMessage("[TTS] >> " .. text, 0xAAAAAA)
end

-- ============================================================
-- ImGui Style
-- ============================================================
imgui.OnInitialize(function()
    imgui.GetIO().IniFilename = nil
    local s = imgui.GetStyle()
    s.WindowRounding = 6.0
    s.FrameRounding  = 4.0
    s.Colors[imgui.Col.WindowBg]      = imgui.ImVec4(0.08, 0.08, 0.12, 0.96)
    s.Colors[imgui.Col.TitleBgActive] = imgui.ImVec4(0.12, 0.40, 0.75, 1.00)
    s.Colors[imgui.Col.Button]        = imgui.ImVec4(0.15, 0.42, 0.78, 1.00)
    s.Colors[imgui.Col.ButtonHovered] = imgui.ImVec4(0.22, 0.55, 0.90, 1.00)
    s.Colors[imgui.Col.FrameBg]       = imgui.ImVec4(0.14, 0.14, 0.20, 1.00)
    s.Colors[imgui.Col.SliderGrab]    = imgui.ImVec4(0.22, 0.60, 1.00, 1.00)
    s.Colors[imgui.Col.CheckMark]     = imgui.ImVec4(0.22, 0.80, 1.00, 1.00)
end)

-- ============================================================
-- Settings Window
-- ============================================================
imgui.OnFrame(
    function() return showSettings[0] end,
    function()
        imgui.SetNextWindowSize(imgui.ImVec2(290, 0), imgui.Cond.Always)
        imgui.SetNextWindowPos(
            imgui.ImVec2(sizeX * 0.02, sizeY * 0.15),
            imgui.Cond.FirstUseEver)
        imgui.Begin("VoiceTTS - Settings", showSettings,
            imgui.WindowFlags.NoResize + imgui.WindowFlags.NoScrollbar)

        local ttsOn   = chkEnabled[0]
        local isReady = tts and tts.is_ready() == 1
        imgui.TextColored(
            ttsOn and imgui.ImVec4(0.3,1,0.3,1) or imgui.ImVec4(0.7,0.7,0.7,1),
            "TTS: " .. (ttsOn and "ON" or "OFF"))
        imgui.SameLine()
        imgui.TextColored(
            isReady and imgui.ImVec4(0.3,1,0.3,1) or imgui.ImVec4(1,0.6,0.1,1),
            "  Engine: " .. (isReady and "Ready" or "Init..."))
        imgui.Separator()

        if imgui.Button(ttsOn and "TTS OFF" or "TTS ON", imgui.ImVec2(270, 30)) then
            chkEnabled[0] = not ttsOn
            ini.settings.enabled = chkEnabled[0]
            saveConfig()
            if tts then
                if chkEnabled[0] then tts.enable() else tts.disable() end
            end
            if not chkEnabled[0] then showInput[0] = false end
            sampAddChatMessage("[TTS] " .. (chkEnabled[0] and "ON" or "OFF"),
                chkEnabled[0] and 0x00FF88 or 0xFF8800)
        end

        imgui.Spacing(); imgui.Separator()
        imgui.PushItemWidth(265)

        imgui.Text("Pitch:")
        if imgui.SliderFloat("##pitch", sliderPitch, 0.1, 3.0, "%.1f") then
            ini.settings.pitch = sliderPitch[0]; saveConfig()
            -- pitch diapply saat speak() dipanggil
        end

        imgui.Text("Rate (kecepatan):")
        if imgui.SliderFloat("##rate", sliderRate, 0.1, 3.0, "%.1f") then
            ini.settings.rate = sliderRate[0]; saveConfig()
        end

        imgui.Text("Volume:")
        if imgui.SliderInt("##vol", sliderVol, 0, 100) then
            ini.settings.volume = sliderVol[0]; saveConfig()
            if tts then tts.set_volume(sliderVol[0]) end
        end

        imgui.Spacing()
        imgui.Text("Language:")
        if imgui.Combo("##lang", langIdx, langItems, #LANGS) then
            local lang = LANGS[langIdx[0] + 1]
            ini.settings.lang = lang; saveConfig()
            if tts then tts.set_lang(lang) end
            sampAddChatMessage("[TTS] Lang=" .. lang, 0x00FFFF)
        end

        imgui.Spacing(); imgui.Separator()

        if imgui.Button(showInput[0] and "Tutup Input" or "Buka Input",
                imgui.ImVec2(270, 28)) then
            if chkEnabled[0] then
                showInput[0] = not showInput[0]
            else
                sampAddChatMessage("[TTS] Aktifkan TTS dulu", 0xFFAA00)
            end
        end

        imgui.PopItemWidth()
        imgui.End()
    end
)

-- ============================================================
-- Input Window
-- ============================================================
imgui.OnFrame(
    function() return showInput[0] end,
    function()
        imgui.SetNextWindowSize(imgui.ImVec2(340, 52), imgui.Cond.Always)
        imgui.SetNextWindowPos(
            imgui.ImVec2(sizeX * 0.5 - 170, sizeY * 0.25),
            imgui.Cond.FirstUseEver)
        imgui.SetNextWindowBgAlpha(0.90)
        imgui.Begin("##ttsinput", nil,
            imgui.WindowFlags.NoResize    +
            imgui.WindowFlags.NoScrollbar +
            imgui.WindowFlags.NoTitleBar  +
            imgui.WindowFlags.NoCollapse)
        imgui.PushItemWidth(290)
        local submitted = imgui.InputText("##inp", inputBuf, 512,
            imgui.InputTextFlags.EnterReturnsTrue)
        imgui.PopItemWidth()
        imgui.SameLine()
        if submitted or imgui.Button(">>", imgui.ImVec2(36, 0)) then
            speak(ffi.string(inputBuf))
            ffi.fill(inputBuf, ffi.sizeof(inputBuf))
            imgui.SetKeyboardFocusHere(-1)
        end
        imgui.End()
    end
)

-- ============================================================
-- Main
-- ============================================================
function main()
    while not isSampAvailable() do wait(100) end
    wait(2000)

    sampAddChatMessage("[VoiceTTS] v2.0 loading...", 0xFFFF00)

    -- Tunggu addr file dari .so (max 10 detik)
    for i = 1, 10 do
        local f = io.open(ADDR_FILE, "r")
        if f then
            local addr = tonumber(f:read("*l")); f:close()
            if addr and addr ~= 0 then
                local ok, api = pcall(function()
                    return ffi.cast("TtsAPI*", addr)
                end)
                if ok and api then
                    tts = api
                    os.remove(ADDR_FILE)
                    break
                end
            end
        end
        wait(1000)
    end

    if not tts then
        sampAddChatMessage("[TTS] GAGAL load engine!", 0xFF4444)
        while true do wait(1000) end
    end

    -- Apply saved config
    tts.set_pitch(sliderPitch[0])
    tts.set_rate(sliderRate[0])
    tts.set_volume(sliderVol[0])
    tts.set_lang(ini.settings.lang or "id")
    if chkEnabled[0] then tts.enable() else tts.disable() end

    -- Tunggu engine ready (max 5 detik)
    for i = 1, 10 do
        if tts.is_ready() == 1 then break end
        wait(500)
    end

    if tts.is_ready() == 1 then
        sampAddChatMessage("[TTS] OK — /ttsui | Google TTS ready", 0x00FF88)
    else
        sampAddChatMessage("[TTS] WARNING: engine lambat init, coba speak nanti", 0xFFAA00)
    end

    -- Commands
    sampRegisterChatCommand("ttsui", function()
        showSettings[0] = not showSettings[0]
    end)

    sampRegisterChatCommand("tts", function(arg)
        if arg == "on" then
            chkEnabled[0] = true; tts.enable()
            ini.settings.enabled = true; saveConfig()
            sampAddChatMessage("[TTS] ON", 0x00FF88)
        elseif arg == "off" then
            chkEnabled[0] = false; tts.disable()
            showInput[0]  = false
            ini.settings.enabled = false; saveConfig()
            sampAddChatMessage("[TTS] OFF", 0xFF8800)
        elseif arg and #arg > 0 then
            speak(arg)
        else
            sampAddChatMessage("[TTS] /tts <text>|on|off | /ttsui", 0xFFFF00)
        end
    end)

    sampRegisterChatCommand("ttsinput", function()
        if chkEnabled[0] then
            showInput[0] = not showInput[0]
        else
            sampAddChatMessage("[TTS] Aktifkan TTS dulu", 0xFFAA00)
        end
    end)

    sampRegisterChatCommand("ttspitch", function(arg)
        local v = tonumber(arg)
        if v and tts then
            sliderPitch[0] = v
            ini.settings.pitch = v; saveConfig()
            sampAddChatMessage("[TTS] Pitch=" .. string.format("%.1f", v), 0x00FFFF)
        end
    end)

    sampRegisterChatCommand("ttsrate", function(arg)
        local v = tonumber(arg)
        if v and tts then
            sliderRate[0] = v
            ini.settings.rate = v; saveConfig()
            sampAddChatMessage("[TTS] Rate=" .. string.format("%.1f", v), 0x00FFFF)
        end
    end)

    sampRegisterChatCommand("ttsvol", function(arg)
        local v = tonumber(arg)
        if v and tts then
            sliderVol[0] = math.floor(v)
            tts.set_volume(math.floor(v))
            ini.settings.volume = math.floor(v); saveConfig()
            sampAddChatMessage("[TTS] Vol=" .. math.floor(v), 0x00FFFF)
        end
    end)

    sampRegisterChatCommand("ttslang", function(arg)
        if arg and #arg > 0 and tts then
            tts.set_lang(arg)
            ini.settings.lang = arg; saveConfig()
            for i, v in ipairs(LANGS) do
                if v == arg then langIdx[0] = i - 1; break end
            end
            sampAddChatMessage("[TTS] Lang=" .. arg, 0x00FFFF)
        else
            sampAddChatMessage("[TTS] /ttslang id|en|ms|en-US|...", 0xFFFF00)
        end
    end)

    while true do wait(1000) end
end
