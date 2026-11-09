#pragma once
// mic_level.h -- the selected capture device's Windows endpoint volume
// (IAudioEndpointVolume): slider write-through, read-back, and the cached
// handle used by the auto calibration (dB API lives in calibration.cpp via
// EnsureMicVolume + MicVolume()).

#include <string>

#include <windows.h>

struct IAudioEndpointVolume;

void ReleaseMicVolume();
bool EnsureMicVolume (const std::wstring& deviceId);
IAudioEndpointVolume* MicVolume(); // valid after EnsureMicVolume() == true
void ReleaseMicVolumeIfNotCurrent (const std::wstring& currentDeviceId);
void ApplyMicLevel (HWND hwnd);
void RefreshMicLevelFromDevice (HWND hwnd);
