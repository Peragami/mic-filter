// settings_window.cpp -- see settings_window.h.
#include "settings_window.h"

#include <algorithm>
#include <string>
#include <vector>

#include <commctrl.h>

#include "log.h"
#include "preset.h"
#include "ui_shared.h"

namespace
{
// Control ids (settings window only).
constexpr int S_PRESET_LIST = 2001;
constexpr int S_PRESET_NEW = 2002;
constexpr int S_PRESET_RENAME = 2003;
constexpr int S_PRESET_DELETE = 2004;
constexpr int S_GROUP_LIST = 2005;
constexpr int S_GROUP_ADD = 2006;
constexpr int S_GROUP_RENAME = 2007;
constexpr int S_GROUP_DELETE = 2008;
constexpr int S_FILTER_LIST = 2009;
constexpr int S_ALL_ON = 2010;
constexpr int S_ALL_OFF = 2011;
constexpr int S_MOVE_COMBO = 2012;
constexpr int S_MOVE_BUTTON = 2013;
constexpr int S_ORDER_LIST = 2014;
constexpr int S_ORDER_UP = 2015;
constexpr int S_ORDER_DOWN = 2016;
constexpr int S_APPLY = 2017;
constexpr int S_CLOSE = 2018;

HWND g_win = nullptr;
HWND g_parent = nullptr;
std::wstring g_mainCurrent; // main window's active preset name
Preset g_edit;              // the preset being edited
bool g_editIsMainCurrent = false;
int g_selGroup = 0;
bool g_refreshing = false;

HWND SCtl (int id) { return GetDlgItem (g_win, id); }

int RegistryIndex (const std::string& key)
{
    for (int i = 0; i < kNumFilterInfos; ++i)
        if (key == kFilterRegistry[i].key) return i;
    return -1;
}

// Save g_edit; if it is the main window's active preset, tell main to
// reload + re-apply it (lParam null = rescan, keep your selection).
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

// --- Editor refresh ---------------------------------------------------------
void RefreshFilters()
{
    g_refreshing = true;
    HWND lv = SCtl (S_FILTER_LIST);
    ListView_DeleteAllItems (lv);
    if (g_selGroup >= 0 && g_selGroup < (int) g_edit.groups.size())
    {
        const PresetGroup& grp = g_edit.groups[(size_t) g_selGroup];
        for (size_t j = 0; j < grp.filters.size(); ++j)
        {
            const int ri = RegistryIndex (grp.filters[j]);
            if (ri < 0) continue;
            LVITEMW item {};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = (int) j;
            item.pszText = const_cast<wchar_t*> (kFilterRegistry[ri].label);
            item.lParam = ri;
            ListView_InsertItem (lv, &item);
            const auto it = g_edit.on.find (grp.filters[j]);
            ListView_SetCheckState (lv, (int) j, it != g_edit.on.end() && it->second);
        }
    }
    g_refreshing = false;
}

void RefreshGroups()
{
    HWND lb = SCtl (S_GROUP_LIST);
    SendMessageW (lb, LB_RESETCONTENT, 0, 0);
    for (const auto& g : g_edit.groups)
        SendMessageW (lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM> (g.name.c_str()));
    if (g_selGroup >= (int) g_edit.groups.size()) g_selGroup = (int) g_edit.groups.size() - 1;
    if (g_selGroup < 0) g_selGroup = 0;
    SendMessageW (lb, LB_SETCURSEL, g_selGroup, 0);
    HWND combo = SCtl (S_MOVE_COMBO);
    SendMessageW (combo, CB_RESETCONTENT, 0, 0);
    for (const auto& g : g_edit.groups)
        SendMessageW (combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM> (g.name.c_str()));
    if (!g_edit.groups.empty()) SendMessageW (combo, CB_SETCURSEL, 0, 0);
    RefreshFilters();
}

void RefreshOrder()
{
    // Only stages that are ON in this preset are listed; the stored
    // order keeps every stage, so turning one back on restores its
    // place. Each item carries its index into g_edit.order.
    HWND lb = SCtl (S_ORDER_LIST);
    SendMessageW (lb, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_edit.order.size(); ++i)
    {
        const std::string& key = g_edit.order[i];
        const auto it = g_edit.on.find (key);
        if (it == g_edit.on.end() || !it->second) continue;
        std::wstring text = StageLabel (key);
        if (key == "gate") text += L"（吐息キープもここ）";
        const LRESULT idx = SendMessageW (lb, LB_ADDSTRING, 0,
                                          reinterpret_cast<LPARAM> (text.c_str()));
        SendMessageW (lb, LB_SETITEMDATA, idx, (LPARAM) i);
    }
}

void LoadIntoEditor (const std::wstring& name)
{
    Preset p;
    if (!LoadPreset (name, p)) p = DefaultPreset();
    g_edit = p;
    g_editIsMainCurrent = g_edit.name == g_mainCurrent;
    g_selGroup = 0;
    RefreshGroups();
    RefreshOrder();
    wchar_t title[320];
    swprintf_s (title, L"フィルター設定 - %s", g_edit.name.c_str());
    SetWindowTextW (g_win, title);
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
} // namespace

namespace
{
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
    Preset np = g_edit; // copy current settings as the starting point
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
    g_selGroup = (int) g_edit.groups.size() - 1;
    Commit();
    RefreshGroups();
}

void OnGroupRename()
{
    if (g_edit.groups.empty()) return;
    std::wstring name;
    if (!PromptText (g_win, L"グループ名の変更", g_edit.groups[(size_t) g_selGroup].name, name)) return;
    g_edit.groups[(size_t) g_selGroup].name = name;
    Commit();
    RefreshGroups();
}

void OnGroupDelete()
{
    if (g_edit.groups.empty()) return;
    if (MessageBoxW (g_win, L"グループを削除しますか？（中のフィルターは別のグループへ移ります）",
                     L"MicFilter", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    std::vector<std::string> moving = g_edit.groups[(size_t) g_selGroup].filters;
    g_edit.groups.erase (g_edit.groups.begin() + g_selGroup);
    if (g_edit.groups.empty())
        g_edit.groups.push_back ({ L"フィルター", {} });
    auto& dest = g_edit.groups[0].filters;
    dest.insert (dest.end(), moving.begin(), moving.end());
    g_selGroup = 0;
    Commit();
    RefreshGroups();
}

void OnMoveFilter()
{
    HWND lv = SCtl (S_FILTER_LIST);
    const int sel = ListView_GetNextItem (lv, -1, LVNI_SELECTED);
    if (sel < 0 || g_selGroup >= (int) g_edit.groups.size()) return;
    LVITEMW item {};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    ListView_GetItem (lv, &item);
    const std::string key = kFilterRegistry[item.lParam].key;
    const LRESULT target = SendMessageW (SCtl (S_MOVE_COMBO), CB_GETCURSEL, 0, 0);
    if (target == CB_ERR || (int) target == g_selGroup) return;
    auto& from = g_edit.groups[(size_t) g_selGroup].filters;
    from.erase (std::remove (from.begin(), from.end(), key), from.end());
    g_edit.groups[(size_t) target].filters.push_back (key);
    Commit();
    RefreshGroups();
}

void OnOrderMove (int delta)
{
    HWND lb = SCtl (S_ORDER_LIST);
    const LRESULT sel = SendMessageW (lb, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    const LRESULT next = sel + delta;
    const LRESULT count = SendMessageW (lb, LB_GETCOUNT, 0, 0);
    if (next < 0 || next >= count) return;
    // Swap the two visible neighbours in the stored order (hidden OFF
    // stages between them keep their places).
    const size_t a = (size_t) SendMessageW (lb, LB_GETITEMDATA, sel, 0);
    const size_t b = (size_t) SendMessageW (lb, LB_GETITEMDATA, next, 0);
    std::swap (g_edit.order[a], g_edit.order[b]);
    Commit();
    RefreshOrder();
    SendMessageW (lb, LB_SETCURSEL, next, 0);
}

void OnSetGroupAll (bool on)
{
    if (g_selGroup >= (int) g_edit.groups.size()) return;
    for (const auto& key : g_edit.groups[(size_t) g_selGroup].filters)
        g_edit.on[key] = on;
    Commit();
    RefreshFilters();
    RefreshOrder();
}

LRESULT CALLBACK SettingsProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            g_win = hwnd;
            HFONT font = (HFONT) GetStockObject (DEFAULT_GUI_FONT);
            auto make = [&] (HWND h) { SendMessageW (h, WM_SETFONT, (WPARAM) font, TRUE); return h; };
            auto label = [&] (const wchar_t* text, int x, int y, int w) {
                make (CreateWindowW (L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 20,
                                     hwnd, nullptr, nullptr, nullptr));
            };
            auto button = [&] (const wchar_t* text, int id, int x, int y, int w, int h = 26) {
                return make (CreateWindowW (L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                            x, y, w, h, hwnd,
                                            reinterpret_cast<HMENU> ((INT_PTR) id), nullptr, nullptr));
            };
            auto listbox = [&] (int id, int x, int y, int w, int h) {
                return make (CreateWindowW (L"LISTBOX", L"",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                                                LBS_NOTIFY | WS_VSCROLL,
                                            x, y, w, h, hwnd,
                                            reinterpret_cast<HMENU> ((INT_PTR) id), nullptr, nullptr));
            };

            label (L"プリセット", 16, 10, 180);
            listbox (S_PRESET_LIST, 16, 32, 180, 430);
            button (L"新規", S_PRESET_NEW, 16, 470, 54);
            button (L"名前変更", S_PRESET_RENAME, 74, 470, 68);
            button (L"削除", S_PRESET_DELETE, 146, 470, 50);

            label (L"グループ", 212, 10, 200);
            listbox (S_GROUP_LIST, 212, 32, 200, 140);
            button (L"追加", S_GROUP_ADD, 212, 178, 52, 24);
            button (L"名前変更", S_GROUP_RENAME, 268, 178, 68, 24);
            button (L"削除", S_GROUP_DELETE, 340, 178, 52, 24);

            label (L"フィルター（チェックでON/OFF）", 212, 212, 320);
            button (L"全てON", S_ALL_ON, 212, 234, 70, 22);
            button (L"全てOFF", S_ALL_OFF, 286, 234, 76, 22);
            HWND lv = make (CreateWindowW (WC_LISTVIEWW, L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                                               LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                           212, 262, 320, 296, hwnd,
                                           reinterpret_cast<HMENU> ((INT_PTR) S_FILTER_LIST),
                                           nullptr, nullptr));
            ListView_SetExtendedListViewStyle (lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
            LVCOLUMNW col {};
            col.mask = LVCF_WIDTH | LVCF_TEXT;
            col.cx = 296;
            col.pszText = const_cast<wchar_t*> (L"フィルター");
            ListView_InsertColumn (lv, 0, &col);

            label (L"移動先", 212, 570, 48);
            HWND mc = make (CreateWindowW (L"COMBOBOX", L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                           262, 566, 150, 200, hwnd,
                                           reinterpret_cast<HMENU> ((INT_PTR) S_MOVE_COMBO),
                                           nullptr, nullptr));
            (void) mc;
            button (L"移動", S_MOVE_BUTTON, 418, 566, 70, 24);

            label (L"かける順番（ONのフィルターだけ表示）", 548, 10, 328);
            listbox (S_ORDER_LIST, 548, 32, 328, 430);
            button (L"↑ 上へ", S_ORDER_UP, 548, 470, 80);
            button (L"↓ 下へ", S_ORDER_DOWN, 632, 470, 80);
            make (CreateWindowW (L"STATIC",
                                 L"吐息キープはゲートと一緒に動きます。\n順番とプリセットはクライアント専用で、VST版は固定順です。",
                                 WS_CHILD | WS_VISIBLE, 548, 506, 328, 44,
                                 hwnd, nullptr, nullptr, nullptr));

            button (L"このプリセットを使う", S_APPLY, 596, 566, 150, 30);
            button (L"閉じる", S_CLOSE, 760, 566, 116, 30);

            EnsurePresetStore();
            RefreshPresetList (g_mainCurrent);
            const std::wstring sel = SelectedPresetName();
            LoadIntoEditor (!sel.empty() ? sel : g_mainCurrent);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD (wp))
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
                case S_GROUP_LIST:
                    if (HIWORD (wp) == LBN_SELCHANGE)
                    {
                        const LRESULT sel = SendMessageW (SCtl (S_GROUP_LIST), LB_GETCURSEL, 0, 0);
                        if (sel != LB_ERR) { g_selGroup = (int) sel; RefreshFilters(); }
                    }
                    return 0;
                case S_GROUP_ADD: OnGroupAdd(); return 0;
                case S_GROUP_RENAME: OnGroupRename(); return 0;
                case S_GROUP_DELETE: OnGroupDelete(); return 0;
                case S_ALL_ON: OnSetGroupAll (true); return 0;
                case S_ALL_OFF: OnSetGroupAll (false); return 0;
                case S_MOVE_BUTTON: OnMoveFilter(); return 0;
                case S_ORDER_UP: OnOrderMove (-1); return 0;
                case S_ORDER_DOWN: OnOrderMove (+1); return 0;
                case S_APPLY:
                    g_mainCurrent = g_edit.name;
                    g_editIsMainCurrent = true;
                    NotifyMainSelect (g_edit.name);
                    return 0;
                case S_CLOSE: DestroyWindow (hwnd); return 0;
            }
            break;
        case WM_NOTIFY:
        {
            auto* nm = reinterpret_cast<NMHDR*> (lp);
            if (nm->idFrom == S_FILTER_LIST && nm->code == LVN_ITEMCHANGED && !g_refreshing)
            {
                auto* nlv = reinterpret_cast<NMLISTVIEW*> (lp);
                if ((nlv->uChanged & LVIF_STATE) &&
                    ((nlv->uNewState ^ nlv->uOldState) & LVIS_STATEIMAGEMASK))
                {
                    LVITEMW item {};
                    item.mask = LVIF_PARAM;
                    item.iItem = nlv->iItem;
                    ListView_GetItem (nm->hwndFrom, &item);
                    const std::string key = kFilterRegistry[item.lParam].key;
                    g_edit.on[key] = ListView_GetCheckState (nm->hwndFrom, nlv->iItem) != 0;
                    Commit();
                    RefreshOrder();
                }
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow (hwnd);
            return 0;
        case WM_DESTROY:
            g_win = nullptr;
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
    INITCOMMONCONTROLSEX icc { sizeof (icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx (&icc);
    HWND win = CreateWindowW (L"MicFilterSettingsWindow", L"フィルター設定",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 908, 660,
                              parent, nullptr, hInst, nullptr);
    if (win) ShowWindow (win, SW_SHOW);
}
