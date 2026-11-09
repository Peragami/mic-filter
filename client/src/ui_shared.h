#pragma once
// ui_shared.h -- control ids, tiny window helpers and the globals shared
// between main.cpp, mic_level.cpp and passive_calibration.cpp (single-window app,
// so these are plain globals by design).

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

#include "wasapi_engine.h"

constexpr int IDC_INPUT_COMBO = 1001;
constexpr int IDC_OUTPUT_COMBO = 1002;
constexpr int IDC_BUFFER_COMBO = 1003;
constexpr int IDC_START_BUTTON = 1006;
constexpr int IDC_METER = 1007;
constexpr int IDC_STATUS = 1008;
constexpr int IDC_MIC_LEVEL = 1009;
constexpr int IDC_MIC_LEVEL_VALUE = 1010;
constexpr int IDC_AUTO_GAIN_CHECK = 1011;
constexpr int IDC_DRY_CHECK = 1012;
constexpr int IDC_FILTER_BASE = 1020; // + index into the filter table in main.cpp

constexpr UINT_PTR kTimerId = 1;
constexpr UINT WM_DEVICECHANGE_MSG = WM_APP + 1; // posted by DeviceNotificationClient

extern WasapiEngine g_engine;
extern std::vector<AudioDeviceInfo> g_captureDevices;
extern std::vector<AudioDeviceInfo> g_renderDevices;
extern bool g_running;

void UpdateStatus (HWND hwnd, const std::wstring& prefix = L"");

inline HWND Ctl (HWND parent, int id) { return GetDlgItem (parent, id); }

inline void AddLabel (HWND parent, const wchar_t* text, int x, int y, int w)
{
    CreateWindowW (L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 20,
                   parent, nullptr, nullptr, nullptr);
}

inline HWND AddCombo (HWND parent, int id, int x, int y, int w)
{
    return CreateWindowW (L"COMBOBOX", L"",
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                          x, y, w, 200, parent, reinterpret_cast<HMENU> ((INT_PTR) id),
                          nullptr, nullptr);
}

inline void FillDeviceCombo (HWND combo, const std::vector<AudioDeviceInfo>& devices,
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

inline std::wstring SelectedDeviceId (HWND combo, const std::vector<AudioDeviceInfo>& devices)
{
    const LRESULT sel = SendMessageW (combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return L"";
    const LRESULT data = SendMessageW (combo, CB_GETITEMDATA, sel, 0);
    if (data == CB_ERR || (size_t) data >= devices.size()) return L"";
    return devices[(size_t) data].id;
}

inline bool IsChecked (HWND hwnd, int idc)
{
    return SendMessageW (Ctl (hwnd, idc), BM_GETCHECK, 0, 0) == BST_CHECKED;
}
