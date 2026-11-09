#pragma once
// updater.h -- self-update for the dedicated client (v0.10+).
// Flow: fetch dist/update/latest.json from the public GitHub repo
// (raw.githubusercontent.com) -> if newer, the UI asks -> download the
// client zip -> verify SHA-256 -> extract with the system tar.exe ->
// a small .bat waits for this process to exit, swaps the exe and
// relaunches. Only MicFilterClient.exe is replaced; the bundled VST
// folder is not touched (DSP changes live in the exe itself since v0.7).
// All network/disk work runs on a worker thread; results come back as
// window messages (payloads are heap-allocated, receiver deletes).

#include <string>

#include <windows.h>

#include "update_check.h"

struct UpdateOffer
{
    UpdateInfo info;
    bool userInitiated = false;
};

constexpr UINT WM_UPDATE_AVAILABLE = WM_APP + 2; // lParam: UpdateOffer*
constexpr UINT WM_UPDATE_UPTODATE = WM_APP + 3;  // manual check only
constexpr UINT WM_UPDATE_FAILED = WM_APP + 4;    // lParam: std::wstring*
constexpr UINT WM_UPDATE_APPLY = WM_APP + 5;     // close now; the .bat swaps the exe

void CheckForUpdateAsync (HWND hwnd, bool userInitiated);
void DownloadAndApplyAsync (HWND hwnd, UpdateInfo info);
std::wstring Utf8ToWide (const std::string& s);
