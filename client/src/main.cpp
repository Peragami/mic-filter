// MicFilter dedicated client -- tiny Win32 GUI around WasapiEngine.
// No framework: plain Win32 controls, WASAPI does the audio, the DSP
// chain is embedded in the engine (v0.7+). Mic level lives in
// mic_level.cpp, passive auto calibration in passive_calibration.cpp,
// presets in preset_*.cpp, filter settings window in settings_window.cpp,
// shared UI bits in ui_shared.h. See README.md for usage.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include "passive_calibration.h"
#include "preset.h"
#include "settings_window.h"
#include "log.h"
#include "updater.h"
#include "vst3/source/version.h" // STRING_VERSION
#include "mic_level.h"
#include "ui_shared.h"
#include "vst3/source/paramids.h" // MicFilterParams::ParamID (shared with the plugin)
#include "wasapi_engine.h"

// The live preset (v0.14): filter on/off + stage order come from here.
// Stored as JSON under <exe dir>\presets and edited in the settings
// window; the main window only picks which preset is active.
Preset g_currentPreset = DefaultPreset();
std::string g_soloKey; // settings-window solo audition: only this filter runs

void ApplyPresetLive();              // defined below
void RefreshPresetCombo (HWND hwnd); // defined below

WasapiEngine g_engine;
std::vector<AudioDeviceInfo> g_captureDevices;
std::vector<AudioDeviceInfo> g_renderDevices;
float g_meterDisplay = 0.0f;
bool g_running = false;

// Output volume (v0.25): post-chain makeup gain, -12..+12 dB in 0.5 dB
// steps. Global (not per-preset), persisted in settings.ini next to
// the exe so it survives restarts and auto-updates.
double g_outGainDb = 0.0;

std::wstring SettingsIniPath()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW (nullptr, exe, MAX_PATH);
    std::wstring p (exe);
    const size_t slash = p.find_last_of (L"\\/");
    if (slash != std::wstring::npos) p.resize (slash + 1);
    return p + L"settings.ini";
}
void LoadOutGain()
{
    const int x2 = GetPrivateProfileIntW (L"client", L"OutGainDbX2", 0,
                                          SettingsIniPath().c_str());
    g_outGainDb = std::max (-12.0, std::min (12.0, x2 / 2.0));
}
void SaveOutGain()
{
    wchar_t buf[16];
    swprintf_s (buf, L"%d", (int) std::lround (g_outGainDb * 2));
    WritePrivateProfileStringW (L"client", L"OutGainDbX2", buf,
                                SettingsIniPath().c_str());
}
void ApplyOutGain (HWND hwnd)
{
    const LRESULT pos = SendMessageW (Ctl (hwnd, IDC_OUT_LEVEL), TBM_GETPOS, 0, 0);
    g_outGainDb = (int) pos / 2.0 - 12.0;
    wchar_t buf[32];
    if (g_outGainDb == 0.0) swprintf_s (buf, L"0 dB");
    else swprintf_s (buf, L"%+.1f dB", g_outGainDb);
    SetWindowTextW (Ctl (hwnd, IDC_OUT_LEVEL_VALUE), buf);
    g_engine.SetParameter ((unsigned int) MicFilterParams::kOutGain,
                           MicFilterParams::toNormalized (MicFilterParams::kOutGain,
                                                          (float) g_outGainDb));
    SaveOutGain();
}

// Device hot-plug tracking (IMMNotificationClient).
IMMDeviceEnumerator* g_deviceEnumerator = nullptr;
IMMNotificationClient* g_notificationClient = nullptr;

// Posts WM_DEVICECHANGE_MSG to the window whenever an audio endpoint is
// added / removed / changes state / the default changes. Callbacks arrive
// on a COM thread, so this only ever posts a message.
class DeviceNotificationClient final : public IMMNotificationClient
{
public:
    explicit DeviceNotificationClient (HWND hwnd) : hwnd_ (hwnd) {}
    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof (IMMNotificationClient))
        {
            *ppv = static_cast<IMMNotificationClient*> (this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG v = (ULONG) --ref_;
        if (v == 0) delete this;
        return v;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged (LPCWSTR, DWORD) override { Notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded (LPCWSTR) override { Notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved (LPCWSTR) override { Notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged (EDataFlow, ERole, LPCWSTR) override { Notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged (LPCWSTR, const PROPERTYKEY) override { return S_OK; }
private:
    void Notify() { if (hwnd_) PostMessageW (hwnd_, WM_DEVICECHANGE_MSG, 0, 0); }
    std::atomic<long> ref_ { 1 };
    HWND hwnd_;
};

// Rebuild both combos after a hot-plug notification, keeping the current
// selection by device id where the device still exists.
void LogDeviceList()
{
    miclog::Write ("app: capture devices: %d", (int) g_captureDevices.size());
    for (const auto& d : g_captureDevices)
        miclog::WriteW (L"  in: %s [%s]", d.name.c_str(), d.id.empty() ? L"default" : d.id.c_str());
    miclog::Write ("app: render devices: %d", (int) g_renderDevices.size());
    for (const auto& d : g_renderDevices)
        miclog::WriteW (L"  out: %s [%s]", d.name.c_str(), d.id.empty() ? L"default" : d.id.c_str());
}

void ReenumerateDevices (HWND hwnd)
{
    const std::wstring capId = SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices);
    const std::wstring renId = SelectedDeviceId (Ctl (hwnd, IDC_OUTPUT_COMBO), g_renderDevices);
    g_captureDevices = EnumerateCaptureDevices();
    g_renderDevices = EnumerateRenderDevices();
    LogDeviceList();
    SendMessageW (Ctl (hwnd, IDC_INPUT_COMBO), CB_RESETCONTENT, 0, 0);
    SendMessageW (Ctl (hwnd, IDC_OUTPUT_COMBO), CB_RESETCONTENT, 0, 0);
    FillDeviceCombo (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices, capId);
    FillDeviceCombo (Ctl (hwnd, IDC_OUTPUT_COMBO), g_renderDevices, renId);
    ReleaseMicVolumeIfNotCurrent (SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices));
}

int SelectedBlockFrames (HWND hwnd)
{
    const int values[] = { 32, 64, 128, 256 };
    const LRESULT sel = SendMessageW (Ctl (hwnd, IDC_BUFFER_COMBO), CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 3) return ClientConfig::preferredBufferSamples;
    return values[sel];
}

void SelectBlockFrames (HWND hwnd, int frames)
{
    const int values[] = { 32, 64, 128, 256 };
    for (int i = 0; i < 4; ++i)
        if (values[i] == frames)
            SendMessageW (Ctl (hwnd, IDC_BUFFER_COMBO), CB_SETCURSEL, i, 0);
}

const wchar_t* ModeName (ClientConfig::Mode m)
{
    switch (m)
    {
        case ClientConfig::Mode::WasapiExclusive: return L"WASAPI排他";
        case ClientConfig::Mode::WasapiLowLatencyShared: return L"低遅延共有 (IAudioClient3)";
        default: return L"通常共有";
    }
}

// The engine runs from launch until the window closes (v0.11: the
// start/stop button is gone). Device / buffer changes restart it.
constexpr UINT WM_AUTOSTART_MSG = WM_APP + 6;
constexpr UINT WM_RESTART_MSG = WM_APP + 7;
bool g_restartPending = false;

void ScheduleRestart (HWND hwnd)
{
    if (g_restartPending) return;
    g_restartPending = true;
    PostMessageW (hwnd, WM_RESTART_MSG, 0, 0);
}

// Toggle buttons (BS_PUSHLIKE) show their state in the caption too:
// a filled dot when on, an open dot when off.
void SetToggleText (HWND button, bool on, const wchar_t* label)
{
    const std::wstring text = std::wstring (on ? L"● " : L"○ ") + label;
    SetWindowTextW (button, text.c_str());
}

void UpdateStatus (HWND hwnd, const std::wstring& prefix)
{
    std::wstring text;
    if (g_running)
    {
        const int block = g_engine.ActiveBlockFrames();
        wchar_t buf[512];
        swprintf_s (buf,
                    L"動作中 / %s / バッファ %d（往復目安 約 %.1f ms）/ XRUN %llu",
                    ModeName (g_engine.ActiveMode()), block,
                    ClientConfig::estimatedRoundTripMs (block),
                    g_engine.XrunCount());
        text = buf;
    }
    else text = L"停止中";
    if (!prefix.empty()) text = prefix + L" / " + text;
    SetWindowTextW (Ctl (hwnd, IDC_STATUS), text.c_str());
}

void ApplyPresetLive()
{
    for (int i = 0; i < kNumFilterInfos; ++i)
    {
        const FilterInfo& f = kFilterRegistry[i];
        const auto it = g_currentPreset.on.find (f.key);
        bool on = it != g_currentPreset.on.end() && it->second;
        if (!g_soloKey.empty()) on = (f.key == g_soloKey); // solo audition override
        g_engine.SetParameter ((unsigned int) f.paramId, on ? 1.0 : 0.0);
    }
    for (int i = 0; i < kNumKnobs; ++i)
    {
        const KnobDef& k = kKnobs[i];
        const auto it = g_currentPreset.values.find (k.valueKey);
        const float plain = it != g_currentPreset.values.end() ? (float) it->second
                            : MicFilterParams::rangeFor (k.paramId).def;
        g_engine.SetParameter ((unsigned int) k.paramId,
                               MicFilterParams::toNormalized (k.paramId, plain));
    }
    g_engine.SetStageOrder (StageOrderIds (g_currentPreset));
    miclog::Write ("preset: applied '%s' live", WideToUtf8 (g_currentPreset.name).c_str());
}

// (Re)fill the preset combo from the presets folder. Keeps the current
// preset selected when it still exists; loads the selection into
// g_currentPreset when it changed or was never loaded.
void RefreshPresetCombo (HWND hwnd)
{
    EnsurePresetStore();
    const std::vector<std::wstring> names = ListPresetNames();
    HWND combo = Ctl (hwnd, IDC_PRESET_COMBO);
    SendMessageW (combo, CB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t i = 0; i < names.size(); ++i)
    {
        SendMessageW (combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM> (names[i].c_str()));
        if (names[i] == g_currentPreset.name) select = (int) i;
    }
    if (!names.empty())
    {
        SendMessageW (combo, CB_SETCURSEL, select, 0);
        Preset p;
        if (LoadPreset (names[(size_t) select], p)) g_currentPreset = p;
    }
}

bool StartEngine (HWND hwnd)
{
    EngineConfig cfg;
    cfg.inputDeviceId = SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices);
    cfg.outputDeviceId = SelectedDeviceId (Ctl (hwnd, IDC_OUTPUT_COMBO), g_renderDevices);
    cfg.blockFrames = SelectedBlockFrames (hwnd);
    for (int i = 0; i < kNumFilterInfos; ++i)
    {
        const FilterInfo& f = kFilterRegistry[i];
        const auto it = g_currentPreset.on.find (f.key);
        cfg.initialParams.push_back ({ (unsigned int) f.paramId,
                                       it != g_currentPreset.on.end() && it->second ? 1.0 : 0.0 });
    }
    for (int i = 0; i < kNumKnobs; ++i)
    {
        const KnobDef& k = kKnobs[i];
        const auto it = g_currentPreset.values.find (k.valueKey);
        const float plain = it != g_currentPreset.values.end() ? (float) it->second
                            : MicFilterParams::rangeFor (k.paramId).def;
        cfg.initialParams.push_back ({ (unsigned int) k.paramId,
                                       (double) MicFilterParams::toNormalized (k.paramId, plain) });
    }
    cfg.initialParams.push_back ({ (unsigned int) MicFilterParams::kOutGain,
                                   (double) MicFilterParams::toNormalized (
                                       MicFilterParams::kOutGain, (float) g_outGainDb) });
    cfg.initialStageOrder = StageOrderIds (g_currentPreset);
    std::wstring err;
    if (!g_engine.Start (cfg, err))
    {
        miclog::WriteW (L"app: start failed: %s", err.c_str());
        g_running = false;
        SetWindowTextW (Ctl (hwnd, IDC_STATUS),
                        (L"開始できませんでした：" + err).c_str());
        return false;
    }
    miclog::Write ("app: started (mode=%d block=%d)", (int) g_engine.ActiveMode(),
                   g_engine.ActiveBlockFrames());
    if (IsChecked (hwnd, IDC_DRY_CHECK)) g_engine.SetBypass (true);
    PassiveCalOnStart (hwnd);
    g_running = true;
    UpdateStatus (hwnd);
    return true;
}

void StopEngine (HWND hwnd)
{
    miclog::Write ("app: stopping (xruns=%llu)", g_engine.XrunCount());
    g_engine.Stop();
    g_running = false;
    SendMessageW (Ctl (hwnd, IDC_METER), PBM_SETPOS, 0, 0);
    g_meterDisplay = 0.0f;
    UpdateStatus (hwnd);
}

LRESULT CALLBACK WndProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            HFONT font = (HFONT) GetStockObject (DEFAULT_GUI_FONT);
            auto make = [&] (HWND h) { SendMessageW (h, WM_SETFONT, (WPARAM) font, TRUE); return h; };

            AddLabel (hwnd, L"入力デバイス（マイク）", 16, 14, 300);
            make (AddCombo (hwnd, IDC_INPUT_COMBO, 16, 36, 470));
            AddLabel (hwnd, L"出力デバイス", 16, 76, 300);
            make (AddCombo (hwnd, IDC_OUTPUT_COMBO, 16, 98, 470));
            AddLabel (hwnd, L"バッファ（サンプル数 @48kHz）", 16, 138, 300);
            HWND buf = make (AddCombo (hwnd, IDC_BUFFER_COMBO, 16, 160, 140));
            for (const wchar_t* s : { L"32", L"64", L"128", L"256" })
                SendMessageW (buf, CB_ADDSTRING, 0, reinterpret_cast<LPARAM> (s));
            SendMessageW (buf, CB_SETCURSEL, 1, 0); // 64 = default (ClientConfig)
            AddLabel (hwnd, L"音質モード：低遅延（AIなし）", 176, 162, 300);

            AddLabel (hwnd, L"Windows マイクレベル（目安50／最大声でピーク -12〜-6dB）",
                      16, 192, 470);
            HWND level = make (CreateWindowW (TRACKBAR_CLASSW, L"",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS,
                                              16, 214, 380, 28, hwnd,
                                              reinterpret_cast<HMENU> ((INT_PTR) IDC_MIC_LEVEL),
                                              nullptr, nullptr));
            SendMessageW (level, TBM_SETRANGE, TRUE, MAKELPARAM (0, 100));
            SendMessageW (level, TBM_SETPAGESIZE, 0, 10);
            SendMessageW (level, TBM_SETPOS, TRUE, 50); // initial value 50 (docs/gain-staging.md)
            make (CreateWindowW (L"STATIC", L"50", WS_CHILD | WS_VISIBLE,
                                 406, 218, 80, 20, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_MIC_LEVEL_VALUE),
                                 nullptr, nullptr));

            HWND agc = make (CreateWindowW (L"BUTTON", L"自動補正",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                            16, 257, 110, 24, hwnd,
                                            reinterpret_cast<HMENU> ((INT_PTR) IDC_AUTO_GAIN_CHECK),
                                            nullptr, nullptr));
            SendMessageW (agc, BM_SETCHECK, BST_CHECKED, 0); // passive calibration on by default
            make (CreateWindowW (L"BUTTON", L"停止",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 134, 257, 64, 24, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_STOP_CHECK),
                                 nullptr, nullptr));
            HWND meter = CreateWindowW (PROGRESS_CLASSW, L"",
                                        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                        206, 258, 280, 20, hwnd,
                                        reinterpret_cast<HMENU> ((INT_PTR) IDC_METER),
                                        nullptr, nullptr);
            SendMessageW (meter, PBM_SETRANGE, 0, MAKELPARAM (0, 1000));
            make (CreateWindowW (L"STATIC", L"停止中", WS_CHILD | WS_VISIBLE,
                                 16, 294, 470, 40, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_STATUS),
                                 nullptr, nullptr));

            AddLabel (hwnd, L"プリセット（フィルター設定は別ウィンドウで）", 16, 342, 470);
            make (AddCombo (hwnd, IDC_PRESET_COMBO, 16, 364, 300));
            make (CreateWindowW (L"BUTTON", L"フィルター設定…", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 326, 362, 160, 26, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_SETTINGS_BUTTON),
                                 nullptr, nullptr));
            {
                HWND dry = make (CreateWindowW (L"BUTTON", L"",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                                                16, 398, 470, 24, hwnd,
                                                reinterpret_cast<HMENU> ((INT_PTR) IDC_DRY_CHECK),
                                                nullptr, nullptr));
                SetToggleText (dry, false, L"ドライ（全フィルターOFFの音を聞く）");
            }

            LoadOutGain();
            AddLabel (hwnd, L"出力音量（最終音量。上げすぎると大きな声が歪みます）", 16, 430, 470);
            {
                HWND lvl = make (CreateWindowW (TRACKBAR_CLASSW, L"",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS,
                                                16, 452, 380, 28, hwnd,
                                                reinterpret_cast<HMENU> ((INT_PTR) IDC_OUT_LEVEL),
                                                nullptr, nullptr));
                SendMessageW (lvl, TBM_SETRANGE, TRUE, MAKELPARAM (0, 48)); // -12..+12 dB, 0.5 steps
                SendMessageW (lvl, TBM_SETPAGESIZE, 0, 4);
                SendMessageW (lvl, TBM_SETPOS, TRUE, (LPARAM) std::lround ((g_outGainDb + 12.0) * 2));
                wchar_t buf[32];
                if (g_outGainDb == 0.0) swprintf_s (buf, L"0 dB");
                else swprintf_s (buf, L"%+.1f dB", g_outGainDb);
                make (CreateWindowW (L"STATIC", buf, WS_CHILD | WS_VISIBLE,
                                     406, 456, 80, 20, hwnd,
                                     reinterpret_cast<HMENU> ((INT_PTR) IDC_OUT_LEVEL_VALUE),
                                     nullptr, nullptr));
            }

            g_captureDevices = EnumerateCaptureDevices();
            g_renderDevices = EnumerateRenderDevices();
            LogDeviceList();
            FillDeviceCombo (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices);
            FillDeviceCombo (Ctl (hwnd, IDC_OUTPUT_COMBO), g_renderDevices);

            // Hot-plug notifications -> WM_DEVICECHANGE_MSG -> re-enumerate.
            if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                             __uuidof (IMMDeviceEnumerator),
                                             reinterpret_cast<void**> (&g_deviceEnumerator))) &&
                g_deviceEnumerator)
            {
                g_notificationClient = new DeviceNotificationClient (hwnd);
                g_deviceEnumerator->RegisterEndpointNotificationCallback (g_notificationClient);
            }
            {
                wchar_t vbuf[64];
                swprintf_s (vbuf, L"バージョン %hs", STRING_VERSION);
                make (CreateWindowW (L"STATIC", vbuf, WS_CHILD | WS_VISIBLE,
                                     16, 496, 200, 20, hwnd,
                                     reinterpret_cast<HMENU> ((INT_PTR) IDC_VERSION_LABEL),
                                     nullptr, nullptr));
                make (CreateWindowW (L"BUTTON", L"更新を確認", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     366, 492, 120, 26, hwnd,
                                     reinterpret_cast<HMENU> ((INT_PTR) IDC_UPDATE_BUTTON),
                                     nullptr, nullptr));
            }
            RefreshPresetCombo (hwnd); // also loads g_currentPreset
            SetTimer (hwnd, kTimerId, 50, nullptr);
            CheckForUpdateAsync (hwnd, false); // silent auto-check on launch
            PostMessageW (hwnd, WM_AUTOSTART_MSG, 0, 0); // engine always on
            return 0;
        }
        case WM_AUTOSTART_MSG:
            if (!IsChecked (hwnd, IDC_STOP_CHECK)) StartEngine (hwnd);
            return 0;
        case WM_RESTART_MSG:
            g_restartPending = false;
            StopEngine (hwnd);
            if (!IsChecked (hwnd, IDC_STOP_CHECK)) StartEngine (hwnd);
            return 0;
        case WM_DEVICECHANGE_MSG:
            miclog::Write ("app: audio device change notification");
            ReenumerateDevices (hwnd);
            return 0;
        case WM_UPDATE_AVAILABLE:
        {
            auto* offer = reinterpret_cast<UpdateOffer*> (lp);
            wchar_t buf[1024];
            const std::wstring ver = Utf8ToWide (offer->info.version);
            const std::wstring notes = Utf8ToWide (offer->info.notes);
            swprintf_s (buf, L"新しいバージョン %s があります。\n%s\n\n今すぐ更新しますか？（アプリは一度終了して再起動します）",
                        ver.c_str(), notes.c_str());
            const int answer = MessageBoxW (hwnd, buf, L"MicFilter 更新",
                                            MB_YESNO | MB_ICONINFORMATION);
            if (answer == IDYES)
            {
                UpdateStatus (hwnd, L"更新をダウンロード中…");
                DownloadAndApplyAsync (hwnd, offer->info);
            }
            delete offer;
            return 0;
        }
        case WM_UPDATE_UPTODATE:
            UpdateStatus (hwnd, L"お使いのバージョンは最新です");
            return 0;
        case WM_UPDATE_FAILED:
        {
            auto* message = reinterpret_cast<std::wstring*> (lp);
            MessageBoxW (hwnd, message->c_str(), L"更新エラー", MB_OK | MB_ICONWARNING);
            delete message;
            return 0;
        }
        case WM_UPDATE_APPLY:
            if (g_running) StopEngine (hwnd);
            DestroyWindow (hwnd);
            return 0;
        case WM_HSCROLL:
            if ((HWND) lp == Ctl (hwnd, IDC_MIC_LEVEL))
            {
                ApplyMicLevel (hwnd);
                return 0;
            }
            if ((HWND) lp == Ctl (hwnd, IDC_OUT_LEVEL))
            {
                ApplyOutGain (hwnd);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (LOWORD (wp) == IDC_INPUT_COMBO && HIWORD (wp) == CBN_SELCHANGE)
            {
                RefreshMicLevelFromDevice (hwnd);
                ScheduleRestart (hwnd);
                return 0;
            }
            if ((LOWORD (wp) == IDC_OUTPUT_COMBO || LOWORD (wp) == IDC_BUFFER_COMBO) &&
                HIWORD (wp) == CBN_SELCHANGE)
            {
                ScheduleRestart (hwnd);
                return 0;
            }
            if (LOWORD (wp) == IDC_STOP_CHECK)
            {
                if (IsChecked (hwnd, IDC_STOP_CHECK))
                {
                    if (g_running) StopEngine (hwnd);
                    miclog::Write ("app: stopped by user checkbox");
                }
                else
                {
                    StartEngine (hwnd);
                    miclog::Write ("app: resumed by user checkbox");
                }
                return 0;
            }
            if (LOWORD (wp) == IDC_UPDATE_BUTTON)
            {
                UpdateStatus (hwnd, L"更新を確認中…");
                CheckForUpdateAsync (hwnd, true);
                return 0;
            }
            if (LOWORD (wp) == IDC_AUTO_GAIN_CHECK)
            {
                miclog::Write ("app: passive auto calibration = %d",
                               IsChecked (hwnd, IDC_AUTO_GAIN_CHECK) ? 1 : 0);
                return 0;
            }
            if (LOWORD (wp) == IDC_DRY_CHECK)
            {
                const bool dry = IsChecked (hwnd, IDC_DRY_CHECK);
                g_engine.SetBypass (dry);
                SetToggleText (Ctl (hwnd, IDC_DRY_CHECK), dry, L"ドライ（全フィルターOFFの音を聞く）");
                miclog::Write ("app: dry(bypass) = %d", dry ? 1 : 0);
                return 0;
            }
            if (LOWORD (wp) == IDC_PRESET_COMBO && HIWORD (wp) == CBN_SELCHANGE)
            {
                const LRESULT sel = SendMessageW (Ctl (hwnd, IDC_PRESET_COMBO), CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR)
                {
                    wchar_t buf[256];
                    SendMessageW (Ctl (hwnd, IDC_PRESET_COMBO), CB_GETLBTEXT, sel,
                                  reinterpret_cast<LPARAM> (buf));
                    Preset p;
                    if (LoadPreset (buf, p))
                    {
                        g_currentPreset = p;
                        if (g_running) ApplyPresetLive();
                        miclog::Write ("preset: selected '%s'", WideToUtf8 (p.name).c_str());
                    }
                }
                return 0;
            }
            if (LOWORD (wp) == IDC_SETTINGS_BUTTON)
            {
                OpenSettingsWindow (hwnd, g_currentPreset.name);
                return 0;
            }
            break;
        case WM_PRESET_CHANGED:
        {
            // The settings window saved something. lParam may carry the
            // name the main window should select (rename / new / apply).
            std::wstring selectName = g_currentPreset.name;
            if (lp)
            {
                selectName = *reinterpret_cast<std::wstring*> (lp);
                delete reinterpret_cast<std::wstring*> (lp);
            }
            const std::wstring before = g_currentPreset.name;
            g_currentPreset.name = selectName;
            RefreshPresetCombo (hwnd);
            if (g_running) ApplyPresetLive();
            (void) before;
            return 0;
        }
        case WM_SOLO_CHANGED:
        {
            // Solo audition from the settings window: run only one
            // filter so its effect can be heard on its own. The preset
            // itself is never modified by this.
            if (lp)
            {
                g_soloKey = *reinterpret_cast<std::string*> (lp);
                delete reinterpret_cast<std::string*> (lp);
            }
            else g_soloKey.clear();
            miclog::Write ("solo: %s", g_soloKey.empty() ? "(off)" : g_soloKey.c_str());
            if (g_running) ApplyPresetLive();
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerId)
            {
                if (g_running)
                {
                    const std::wstring notice = PassiveCalOnTimerTick (hwnd);
                    // Auto buffer step-up requested by the engine (XRUNs).
                    const int requested = g_engine.RequestedBufferFrames();
                    if (requested > 0 && requested != SelectedBlockFrames (hwnd))
                    {
                        StopEngine (hwnd);
                        SelectBlockFrames (hwnd, requested);
                        if (StartEngine (hwnd))
                            UpdateStatus (hwnd, L"XRUN多発のためバッファを自動で上げました");
                        return 0;
                    }
                    const float peak = g_engine.PeakLevel();
                    g_meterDisplay = std::max (peak, g_meterDisplay * 0.90f);
                    SendMessageW (Ctl (hwnd, IDC_METER), PBM_SETPOS,
                                  (WPARAM) (g_meterDisplay * 1000.0f), 0);
                    UpdateStatus (hwnd, notice);
                }
                return 0;
            }
            break;
        case WM_CLOSE:
            if (g_running) StopEngine (hwnd);
            DestroyWindow (hwnd);
            return 0;
        case WM_DESTROY:
            if (g_deviceEnumerator && g_notificationClient)
                g_deviceEnumerator->UnregisterEndpointNotificationCallback (g_notificationClient);
            if (g_notificationClient) { g_notificationClient->Release(); g_notificationClient = nullptr; }
            if (g_deviceEnumerator) { g_deviceEnumerator->Release(); g_deviceEnumerator = nullptr; }
            ReleaseMicVolume();
            PostQuitMessage (0);
            return 0;
    }
    return DefWindowProcW (hwnd, msg, wp, lp);
}

int APIENTRY wWinMain (HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow)
{
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW (nullptr, exePath, MAX_PATH);
        std::wstring dir = exePath;
        const size_t slash = dir.find_last_of (L"\\/");
        if (slash != std::wstring::npos) dir = dir.substr (0, slash);
        miclog::Init (dir);
    }
    miclog::Write ("app: MicFilterClient starting");
    SetProcessDPIAware();
    CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc { sizeof (icc), ICC_PROGRESS_CLASS | ICC_BAR_CLASSES };
    InitCommonControlsEx (&icc);

    WNDCLASSW wc {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MicFilterClientWindow";
    RegisterClassW (&wc);

    HWND hwnd = CreateWindowW (wc.lpszClassName, L"MicFilter クライアント",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, 540, 556,
                               nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow (hwnd, nShow);

    MSG msg {};
    while (GetMessageW (&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage (&msg);
        DispatchMessageW (&msg);
    }
    CoUninitialize();
    return (int) msg.wParam;
}
