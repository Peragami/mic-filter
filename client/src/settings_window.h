#pragma once
// settings_window.h -- the filter settings window (v0.14): edit presets
// (create / rename / delete), organize filters into groups, toggle
// filters, and reorder the DSP stages. Modeless, so changes to the
// currently used preset are audible live. All edits autosave to the
// presets folder and notify the main window (WM_PRESET_CHANGED).

#include <string>

#include <windows.h>

void OpenSettingsWindow (HWND parent, const std::wstring& mainCurrentPreset);
