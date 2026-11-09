// MicFilter dedicated client -- tiny Win32 GUI around WasapiEngine.
// No framework: plain Win32 controls, WASAPI does the audio, the DSP
// chain is embedded in the engine (v0.7+). Mic level lives in
// mic_level.cpp, auto calibration in calibration.cpp, shared UI bits in
// ui_shared.h. See README.md for usage.

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

#include "calibration.h"
#include "log.h"
#include "mic_level.h"
#include "ui_shared.h"
#include "vst3/source/paramids.h" // MicFilterParams::ParamID (shared with the plugin)
#include "wasapi_engine.h"

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

WasapiEngine g_engine;
std::vector<AudioDeviceInfo> g_captureDevices;
std::vector<AudioDeviceInfo> g_renderDevices;
float g_meterDisplay = 0.0f;
bool g_running = false;

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

void SetControlsEnabled (HWND hwnd, bool enabled)
{
    const int ids[] = { IDC_INPUT_COMBO, IDC_OUTPUT_COMBO, IDC_BUFFER_COMBO };
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
    CalibrationOnEngineStopped (hwnd);
    miclog::Write ("app: stopping (xruns=%llu)", g_engine.XrunCount());
    g_engine.Stop();
    g_running = false;
    SetWindowTextW (Ctl (hwnd, IDC_START_BUTTON), L"開始");
    SetControlsEnabled (hwnd, true);
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

            make (CreateWindowW (L"BUTTON", L"開始", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                 16, 252, 120, 32, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_START_BUTTON),
                                 nullptr, nullptr));
            make (CreateWindowW (L"BUTTON", L"自動較正", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 146, 252, 104, 32, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_CALIB_BUTTON),
                                 nullptr, nullptr));
            HWND meter = CreateWindowW (PROGRESS_CLASSW, L"",
                                        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                        262, 258, 224, 20, hwnd,
                                        reinterpret_cast<HMENU> ((INT_PTR) IDC_METER),
                                        nullptr, nullptr);
            SendMessageW (meter, PBM_SETRANGE, 0, MAKELPARAM (0, 1000));
            make (CreateWindowW (L"STATIC", L"停止中", WS_CHILD | WS_VISIBLE,
                                 16, 294, 470, 40, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) IDC_STATUS),
                                 nullptr, nullptr));

            AddLabel (hwnd, L"フィルター（動作中に切り替えOK、そのまま音で比べられます）", 16, 342, 470);
            {
                HWND dry = make (CreateWindowW (L"BUTTON", L"ドライ（全フィルターOFFの音を聞く）",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                16, 366, 470, 22, hwnd,
                                                reinterpret_cast<HMENU> ((INT_PTR) IDC_DRY_CHECK),
                                                nullptr, nullptr));
                (void) dry;
            }
            for (int i = 0; i < kNumFilters; ++i)
            {
                const int col = i % 2, row = i / 2;
                HWND cb = make (CreateWindowW (L"BUTTON", kFilterDefs[i].label,
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                               16 + col * 245, 396 + row * 24, 240, 22, hwnd,
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
                if (CalibrationActive()) CalibCancel (hwnd);
                else CalibStart (hwnd);
                return 0;
            }
            if (LOWORD (wp) == IDC_DRY_CHECK)
            {
                const bool dry = IsChecked (hwnd, IDC_DRY_CHECK);
                if (!CalibrationActive()) g_engine.SetBypass (dry);
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
            break;
        case WM_TIMER:
            if (wp == kTimerId)
            {
                if (g_running)
                {
                    CalibrationOnTimerTick (hwnd);
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
                               CW_USEDEFAULT, CW_USEDEFAULT, 540, 640,
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
