#pragma once
// calibration.h -- automatic Windows mic-level calibration
// (docs/gain-staging.md "step 1 automation"): while active the engine is
// bypassed (dry) and the raw peak is measured for 3 s, then the endpoint
// level is nudged in dB toward a -9 dB peak, up to 3 rounds.

#include <windows.h>

void CalibStart (HWND hwnd);
void CalibCancel (HWND hwnd);
bool CalibrationActive();
void CalibrationOnTimerTick (HWND hwnd);   // call from the 50 ms UI timer
void CalibrationOnEngineStopped (HWND hwnd); // engine stop cancels calibration
