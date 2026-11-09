// mic_level.cpp -- see mic_level.h.
#include "mic_level.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include <windows.h>

#include "log.h"
#include "ui_shared.h"

// --- Windows mic level (IAudioEndpointVolume on the capture device) -------

IAudioEndpointVolume* g_micVolume = nullptr;
std::wstring g_micVolumeDeviceId;

IAudioEndpointVolume* MicVolume() { return g_micVolume; }

void ReleaseMicVolumeIfNotCurrent (const std::wstring& currentDeviceId)
{
    if (g_micVolume && g_micVolumeDeviceId != currentDeviceId)
        ReleaseMicVolume();
}

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

