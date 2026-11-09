// passive_calibration.cpp -- see passive_calibration.h.
#include "passive_calibration.h"

#include <algorithm>
#include <cmath>

#include <endpointvolume.h>

#include "log.h"
#include "mic_level.h"
#include "passive_gain.h"
#include "ui_shared.h"

namespace
{
PassiveGain g_gain;
unsigned long long g_lastClipCount = 0;
std::wstring g_notice;      // last adjustment message
double g_noticeAgeSec = 1e9; // seconds since the notice was set

// Move the selected capture device's endpoint level by deltaDb (clamped
// to the device's dB range, via IAudioEndpointVolume's dB API so the
// taper curve does not distort the math). Returns false if the level
// could not be changed (or was already at the clamp).
bool AdjustEndpointDb (HWND hwnd, float deltaDb)
{
    const std::wstring id = SelectedDeviceId (Ctl (hwnd, IDC_INPUT_COMBO), g_captureDevices);
    if (!EnsureMicVolume (id))
    {
        miclog::Write ("passive-cal: endpoint volume unavailable");
        return false;
    }
    float curDb = 0.0f, minDb = 0.0f, maxDb = 0.0f, stepDb = 0.0f;
    if (!SUCCEEDED (MicVolume()->GetMasterVolumeLevel (&curDb)) ||
        !SUCCEEDED (MicVolume()->GetVolumeRange (&minDb, &maxDb, &stepDb)))
    {
        miclog::Write ("passive-cal: could not read endpoint level/range");
        return false;
    }
    const float next = std::min (maxDb, std::max (minDb, curDb + deltaDb));
    if (std::fabs (next - curDb) < 0.05f)
    {
        miclog::Write ("passive-cal: at range limit (%.1f dB), delta %.1f not applied",
                       curDb, deltaDb);
        return false;
    }
    const HRESULT hr = MicVolume()->SetMasterVolumeLevel (next, nullptr);
    miclog::Write ("passive-cal: endpoint level %.1f dB -> %.1f dB (delta %.1f, hr=0x%08lX)",
                   curDb, next, deltaDb, (unsigned long) hr);
    return SUCCEEDED (hr);
}
} // namespace

void PassiveCalOnStart (HWND hwnd)
{
    (void) hwnd;
    g_gain.reset();
    g_lastClipCount = 0;
    g_engine.ResetClipCount();
    (void) g_engine.TakeRawPeak();
    g_notice.clear();
    g_noticeAgeSec = 1e9;
    miclog::Write ("passive-cal: armed (down -3 dB on clip / up +1.5 dB per 15 s when quiet)");
}

std::wstring PassiveCalOnTimerTick (HWND hwnd)
{
    g_noticeAgeSec += 0.05;
    const float peak = g_engine.TakeRawPeak();
    const unsigned long long clips = g_engine.ClipCount();
    const bool clipped = clips != g_lastClipCount;
    g_lastClipCount = clips;

    if (!IsChecked (hwnd, IDC_AUTO_GAIN_CHECK))
    {
        g_gain.reset(); // re-enabling starts from fresh statistics
        return {};
    }

    const PassiveGain::Decision d = g_gain.update (peak, clipped, 0.05);
    if (d.action == PassiveGain::Action::None)
        return g_noticeAgeSec < 4.0 ? g_notice : std::wstring();

    if (AdjustEndpointDb (hwnd, d.deltaDb))
    {
        RefreshMicLevelFromDevice (hwnd); // slider shows the new level
        wchar_t buf[160];
        if (d.action == PassiveGain::Action::Down)
            swprintf_s (buf, L"自動補正：音割れ検出、マイクレベルを %.1f dB 下げました", -d.deltaDb);
        else
            swprintf_s (buf, L"自動補正：声が小さめなのでマイクレベルを %.1f dB 上げました", d.deltaDb);
        g_notice = buf;
        g_noticeAgeSec = 0.0;
        return g_notice;
    }
    return g_noticeAgeSec < 4.0 ? g_notice : std::wstring();
}
