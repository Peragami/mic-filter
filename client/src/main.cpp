// MicFilter dedicated client -- tiny Win32 GUI around WasapiEngine.
// No framework: plain Win32 controls, WASAPI does the audio, the .vst3
// does the filtering. See README.md for usage.

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

#include "log.h"
#include "vst3/source/paramids.h" // MicFilterParams::ParamID (shared with the plugin)
#include "wasapi_engine.h"

namespace {

constexpr int IDC_INPUT_COMBO = 1001;
constexpr int IDC_OUTPUT_COMBO = 1002;
constexpr int IDC_BUFFER_COMBO = 1003;
constexpr int IDC_PLUGIN_EDIT = 1004;
constexpr int IDC_BROWSE_BUTTON = 1005;
constexpr int IDC_START_BUTTON = 1006;
constexpr int IDC_METER = 1007;
constexpr int IDC_STATUS = 1008;
constexpr int IDC_MIC_LEVEL = 1009;
constexpr int IDC_MIC_LEVEL_VALUE = 1010;
constexpr int IDC_CALIB_BUTTON = 1011;
constexpr int IDC_DRY_CHECK = 1012;
constexpr int IDC_FILTER_BASE = 1020; // + index into kFilterDefs (11 entries)

// Per-filter on/off switches. Param ids come from the plugin's paramids.h;
// all of these default to ON in the plugin, so the boxes start checked.
struct FilterDef { int paramId; const wchar_t* label; bool defOn; };
constexpr FilterDef kFilterDefs[] = {
    { MicFilterParams::kHpfOn,       L"ハイパス（低音カット）", true },
    { MicFilterParams::kHumOn,       L"ハムノッチ（電源ブーン除去）", true },
    { MicFilterParams::kGateOn,      L"ノイズゲート", true },
    { MicFilterParams::kMultibandOn, L"ノイズ抑制（帯域別）", true },
    { MicFilterParams::kSpectralOn,  L"ヒス除去（スペクトル減算）", false },
    { MicFilterParams::kRnnoiseOn,   L"AIノイズ除去（RNNoise）", false },
    { MicFilterParams::kBweOn,       L"AI帯域拡張（声のツヤ・超解像）", false }, // +10.7ms latency when ON
    { MicFilterParams::kEqOn,        L"EQ（音質補正）", true },
    { MicFilterParams::kDuckOn,      L"クリック音カット（マウス/キー）", true },
    { MicFilterParams::kBreathOn,    L"吐息キープ（息の音を残す）", false },
    { MicFilterParams::kDeclickOn,   L"リップノイズ除去", true },
    { MicFilterParams::kDeclipOn,    L"音割れ補正（デクリップ）", true },
    { MicFilterParams::kDeesserOn,   L"ディエッサー（サ行を柔らかく）", true },
    { MicFilterParams::kExciterOn,   L"エキサイター（高音のツヤ）", true },
    { MicFilterParams::kCompOn,      L"コンプレッサー（音量を揃える）", true },
    { MicFilterParams::kLimiterOn,   L"リミッター（天井）", true },
};
constexpr int kNumFilters = (int) (sizeof (kFilterDefs) / sizeof (kFilterDefs[0]));
constexpr UINT_PTR kTimerId = 1;
constexpr UINT WM_DEVICECHANGE_MSG = WM_APP + 1; // posted by DeviceNotificationClient

WasapiEngine g_engine;
std::vector<AudioDeviceInfo> g_captureDevices;
std::vector<AudioDeviceInfo> g_renderDevices;
float g_meterDisplay = 0.0f;
bool g_running = false;

// Auto level calibration state (docs/gain-staging.md "step 1 automation"):
// while active the engine is bypassed (dry) and we measure the raw peak for
// kCalibTicks, then nudge the Windows endpoint level (in dB) toward a target
// peak of -9 dB, up to kCalibRounds times.
bool g_calibrating = false;
int g_calibTicksLeft = 0;
int g_calibRound = 0;
constexpr int kCalibTicks = 60;   // 60 * 50 ms = 3 s per round
constexpr int kCalibRounds = 3;
constexpr float kCalibTargetDb = -9.0f; // centre of the -12..-6 window

void UpdateStatus (HWND hwnd, const std::wstring& prefix = L""); // defined below

// Device hot-plug tracking (IMMNotificationClient) + the capture device's
// Windows level (IAudioEndpointVolume), cached for the selected device.
IMMDeviceEnumerator* g_deviceEnumerator = nullptr;
IMMNotificationClient* g_notificationClient = nullptr;
IAudioEndpointVolume* g_micVolume = nullptr;
std::wstring g_micVolumeDeviceId;

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

HWND Ctl (HWND parent, int id) { return GetDlgItem (parent, id); }

void AddLabel (HWND parent, const wchar_t* text, int x, int y, int w)
{
    CreateWindowW (L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 20,
                   parent, nullptr, nullptr, nullptr);
}

HWND AddCombo (HWND parent, int id, int x, int y, int w)
{
    HWND h = CreateWindowW (L"COMBOBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                            x, y, w, 200, parent, reinterpret_cast<HMENU> ((INT_PTR) id),
                            nullptr, nullptr);
    return h;
}

void FillDeviceCombo (HWND combo, const std::vector<AudioDeviceInfo>& devices,
                      const std::wstring& preferredId = L"")
{
    int select = 0;
    for (size_t i = 0; i < devices.size(); ++i)
    {
        const int idx = (int) SendMessageW (combo, CB_ADDSTRING, 0,
                                            reinterpret_cast<LPARAM> (devices[i].name.c_str()));
        SendMessageW (combo, CB_SETITEMDATA, idx, (LPARAM) i);
        if (!preferredId.empty() && devices[i].id == preferredId) select = idx;
        else if (preferredId.empty() &&
                 devices[i].name.find (L"（既定）") != std::wstring::npos) select = idx;
    }
    if (!devices.empty()) SendMessageW (combo, CB_SETCURSEL, select, 0);
}

std::wstring SelectedDeviceId (HWND combo, const std::vector<AudioDeviceInfo>& devices)
{
    const LRESULT sel = SendMessageW (combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return L"";
    const LRESULT data = SendMessageW (combo, CB_GETITEMDATA, sel, 0);
    if (data == CB_ERR || (size_t) data >= devices.size()) return L"";
    return devices[(size_t) data].id;
}

// --- Windows mic level (IAudioEndpointVolume on the capture device) -------

void ReleaseMicVolume()
{
    if (g_micVolume) { g_micVolume->Release(); g_micVolume = nullptr; }
    g_micVolumeDeviceId.clear();
}

bool EnsureMicVolume (const std::wstring& deviceId)
{
    if (g_micVolume && g_micVolumeDeviceId == deviceId) return true;
    ReleaseMicVolume();
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator),
                                  reinterpret_cast<void**> (&en))) || !en)
        return false;
    IMMDevice* dev = nullptr;
    const HRESULT hr = deviceId.empty()
        ? en->GetDefaultAudioEndpoint (eCapture, eConsole, &dev)
        : en->GetDevice (deviceId.c_str(), &dev);
    en->Release();
    if (FAILED (hr) || !dev) return false;
    const HRESULT act = dev->Activate (__uuidof (IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**> (&g_micVolume));
    dev->Release();
    if (FAILED (act) || !g_micVolume) return false;
    g_micVolumeDeviceId = deviceId;
    return true;
}

void SetMicLevelLabel (HWND hwnd, int pos)
{
    wchar_t buf[16];
    swprintf_s (buf, L"%d", pos);
    SetWindowTextW (Ctl (hwnd, IDC_MIC_LEVEL_VALUE), buf);
}

// Slider moved by the user: write straight through to the device.
void ApplyMicLevel (HWND hwnd)
{
    const int pos = (int) SendMessageW (Ctl (hwnd, IDC_MIC_LEVEL), TBM_GETPOS, 0, 0);
    SetMicLevelLabel (hwnd, pos);
    const std::wstring id = SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices);
    if (EnsureMicVolume (id))
    {
        const HRESULT hr = g_micVolume->SetMasterVolumeLevelScalar (pos / 100.0f, nullptr);
        miclog::Write ("app: mic level slider -> %d (hr=0x%08lX)", pos, (unsigned long) hr);
    }
}

// Capture device (re)selected by the user: show its actual current level.
void RefreshMicLevelFromDevice (HWND hwnd)
{
    const std::wstring id = SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices);
    ReleaseMicVolume();
    float v = 0.0f;
    if (EnsureMicVolume (id) &&
        SUCCEEDED (g_micVolume->GetMasterVolumeLevelScalar (&v)))
    {
        const int pos = (int) (v * 100.0f + 0.5f);
        SendMessageW (Ctl (hwnd, IDC_MIC_LEVEL), TBM_SETPOS, TRUE, pos);
        SetMicLevelLabel (hwnd, pos);
    }
}

// --- Auto level calibration ----------------------------------------------
// Adjusts the Windows endpoint level **in dB** (IAudioEndpointVolume's dB
// API, so the taper curve does not distort the math). Windows level first:
// it decides clipping / hiss before the app ever sees the signal. Any gain
// still missing at the top of the range is reported, not silently faked.

bool IsChecked (HWND hwnd, int idc)
{
    return SendMessageW (Ctl (hwnd, idc), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void CalibFinish (HWND hwnd, const std::wstring& result)
{
    miclog::WriteW (L"calib: finish: %s", result.c_str());
    g_calibrating = false;
    g_engine.SetBypass (IsChecked (hwnd, IDC_DRY_CHECK)); // restore the dry toggle's state
    SetWindowTextW (Ctl (hwnd, IDC_CALIB_BUTTON), L"自動較正");
    RefreshMicLevelFromDevice (hwnd); // slider shows the level we ended at
    UpdateStatus (hwnd, result);
}

void CalibStart (HWND hwnd)
{
    if (!g_running)
    {
        MessageBoxW (hwnd, L"先に「開始」でマイクを通してから較正してください",
                     L"MicFilter", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring id = SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices);
    if (!EnsureMicVolume (id))
    {
        MessageBoxW (hwnd, L"このマイクの音量をアプリから変更できませんでした",
                     L"MicFilter", MB_OK | MB_ICONWARNING);
        return;
    }
    miclog::Write ("calib: start (target peak %.1f dB, window %d..%d, rounds<=%d)",
                   kCalibTargetDb, -12, -6, kCalibRounds);
    g_calibrating = true;
    g_calibRound = 0;
    g_calibTicksLeft = kCalibTicks;
    g_engine.SetBypass (true); // all filters off while measuring (user's rule)
    g_engine.ResetClipCount();
    (void) g_engine.TakeRawPeak();
    SetWindowTextW (Ctl (hwnd, IDC_CALIB_BUTTON), L"較正中止");
    UpdateStatus (hwnd, L"較正中：フィルター全OFF。一番大きな声で喋ってください…");
}

void CalibCancel (HWND hwnd) { CalibFinish (hwnd, L"較正を中止しました"); }

// One measurement window finished: decide and (maybe) adjust, in dB.
void CalibEvaluate (HWND hwnd)
{
    const float peak = g_engine.TakeRawPeak();
    const unsigned long long clips = g_engine.ClipCount();
    g_engine.ResetClipCount();
    const float peakDb = 20.0f * std::log10 (std::max (peak, 1e-6f));
    ++g_calibRound;
    miclog::Write ("calib: round %d measured peak=%.4f (%.1f dB) clips=%llu",
                   g_calibRound, peak, peakDb, clips);

    wchar_t buf[256];
    if (peak < 0.01f) // ~ -40 dB: basically no voice captured
    {
        CalibFinish (hwnd, L"較正：声が小さすぎて測れませんでした。マイクに近づいて、もう一度");
        return;
    }
    float deltaDb = 0.0f;
    if (clips > 0 || peakDb > -6.0f)
        deltaDb = (kCalibTargetDb - peakDb) - 1.0f; // clipped or hot: down, +1 dB safety
    else if (peakDb < -12.0f)
        deltaDb = kCalibTargetDb - peakDb;          // too quiet: up
    else
    {
        swprintf_s (buf, L"較正完了：ピーク %.1f dB（目標 -12〜-6）", peakDb);
        CalibFinish (hwnd, buf);
        return;
    }
    if (g_calibRound >= kCalibRounds)
    {
        swprintf_s (buf, L"較正：%d回調整しても範囲に入らず（ピーク %.1f dB）。Windowsレベルの上限/下限に達した可能性",
                    kCalibRounds, peakDb);
        CalibFinish (hwnd, buf);
        return;
    }
    const std::wstring id = SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices);
    if (EnsureMicVolume (id))
    {
        float curDb = 0.0f, minDb = 0.0f, maxDb = 0.0f, stepDb = 0.0f;
        if (SUCCEEDED (g_micVolume->GetMasterVolumeLevel (&curDb)) &&
            SUCCEEDED (g_micVolume->GetVolumeRange (&minDb, &maxDb, &stepDb)))
        {
            const float next = std::min (maxDb, std::max (minDb, curDb + deltaDb));
            const HRESULT hr = g_micVolume->SetMasterVolumeLevel (next, nullptr);
            miclog::Write ("calib: endpoint level %.1f dB -> %.1f dB (range %.1f..%.1f, delta %.1f, hr=0x%08lX)",
                           curDb, next, minDb, maxDb, deltaDb, (unsigned long) hr);
        }
        else
            miclog::Write ("calib: could not read endpoint level/range");
    }
    g_calibTicksLeft = kCalibTicks;
    UpdateStatus (hwnd, L"較正中：レベルを調整しました。もう一度、一番大きな声で…");
}

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
    if (g_micVolume && g_micVolumeDeviceId !=
                           SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices))
        ReleaseMicVolume();
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

void SetControlsEnabled (HWND hwnd, bool enabled)
{
    const int ids[] = { IDC_INPUT_COMBO, IDC_OUTPUT_COMBO, IDC_BUFFER_COMBO,
                        IDC_PLUGIN_EDIT, IDC_BROWSE_BUTTON };
    for (int id : ids) EnableWindow (Ctl (hwnd, id), enabled);
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

bool DoStart (HWND hwnd)
{
    EngineConfig cfg;
    cfg.inputDeviceId = SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices);
    cfg.outputDeviceId = SelectedDeviceId (Ctl (hwnd, IDC_OUTPUT_COMBO), g_renderDevices);
    cfg.blockFrames = SelectedBlockFrames (hwnd);
    for (int i = 0; i < kNumFilters; ++i)
        cfg.initialParams.push_back ({ (unsigned int) kFilterDefs[i].paramId,
                                       IsChecked (hwnd, IDC_FILTER_BASE + i) ? 1.0 : 0.0 });
    wchar_t path[MAX_PATH] = {};
    GetWindowTextW (Ctl (hwnd, IDC_PLUGIN_EDIT), path, MAX_PATH);
    cfg.pluginPath = path;
    if (cfg.pluginPath.empty())
    {
        MessageBoxW (hwnd, L"プラグイン（.vst3）のパスを指定してください", L"MicFilter",
                     MB_OK | MB_ICONWARNING);
        return false;
    }
    std::wstring err;
    if (!g_engine.Start (cfg, err))
    {
        miclog::WriteW (L"app: start failed: %s", err.c_str());
        MessageBoxW (hwnd, err.c_str(), L"開始できませんでした", MB_OK | MB_ICONERROR);
        return false;
    }
    miclog::Write ("app: started (mode=%d block=%d)", (int) g_engine.ActiveMode(),
                   g_engine.ActiveBlockFrames());
    if (IsChecked (hwnd, IDC_DRY_CHECK)) g_engine.SetBypass (true);
    g_running = true;
    SetWindowTextW (Ctl (hwnd, IDC_START_BUTTON), L"停止");
    SetControlsEnabled (hwnd, false);
    UpdateStatus (hwnd);
    return true;
}

void DoStop (HWND hwnd)
{
    if (g_calibrating) // engine stops: calibration cannot continue
    {
        g_calibrating = false;
        SetWindowTextW (Ctl (hwnd, IDC_CALIB_BUTTON), L"自動較正");
    }
    miclog::Write ("app: stopping (xruns=%llu)", g_engine.XrunCount());
    g_engine.Stop();
    g_running = false;
    SetWindowTextW (Ctl (hwnd, IDC_START_BUTTON), L"開始");
    SetControlsEnabled (hwnd, true);
    SendMessageW (Ctl (hwnd, IDC_METER), PBM_SETPOS, 0, 0);
    g_meterDisplay = 0.0f;
    UpdateStatus (hwnd);
}

std::wstring DefaultPluginPath()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW (nullptr, exePath, MAX_PATH);
    std::wstring dir = exePath;
    const size_t slash = dir.find_last_of (L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr (0, slash);
    const std::wstring candidates[] = {
        dir + L"\\MicFilter.vst3",
        dir + L"\\..\\vst3\\MicFilter.vst3",
        dir + L"\\..\\..\\vst3\\MicFilter.vst3",
        dir + L"\\..\\..\\vst3\\build\\VST3\\Release\\MicFilter.vst3", // dev layout of the vst3/ workstream
    };
    for (const auto& c : candidates)
        if (GetFileAttributesW (c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    return candidates[0];
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

            AddLabel (hwnd, L"Windows マイクレベル（目安：まず50、一番大きな声でピーク -12〜-6dB）",
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

            AddLabel (hwnd, L"プラグイン（MicFilter.vst3）", 16, 252, 300);
            HWND edit = make (CreateWindowW (L"EDIT", DefaultPluginPath().c_str(),
                                             WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                             16, 274, 380, 24, hwnd,
                                             reinterpret_cast<HMENU> ((INT_PTR) IDC_PLUGIN_EDIT),
                                             nullptr, nullptr));
            (void) edit;
            make (CreateWindowW (L"BUTTON", L"参照…", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 406, 274, 80, 24, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_BROWSE_BUTTON),
                                 nullptr, nullptr));
            make (CreateWindowW (L"BUTTON", L"開始", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                 16, 314, 120, 32, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_START_BUTTON),
                                 nullptr, nullptr));
            make (CreateWindowW (L"BUTTON", L"自動較正", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 146, 314, 104, 32, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_CALIB_BUTTON),
                                 nullptr, nullptr));
            HWND meter = CreateWindowW (PROGRESS_CLASSW, L"",
                                        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                        262, 320, 224, 20, hwnd,
                                        reinterpret_cast<HMENU> ((INT_PTR) IDC_METER),
                                        nullptr, nullptr);
            SendMessageW (meter, PBM_SETRANGE, 0, MAKELPARAM (0, 1000));
            make (CreateWindowW (L"STATIC", L"停止中", WS_CHILD | WS_VISIBLE,
                                 16, 356, 470, 40, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_STATUS),
                                 nullptr, nullptr));

            AddLabel (hwnd, L"フィルター（動作中に切り替えOK、そのまま音で比べられます）", 16, 404, 470);
            {
                HWND dry = make (CreateWindowW (L"BUTTON", L"ドライ（全フィルターOFFの音を聞く）",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                16, 428, 470, 22, hwnd,
                                                reinterpret_cast<HMENU> ((INT_PTR) IDC_DRY_CHECK),
                                                nullptr, nullptr));
                (void) dry;
            }
            for (int i = 0; i < kNumFilters; ++i)
            {
                const int col = i % 2, row = i / 2;
                HWND cb = make (CreateWindowW (L"BUTTON", kFilterDefs[i].label,
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                               16 + col * 245, 458 + row * 24, 240, 22, hwnd,
                                               reinterpret_cast<HMENU> ((INT_PTR) (IDC_FILTER_BASE + i)),
                                               nullptr, nullptr));
                SendMessageW (cb, BM_SETCHECK, kFilterDefs[i].defOn ? BST_CHECKED : BST_UNCHECKED, 0);
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
            SetTimer (hwnd, kTimerId, 50, nullptr);
            return 0;
        }
        case WM_DEVICECHANGE_MSG:
            miclog::Write ("app: audio device change notification");
            ReenumerateDevices (hwnd);
            return 0;
        case WM_HSCROLL:
            if ((HWND) lp == Ctl (hwnd, IDC_MIC_LEVEL))
            {
                ApplyMicLevel (hwnd);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (LOWORD (wp) == IDC_INPUT_COMBO && HIWORD (wp) == CBN_SELCHANGE)
            {
                RefreshMicLevelFromDevice (hwnd);
                return 0;
            }
            if (LOWORD (wp) == IDC_START_BUTTON)
            {
                if (g_running) DoStop (hwnd);
                else DoStart (hwnd);
                return 0;
            }
            if (LOWORD (wp) == IDC_CALIB_BUTTON)
            {
                if (g_calibrating) CalibCancel (hwnd);
                else CalibStart (hwnd);
                return 0;
            }
            if (LOWORD (wp) == IDC_DRY_CHECK)
            {
                const bool dry = IsChecked (hwnd, IDC_DRY_CHECK);
                if (!g_calibrating) g_engine.SetBypass (dry);
                miclog::Write ("app: dry(bypass) = %d", dry ? 1 : 0);
                return 0;
            }
            if (LOWORD (wp) >= IDC_FILTER_BASE && LOWORD (wp) < IDC_FILTER_BASE + kNumFilters)
            {
                const int i = LOWORD (wp) - IDC_FILTER_BASE;
                const bool on = IsChecked (hwnd, IDC_FILTER_BASE + i);
                g_engine.SetParameter ((unsigned int) kFilterDefs[i].paramId, on ? 1.0 : 0.0);
                miclog::Write ("app: filter param %d -> %d", kFilterDefs[i].paramId, on ? 1 : 0);
                return 0;
            }
            if (LOWORD (wp) == IDC_BROWSE_BUTTON)
            {
                wchar_t file[MAX_PATH] = {};
                GetWindowTextW (Ctl (hwnd, IDC_PLUGIN_EDIT), file, MAX_PATH);
                OPENFILENAMEW ofn {};
                ofn.lStructSize = sizeof (ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"VST3 プラグイン (*.vst3)\0*.vst3\0すべてのファイル (*.*)\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW (&ofn))
                    SetWindowTextW (Ctl (hwnd, IDC_PLUGIN_EDIT), file);
                return 0;
            }
            break;
        case WM_TIMER:
            if (wp == kTimerId)
            {
                if (g_running)
                {
                    if (g_calibrating && --g_calibTicksLeft <= 0)
                        CalibEvaluate (hwnd);
                    // Auto buffer step-up requested by the engine (XRUNs).
                    const int requested = g_engine.RequestedBufferFrames();
                    if (requested > 0 && requested != SelectedBlockFrames (hwnd))
                    {
                        DoStop (hwnd);
                        SelectBlockFrames (hwnd, requested);
                        if (DoStart (hwnd))
                            UpdateStatus (hwnd, L"XRUN多発のためバッファを自動で上げました");
                        return 0;
                    }
                    const float peak = g_engine.PeakLevel();
                    g_meterDisplay = std::max (peak, g_meterDisplay * 0.90f);
                    SendMessageW (Ctl (hwnd, IDC_METER), PBM_SETPOS,
                                  (WPARAM) (g_meterDisplay * 1000.0f), 0);
                    UpdateStatus (hwnd);
                }
                return 0;
            }
            break;
        case WM_CLOSE:
            if (g_running) DoStop (hwnd);
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

} // namespace

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
                               CW_USEDEFAULT, CW_USEDEFAULT, 540, 750,
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
