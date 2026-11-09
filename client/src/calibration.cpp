// calibration.cpp -- see calibration.h.
#include "calibration.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <endpointvolume.h>

#include "log.h"
#include "mic_level.h"
#include "ui_shared.h"

// State (file-local).
bool g_calibrating = false;
int g_calibTicksLeft = 0;
int g_calibRound = 0;
constexpr int kCalibTicks = 60;   // 60 * 50 ms = 3 s per round
constexpr int kCalibRounds = 3;
constexpr float kCalibTargetDb = -9.0f; // centre of the -12..-6 window

void CalibEvaluate (HWND hwnd); // defined below

// --- Auto level calibration ----------------------------------------------
// Adjusts the Windows endpoint level **in dB** (IAudioEndpointVolume's dB
// API, so the taper curve does not distort the math). Windows level first:
// it decides clipping / hiss before the app ever sees the signal. Any gain
// still missing at the top of the range is reported, not silently faked.



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
        if (SUCCEEDED (MicVolume()->GetMasterVolumeLevel (&curDb)) &&
            SUCCEEDED (MicVolume()->GetVolumeRange (&minDb, &maxDb, &stepDb)))
        {
            const float next = std::min (maxDb, std::max (minDb, curDb + deltaDb));
            const HRESULT hr = MicVolume()->SetMasterVolumeLevel (next, nullptr);
            miclog::Write ("calib: endpoint level %.1f dB -> %.1f dB (range %.1f..%.1f, delta %.1f, hr=0x%08lX)",
                           curDb, next, minDb, maxDb, deltaDb, (unsigned long) hr);
        }
        else
            miclog::Write ("calib: could not read endpoint level/range");
    }
    g_calibTicksLeft = kCalibTicks;
    UpdateStatus (hwnd, L"較正中：レベルを調整しました。もう一度、一番大きな声で…");
}


bool CalibrationActive() { return g_calibrating; }

void CalibrationOnTimerTick (HWND hwnd)
{
    if (g_calibrating && --g_calibTicksLeft <= 0)
        CalibEvaluate (hwnd);
}

void CalibrationOnEngineStopped (HWND hwnd)
{
    if (g_calibrating) // engine stops: calibration cannot continue
    {
        g_calibrating = false;
        SetWindowTextW (Ctl (hwnd, IDC_CALIB_BUTTON), L"自動較正");
    }
}
