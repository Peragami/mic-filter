// settings_window.cpp -- see settings_window.h.
// v0.20: DAW-style board. Module cards sit in signal-flow order
// (left to right, wrapping); each card has a power LED, rotary knobs
// (drag / wheel / double-click to reset), and can be dragged to
// reorder. Consecutive cards can be bundled into order blocks, drawn
// as backplates. Everything autosaves.
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
// DAW-style board: module cards in signal-flow order (left to right,
// wrapping), rotary knobs, power LEDs, drag to reorder, blocks drawn
// as backplates. Everything is custom-drawn in one child window.
// =====================================================================
constexpr int kCardH = 140;
constexpr int kKnobR = 16;
constexpr int kChipW = 176;
constexpr int kChipH = 54;

struct KnobGeom
{
    const KnobDef* def = nullptr;
    POINT center { 0, 0 };
};
struct CardGeom
{
    std::string key;
    RECT rc {};
    int orderPos = 0;
    std::vector<KnobGeom> knobs;
    RECT breathRc {};
    bool hasBreath = false;
    RECT soloRc {};
};
struct ChipGeom
{
    int blockIdx = -1;
    RECT rc {};
    RECT unfoldRc {};
};
struct PlateGeom
{
    int blockIdx = -1;
    RECT rc {};       // backplate segment around member cards of one row
    RECT tabRc {};    // top band (drag / double-click to rename)
    RECT foldRc {};   // first segment only
    RECT dissolveRc {}; // first segment only
    bool first = false;
};
std::vector<CardGeom> g_cards;
std::vector<ChipGeom> g_chips;
std::vector<PlateGeom> g_plates;
int g_contentH = 0;

int BlockOf (const std::string& key)
{
    for (size_t b = 0; b < g_edit.blocks.size(); ++b)
        for (const auto& k : g_edit.blocks[b].filters)
            if (k == key) return (int) b;
    return -1;
}

int KnobCountOf (const std::string& key)
{
    int n = 0;
    for (int i = 0; i < kNumKnobs; ++i)
        if (kKnobs[i].filterKey == key) ++n;
    return n;
}

int CardWidthOf (const std::string& key)
{
    return std::max (110, 26 + KnobCountOf (key) * 48);
}

struct FlowUnit
{
    bool chip = false;
    int blockIdx = -1;
    std::string key;
    int width = 0;
};

void LayoutBoard()
{
    g_cards.clear();
    g_chips.clear();
    g_plates.clear();
    RECT client {};
    GetClientRect (g_board, &client);
    const int panelW = (int) (client.right - client.left);

    // Reading-order units: cards, with collapsed blocks as one chip.
    std::vector<FlowUnit> units;
    std::set<int> emitted;
    std::map<std::string, int> orderPos;
    for (size_t i = 0; i < g_edit.order.size(); ++i) orderPos[g_edit.order[i]] = (int) i;
    for (const auto& key : g_edit.order)
    {
        const int b = BlockOf (key);
        if (b >= 0)
        {
            if (!emitted.count (b))
            {
                emitted.insert (b);
                if (g_edit.blocks[(size_t) b].collapsed)
                {
                    FlowUnit u;
                    u.chip = true;
                    u.blockIdx = b;
                    u.width = kChipW;
                    units.push_back (u);
                }
            }
            if (g_edit.blocks[(size_t) b].collapsed) continue;
        }
        FlowUnit u;
        u.key = key;
        u.width = CardWidthOf (key);
        units.push_back (u);
    }

    // Wrap into rows.
    struct Row { std::vector<FlowUnit> units; bool hasPlate = false; };
    std::vector<Row> rows;
    {
        Row cur;
        int x = 14;
        for (const auto& u : units)
        {
            if (x + u.width > panelW - 14 && !cur.units.empty())
            {
                rows.push_back (cur);
                cur = Row();
                x = 14;
            }
            cur.units.push_back (u);
            x += u.width + 12;
        }
        if (!cur.units.empty()) rows.push_back (cur);
    }
    for (auto& row : rows)
        for (const auto& u : row.units)
            if (!u.chip && BlockOf (u.key) >= 0) row.hasPlate = true;

    // Assign coordinates; collect plate segments per (block, row).
    std::map<std::pair<int, int>, PlateGeom> plateSegs;
    int y = 10;
    for (size_t r = 0; r < rows.size(); ++r)
    {
        const int head = rows[r].hasPlate ? 26 : 8;
        const int cardTop = y + head;
        int x = 14;
        for (const auto& u : rows[r].units)
        {
            if (u.chip)
            {
                ChipGeom cg;
                cg.blockIdx = u.blockIdx;
                cg.rc = { x, cardTop, x + u.width, cardTop + kChipH };
                cg.unfoldRc = { cg.rc.right - 52, cg.rc.top + 15, cg.rc.right - 10, cg.rc.bottom - 15 };
                g_chips.push_back (cg);
            }
            else
            {
                CardGeom cg;
                cg.key = u.key;
                cg.orderPos = orderPos[u.key];
                cg.rc = { x, cardTop, x + u.width, cardTop + kCardH };
                cg.soloRc = { cg.rc.left + 44, cg.rc.bottom - 21, cg.rc.left + 84, cg.rc.bottom - 5 };
                int kn = 0;
                for (int i = 0; i < kNumKnobs; ++i)
                    if (kKnobs[i].filterKey == u.key) ++kn;
                int ki = 0;
                for (int i = 0; i < kNumKnobs; ++i)
                {
                    if (kKnobs[i].filterKey != u.key) continue;
                    KnobGeom kg;
                    kg.def = &kKnobs[i];
                    kg.center = { cg.rc.left + (u.width - (kn - 1) * 48) / 2 + ki * 48,
                                  cg.rc.top + 88 };
                    cg.knobs.push_back (kg);
                    ++ki;
                }
                if (u.key == "gate")
                {
                    cg.hasBreath = true;
                    cg.breathRc = { cg.rc.right - 86, cg.rc.top + 30, cg.rc.right - 8, cg.rc.top + 49 };
                }
                g_cards.push_back (cg);
                const int b = BlockOf (u.key);
                if (b >= 0)
                {
                    auto& pg = plateSegs[{ b, (int) r }];
                    if (pg.blockIdx < 0)
                    {
                        pg.blockIdx = b;
                        pg.rc = { cg.rc.left - 7, cg.rc.top - 22, cg.rc.right + 7, cg.rc.bottom + 7 };
                    }
                    else
                    {
                        pg.rc.left = std::min (pg.rc.left, cg.rc.left - 7);
                        pg.rc.right = std::max (pg.rc.right, cg.rc.right + 7);
                        pg.rc.top = std::min (pg.rc.top, cg.rc.top - 22);
                        pg.rc.bottom = std::max (pg.rc.bottom, cg.rc.bottom + 7);
                    }
                }
            }
            x += u.width + 12;
        }
        y = cardTop + kCardH + 16;
    }
    g_contentH = y;
    std::set<int> firstSeen;
    for (auto& kv : plateSegs)
    {
        PlateGeom pg = kv.second;
        pg.first = !firstSeen.count (pg.blockIdx);
        firstSeen.insert (pg.blockIdx);
        pg.tabRc = { pg.rc.left + 4, pg.rc.top + 1, pg.rc.right - 4, pg.rc.top + 21 };
        if (pg.first)
        {
            // Narrow plates shrink the buttons to glyphs so the name fits.
            const bool wide = (pg.rc.right - pg.rc.left) >= 240;
            const int dw = wide ? 46 : 18, fw = wide ? 44 : 16;
            pg.dissolveRc = { pg.rc.right - 6 - dw, pg.rc.top + 2, pg.rc.right - 6, pg.rc.top + 20 };
            pg.foldRc = { pg.dissolveRc.left - 4 - fw, pg.rc.top + 2, pg.dissolveRc.left - 4, pg.rc.top + 20 };
            pg.tabRc.right = pg.foldRc.left - 2;
        }
        g_plates.push_back (pg);
    }
}

// --- Drawing ---------------------------------------------------------------
Gdiplus::Color AccentFor (int pos, bool on)
{
    static const Gdiplus::Color kPalette[] = {
        { 255, 96, 142, 205 }, { 255, 118, 178, 118 }, { 255, 205, 150, 92 },
        { 255, 172, 122, 205 }, { 255, 92, 182, 192 }, { 255, 205, 112, 112 },
        { 255, 152, 162, 94 }, { 255, 122, 132, 205 },
    };
    if (!on) return Gdiplus::Color (255, 110, 114, 120);
    return kPalette[pos % 8];
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

// Drag state for the board.
bool g_bDragging = false;
int g_bDragKind = 0; // 1 = card, 2 = block
std::string g_bDragKey;
int g_bDragBlock = -1;
POINT g_bDragStart { 0, 0 };
POINT g_bDragNow { 0, 0 };
bool g_bHasDrop = false;
std::string g_bDropBefore;
std::wstring g_bDropInside;
RECT g_bCaret {};
const KnobDef* g_knobDragDef = nullptr;
int g_knobDragStartY = 0;
double g_knobDragStartVal = 0;
int g_pendingAction = 0; // 1 = fold, 2 = dissolve, 3 = unfold
int g_pendingBlock = -1;

std::wstring KnobValueOnly (const KnobDef& def, double v)
{
    wchar_t buf[64];
    switch (def.format)
    {
        case 0: swprintf_s (buf, L"%d%s", (int) std::lround (v), def.unit); break;
        case 1: swprintf_s (buf, L"%.1f%s", v, def.unit); break;
        case 3: swprintf_s (buf, L"%d%%", (int) std::lround (v * 100.0)); break;
        case 4: swprintf_s (buf, L"%.1f:1", v); break;
        default: swprintf_s (buf, L"%.2f%s", v, def.unit); break;
    }
    return buf;
}

void DrawBoard (HDC hdc, const RECT& client)
{
    const int W = (int) (client.right - client.left), H = (int) (client.bottom - client.top);
    HDC mem = CreateCompatibleDC (hdc);
    HBITMAP bmp = CreateCompatibleBitmap (hdc, W, H);
    HGDIOBJ oldBmp = SelectObject (mem, bmp);
    {
        Gdiplus::Graphics g (mem);
        g.SetSmoothingMode (Gdiplus::SmoothingModeAntiAlias);
        g.Clear (Gdiplus::Color (255, 34, 36, 40));
        g.TranslateTransform (0.0f, (float) -g_boardScroll);

        const Gdiplus::Color kText (255, 228, 230, 234);
        const Gdiplus::Color kDim (255, 150, 154, 160);

        // Backplates.
        for (const auto& pg : g_plates)
        {
            FillRoundRect (g, pg.rc, 8, Gdiplus::Color (255, 46, 49, 56));
            StrokeRoundRect (g, pg.rc, 8, Gdiplus::Color (255, 84, 88, 98));
            if (pg.first)
            {
                const OrderBlock& blk = g_edit.blocks[(size_t) pg.blockIdx];
                RECT nameRc { pg.rc.left + 10, pg.rc.top + 1, pg.foldRc.left - 4, pg.rc.top + 21 };
                DrawText (g, blk.name, nameRc, 9, kText, false, true);
                const bool wideBtns = (pg.foldRc.right - pg.foldRc.left) >= 40;
                DrawText (g, wideBtns ? L"閉じる" : L"－", pg.foldRc, 8, kDim, true);
                DrawText (g, wideBtns ? L"解散" : L"×", pg.dissolveRc, 8, kDim, true);
            }
        }
        // Collapsed chips.
        for (const auto& cg : g_chips)
        {
            const OrderBlock& blk = g_edit.blocks[(size_t) cg.blockIdx];
            FillRoundRect (g, cg.rc, 8, Gdiplus::Color (255, 52, 55, 62));
            StrokeRoundRect (g, cg.rc, 8, Gdiplus::Color (255, 96, 100, 110));
            RECT nameRc { cg.rc.left + 10, cg.rc.top, cg.unfoldRc.left - 4, cg.rc.bottom };
            DrawText (g, L"▶ " + blk.name + L"（" + std::to_wstring (blk.filters.size()) + L"）",
                      nameRc, 10, kText, false, true);
            DrawText (g, L"開く", cg.unfoldRc, 9, kDim, true);
        }
        // Cards.
        for (const auto& cg : g_cards)
        {
            const FilterInfo* info = FindFilter (cg.key);
            const bool on = g_edit.on.count (cg.key) && g_edit.on.at (cg.key);
            const Gdiplus::Color accent = AccentFor (cg.orderPos, on);
            FillRoundRect (g, cg.rc, 8, Gdiplus::Color (255, 48, 51, 58));
            if (cg.key == g_selKey)
                StrokeRoundRect (g, cg.rc, 8, Gdiplus::Color (255, 235, 238, 242), 2.0f);
            else
                StrokeRoundRect (g, cg.rc, 8, Gdiplus::Color (255, 76, 79, 88));
            RECT strip { cg.rc.left + 8, cg.rc.top + 4, cg.rc.right - 8, cg.rc.top + 7 };
            FillRoundRect (g, strip, 1.5f, accent);
            RECT titleRc { cg.rc.left + 9, cg.rc.top + 9, cg.rc.right - 24, cg.rc.top + 29 };
            DrawText (g, info ? info->label : L"?", titleRc, 9.0f,
                      on ? kText : kDim, false, true);
            // Power LED.
            const POINT pc { cg.rc.right - 15, cg.rc.top + 18 };
            Gdiplus::SolidBrush led (on ? Gdiplus::Color (255, 92, 224, 130)
                                        : Gdiplus::Color (255, 74, 77, 84));
            g.FillEllipse (&led, pc.x - 6, pc.y - 6, 12, 12);
            // Order badge.
            RECT badgeRc { cg.rc.left + 9, cg.rc.bottom - 18, cg.rc.left + 40, cg.rc.bottom - 2 };
            DrawText (g, L"#" + std::to_wstring (cg.orderPos + 1), badgeRc, 8, kDim);
            // Solo pill: audition this filter alone (preset untouched).
            {
                const bool soloOn = !g_soloKey.empty() && g_soloKey == cg.key;
                FillRoundRect (g, cg.soloRc, 6, soloOn ? Gdiplus::Color (255, 148, 106, 24)
                                                       : Gdiplus::Color (255, 58, 61, 68));
                DrawText (g, soloOn ? L"ソロ中" : L"ソロ", cg.soloRc, 8,
                          soloOn ? kText : kDim, true);
            }
            // Breath pill (gate).
            if (cg.hasBreath)
            {
                const bool bOn = g_edit.on.count ("breath") && g_edit.on.at ("breath");
                FillRoundRect (g, cg.breathRc, 8, bOn ? Gdiplus::Color (255, 66, 92, 74)
                                                      : Gdiplus::Color (255, 58, 61, 68));
                DrawText (g, L"吐息キープ", cg.breathRc, 8, bOn ? kText : kDim, true);
            }
            // Knobs.
            for (const auto& kg : cg.knobs)
            {
                const double v = KnobValue (*kg.def);
                const auto& range = MicFilterParams::rangeFor (kg.def->paramId);
                const double t = (v - range.min) / (range.max - range.min);
                const float ang = (float) (-225.0 + t * 270.0); // degrees, 0 = east
                const float rad = ang * 3.14159265f / 180.0f;
                RECT labelRc { kg.center.x - 36, kg.center.y - 37, kg.center.x + 36, kg.center.y - 21 };
                DrawText (g, kg.def->label, labelRc, 8, on ? kText : kDim, true);
                Gdiplus::SolidBrush body (Gdiplus::Color (255, 26, 27, 31));
                g.FillEllipse (&body, kg.center.x - kKnobR, kg.center.y - kKnobR,
                               kKnobR * 2, kKnobR * 2);
                Gdiplus::Pen rim (accent, 2.0f);
                g.DrawEllipse (&rim, kg.center.x - kKnobR, kg.center.y - kKnobR,
                               kKnobR * 2, kKnobR * 2);
                Gdiplus::Pen ptr (on ? kText : kDim, 2.5f);
                g.DrawLine (&ptr, (float) kg.center.x, (float) kg.center.y,
                            kg.center.x + (float) (std::cos (rad) * (kKnobR - 3)),
                            kg.center.y + (float) (std::sin (rad) * (kKnobR - 3)));
                RECT valRc { kg.center.x - 36, kg.center.y + 19, kg.center.x + 36, kg.center.y + 35 };
                DrawText (g, KnobValueOnly (*kg.def, v), valRc, 8, on ? kText : kDim, true);
            }
            if (cg.knobs.empty())
            {
                // Neutral level-bars motif for knob-less modules.
                const int bx = (cg.rc.left + cg.rc.right) / 2, by = cg.rc.top + 88;
                const int hs[5] = { 10, 18, 26, 14, 21 };
                for (int i = 0; i < 5; ++i)
                {
                    RECT bar { bx - 22 + i * 10, by - hs[i] / 2, bx - 16 + i * 10, by + hs[i] / 2 };
                    FillRoundRect (g, bar, 2, on ? accent : Gdiplus::Color (255, 84, 87, 94));
                }
            }
        }
        // Flow arrows between neighbours in a row.
        for (size_t i = 0; i + 1 < g_cards.size(); ++i)
        {
            const RECT& a = g_cards[i].rc;
            const RECT& b = g_cards[i + 1].rc;
            if (a.top == b.top && b.left > a.right)
            {
                const int cx = (a.right + b.left) / 2, cy = a.top + kCardH / 2;
                Gdiplus::SolidBrush br (Gdiplus::Color (255, 120, 124, 132));
                Gdiplus::Point pts[3] = { { cx - 3, cy - 5 }, { cx - 3, cy + 5 }, { cx + 4, cy } };
                g.FillPolygon (&br, pts, 3);
            }
        }
        // Drop caret.
        if (g_bDragging && g_bHasDrop)
        {
            Gdiplus::SolidBrush br (Gdiplus::Color (255, 250, 200, 90));
            g.FillRectangle (&br, (float) g_bCaret.left, (float) g_bCaret.top,
                             3.0f, (float) (g_bCaret.bottom - g_bCaret.top));
        }
        if (g_editLocked)
        {
            Gdiplus::SolidBrush wash (Gdiplus::Color (46, 190, 196, 205));
            g.FillRectangle (&wash, 0, g_boardScroll, W, H);
        }
    }
    BitBlt (hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject (mem, oldBmp);
    DeleteObject (bmp);
    DeleteDC (mem);
}

void RefreshBoard()
{
    if (g_board) InvalidateRect (g_board, nullptr, FALSE);
}

// --- Hit testing & dropping ----------------------------------------------------
struct Hit
{
    int kind = 0; // 1 knob, 2 power, 3 breath, 4 card, 5 plate tab, 6 fold, 7 dissolve, 8 chip, 9 unfold, 10 solo
    const CardGeom* card = nullptr;
    const KnobDef* knob = nullptr;
    int blockIdx = -1;
};

bool InRect (const RECT& rc, POINT pt) { return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom; }

Hit HitTest (POINT pt)
{
    Hit h;
    for (const auto& pg : g_plates)
        if (pg.first)
        {
            if (InRect (pg.foldRc, pt)) { h.kind = 6; h.blockIdx = pg.blockIdx; return h; }
            if (InRect (pg.dissolveRc, pt)) { h.kind = 7; h.blockIdx = pg.blockIdx; return h; }
        }
    for (const auto& cg : g_chips)
    {
        if (InRect (cg.unfoldRc, pt)) { h.kind = 9; h.blockIdx = cg.blockIdx; return h; }
        if (InRect (cg.rc, pt)) { h.kind = 8; h.blockIdx = cg.blockIdx; return h; }
    }
    for (const auto& cg : g_cards)
    {
        for (const auto& kg : cg.knobs)
        {
            const int dx = pt.x - kg.center.x, dy = pt.y - kg.center.y;
            if (dx * dx + dy * dy <= (kKnobR + 4) * (kKnobR + 4))
            {
                h.kind = 1; h.card = &cg; h.knob = kg.def; return h;
            }
        }
        const POINT pc { cg.rc.right - 15, cg.rc.top + 18 };
        const int dx = pt.x - pc.x, dy = pt.y - pc.y;
        if (dx * dx + dy * dy <= 11 * 11) { h.kind = 2; h.card = &cg; return h; }
        if (cg.hasBreath && InRect (cg.breathRc, pt)) { h.kind = 3; h.card = &cg; return h; }
        if (InRect (cg.soloRc, pt)) { h.kind = 10; h.card = &cg; return h; }
    }
    for (const auto& pg : g_plates)
        if (InRect (pg.tabRc, pt)) { h.kind = 5; h.blockIdx = pg.blockIdx; return h; }
    for (const auto& cg : g_cards)
        if (InRect (cg.rc, pt)) { h.kind = 4; h.card = &cg; return h; }
    return h;
}

bool IsDraggedKey (const std::string& key)
{
    if (g_bDragKind == 1) return key == g_bDragKey;
    if (g_bDragKind == 2 && g_bDragBlock >= 0 && g_bDragBlock < (int) g_edit.blocks.size())
        for (const auto& k : g_edit.blocks[(size_t) g_bDragBlock].filters)
            if (k == key) return true;
    return false;
}

struct Anchor
{
    std::string key;   // card key, or first member key for a chip
    int blockIdx = -1; // block the anchor belongs to (chip: its block)
    POINT center { 0, 0 };
    RECT rc {};
};

std::vector<Anchor> BuildAnchors()
{
    std::vector<Anchor> out;
    std::set<int> emitted;
    for (const auto& key : g_edit.order)
    {
        if (IsDraggedKey (key)) continue;
        const int b = BlockOf (key);
        if (b >= 0 && g_edit.blocks[(size_t) b].collapsed)
        {
            if (emitted.count (b)) continue;
            emitted.insert (b);
            for (const auto& cg : g_chips)
                if (cg.blockIdx == b)
                {
                    Anchor a;
                    a.key = key;
                    a.blockIdx = b;
                    a.rc = cg.rc;
                    a.center = { (cg.rc.left + cg.rc.right) / 2, (cg.rc.top + cg.rc.bottom) / 2 };
                    out.push_back (a);
                }
            continue;
        }
        for (const auto& cg : g_cards)
            if (cg.key == key)
            {
                Anchor a;
                a.key = key;
                a.blockIdx = b;
                a.rc = cg.rc;
                a.center = { (cg.rc.left + cg.rc.right) / 2, (cg.rc.top + cg.rc.bottom) / 2 };
                out.push_back (a);
            }
    }
    return out;
}

void ComputeDrop (POINT pt)
{
    const std::vector<Anchor> anchors = BuildAnchors();
    g_bDropBefore.clear();
    g_bDropInside.clear();
    g_bHasDrop = true;
    size_t ins = anchors.size();
    for (size_t i = 0; i < anchors.size(); ++i)
    {
        const Anchor& a = anchors[i];
        if (pt.y < a.rc.top || (pt.y < a.rc.bottom && pt.x < a.center.x)) { ins = i; break; }
    }
    if (ins < anchors.size()) g_bDropBefore = anchors[ins].key;
    // Inside a block: dropped on a chip, or between two members of one block.
    for (const auto& cg : g_chips)
        if (InRect (cg.rc, pt) && cg.blockIdx != g_bDragBlock)
        {
            g_bDropInside = g_edit.blocks[(size_t) cg.blockIdx].name;
            return;
        }
    if (ins > 0 && ins < anchors.size())
    {
        const int pb = anchors[ins - 1].blockIdx, nb = anchors[ins].blockIdx;
        if (pb >= 0 && pb == nb && pb != g_bDragBlock)
            g_bDropInside = g_edit.blocks[(size_t) pb].name;
    }
    // Caret geometry.
    if (ins < anchors.size())
    {
        const RECT& r = anchors[ins].rc;
        g_bCaret = { r.left - 8, r.top, r.left - 5, r.bottom };
    }
    else if (!anchors.empty())
    {
        const RECT& r = anchors.back().rc;
        g_bCaret = { r.right + 5, r.top, r.right + 8, r.bottom };
    }
    else g_bHasDrop = false;
}

void ApplyDrop()
{
    auto& ord = g_edit.order;
    auto insertPos = [&] {
        if (g_bDropBefore.empty()) return ord.end();
        return std::find (ord.begin(), ord.end(), g_bDropBefore);
    };
    if (g_bDragKind == 1)
    {
        const std::string key = g_bDragKey;
        const int oldBlock = BlockOf (key);
        if (oldBlock >= 0)
        {
            auto& f = g_edit.blocks[(size_t) oldBlock].filters;
            f.erase (std::remove (f.begin(), f.end(), key), f.end());
            if (f.empty()) g_edit.blocks.erase (g_edit.blocks.begin() + oldBlock);
        }
        ord.erase (std::remove (ord.begin(), ord.end(), key), ord.end());
        ord.insert (insertPos(), key);
        if (!g_bDropInside.empty())
            for (auto& blk : g_edit.blocks)
                if (blk.name == g_bDropInside)
                {
                    blk.filters.push_back (key);
                    std::sort (blk.filters.begin(), blk.filters.end(),
                               [&] (const std::string& a, const std::string& b) {
                                   return std::find (ord.begin(), ord.end(), a) <
                                          std::find (ord.begin(), ord.end(), b);
                               });
                    break;
                }
        g_selKey = key;
    }
    else if (g_bDragKind == 2 && g_bDragBlock >= 0 && g_bDragBlock < (int) g_edit.blocks.size())
    {
        const std::vector<std::string> chunk = g_edit.blocks[(size_t) g_bDragBlock].filters;
        for (const auto& k : chunk)
            ord.erase (std::remove (ord.begin(), ord.end(), k), ord.end());
        ord.insert (insertPos(), chunk.begin(), chunk.end());
    }
    Commit();
    RefreshBoard();
}

void ClampBoardScroll()
{
    RECT client {};
    GetClientRect (g_board, &client);
    const int maxScroll = std::max (0, g_contentH - (int) (client.bottom - client.top));
    g_boardScroll = std::clamp (g_boardScroll, 0, maxScroll);
}

LRESULT CALLBACK BoardProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps {};
            HDC hdc = BeginPaint (hwnd, &ps);
            LayoutBoard();
            ClampBoardScroll();
            RECT client {};
            GetClientRect (hwnd, &client);
            DrawBoard (hdc, client);
            EndPaint (hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        {
            SetFocus (hwnd);
            LayoutBoard();
            const POINT clientPt { (short) LOWORD (lp), (short) HIWORD (lp) };
            const POINT pt { clientPt.x, clientPt.y + g_boardScroll };
            const Hit h = HitTest (pt);
            if (h.kind == 10 && h.card)
            {
                // Solo audition: allowed even for locked presets (it
                // changes no settings), but only while this preset is
                // the one playing on the main window.
                if (g_editIsMainCurrent)
                {
                    g_soloKey = (g_soloKey == h.card->key) ? "" : h.card->key;
                    SendSolo();
                    g_selKey = h.card->key;
                    RefreshBoard();
                }
                return 0;
            }
            if (g_editLocked)
            {
                if (h.kind == 4 && h.card) { g_selKey = h.card->key; RefreshBoard(); }
                return 0;
            }
            switch (h.kind)
            {
                case 1:
                    g_knobDragDef = h.knob;
                    g_knobDragStartY = clientPt.y;
                    g_knobDragStartVal = KnobValue (*h.knob);
                    if (h.card) g_selKey = h.card->key;
                    SetCapture (hwnd);
                    break;
                case 2:
                    g_edit.on[h.card->key] = !(g_edit.on.count (h.card->key) && g_edit.on.at (h.card->key));
                    g_selKey = h.card->key;
                    Commit();
                    RefreshBoard();
                    break;
                case 3:
                    g_edit.on["breath"] = !(g_edit.on.count ("breath") && g_edit.on.at ("breath"));
                    Commit();
                    RefreshBoard();
                    break;
                case 6: case 7: case 9:
                    g_pendingAction = h.kind == 6 ? 1 : h.kind == 7 ? 2 : 3;
                    g_pendingBlock = h.blockIdx;
                    SetCapture (hwnd);
                    break;
                case 5: case 8:
                    g_bDragKind = 2;
                    g_bDragBlock = h.blockIdx;
                    g_bDragging = false;
                    g_bDragStart = g_bDragNow = pt;
                    SetCapture (hwnd);
                    break;
                case 4:
                    g_selKey = h.card->key;
                    g_bDragKind = 1;
                    g_bDragKey = h.card->key;
                    g_bDragging = false;
                    g_bDragStart = g_bDragNow = pt;
                    SetCapture (hwnd);
                    RefreshBoard();
                    break;
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            const POINT clientPt { (short) LOWORD (lp), (short) HIWORD (lp) };
            if (g_knobDragDef)
            {
                const auto& range = MicFilterParams::rangeFor (g_knobDragDef->paramId);
                const double span = (wp & MK_SHIFT) ? 1400.0 : 140.0;
                double v = g_knobDragStartVal +
                           (g_knobDragStartY - clientPt.y) * (range.max - range.min) / span;
                v = std::round ((v - range.min) / g_knobDragDef->step) * g_knobDragDef->step + range.min;
                v = std::clamp (v, (double) range.min, (double) range.max);
                if (v != KnobValue (*g_knobDragDef))
                {
                    g_edit.values[g_knobDragDef->valueKey] = v;
                    Commit();
                    RefreshBoard();
                }
                return 0;
            }
            if ((g_bDragKind == 1 || g_bDragKind == 2) && (wp & MK_LBUTTON))
            {
                const POINT pt { clientPt.x, clientPt.y + g_boardScroll };
                g_bDragNow = pt;
                if (!g_bDragging &&
                    (std::abs (pt.x - g_bDragStart.x) > 7 || std::abs (pt.y - g_bDragStart.y) > 7))
                    g_bDragging = true;
                if (g_bDragging)
                {
                    LayoutBoard();
                    ComputeDrop (pt);
                    RefreshBoard();
                }
            }
            return 0;
        }
        case WM_LBUTTONUP:
        {
            const bool hadCapture = GetCapture() == hwnd;
            if (g_knobDragDef) { g_knobDragDef = nullptr; ReleaseCapture(); return 0; }
            if (g_pendingAction)
            {
                LayoutBoard();
                const POINT pt { (short) LOWORD (lp), (short) HIWORD (lp) + g_boardScroll };
                const Hit h = HitTest (pt);
                const int want = g_pendingAction == 1 ? 6 : g_pendingAction == 2 ? 7 : 9;
                if (h.kind == want && h.blockIdx == g_pendingBlock &&
                    g_pendingBlock >= 0 && g_pendingBlock < (int) g_edit.blocks.size())
                {
                    if (g_pendingAction == 1) g_edit.blocks[(size_t) g_pendingBlock].collapsed = true;
                    else if (g_pendingAction == 2) g_edit.blocks.erase (g_edit.blocks.begin() + g_pendingBlock);
                    else g_edit.blocks[(size_t) g_pendingBlock].collapsed = false;
                    Commit();
                    RefreshBoard();
                }
                g_pendingAction = 0;
                g_pendingBlock = -1;
                if (hadCapture) ReleaseCapture();
                return 0;
            }
            if (g_bDragKind)
            {
                if (g_bDragging) ApplyDrop();
                g_bDragKind = 0;
                g_bDragging = false;
                g_bHasDrop = false;
                if (hadCapture) ReleaseCapture();
                RefreshBoard();
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:
        {
            LayoutBoard();
            const POINT pt { (short) LOWORD (lp), (short) HIWORD (lp) + g_boardScroll };
            const Hit h = HitTest (pt);
            g_knobDragDef = nullptr;
            g_bDragKind = 0;
            g_bDragging = false;
            if (g_editLocked) return 0;
            if (h.kind == 1 && h.knob)
            {
                const auto& range = MicFilterParams::rangeFor (h.knob->paramId);
                g_edit.values[h.knob->valueKey] = range.def;
                if (h.card) g_selKey = h.card->key;
                Commit();
                RefreshBoard();
            }
            else if (h.kind == 5 && h.blockIdx >= 0 && h.blockIdx < (int) g_edit.blocks.size())
            {
                std::wstring name;
                if (PromptText (g_win, L"ブロック名の変更", g_edit.blocks[(size_t) h.blockIdx].name, name))
                {
                    g_edit.blocks[(size_t) h.blockIdx].name = name;
                    Commit();
                    RefreshBoard();
                }
            }
            else if (h.kind == 8 && h.blockIdx >= 0 && h.blockIdx < (int) g_edit.blocks.size())
            {
                g_edit.blocks[(size_t) h.blockIdx].collapsed = false;
                Commit();
                RefreshBoard();
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            POINT sp { (short) LOWORD (lp), (short) HIWORD (lp) };
            ScreenToClient (hwnd, &sp);
            LayoutBoard();
            const POINT pt { sp.x, sp.y + g_boardScroll };
            const Hit h = HitTest (pt);
            const int notches = (int) GET_WHEEL_DELTA_WPARAM (wp) / 120;
            if (!g_editLocked && h.kind == 1 && h.knob && notches != 0)
            {
                const auto& range = MicFilterParams::rangeFor (h.knob->paramId);
                double v = KnobValue (*h.knob) + notches * h.knob->step * 4.0;
                v = std::round ((v - range.min) / h.knob->step) * h.knob->step + range.min;
                v = std::clamp (v, (double) range.min, (double) range.max);
                if (v != KnobValue (*h.knob))
                {
                    g_edit.values[h.knob->valueKey] = v;
                    Commit();
                    RefreshBoard();
                }
                return 0;
            }
            g_boardScroll -= notches * 46;
            ClampBoardScroll();
            RefreshBoard();
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_knobDragDef = nullptr;
            g_pendingAction = 0;
            g_bDragKind = 0;
            g_bDragging = false;
            g_bHasDrop = false;
            RefreshBoard();
            return 0;
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
    EnableWindow (SCtl (S_BLOCK_CREATE), en);
    EnableWindow (SCtl (S_NOISE_ON), en);
    EnableWindow (SCtl (S_NOISE_OFF), en);
    SetWindowTextW (SCtl (S_LOCK_LABEL), g_editLocked ? L"変更不可" : L"");
}

void LoadIntoEditor (const std::wstring& name)
{
    ClearSolo(); // solo belongs to the previously shown preset
    Preset p;
    if (!LoadPreset (name, p)) p = DefaultPreset();
    g_edit = p;
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

void OnBlockCreate()
{
    if (g_editLocked) return;
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
    RefreshBoard();
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

            button (L"選択カードからブロック作成", S_BLOCK_CREATE, 16, 42, 180, 25);
            label (L"「ソロ」＝そのフィルターだけ聞く", 208, 45, 380);
            button (L"ノイズ除去系 全部ON", S_NOISE_ON, 600, 42, 150, 25);
            button (L"全部OFF", S_NOISE_OFF, 758, 42, 100, 25);

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
                case S_BLOCK_CREATE: OnBlockCreate(); return 0;
                case S_NOISE_ON: case S_NOISE_OFF:
                {
                    if (g_editLocked) return 0;
                    const bool on = (id == S_NOISE_ON);
                    for (int i = 0; i < kNumNoiseGroupKeys; ++i)
                        g_edit.on[kNoiseGroupKeys[i]] = on;
                    Commit();
                    RefreshBoard();
                    return 0;
                }
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
