#pragma once
// passive_calibration.h -- passive ("ながら") Windows mic-level
// calibration (docs/gain-staging.md): no dedicated measurement time.
// While the engine runs, the UI timer feeds the raw input peak/clip state
// to PassiveGain (passive_gain.h); when it decides, the endpoint level is
// nudged in dB and the slider follows. Enabled by the 「自動補正」
// checkbox (IDC_AUTO_GAIN_CHECK), default ON. This replaced the old
// press-to-measure 自動較正 button in v0.9, at the user's request.

#include <string>

#include <windows.h>

void PassiveCalOnStart (HWND hwnd); // reset state when the engine starts
// One 50 ms UI tick. Returns a status prefix to show (adjustment notices
// linger for a few seconds), or an empty string.
std::wstring PassiveCalOnTimerTick (HWND hwnd);
