// settings_window.cpp -- see settings_window.h.
// v0.24: grouped list (the user's wireframe). Groups stack vertically,
// filter rows inside follow the fixed canonical processing order.
// Group and row on/off switches, per-row solo audition, and a
// double-click opens the per-filter settings dialog. Everything
// autosaves. (The v0.20 DAW board and its drag/blocks are gone.)
// GDI+ needs the full Windows headers (PROPID & friends), so drop
// WIN32_LEAN_AND_MEAN for this file only.
#undef WIN32_LEAN_AND_MEAN
#include "settings_window.h"

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <commctrl.h>

#include "log.h"
#include "preset.h"
#include "ui_shared.h"

namespace
{
// Top-level control ids (settings window).
constexpr int S_PRESET_COMBO = 2001;
constexpr int S_PRESET_NEW = 2002;
constexpr int S_PRESET_RENAME = 2003;
constexpr int S_LOCK_LABEL = 2019;
constexpr int S_PRESET_DELETE = 2004;
constexpr int S_BLOCK_CREATE = 2012;
constexpr int S_NOISE_ON = 2020;
constexpr int S_NOISE_OFF = 2021;
constexpr int S_APPLY = 2017;
constexpr int S_CLOSE = 2018;
HWND g_win = nullptr;
HWND g_parent = nullptr;
HWND g_board = nullptr;
std::wstring g_mainCurrent;
Preset g_edit;
bool g_editIsMainCurrent = false;
bool g_editLocked = false; // protected built-in preset (標準 / AIプリセット)
std::string g_selKey = "gate"; // selected card (for block creation)
std::string g_soloKey;         // solo audition: only this filter is audible

void SendSolo()
{
    if (g_parent)
        PostMessageW (g_parent, WM_SOLO_CHANGED, 0,
                      reinterpret_cast<LPARAM> (new std::string (g_soloKey)));
}

void ClearSolo()
{
    if (!g_soloKey.empty()) { g_soloKey.clear(); SendSolo(); }
}
int g_boardScroll = 0;
HFONT g_font = nullptr;

HWND SCtl (int id) { return GetDlgItem (g_win, id); }

void Commit()
{
    if (g_editLocked) return; // protected presets never change
    SavePreset (g_edit);
    if (g_editIsMainCurrent && g_parent)
        PostMessageW (g_parent, WM_PRESET_CHANGED, 0, 0);
}

void NotifyMainSelect (const std::wstring& name)
{
    if (g_parent)
        PostMessageW (g_parent, WM_PRESET_CHANGED, 0,
                      reinterpret_cast<LPARAM> (new std::wstring (name)));
}

std::wstring KnobValueText (const KnobDef& def, double v)
{
    wchar_t buf[160];
    wchar_t val[48];
    switch (def.format)
    {
        case 0: swprintf_s (val, L"%.0f%ls", v, def.unit); break;
        case 1: swprintf_s (val, L"%.1f%ls", v, def.unit); break;
        case 2: swprintf_s (val, L"%.2f%ls", v, def.unit); break;
        case 3: swprintf_s (val, L"%.0f%%", v * 100.0); break;
        case 4: swprintf_s (val, L"%.1f:1", v); break;
        default: swprintf_s (val, L"%g", v); break;
    }
    swprintf_s (buf, L"%ls　%ls", def.label, val);
    return buf;
}

double KnobValue (const KnobDef& def)
{
    const auto it = g_edit.values.find (def.valueKey);
    return it != g_edit.values.end() ? it->second
                                     : (double) MicFilterParams::rangeFor (def.paramId).def;
}

// --- Text prompt (tiny modal dialog) --------------------------------------
struct PromptState
{
    std::wstring result;
    bool ok = false;
    bool done = false;
    HWND edit = nullptr;
};
PromptState g_prompt;

LRESULT CALLBACK PromptProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            HFONT font = (HFONT) GetStockObject (DEFAULT_GUI_FONT);
            g_prompt.edit = CreateWindowW (L"EDIT", L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                           12, 12, 316, 24, hwnd,
                                           reinterpret_cast<HMENU> ((INT_PTR) 3001),
                                           nullptr, nullptr);
            HWND ok = CreateWindowW (L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                     140, 48, 88, 26, hwnd, reinterpret_cast<HMENU> ((INT_PTR) IDOK),
                                     nullptr, nullptr);
            HWND cancel = CreateWindowW (L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         236, 48, 92, 26, hwnd, reinterpret_cast<HMENU> ((INT_PTR) IDCANCEL),
                                         nullptr, nullptr);
            SendMessageW (g_prompt.edit, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW (ok, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW (cancel, WM_SETFONT, (WPARAM) font, TRUE);
            SetWindowTextW (g_prompt.edit, g_prompt.result.c_str());
            SetFocus (g_prompt.edit);
            SendMessageW (g_prompt.edit, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD (wp) == IDOK)
            {
                wchar_t buf[256];
                GetWindowTextW (g_prompt.edit, buf, 256);
                g_prompt.result = buf;
                g_prompt.ok = !g_prompt.result.empty();
                g_prompt.done = true;
                DestroyWindow (hwnd);
                return 0;
            }
            if (LOWORD (wp) == IDCANCEL)
            {
                g_prompt.done = true;
                DestroyWindow (hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            g_prompt.done = true;
            DestroyWindow (hwnd);
            return 0;
    }
    return DefWindowProcW (hwnd, msg, wp, lp);
}

bool PromptText (HWND parent, const wchar_t* title, const std::wstring& initial, std::wstring& out)
{
    static bool registered = false;
    HINSTANCE hInst = GetModuleHandleW (nullptr);
    if (!registered)
    {
        WNDCLASSW wc {};
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MicFilterPrompt";
        RegisterClassW (&wc);
        registered = true;
    }
    g_prompt = PromptState {};
    g_prompt.result = initial;
    RECT pr {};
    GetWindowRect (parent, &pr);
    HWND dlg = CreateWindowW (L"MicFilterPrompt", title,
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              pr.left + 60, pr.top + 60, 352, 124,
                              parent, nullptr, hInst, nullptr);
    if (!dlg) return false;
    EnableWindow (parent, FALSE);
    ShowWindow (dlg, SW_SHOW);
    MSG msg {};
    while (!g_prompt.done)
    {
        if (GetMessageW (&msg, nullptr, 0, 0) <= 0) break;
        TranslateMessage (&msg);
        DispatchMessageW (&msg);
    }
    EnableWindow (parent, TRUE);
    SetForegroundWindow (parent);
    if (g_prompt.ok) out = g_prompt.result;
    return g_prompt.ok;
}
} // namespace
namespace
{
// =====================================================================
// Grouped list (v0.24, the user's wireframe): groups stacked
// vertically, filter rows inside in processing order (top = first).
// Group headers and every row carry an on/off switch; a row's "solo"
// pill auditions that filter alone; double-clicking a row opens the
// per-filter settings dialog. No drag reorder, no blocks: the order
// is the fixed canonical group order.
// =====================================================================
constexpr int kRowH = 46;
constexpr int kHeadH = 32;

struct RowGeom
{
    std::string key;
    int groupIdx = -1;
    RECT rc {};
    RECT powerRc {};
    RECT soloRc {};
};
struct GroupGeom
{
    int groupIdx = -1;
    RECT rc {};
    RECT toggleRc {};
};
std::vector<RowGeom> g_rows;
std::vector<GroupGeom> g_groupGeoms;
int g_contentH = 0;

const FilterInfo* FindFilterInfo (const std::string& key)
{
    for (int i = 0; i < kNumFilterInfos; ++i)
        if (key == kFilterRegistry[i].key) return &kFilterRegistry[i];
    return nullptr;
}

bool FilterOn (const std::string& key)
{
    const auto it = g_edit.on.find (key);
    return it != g_edit.on.end() && it->second;
}

void FillRoundRect (Gdiplus::Graphics& g, const RECT& rc, float rad, const Gdiplus::Color& col)
{
    Gdiplus::GraphicsPath path;
    const float x = (float) rc.left, y = (float) rc.top;
    const float w = (float) (rc.right - rc.left), h = (float) (rc.bottom - rc.top);
    const float d = rad * 2;
    path.AddArc (x, y, d, d, 180, 90);
    path.AddArc (x + w - d, y, d, d, 270, 90);
    path.AddArc (x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc (x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush (col);
    g.FillPath (&brush, &path);
}

void StrokeRoundRect (Gdiplus::Graphics& g, const RECT& rc, float rad, const Gdiplus::Color& col, float width = 1.0f)
{
    Gdiplus::GraphicsPath path;
    const float x = (float) rc.left, y = (float) rc.top;
    const float w = (float) (rc.right - rc.left), h = (float) (rc.bottom - rc.top);
    const float d = rad * 2;
    path.AddArc (x, y, d, d, 180, 90);
    path.AddArc (x + w - d, y, d, d, 270, 90);
    path.AddArc (x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc (x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::Pen pen (col, width);
    g.DrawPath (&pen, &path);
}

void DrawText (Gdiplus::Graphics& g, const std::wstring& text, const RECT& rc,
               float size, const Gdiplus::Color& col, bool center = false, bool bold = false)
{
    Gdiplus::FontFamily fam (L"MS UI Gothic");
    Gdiplus::Font font (&fam, size, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                        Gdiplus::UnitPoint);
    Gdiplus::SolidBrush brush (col);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment (center ? Gdiplus::StringAlignmentCenter : Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment (Gdiplus::StringAlignmentCenter);
    fmt.SetTrimming (Gdiplus::StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags (Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::RectF rf ((float) rc.left, (float) rc.top, (float) (rc.right - rc.left),
                       (float) (rc.bottom - rc.top));
    g.DrawString (text.c_str(), -1, &font, rf, &fmt, &brush);
}

void LayoutList()
{
    g_rows.clear();
    g_groupGeoms.clear();
    RECT client {};
    GetClientRect (g_board, &client);
    const int W = (int) (client.right - client.left);
    const int left = 12, right = W - 12;
    int y = 10;
    for (int gi = 0; gi < kNumFilterGroups; ++gi)
    {
        GroupGeom gg;
        gg.groupIdx = gi;
        gg.rc = { left, y, right, y + kHeadH };
        gg.toggleRc = { right - 76, y + 4, right - 4, y + kHeadH - 4 };
        g_groupGeoms.push_back (gg);
        y += kHeadH + 4;
        const FilterGroupDef& grp = kFilterGroups[gi];
        for (int i = 0; i < grp.count; ++i)
        {
            RowGeom rg;
            rg.key = grp.keys[i];
            rg.groupIdx = gi;
            rg.rc = { left, y, right, y + kRowH };
            const int cy = y + kRowH / 2;
            rg.powerRc = { left + 10, cy - 11, left + 62, cy + 11 };
            rg.soloRc = { right - 66, cy - 11, right - 10, cy + 11 };
            g_rows.push_back (rg);
            y += kRowH + 4;
        }
        y += 10;
    }
    g_contentH = y;
}

void GroupState (int gi, bool& allOn, bool& anyOn)
{
    allOn = true;
    anyOn = false;
    const FilterGroupDef& grp = kFilterGroups[gi];
    for (int i = 0; i < grp.count; ++i)
    {
        if (FilterOn (grp.keys[i])) anyOn = true;
        else allOn = false;
    }
}

std::wstring RowSummary (const std::string& key)
{
    std::wstring out;
    for (int i = 0; i < kNumKnobs; ++i)
    {
        if (kKnobs[i].filterKey != key) continue;
        if (!out.empty()) out += L" ・ ";
        out += KnobValueText (kKnobs[i], KnobValue (kKnobs[i]));
    }
    return out;
}

void PaintList (HWND hwnd, HDC hdc)
{
    RECT client {};
    GetClientRect (hwnd, &client);
    const int W = (int) (client.right - client.left), H = (int) (client.bottom - client.top);
    HDC mem = CreateCompatibleDC (hdc);
    HBITMAP bmp = CreateCompatibleBitmap (hdc, W, H + g_boardScroll + 2);
    HGDIOBJ oldBmp = SelectObject (mem, bmp);
    {
        Gdiplus::Graphics g (mem);
        g.SetSmoothingMode (Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint (Gdiplus::TextRenderingHintClearTypeGridFit);
        const Gdiplus::Color kText (255, 228, 230, 234);
        const Gdiplus::Color kDim (255, 150, 154, 160);
        Gdiplus::SolidBrush bg (Gdiplus::Color (255, 30, 31, 36));
        g.FillRectangle (&bg, 0, 0, W, H + g_boardScroll + 2);
        LayoutList();
        for (const auto& gg : g_groupGeoms)
        {
            RECT nameRc { gg.rc.left + 4, gg.rc.top, gg.toggleRc.left - 8, gg.rc.bottom };
            DrawText (g, kFilterGroups[gg.groupIdx].name, nameRc, 11, kText, false, true);
            bool allOn, anyOn;
            GroupState (gg.groupIdx, allOn, anyOn);
            const Gdiplus::Color fill = allOn ? Gdiplus::Color (255, 38, 84, 52)
                                        : anyOn ? Gdiplus::Color (255, 120, 92, 30)
                                                : Gdiplus::Color (255, 58, 61, 68);
            FillRoundRect (g, gg.toggleRc, 8, fill);
            DrawText (g, allOn ? L"ON" : anyOn ? L"一部ON" : L"OFF", gg.toggleRc, 8,
                      (allOn || anyOn) ? kText : kDim, true);
        }
        for (const auto& rg : g_rows)
        {
            const bool on = FilterOn (rg.key);
            const FilterInfo* info = FindFilterInfo (rg.key);
            FillRoundRect (g, rg.rc, 8, on ? Gdiplus::Color (255, 54, 57, 64)
                                           : Gdiplus::Color (255, 40, 42, 47));
            if (g_selKey == rg.key)
                StrokeRoundRect (g, rg.rc, 8, Gdiplus::Color (255, 235, 238, 242), 1.5f);
            FillRoundRect (g, rg.powerRc, 8, on ? Gdiplus::Color (255, 38, 84, 52)
                                                : Gdiplus::Color (255, 58, 61, 68));
            DrawText (g, on ? L"ON" : L"OFF", rg.powerRc, 8, on ? kText : kDim, true);
            RECT nameRc { rg.rc.left + 74, rg.rc.top, rg.rc.right - 340, rg.rc.bottom };
            DrawText (g, info ? info->label : L"?", nameRc, 10, on ? kText : kDim, false, true);
            const std::wstring sum = RowSummary (rg.key);
            if (!sum.empty())
            {
                RECT sumRc { rg.rc.right - 336, rg.rc.top, rg.soloRc.left - 10, rg.rc.bottom };
                Gdiplus::FontFamily fam (L"MS UI Gothic");
                Gdiplus::Font font (&fam, 8.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                Gdiplus::SolidBrush brush (kDim);
                Gdiplus::StringFormat fmt;
                fmt.SetAlignment (Gdiplus::StringAlignmentFar);
                fmt.SetLineAlignment (Gdiplus::StringAlignmentCenter);
                fmt.SetTrimming (Gdiplus::StringTrimmingEllipsisCharacter);
                fmt.SetFormatFlags (Gdiplus::StringFormatFlagsNoWrap);
                Gdiplus::RectF rf ((float) sumRc.left, (float) sumRc.top,
                                   (float) (sumRc.right - sumRc.left), (float) (sumRc.bottom - sumRc.top));
                g.DrawString (sum.c_str(), -1, &font, rf, &fmt, &brush);
            }
            const bool soloOn = !g_soloKey.empty() && g_soloKey == rg.key;
            FillRoundRect (g, rg.soloRc, 8, soloOn ? Gdiplus::Color (255, 148, 106, 24)
                                                    : Gdiplus::Color (255, 58, 61, 68));
            DrawText (g, soloOn ? L"ソロ中" : L"ソロ", rg.soloRc, 8, soloOn ? kText : kDim, true);
        }
        if (g_editLocked)
        {
            Gdiplus::SolidBrush wash (Gdiplus::Color (40, 190, 196, 205));
            g.FillRectangle (&wash, 0, 0, W, H + g_boardScroll + 2);
        }
    }
    BitBlt (hdc, 0, 0, W, H, mem, 0, g_boardScroll, SRCCOPY);
    SelectObject (mem, oldBmp);
    DeleteObject (bmp);
    DeleteDC (mem);
}

void RefreshBoard()
{
    if (g_board) InvalidateRect (g_board, nullptr, FALSE);
}

bool InRect (const RECT& rc, POINT pt)
{
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

struct ListHit
{
    int kind = 0; // 1 power, 2 solo, 3 group toggle, 4 row
    const RowGeom* row = nullptr;
    int groupIdx = -1;
};

ListHit HitList (POINT pt)
{
    ListHit h;
    for (const auto& gg : g_groupGeoms)
        if (InRect (gg.toggleRc, pt)) { h.kind = 3; h.groupIdx = gg.groupIdx; return h; }
    for (const auto& rg : g_rows)
    {
        if (InRect (rg.powerRc, pt)) { h.kind = 1; h.row = &rg; return h; }
        if (InRect (rg.soloRc, pt)) { h.kind = 2; h.row = &rg; return h; }
        if (InRect (rg.rc, pt)) { h.kind = 4; h.row = &rg; return h; }
    }
    return h;
}

// --- Per-filter settings dialog -----------------------------------------
// Opened by double-clicking a row: the filter's own window with an
// on/off checkbox and one slider per knob (plus the breath checkbox
// for the gate). Changes apply live; the preset file is untouched for
// protected presets (controls disabled there instead).
std::string g_dlgKey;
HWND g_dlg = nullptr;
bool g_dlgDone = false;
constexpr int D_ONOFF = 3001;
constexpr int D_BREATH = 3002;
constexpr int D_CLOSE = 3003;
constexpr int D_TRACK0 = 3100; // + knob index
constexpr int D_LABEL0 = 3200; // + knob index

std::vector<const KnobDef*> DialogKnobs()
{
    std::vector<const KnobDef*> out;
    for (int i = 0; i < kNumKnobs; ++i)
        if (kKnobs[i].filterKey == g_dlgKey) out.push_back (&kKnobs[i]);
    return out;
}

int TrackTicks (const KnobDef& def)
{
    const auto& r = MicFilterParams::rangeFor (def.paramId);
    const double span = (double) r.max - r.min;
    int ticks = def.step > 0 ? (int) std::lround (span / def.step) : 100;
    return std::max (2, std::min (400, ticks));
}

int TrackPos (const KnobDef& def, double v)
{
    const auto& r = MicFilterParams::rangeFor (def.paramId);
    const double t = (v - r.min) / ((double) r.max - r.min);
    return (int) std::lround (t * TrackTicks (def));
}

double TrackValue (const KnobDef& def, int pos)
{
    const auto& r = MicFilterParams::rangeFor (def.paramId);
    double v = r.min + ((double) r.max - r.min) * pos / TrackTicks (def);
    if (def.step > 0) v = r.min + std::round ((v - r.min) / def.step) * def.step;
    return std::max ((double) r.min, std::min ((double) r.max, v));
}

void DialogRefreshLabels (HWND hwnd)
{
    const auto knobs = DialogKnobs();
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        HWND lab = GetDlgItem (hwnd, D_LABEL0 + (int) i);
        if (lab) SetWindowTextW (lab, KnobValueText (*knobs[i], KnobValue (*knobs[i])).c_str());
    }
}

LRESULT CALLBACK FilterDlgProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            const FilterInfo* info = FindFilterInfo (g_dlgKey);
            const bool locked = g_editLocked;
            HWND chk = CreateWindowW (L"BUTTON", info ? info->label : L"フィルター",
                                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      16, 12, 380, 22, hwnd,
                                      reinterpret_cast<HMENU> ((INT_PTR) D_ONOFF),
                                      nullptr, nullptr);
            SendMessageW (chk, WM_SETFONT, (WPARAM) g_font, TRUE);
            SendMessageW (chk, BM_SETCHECK, FilterOn (g_dlgKey) ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow (chk, locked ? FALSE : TRUE);
            int y = 48;
            const auto knobs = DialogKnobs();
            if (knobs.empty())
            {
                HWND st = CreateWindowW (L"STATIC", L"調整できる項目はありません",
                                         WS_CHILD | WS_VISIBLE, 16, y, 380, 20, hwnd,
                                         nullptr, nullptr, nullptr);
                SendMessageW (st, WM_SETFONT, (WPARAM) g_font, TRUE);
                y += 30;
            }
            for (size_t i = 0; i < knobs.size(); ++i)
            {
                HWND lab = CreateWindowW (L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                         16, y, 380, 18, hwnd,
                                         reinterpret_cast<HMENU> ((INT_PTR) (D_LABEL0 + (int) i)),
                                         nullptr, nullptr);
                SendMessageW (lab, WM_SETFONT, (WPARAM) g_font, TRUE);
                HWND tr = CreateWindowW (TRACKBAR_CLASSW, L"",
                                         WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                         16, y + 18, 380, 26, hwnd,
                                         reinterpret_cast<HMENU> ((INT_PTR) (D_TRACK0 + (int) i)),
                                         nullptr, nullptr);
                SendMessageW (tr, TBM_SETRANGE, TRUE, MAKELPARAM (0, TrackTicks (*knobs[i])));
                SendMessageW (tr, TBM_SETPOS, TRUE, TrackPos (*knobs[i], KnobValue (*knobs[i])));
                EnableWindow (tr, locked ? FALSE : TRUE);
                y += 52;
            }
            if (g_dlgKey == "gate")
            {
                HWND br = CreateWindowW (L"BUTTON", L"吐息キープ（息の音を残す）",
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         16, y, 380, 22, hwnd,
                                         reinterpret_cast<HMENU> ((INT_PTR) D_BREATH),
                                         nullptr, nullptr);
                SendMessageW (br, WM_SETFONT, (WPARAM) g_font, TRUE);
                SendMessageW (br, BM_SETCHECK, FilterOn ("breath") ? BST_CHECKED : BST_UNCHECKED, 0);
                EnableWindow (br, locked ? FALSE : TRUE);
                y += 32;
            }
            HWND cl = CreateWindowW (L"BUTTON", L"閉じる", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                     296, y + 4, 100, 28, hwnd,
                                     reinterpret_cast<HMENU> ((INT_PTR) D_CLOSE),
                                     nullptr, nullptr);
            SendMessageW (cl, WM_SETFONT, (WPARAM) g_font, TRUE);
            DialogRefreshLabels (hwnd);
            return 0;
        }
        case WM_COMMAND:
        {
            const int id = LOWORD (wp);
            if (id == D_CLOSE) { DestroyWindow (hwnd); return 0; }
            if (id == D_ONOFF && !g_editLocked)
            {
                g_edit.on[g_dlgKey] = SendMessageW ((HWND) lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                Commit();
                RefreshBoard();
                return 0;
            }
            if (id == D_BREATH && !g_editLocked)
            {
                g_edit.on["breath"] = SendMessageW ((HWND) lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                Commit();
                RefreshBoard();
                return 0;
            }
            break;
        }
        case WM_HSCROLL:
        {
            if (g_editLocked) return 0;
            const auto knobs = DialogKnobs();
            for (size_t i = 0; i < knobs.size(); ++i)
            {
                if ((HWND) lp != GetDlgItem (hwnd, D_TRACK0 + (int) i)) continue;
                const int pos = (int) SendMessageW ((HWND) lp, TBM_GETPOS, 0, 0);
                g_edit.values[knobs[i]->valueKey] = TrackValue (*knobs[i], pos);
                Commit();
                DialogRefreshLabels (hwnd);
                RefreshBoard();
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow (hwnd);
            return 0;
        case WM_DESTROY:
            g_dlg = nullptr;
            g_dlgDone = true;
            return 0;
    }
    return DefWindowProcW (hwnd, msg, wp, lp);
}

void ShowFilterDialog (const std::string& key)
{
    g_dlgKey = key;
    g_dlgDone = false;
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSW wc {};
        wc.lpfnWndProc = FilterDlgProc;
        wc.hInstance = GetModuleHandleW (nullptr);
        wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MicFilterFilterDlg";
        RegisterClassW (&wc);
        reg = true;
    }
    INITCOMMONCONTROLSEX icc { sizeof (icc), ICC_BAR_CLASSES };
    InitCommonControlsEx (&icc);
    const FilterInfo* info = FindFilterInfo (key);
    std::wstring title = info ? info->label : L"フィルター設定";
    const int knobCount = (int) DialogKnobs().size();
    const int height = 48 + (knobCount ? knobCount * 52 : 30) + (key == "gate" ? 32 : 0) + 52;
    EnableWindow (g_win, FALSE);
    RECT wr {};
    GetWindowRect (g_win, &wr);
    const int dx = (int) (wr.left + ((wr.right - wr.left) - 428) / 2);
    const int dy = (int) (wr.top + ((wr.bottom - wr.top) - height) / 2);
    g_dlg = CreateWindowW (L"MicFilterFilterDlg", title.c_str(),
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                           dx, dy, 428, height,
                           g_win, nullptr, GetModuleHandleW (nullptr), nullptr);
    if (g_dlg) ShowWindow (g_dlg, SW_SHOW);
    MSG msg {};
    while (!g_dlgDone && GetMessageW (&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage (&msg);
        DispatchMessageW (&msg);
    }
    EnableWindow (g_win, TRUE);
    SetForegroundWindow (g_win);
}

LRESULT CALLBACK BoardProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps {};
            HDC hdc = BeginPaint (hwnd, &ps);
            PaintList (hwnd, hdc);
            EndPaint (hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        {
            SetFocus (hwnd);
            LayoutList();
            const POINT clientPt { (short) LOWORD (lp), (short) HIWORD (lp) };
            const POINT pt { clientPt.x, clientPt.y + g_boardScroll };
            const ListHit h = HitList (pt);
            if (h.kind == 2 && h.row)
            {
                // Solo: audition only, allowed even when locked.
                if (g_editIsMainCurrent)
                {
                    g_soloKey = (g_soloKey == h.row->key) ? "" : h.row->key;
                    SendSolo();
                    g_selKey = h.row->key;
                    RefreshBoard();
                }
                return 0;
            }
            if (g_editLocked)
            {
                if (h.kind == 4 && h.row) { g_selKey = h.row->key; RefreshBoard(); }
                return 0;
            }
            switch (h.kind)
            {
                case 1:
                    g_edit.on[h.row->key] = !FilterOn (h.row->key);
                    g_selKey = h.row->key;
                    Commit();
                    RefreshBoard();
                    break;
                case 3:
                {
                    bool allOn, anyOn;
                    GroupState (h.groupIdx, allOn, anyOn);
                    const FilterGroupDef& grp = kFilterGroups[h.groupIdx];
                    for (int i = 0; i < grp.count; ++i) g_edit.on[grp.keys[i]] = !allOn;
                    Commit();
                    RefreshBoard();
                    break;
                }
                case 4:
                    g_selKey = h.row->key;
                    RefreshBoard();
                    break;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:
        {
            LayoutList();
            const POINT clientPt { (short) LOWORD (lp), (short) HIWORD (lp) };
            const POINT pt { clientPt.x, clientPt.y + g_boardScroll };
            const ListHit h = HitList (pt);
            if (h.kind == 4 && h.row)
            {
                g_selKey = h.row->key;
                RefreshBoard();
                ShowFilterDialog (h.row->key);
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            RECT client {};
            GetClientRect (hwnd, &client);
            const int viewH = (int) (client.bottom - client.top);
            const int maxScroll = std::max (0, g_contentH - viewH + 8);
            const int delta = (short) HIWORD (wp);
            g_boardScroll = std::max (0, std::min (maxScroll, g_boardScroll - delta / 2));
            RefreshBoard();
            return 0;
        }
        case WM_SIZE:
            RefreshBoard();
            return 0;
    }
    return DefWindowProcW (hwnd, msg, wp, lp);
}
// --- Preset bar / editor state ---------------------------------------------------
void RefreshPresetCombo (const std::wstring& selectName)
{
    const std::vector<std::wstring> names = ListPresetNames();
    HWND combo = SCtl (S_PRESET_COMBO);
    SendMessageW (combo, CB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t i = 0; i < names.size(); ++i)
    {
        SendMessageW (combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM> (names[i].c_str()));
        if (names[i] == selectName) select = (int) i;
    }
    if (!names.empty()) SendMessageW (combo, CB_SETCURSEL, select, 0);
}

std::wstring SelectedPresetName()
{
    HWND combo = SCtl (S_PRESET_COMBO);
    const LRESULT sel = SendMessageW (combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return L"";
    wchar_t buf[256];
    SendMessageW (combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM> (buf));
    return buf;
}

void ApplyLockState()
{
    const BOOL en = g_editLocked ? FALSE : TRUE;
    EnableWindow (SCtl (S_PRESET_RENAME), en);
    EnableWindow (SCtl (S_PRESET_DELETE), en);
    SetWindowTextW (SCtl (S_LOCK_LABEL), g_editLocked ? L"変更不可" : L"");
}

void LoadIntoEditor (const std::wstring& name)
{
    ClearSolo(); // solo belongs to the previously shown preset
    Preset p;
    if (!LoadPreset (name, p)) p = DefaultPreset();
    g_edit = p;
    // v0.24: the list IS the order. Normalize every edited preset to
    // the canonical group order; order blocks no longer exist.
    g_edit.order.assign (kCanonicalStageKeys, kCanonicalStageKeys + kNumCanonicalStageKeys);
    g_edit.blocks.clear();
    g_editIsMainCurrent = g_edit.name == g_mainCurrent;
    g_editLocked = IsProtectedPreset (g_edit.name);
    g_selKey = "gate";
    g_boardScroll = 0;
    RefreshBoard();
    ApplyLockState();
    wchar_t title[340];
    swprintf_s (title, g_editLocked ? L"フィルター設定 - %s（変更不可）" : L"フィルター設定 - %s",
                g_edit.name.c_str());
    SetWindowTextW (g_win, title);
}

// --- Actions ------------------------------------------------------------------
void OnPresetNew()
{
    std::wstring name;
    if (!PromptText (g_win, L"新規プリセット（標準のコピー）", L"", name)) return;
    for (const auto& n : ListPresetNames())
        if (n == name)
        {
            MessageBoxW (g_win, L"同じ名前のプリセットがあります", L"MicFilter", MB_OK | MB_ICONWARNING);
            return;
        }
    Preset np = DefaultPreset(); // new presets always start from 標準
    np.name = name;
    SavePreset (np);
    RefreshPresetCombo (name);
    LoadIntoEditor (name);
}

void OnPresetRename()
{
    if (g_editLocked) return;
    std::wstring name;
    if (!PromptText (g_win, L"プリセット名の変更", g_edit.name, name)) return;
    if (name == g_edit.name) return;
    for (const auto& n : ListPresetNames())
        if (n == name)
        {
            MessageBoxW (g_win, L"同じ名前のプリセットがあります", L"MicFilter", MB_OK | MB_ICONWARNING);
            return;
        }
    const std::wstring old = g_edit.name;
    const bool wasMain = g_editIsMainCurrent;
    g_edit.name = name;
    SavePreset (g_edit);
    DeletePreset (old);
    if (wasMain)
    {
        g_mainCurrent = name;
        NotifyMainSelect (name);
    }
    RefreshPresetCombo (name);
    LoadIntoEditor (name);
}

void OnPresetDelete()
{
    if (g_editLocked) return;
    if (MessageBoxW (g_win, L"このプリセットを削除しますか？", L"MicFilter",
                     MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    const bool wasMain = g_editIsMainCurrent;
    DeletePreset (g_edit.name);
    const std::wstring next = L"標準";
    if (wasMain)
    {
        g_mainCurrent = next;
        NotifyMainSelect (next);
    }
    RefreshPresetCombo (next);
    LoadIntoEditor (next);
}

LRESULT CALLBACK SettingsProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            g_win = hwnd;
            g_font = (HFONT) GetStockObject (DEFAULT_GUI_FONT);
            auto make = [&] (HWND h) { SendMessageW (h, WM_SETFONT, (WPARAM) g_font, TRUE); return h; };
            auto label = [&] (const wchar_t* text, int x, int y, int w) {
                make (CreateWindowW (L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 20,
                                     hwnd, nullptr, nullptr, nullptr));
            };
            auto button = [&] (const wchar_t* text, int id, int x, int y, int w, int h = 26) {
                return make (CreateWindowW (L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                            x, y, w, h, hwnd,
                                            reinterpret_cast<HMENU> ((INT_PTR) id), nullptr, nullptr));
            };

            label (L"プリセット", 16, 13, 64);
            make (CreateWindowW (L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 84, 9, 250, 200, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) S_PRESET_COMBO), nullptr, nullptr));
            make (CreateWindowW (L"STATIC", L"", WS_CHILD | WS_VISIBLE, 342, 13, 110, 20,
                                 hwnd, reinterpret_cast<HMENU> ((INT_PTR) S_LOCK_LABEL),
                                 nullptr, nullptr));
            button (L"新規", S_PRESET_NEW, 470, 8, 54, 25);
            button (L"名前変更", S_PRESET_RENAME, 528, 8, 68, 25);
            button (L"削除", S_PRESET_DELETE, 600, 8, 50, 25);

            label (L"上から順に音が通ります。「ソロ」＝そのフィルターだけ聞く／行をダブルクリックで詳細設定",
                   16, 45, 900);

            g_board = CreateWindowW (L"MicFilterBoard", L"",
                                     WS_CHILD | WS_VISIBLE | WS_BORDER,
                                     16, 74, 952, 538, hwnd, nullptr,
                                     GetModuleHandleW (nullptr), nullptr);

            button (L"このプリセットを使う", S_APPLY, 664, 622, 150, 30);
            button (L"閉じる", S_CLOSE, 828, 622, 140, 30);

            EnsurePresetStore();
            RefreshPresetCombo (g_mainCurrent);
            const std::wstring sel = SelectedPresetName();
            LoadIntoEditor (!sel.empty() ? sel : g_mainCurrent);
            return 0;
        }
        case WM_COMMAND:
        {
            const int id = LOWORD (wp);
            switch (id)
            {
                case S_PRESET_COMBO:
                    if (HIWORD (wp) == CBN_SELCHANGE)
                    {
                        const std::wstring name = SelectedPresetName();
                        if (!name.empty() && name != g_edit.name) LoadIntoEditor (name);
                    }
                    return 0;
                case S_PRESET_NEW: OnPresetNew(); return 0;
                case S_PRESET_RENAME: OnPresetRename(); return 0;
                case S_PRESET_DELETE: OnPresetDelete(); return 0;

                case S_APPLY:
                    g_mainCurrent = g_edit.name;
                    g_editIsMainCurrent = true;
                    NotifyMainSelect (g_edit.name);
                    return 0;
                case S_CLOSE: DestroyWindow (hwnd); return 0;
            }
            break;
        }
        case WM_CLOSE:
            ClearSolo();
            DestroyWindow (hwnd);
            return 0;
        case WM_DESTROY:
            g_win = nullptr;
            g_board = nullptr;
            return 0;
    }
    return DefWindowProcW (hwnd, msg, wp, lp);
}
} // namespace

void OpenSettingsWindow (HWND parent, const std::wstring& mainCurrentPreset)
{
    if (g_win)
    {
        SetForegroundWindow (g_win);
        return;
    }
    g_parent = parent;
    g_mainCurrent = mainCurrentPreset;
    static bool registered = false;
    static ULONG_PTR gdiplusToken = 0;
    HINSTANCE hInst = GetModuleHandleW (nullptr);
    if (!registered)
    {
        Gdiplus::GdiplusStartupInput gdiInput;
        Gdiplus::GdiplusStartup (&gdiplusToken, &gdiInput, nullptr);
        WNDCLASSW wc {};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MicFilterSettingsWindow";
        RegisterClassW (&wc);
        WNDCLASSW bc {};
        bc.lpfnWndProc = BoardProc;
        bc.hInstance = hInst;
        bc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
        bc.hbrBackground = nullptr;
        bc.style = CS_DBLCLKS;
        bc.lpszClassName = L"MicFilterBoard";
        RegisterClassW (&bc);
        registered = true;
    }
    INITCOMMONCONTROLSEX icc { sizeof (icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx (&icc);
    g_boardScroll = 0;
    HWND win = CreateWindowW (L"MicFilterSettingsWindow", L"フィルター設定",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
                              parent, nullptr, hInst, nullptr);
    if (win) ShowWindow (win, SW_SHOW);
}
