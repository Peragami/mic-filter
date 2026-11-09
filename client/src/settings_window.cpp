// settings_window.cpp -- see settings_window.h.
// v0.16: one unified chain view. Rows are the DSP stages in application
// order; each row carries its on/off toggle, its group tag, and its
// knobs (sliders for the filter's value parameters). Groups are managed
// from the bar above the list. Everything autosaves.
#include "settings_window.h"

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
constexpr int S_PRESET_LIST = 2001;
constexpr int S_PRESET_NEW = 2002;
constexpr int S_PRESET_RENAME = 2003;
constexpr int S_PRESET_DELETE = 2004;
constexpr int S_GROUP_COMBO = 2005;
constexpr int S_GROUP_ADD = 2006;
constexpr int S_GROUP_RENAME = 2007;
constexpr int S_GROUP_DELETE = 2008;
constexpr int S_GROUP_ALL_ON = 2009;
constexpr int S_GROUP_ALL_OFF = 2010;
constexpr int S_MOVE_BUTTON = 2011;
constexpr int S_BLOCK_CREATE = 2012;
constexpr int S_APPLY = 2017;
constexpr int S_CLOSE = 2018;
// Block header pool ids: HB_BASE + pool * 3 + {0 name, 1 collapse, 2 dissolve}
constexpr int HB_BASE = 4400;
constexpr int kMaxHeaderPool = 8;

// Row control ids: R_BASE + slot * 16 + sub
// sub: 2 = toggle (also the drag handle), 3 = breath (gate only), 4+j = knob slider j
constexpr int R_BASE = 4000;
constexpr int kRowH = 46;
constexpr int kNumSlots = 15;
// Stage slots in registry order (the 15 reorderable filters).
const char* kSlotKeys[kNumSlots] = {
    "hum", "multiband", "spectral", "rnnoise", "hpf", "eq", "deesser",
    "exciter", "bwe", "gate", "comp", "limiter", "duck", "declick", "declip"
};

struct RowControls
{
    HWND toggle = nullptr, tag = nullptr;
    HWND breath = nullptr;              // gate row only
    HWND knobLabel[3] = {};
    HWND knobSlider[3] = {};
    int knobCount = 0;
    const KnobDef* knobs[3] = {};
};

HWND g_win = nullptr;
HWND g_parent = nullptr;
HWND g_panel = nullptr;
HWND g_frame = nullptr;
RowControls g_rows[kNumSlots];

// Block header pool (blocks are dynamic, headers are reused controls).
struct HeaderControls
{
    HWND nameBtn = nullptr, collapseBtn = nullptr, dissolveBtn = nullptr;
    int blockIdx = -1;
};
HeaderControls g_headers[kMaxHeaderPool];

// Display items of the chain view: block headers + visible rows.
struct DisplayItem
{
    bool header = false;
    int blockIdx = -1;   // when header
    std::string key;     // when row
    int top = 0, height = 0;
};
std::vector<DisplayItem> g_items;

// Drag state (rows and block headers are dragged by their buttons).
HWND g_dragHwnd = nullptr;
DWORD_PTR g_dragRef = 0;
bool g_dragging = false;
int g_dragStartPanelY = 0;
int g_dragPanelY = 0;
int g_dragKind = 0; // 1 = row, 2 = block
std::string g_dragKey;
int g_dragBlock = -1;
std::wstring g_mainCurrent;
Preset g_edit;
bool g_editIsMainCurrent = false;
std::string g_selKey = "gate"; // selected row (for group moves)
int g_scroll = 0;
HFONT g_font = nullptr;

HWND SCtl (int id) { return GetDlgItem (g_win, id); }

int SlotOf (const std::string& key)
{
    for (int s = 0; s < kNumSlots; ++s)
        if (key == kSlotKeys[s]) return s;
    return -1;
}

int RowId (int slot, int sub) { return R_BASE + slot * 16 + sub; }

void Commit()
{
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

std::wstring GroupNameOf (const std::string& key)
{
    for (const auto& g : g_edit.groups)
        for (const auto& k : g.filters)
            if (k == key) return g.name;
    return L"";
}

void SetToggleText (HWND button, bool on, const wchar_t* label)
{
    const std::wstring text = std::wstring (on ? L"● " : L"○ ") + label;
    SetWindowTextW (button, text.c_str());
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
int BlockOf (const std::string& key)
{
    for (size_t b = 0; b < g_edit.blocks.size(); ++b)
        for (const auto& k : g_edit.blocks[b].filters)
            if (k == key) return (int) b;
    return -1;
}

void BuildItems()
{
    g_items.clear();
    std::set<int> emitted;
    for (const auto& key : g_edit.order)
    {
        const int b = BlockOf (key);
        if (b >= 0)
        {
            if (!emitted.count (b))
            {
                emitted.insert (b);
                DisplayItem h;
                h.header = true;
                h.blockIdx = b;
                h.height = 28;
                g_items.push_back (h);
            }
            if (g_edit.blocks[(size_t) b].collapsed) continue; // member hidden
        }
        DisplayItem r;
        r.key = key;
        r.height = kRowH;
        g_items.push_back (r);
    }
    int y = 0;
    for (auto& it : g_items) { it.top = y; y += it.height; }
}

int ContentHeight()
{
    return g_items.empty() ? 0 : g_items.back().top + g_items.back().height;
}

void LayoutRows()
{
    if (!g_panel) return;
    BuildItems();
    RECT rc {};
    GetClientRect (g_panel, &rc);
    const int panelH = (int) (rc.bottom - rc.top);
    const int contentH = ContentHeight();
    const int maxScroll = std::max (0, contentH - panelH);
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    if (g_scroll < 0) g_scroll = 0;

    SCROLLINFO si { sizeof (si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, contentH, (UINT) panelH, g_scroll, 0 };
    SetScrollInfo (g_panel, SB_VERT, &si, TRUE);

    std::set<int> shownSlots;
    int headerPool = 0;
    bool framePlaced = false;
    for (const auto& item : g_items)
    {
        const int y = item.top - g_scroll + 2;
        if (item.header)
        {
            if (headerPool >= kMaxHeaderPool) continue;
            HeaderControls& h = g_headers[headerPool++];
            h.blockIdx = item.blockIdx;
            const OrderBlock& blk = g_edit.blocks[(size_t) item.blockIdx];
            const std::wstring text = std::wstring (blk.collapsed ? L"▶ " : L"▼ ") + blk.name +
                                      L"（" + std::to_wstring (blk.filters.size()) + L"）";
            SetWindowTextW (h.nameBtn, text.c_str());
            SetWindowTextW (h.collapseBtn, blk.collapsed ? L"開く" : L"閉じる");
            SetWindowPos (h.nameBtn, nullptr, 30, y, 470, 24, SWP_NOZORDER | SWP_SHOWWINDOW);
            SetWindowPos (h.collapseBtn, nullptr, 508, y, 80, 24, SWP_NOZORDER | SWP_SHOWWINDOW);
            SetWindowPos (h.dissolveBtn, nullptr, 596, y, 80, 24, SWP_NOZORDER | SWP_SHOWWINDOW);
            continue;
        }
        const int s = SlotOf (item.key);
        if (s < 0) continue;
        shownSlots.insert (s);
        RowControls& r = g_rows[s];
        const int xoff = BlockOf (item.key) >= 0 ? 16 : 0;
        SetWindowPos (r.toggle, nullptr, 6 + xoff, y, 206, 42, SWP_NOZORDER | SWP_SHOWWINDOW);
        if (r.breath)
        {
            SetWindowPos (r.breath, nullptr, 226 + xoff, y, 128, 20, SWP_NOZORDER | SWP_SHOWWINDOW);
            SetWindowPos (r.tag, nullptr, 226 + xoff, y + 23, 128, 16, SWP_NOZORDER | SWP_SHOWWINDOW);
        }
        else SetWindowPos (r.tag, nullptr, 226 + xoff, y + 12, 128, 16, SWP_NOZORDER | SWP_SHOWWINDOW);
        for (int j = 0; j < r.knobCount; ++j)
        {
            const int x = 362 + xoff + j * 124;
            SetWindowPos (r.knobLabel[j], nullptr, x, y, 120, 13, SWP_NOZORDER | SWP_SHOWWINDOW);
            SetWindowPos (r.knobSlider[j], nullptr, x, y + 14, 120, 28, SWP_NOZORDER | SWP_SHOWWINDOW);
        }
        if (!g_dragging && item.key == g_selKey)
        {
            SetWindowPos (g_frame, nullptr, 2, y - 2, 744, kRowH, SWP_NOZORDER | SWP_SHOWWINDOW);
            framePlaced = true;
        }
    }
    for (; headerPool < kMaxHeaderPool; ++headerPool)
    {
        ShowWindow (g_headers[headerPool].nameBtn, SW_HIDE);
        ShowWindow (g_headers[headerPool].collapseBtn, SW_HIDE);
        ShowWindow (g_headers[headerPool].dissolveBtn, SW_HIDE);
        g_headers[headerPool].blockIdx = -1;
    }
    for (int s = 0; s < kNumSlots; ++s)
    {
        if (shownSlots.count (s)) continue;
        RowControls& r = g_rows[s];
        ShowWindow (r.toggle, SW_HIDE);
        ShowWindow (r.tag, SW_HIDE);
        if (r.breath) ShowWindow (r.breath, SW_HIDE);
        for (int j = 0; j < r.knobCount; ++j)
        {
            ShowWindow (r.knobLabel[j], SW_HIDE);
            ShowWindow (r.knobSlider[j], SW_HIDE);
        }
    }
    if (!g_dragging && !framePlaced) ShowWindow (g_frame, SW_HIDE);
}

void RefreshChain()
{
    for (int s = 0; s < kNumSlots; ++s)
    {
        RowControls& r = g_rows[s];
        const std::string key = kSlotKeys[s];
        const FilterInfo* info = FindFilter (key);
        const auto it = g_edit.on.find (key);
        const bool on = it != g_edit.on.end() && it->second;
        SendMessageW (r.toggle, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        SetToggleText (r.toggle, on, info ? info->label : L"?");
        SetWindowTextW (r.tag, GroupNameOf (key).c_str());
        for (int j = 0; j < r.knobCount; ++j)
        {
            const KnobDef& def = *r.knobs[j];
            const auto& range = MicFilterParams::rangeFor (def.paramId);
            const double v = KnobValue (def);
            const int pos = (int) std::lround ((v - range.min) / (range.max - range.min) * 1000.0);
            SendMessageW (r.knobSlider[j], TBM_SETPOS, TRUE, pos);
            SetWindowTextW (r.knobLabel[j], KnobValueText (def, v).c_str());
        }
    }
    if (g_rows[SlotOf ("gate")].breath)
    {
        const auto it = g_edit.on.find ("breath");
        const bool on = it != g_edit.on.end() && it->second;
        HWND b = g_rows[SlotOf ("gate")].breath;
        SendMessageW (b, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        SetToggleText (b, on, L"吐息キープ");
    }
    LayoutRows();
}

void RefreshGroupCombo (int select = -2)
{
    HWND combo = SCtl (S_GROUP_COMBO);
    const LRESULT cur = select == -2 ? SendMessageW (combo, CB_GETCURSEL, 0, 0) : select;
    SendMessageW (combo, CB_RESETCONTENT, 0, 0);
    for (const auto& g : g_edit.groups)
        SendMessageW (combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM> (g.name.c_str()));
    if (!g_edit.groups.empty())
        SendMessageW (combo, CB_SETCURSEL, std::clamp ((int) cur, 0, (int) g_edit.groups.size() - 1), 0);
}

int SelectedGroup()
{
    const LRESULT sel = SendMessageW (SCtl (S_GROUP_COMBO), CB_GETCURSEL, 0, 0);
    return sel == CB_ERR ? -1 : (int) sel;
}

void RefreshPresetList (const std::wstring& selectName)
{
    const std::vector<std::wstring> names = ListPresetNames();
    HWND lb = SCtl (S_PRESET_LIST);
    SendMessageW (lb, LB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t i = 0; i < names.size(); ++i)
    {
        SendMessageW (lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM> (names[i].c_str()));
        if (names[i] == selectName) select = (int) i;
    }
    if (!names.empty()) SendMessageW (lb, LB_SETCURSEL, select, 0);
}

std::wstring SelectedPresetName()
{
    HWND lb = SCtl (S_PRESET_LIST);
    const LRESULT sel = SendMessageW (lb, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return L"";
    wchar_t buf[256];
    SendMessageW (lb, LB_GETTEXT, sel, reinterpret_cast<LPARAM> (buf));
    return buf;
}

void LoadIntoEditor (const std::wstring& name)
{
    Preset p;
    if (!LoadPreset (name, p)) p = DefaultPreset();
    g_edit = p;
    g_editIsMainCurrent = g_edit.name == g_mainCurrent;
    g_selKey = "gate";
    g_scroll = 0;
    RefreshGroupCombo (-1);
    RefreshChain();
    wchar_t title[320];
    swprintf_s (title, L"フィルター設定 - %s", g_edit.name.c_str());
    SetWindowTextW (g_win, title);
}

LRESULT CALLBACK ChainPanelProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_COMMAND:
        case WM_HSCROLL:
            return SendMessageW (g_win, msg, wp, lp);
        case WM_VSCROLL:
        {
            RECT rc {};
            GetClientRect (hwnd, &rc);
            const int page = (int) (rc.bottom - rc.top);
            int y = g_scroll;
            switch (LOWORD (wp))
            {
                case SB_LINEUP: y -= kRowH / 2; break;
                case SB_LINEDOWN: y += kRowH / 2; break;
                case SB_PAGEUP: y -= page; break;
                case SB_PAGEDOWN: y += page; break;
                case SB_THUMBTRACK: y = HIWORD (wp); break;
                default: return 0;
            }
            g_scroll = y;
            LayoutRows();
            return 0;
        }
        case WM_MOUSEWHEEL:
            g_scroll -= (int) (GET_WHEEL_DELTA_WPARAM (wp) / 120.0f * kRowH);
            LayoutRows();
            return 0;
    }
    return DefWindowProcW (hwnd, msg, wp, lp);
}

// --- Drag & drop reordering ---------------------------------------------------
bool IsDraggedKey (const std::string& key)
{
    if (g_dragKind == 1) return key == g_dragKey;
    if (g_dragKind == 2 && g_dragBlock >= 0 && g_dragBlock < (int) g_edit.blocks.size())
        for (const auto& k : g_edit.blocks[(size_t) g_dragBlock].filters)
            if (k == key) return true;
    return false;
}

// From a content-space Y, find where the drop lands: the key that would
// end up right after the dragged item(s), and the block the drop is
// inside of (by name), if any.
void ComputeInsertion (int contentY, std::string& beforeKey, std::wstring& insideBlockName)
{
    beforeKey.clear();
    insideBlockName.clear();
    size_t idx = g_items.size();
    for (size_t i = 0; i < g_items.size(); ++i)
        if (contentY < g_items[i].top + g_items[i].height / 2) { idx = i; break; }
    for (size_t i = idx; i < g_items.size(); ++i)
        if (!g_items[i].header && !IsDraggedKey (g_items[i].key)) { beforeKey = g_items[i].key; break; }
    std::string prevKey;
    for (long i = (long) idx - 1; i >= 0; --i)
        if (!g_items[(size_t) i].header && !IsDraggedKey (g_items[(size_t) i].key)) { prevKey = g_items[(size_t) i].key; break; }
    const int prevBlock = BlockOf (prevKey);
    const int nextBlock = BlockOf (beforeKey);
    if (!beforeKey.empty() && prevBlock >= 0 && prevBlock == nextBlock)
        insideBlockName = g_edit.blocks[(size_t) prevBlock].name;
    else if (idx > 0 && idx <= g_items.size() && g_items[idx - 1].header && nextBlock >= 0 &&
             g_items[idx - 1].blockIdx == nextBlock)
        insideBlockName = g_edit.blocks[(size_t) nextBlock].name;
}

void ShowDragIndicator (int contentY)
{
    size_t idx = g_items.size();
    for (size_t i = 0; i < g_items.size(); ++i)
        if (contentY < g_items[i].top + g_items[i].height / 2) { idx = i; break; }
    const int top = idx < g_items.size() ? g_items[idx].top : ContentHeight();
    SetWindowPos (g_frame, nullptr, 2, top - g_scroll, 744, 3, SWP_NOZORDER | SWP_SHOWWINDOW);
}

void ApplyDragDrop()
{
    std::string beforeKey;
    std::wstring insideName;
    ComputeInsertion (g_dragPanelY + g_scroll, beforeKey, insideName);

    auto insertPos = [&] (std::vector<std::string>& ord) {
        if (beforeKey.empty()) return ord.end();
        return std::find (ord.begin(), ord.end(), beforeKey);
    };

    if (g_dragKind == 1)
    {
        const std::string key = g_dragKey;
        // Leave the old block (drop it if it becomes empty).
        const int oldBlock = BlockOf (key);
        if (oldBlock >= 0)
        {
            auto& f = g_edit.blocks[(size_t) oldBlock].filters;
            f.erase (std::remove (f.begin(), f.end(), key), f.end());
            if (f.empty()) g_edit.blocks.erase (g_edit.blocks.begin() + oldBlock);
        }
        auto& ord = g_edit.order;
        ord.erase (std::remove (ord.begin(), ord.end(), key), ord.end());
        ord.insert (insertPos (ord), key);
        // Join the block the drop landed inside of.
        if (!insideName.empty())
        {
            for (auto& blk : g_edit.blocks)
                if (blk.name == insideName)
                {
                    blk.filters.push_back (key);
                    std::sort (blk.filters.begin(), blk.filters.end(),
                               [&] (const std::string& a, const std::string& b) {
                                   return std::find (ord.begin(), ord.end(), a) <
                                          std::find (ord.begin(), ord.end(), b);
                               });
                    break;
                }
        }
        g_selKey = key;
    }
    else if (g_dragKind == 2 && g_dragBlock >= 0 && g_dragBlock < (int) g_edit.blocks.size())
    {
        const std::vector<std::string> chunk = g_edit.blocks[(size_t) g_dragBlock].filters;
        auto& ord = g_edit.order;
        for (const auto& k : chunk)
            ord.erase (std::remove (ord.begin(), ord.end(), k), ord.end());
        ord.insert (insertPos (ord), chunk.begin(), chunk.end());
    }
    Commit();
    RefreshChain();
}

int PanelYFromCursor (HWND hwnd)
{
    POINT pt {};
    GetCursorPos (&pt);
    ScreenToClient (g_panel, &pt);
    (void) hwnd;
    return pt.y;
}

LRESULT CALLBACK DragSubclassProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR idSubclass, DWORD_PTR refData)
{
    switch (msg)
    {
        case WM_LBUTTONDOWN:
            g_dragHwnd = hwnd;
            g_dragRef = refData;
            g_dragging = false;
            g_dragPanelY = PanelYFromCursor (hwnd);
            g_dragStartPanelY = g_dragPanelY;
            SetCapture (hwnd);
            return DefSubclassProc (hwnd, msg, wp, lp);
        case WM_MOUSEMOVE:
            if (g_dragHwnd == hwnd && (wp & MK_LBUTTON))
            {
                g_dragPanelY = PanelYFromCursor (hwnd);
                if (!g_dragging && std::abs (g_dragPanelY - g_dragStartPanelY) > 7)
                {
                    g_dragging = true;
                    if (refData < 1000)
                    {
                        g_dragKind = 1;
                        g_dragKey = kSlotKeys[refData];
                    }
                    else
                    {
                        g_dragKind = 2;
                        const int pool = (int) refData - 1000;
                        g_dragBlock = (pool >= 0 && pool < kMaxHeaderPool) ? g_headers[pool].blockIdx : -1;
                    }
                }
                if (g_dragging) ShowDragIndicator (g_dragPanelY + g_scroll);
            }
            return DefSubclassProc (hwnd, msg, wp, lp);
        case WM_LBUTTONUP:
            if (g_dragHwnd == hwnd)
            {
                const bool wasDragging = g_dragging;
                g_dragHwnd = nullptr;
                g_dragging = false;
                ReleaseCapture();
                if (wasDragging)
                {
                    g_dragPanelY = PanelYFromCursor (hwnd);
                    ApplyDragDrop();
                    g_dragKind = 0;
                    return 0; // swallow the click
                }
            }
            return DefSubclassProc (hwnd, msg, wp, lp);
        case WM_NCDESTROY:
            RemoveWindowSubclass (hwnd, DragSubclassProc, idSubclass);
            return DefSubclassProc (hwnd, msg, wp, lp);
    }
    return DefSubclassProc (hwnd, msg, wp, lp);
}

void CreateChainRows (HWND panel)
{
    auto mk = [&] (const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        HWND h = CreateWindowW (cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                                panel, reinterpret_cast<HMENU> ((INT_PTR) id), nullptr, nullptr);
        SendMessageW (h, WM_SETFONT, (WPARAM) g_font, TRUE);
        return h;
    };
    g_frame = mk (L"STATIC", L"", SS_BLACKFRAME, 0);
    for (int s = 0; s < kNumSlots; ++s)
    {
        RowControls& r = g_rows[s];
        const std::string key = kSlotKeys[s];
        r.toggle = mk (L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, RowId (s, 2));
        SetWindowSubclass (r.toggle, DragSubclassProc, 0, (DWORD_PTR) s);
        r.tag = mk (L"STATIC", L"", 0, 0);
        if (key == "gate")
            r.breath = mk (L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, RowId (s, 3));
        for (int i = 0; i < kNumKnobs && r.knobCount < 3; ++i)
        {
            if (kKnobs[i].filterKey != key) continue;
            const int j = r.knobCount++;
            r.knobs[j] = &kKnobs[i];
            r.knobLabel[j] = mk (L"STATIC", L"", 0, 0);
            r.knobSlider[j] = mk (TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_NOTICKS, RowId (s, 4 + j));
            SendMessageW (r.knobSlider[j], TBM_SETRANGE, TRUE, MAKELPARAM (0, 1000));
            SendMessageW (r.knobSlider[j], TBM_SETPAGESIZE, 0, 50);
        }
    }
    for (int i = 0; i < kMaxHeaderPool; ++i)
    {
        HeaderControls& h = g_headers[i];
        h.nameBtn = mk (L"BUTTON", L"", WS_TABSTOP, HB_BASE + i * 3 + 0);
        SetWindowSubclass (h.nameBtn, DragSubclassProc, 0, (DWORD_PTR) (1000 + i));
        h.collapseBtn = mk (L"BUTTON", L"", WS_TABSTOP, HB_BASE + i * 3 + 1);
        h.dissolveBtn = mk (L"BUTTON", L"解散", WS_TABSTOP, HB_BASE + i * 3 + 2);
        ShowWindow (h.nameBtn, SW_HIDE);
        ShowWindow (h.collapseBtn, SW_HIDE);
        ShowWindow (h.dissolveBtn, SW_HIDE);
    }
}

// --- Actions ------------------------------------------------------------------
void OnPresetNew()
{
    std::wstring name;
    if (!PromptText (g_win, L"新規プリセット", L"", name)) return;
    for (const auto& n : ListPresetNames())
        if (n == name)
        {
            MessageBoxW (g_win, L"同じ名前のプリセットがあります", L"MicFilter", MB_OK | MB_ICONWARNING);
            return;
        }
    Preset np = g_edit;
    np.name = name;
    SavePreset (np);
    RefreshPresetList (name);
    LoadIntoEditor (name);
}

void OnPresetRename()
{
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
    RefreshPresetList (name);
    LoadIntoEditor (name);
}

void OnPresetDelete()
{
    if (ListPresetNames().size() <= 1)
    {
        MessageBoxW (g_win, L"最後の1個は消せません", L"MicFilter", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW (g_win, L"このプリセットを削除しますか？", L"MicFilter",
                     MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    const bool wasMain = g_editIsMainCurrent;
    DeletePreset (g_edit.name);
    const std::vector<std::wstring> names = ListPresetNames();
    const std::wstring next = names.empty() ? std::wstring (L"標準") : names[0];
    if (names.empty()) SavePreset (DefaultPreset());
    if (wasMain)
    {
        g_mainCurrent = next;
        NotifyMainSelect (next);
    }
    RefreshPresetList (next);
    LoadIntoEditor (next);
}

void OnGroupAdd()
{
    std::wstring name;
    if (!PromptText (g_win, L"新規グループ", L"", name)) return;
    g_edit.groups.push_back ({ name, {} });
    Commit();
    RefreshGroupCombo ((int) g_edit.groups.size() - 1);
    RefreshChain();
}

void OnGroupRename()
{
    const int g = SelectedGroup();
    if (g < 0) return;
    std::wstring name;
    if (!PromptText (g_win, L"グループ名の変更", g_edit.groups[(size_t) g].name, name)) return;
    g_edit.groups[(size_t) g].name = name;
    Commit();
    RefreshGroupCombo (g);
    RefreshChain();
}

void OnGroupDelete()
{
    const int g = SelectedGroup();
    if (g < 0) return;
    if (MessageBoxW (g_win, L"グループを削除しますか？（中のフィルターは別のグループへ移ります）",
                     L"MicFilter", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    std::vector<std::string> moving = g_edit.groups[(size_t) g].filters;
    g_edit.groups.erase (g_edit.groups.begin() + g);
    if (g_edit.groups.empty())
        g_edit.groups.push_back ({ L"フィルター", {} });
    auto& dest = g_edit.groups[0].filters;
    dest.insert (dest.end(), moving.begin(), moving.end());
    Commit();
    RefreshGroupCombo (0);
    RefreshChain();
}

void OnGroupAll (bool on)
{
    const int g = SelectedGroup();
    if (g < 0) return;
    for (const auto& key : g_edit.groups[(size_t) g].filters)
        g_edit.on[key] = on;
    Commit();
    RefreshChain();
}

void OnMoveSelectedRow()
{
    const int g = SelectedGroup();
    if (g < 0) return;
    for (auto& grp : g_edit.groups)
        grp.filters.erase (std::remove (grp.filters.begin(), grp.filters.end(), g_selKey),
                           grp.filters.end());
    g_edit.groups[(size_t) g].filters.push_back (g_selKey);
    Commit();
    RefreshChain();
}

void OnBlockCreate()
{
    if (BlockOf (g_selKey) >= 0)
    {
        MessageBoxW (g_win, L"選択中のフィルターはすでにブロックの中です", L"MicFilter",
                     MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring name;
    if (!PromptText (g_win, L"新規ブロック（並び順用）", L"", name)) return;
    OrderBlock blk;
    blk.name = name;
    blk.filters = { g_selKey };
    g_edit.blocks.push_back (blk);
    Commit();
    RefreshChain();
}

void OnBlockRename (int pool)
{
    const int b = g_headers[pool].blockIdx;
    if (b < 0 || b >= (int) g_edit.blocks.size()) return;
    std::wstring name;
    if (!PromptText (g_win, L"ブロック名の変更", g_edit.blocks[(size_t) b].name, name)) return;
    g_edit.blocks[(size_t) b].name = name;
    Commit();
    LayoutRows();
}

void OnBlockCollapse (int pool)
{
    const int b = g_headers[pool].blockIdx;
    if (b < 0 || b >= (int) g_edit.blocks.size()) return;
    g_edit.blocks[(size_t) b].collapsed = !g_edit.blocks[(size_t) b].collapsed;
    Commit();
    LayoutRows();
}

void OnBlockDissolve (int pool)
{
    const int b = g_headers[pool].blockIdx;
    if (b < 0 || b >= (int) g_edit.blocks.size()) return;
    g_edit.blocks.erase (g_edit.blocks.begin() + b);
    Commit();
    RefreshChain();
}

void OnRowToggle (int slot)
{
    const std::string key = kSlotKeys[slot];
    const bool on = SendMessageW (g_rows[slot].toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.on[key] = on;
    g_selKey = key;
    const FilterInfo* info = FindFilter (key);
    SetToggleText (g_rows[slot].toggle, on, info ? info->label : L"?");
    LayoutRows();
    Commit();
}

void OnBreathToggle()
{
    const int s = SlotOf ("gate");
    const bool on = SendMessageW (g_rows[s].breath, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.on["breath"] = on;
    g_selKey = "gate";
    SetToggleText (g_rows[s].breath, on, L"吐息キープ");
    LayoutRows();
    Commit();
}

void OnKnob (HWND sliderHwnd, int slot, int j)
{
    RowControls& r = g_rows[slot];
    const KnobDef& def = *r.knobs[j];
    const auto& range = MicFilterParams::rangeFor (def.paramId);
    const int pos = (int) SendMessageW (sliderHwnd, TBM_GETPOS, 0, 0);
    double plain = range.min + (range.max - range.min) * (pos / 1000.0);
    plain = std::round ((plain - range.min) / def.step) * def.step + range.min;
    plain = std::clamp (plain, (double) range.min, (double) range.max);
    if (plain == KnobValue (def)) return;
    g_edit.values[def.valueKey] = plain;
    SetWindowTextW (r.knobLabel[j], KnobValueText (def, plain).c_str());
    g_selKey = kSlotKeys[slot];
    Commit();
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

            label (L"プリセット", 16, 10, 180);
            make (CreateWindowW (L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
                                 16, 32, 180, 478, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) S_PRESET_LIST), nullptr, nullptr));
            button (L"新規", S_PRESET_NEW, 16, 518, 54);
            button (L"名前変更", S_PRESET_RENAME, 74, 518, 68);
            button (L"削除", S_PRESET_DELETE, 146, 518, 50);

            label (L"グループ（選択中グループへの操作）", 212, 10, 400);
            make (CreateWindowW (L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 212, 32, 200, 200, hwnd,
                                 reinterpret_cast<HMENU> ((INT_PTR) S_GROUP_COMBO), nullptr, nullptr));
            button (L"追加", S_GROUP_ADD, 418, 31, 52, 25);
            button (L"名前変更", S_GROUP_RENAME, 474, 31, 68, 25);
            button (L"削除", S_GROUP_DELETE, 546, 31, 52, 25);
            button (L"全ON", S_GROUP_ALL_ON, 610, 31, 58, 25);
            button (L"全OFF", S_GROUP_ALL_OFF, 672, 31, 62, 25);
            button (L"選択行をこのグループへ", S_MOVE_BUTTON, 746, 31, 168, 25);

            label (L"並び順ブロック（フィルターのボタン／ブロック名をドラッグで移動）", 212, 62, 500);
            button (L"選択行からブロック作成", S_BLOCK_CREATE, 746, 60, 168, 25);

            static bool panelRegistered = false;
            if (!panelRegistered)
            {
                WNDCLASSW wc {};
                wc.lpfnWndProc = ChainPanelProc;
                wc.hInstance = GetModuleHandleW (nullptr);
                wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
                wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
                wc.lpszClassName = L"MicFilterChainPanel";
                RegisterClassW (&wc);
                panelRegistered = true;
            }
            g_panel = CreateWindowW (L"MicFilterChainPanel", L"",
                                     WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL,
                                     212, 92, 764, 518, hwnd, nullptr,
                                     GetModuleHandleW (nullptr), nullptr);
            CreateChainRows (g_panel);

            button (L"このプリセットを使う", S_APPLY, 664, 622, 150, 30);
            button (L"閉じる", S_CLOSE, 828, 622, 140, 30);

            EnsurePresetStore();
            RefreshPresetList (g_mainCurrent);
            const std::wstring sel = SelectedPresetName();
            LoadIntoEditor (!sel.empty() ? sel : g_mainCurrent);
            return 0;
        }
        case WM_COMMAND:
        {
            const int id = LOWORD (wp);
            if (id >= HB_BASE && id < HB_BASE + kMaxHeaderPool * 3)
            {
                const int pool = (id - HB_BASE) / 3;
                const int sub = (id - HB_BASE) % 3;
                if (sub == 0) { OnBlockRename (pool); return 0; }
                if (sub == 1) { OnBlockCollapse (pool); return 0; }
                if (sub == 2) { OnBlockDissolve (pool); return 0; }
                return 0;
            }
            if (id >= R_BASE && id < R_BASE + kNumSlots * 16)
            {
                const int slot = (id - R_BASE) / 16;
                const int sub = (id - R_BASE) % 16;
                if (sub == 2) { OnRowToggle (slot); return 0; }
                if (sub == 3) { OnBreathToggle(); return 0; }
                return 0;
            }
            switch (id)
            {
                case S_PRESET_LIST:
                    if (HIWORD (wp) == LBN_SELCHANGE)
                    {
                        const std::wstring name = SelectedPresetName();
                        if (!name.empty() && name != g_edit.name) LoadIntoEditor (name);
                    }
                    return 0;
                case S_PRESET_NEW: OnPresetNew(); return 0;
                case S_PRESET_RENAME: OnPresetRename(); return 0;
                case S_PRESET_DELETE: OnPresetDelete(); return 0;
                case S_GROUP_ADD: OnGroupAdd(); return 0;
                case S_GROUP_RENAME: OnGroupRename(); return 0;
                case S_GROUP_DELETE: OnGroupDelete(); return 0;
                case S_GROUP_ALL_ON: OnGroupAll (true); return 0;
                case S_GROUP_ALL_OFF: OnGroupAll (false); return 0;
                case S_MOVE_BUTTON: OnMoveSelectedRow(); return 0;
                case S_BLOCK_CREATE: OnBlockCreate(); return 0;
                case S_APPLY:
                    g_mainCurrent = g_edit.name;
                    g_editIsMainCurrent = true;
                    NotifyMainSelect (g_edit.name);
                    return 0;
                case S_CLOSE: DestroyWindow (hwnd); return 0;
            }
            break;
        }
        case WM_HSCROLL:
        {
            const int code = LOWORD (wp);
            if (code != TB_THUMBTRACK && code != TB_ENDTRACK && code != SB_LINELEFT &&
                code != SB_LINERIGHT && code != SB_PAGELEFT && code != SB_PAGERIGHT)
                return 0;
            HWND slider = reinterpret_cast<HWND> (lp);
            const int id = GetDlgCtrlID (slider);
            if (id >= R_BASE && id < R_BASE + kNumSlots * 16)
            {
                const int slot = (id - R_BASE) / 16;
                const int sub = (id - R_BASE) % 16;
                if (sub >= 4) OnKnob (slider, slot, sub - 4);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow (hwnd);
            return 0;
        case WM_DESTROY:
            g_win = nullptr;
            g_panel = nullptr;
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
    HINSTANCE hInst = GetModuleHandleW (nullptr);
    if (!registered)
    {
        WNDCLASSW wc {};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MicFilterSettingsWindow";
        RegisterClassW (&wc);
        registered = true;
    }
    INITCOMMONCONTROLSEX icc { sizeof (icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx (&icc);
    g_scroll = 0;
    HWND win = CreateWindowW (L"MicFilterSettingsWindow", L"フィルター設定",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
                              parent, nullptr, hInst, nullptr);
    if (win) ShowWindow (win, SW_SHOW);
}
