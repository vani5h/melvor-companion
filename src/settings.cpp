#include "settings.hpp"
#include <algorithm>
#include <format>
#include <stdexcept>
#include <commctrl.h>
namespace companion {
namespace { bool use_dark = true; }
void set_dark_mode(bool enabled) { use_dark = enabled; }
bool dark_mode() { return use_dark; }

std::wstring chord_label(DWORD binding) {
    std::wstring text;
    UINT mods = HIWORD(binding & ~disabled_bit);
    if (mods & MOD_CONTROL) text += L"Ctrl + ";
    if (mods & MOD_ALT) text += L"Alt + ";
    if (mods & MOD_SHIFT) text += L"Shift + ";
    if (mods & MOD_WIN) text += L"Win + ";
    UINT vk = LOWORD(binding);
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    LONG packed = (scan & 255) << 16;
    if (scan >> 8) packed |= 1 << 24;
    wchar_t name[64]{};
    if (GetKeyNameTextW(packed, name, 64)) text += name;
    else text += std::format(L"Key {:02X}", vk);
    return text;
}

namespace {
thread_local DWORD recorded;
thread_local bool accepted;
thread_local HMENU recording_menu;
LRESULT CALLBACK menu_record(int code, WPARAM wp, LPARAM lp) {
    if (code == MSGF_MENU) {
        auto* msg = reinterpret_cast<MSG*>(lp);
        if (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) {
            UINT key = static_cast<UINT>(msg->wParam);
            if (key == VK_ESCAPE) { accepted = true; EndMenu(); return 1; }
            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
            if (key != VK_CONTROL && key != VK_MENU && key != VK_SHIFT && key != VK_LWIN && key != VK_RWIN
                && (mods || (key >= VK_F1 && key <= VK_F24))) {
                recorded = MAKELONG(key, mods);
                auto text = chord_label(recorded) + L"   (Esc to save)";
                MENUITEMINFOW item{sizeof(item)};
                item.fMask=MIIM_STRING;
                item.dwTypeData=text.data();
                SetMenuItemInfoW(recording_menu,0,TRUE,&item);
                HWND popup=FindWindowW(L"#32768",nullptr);
                if(popup) {InvalidateRect(popup,nullptr,FALSE);UpdateWindow(popup);}
            }
            return 1;
        }
        if (msg->message == WM_CHAR || msg->message == WM_SYSCHAR) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}
std::optional<DWORD> capture_shortcut(HWND owner, DWORD binding) {
    recorded = binding; accepted = false;
    HMENU menu = CreatePopupMenu();
    if (!menu) throw std::runtime_error("Could not create shortcut menu");
    recording_menu = menu;
    auto text = chord_label(binding) + L"   (Esc to save)";
    AppendMenuW(menu, MF_GRAYED, 0, text.c_str());
    AppendMenuW(menu, MF_GRAYED, 1, L"Press your shortcut keys");
    style_menu(menu);
    HHOOK hook = SetWindowsHookExW(WH_MSGFILTER, menu_record, nullptr, GetCurrentThreadId());
    if (!hook) { DestroyMenu(menu); throw std::runtime_error("Could not listen for shortcut"); }
    POINT pt{}; GetCursorPos(&pt);
    track_companion_menu(menu, owner, pt, 0);
    UnhookWindowsHookEx(hook);
    DestroyMenu(menu);
    return accepted ? std::optional<DWORD>(recorded) : std::nullopt;
}

namespace {
LRESULT CALLBACK fit_popup(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, fit_popup, id);
    auto result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_WINDOWPOSCHANGING) {
        auto& pos = *reinterpret_cast<WINDOWPOS*>(lp);
        if (pos.flags & SWP_NOMOVE) return result;
        RECT current{};
        GetWindowRect(window, &current);
        const int width = (pos.flags & SWP_NOSIZE) ? current.right - current.left : pos.cx;
        const int height = (pos.flags & SWP_NOSIZE) ? current.bottom - current.top : pos.cy;
        RECT proposed{pos.x, pos.y, pos.x + width, pos.y + height};
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const auto& work = monitor.rcWork;
            pos.x = std::clamp<LONG>(pos.x, work.left, std::max(work.left, work.right - width));
            pos.y = std::clamp<LONG>(pos.y, work.top, std::max(work.top, work.bottom - height));
        }
    }
    return result;
}

// only native popups from our own UI thread during this menu
LRESULT CALLBACK popup_created(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto& msg = *reinterpret_cast<CWPSTRUCT*>(lp);
        if (msg.message == WM_NCCREATE && GetClassLongPtrW(msg.hwnd, GCW_ATOM) == 32768)
            SetWindowSubclass(msg.hwnd, fit_popup, 1, 0);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}

UINT track_companion_menu(HMENU menu, HWND owner, POINT anchor, UINT flags) {
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) {
        // keep the last rows from going behind the taskbar
        const RECT work = monitor.rcWork;
        anchor.x = std::clamp(anchor.x, work.left, work.right - 1);
        anchor.y = std::clamp(anchor.y, work.top, work.bottom - 1);
        if (anchor.y - work.top > work.bottom - anchor.y) flags |= TPM_BOTTOMALIGN;
        if (anchor.x - work.left > work.right - anchor.x) flags |= TPM_RIGHTALIGN;
        MENUINFO info{sizeof(info)};
        info.fMask = MIM_MAXHEIGHT | MIM_APPLYTOSUBMENUS;
        info.cyMax = work.bottom - work.top;
        SetMenuInfo(menu, &info);
    }
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, popup_created, nullptr, GetCurrentThreadId());
    UINT command = TrackPopupMenuEx(menu, flags | TPM_RETURNCMD | TPM_WORKAREA, anchor.x, anchor.y, owner, nullptr);
    if (hook) UnhookWindowsHookEx(hook);
    return command;
}

void style_menu(HMENU menu) {
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = static_cast<HBRUSH>(GetStockObject(use_dark ? BLACK_BRUSH : WHITE_BRUSH));
    SetMenuInfo(menu, &info);
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE;
        GetMenuItemInfoW(menu, i, TRUE, &item);
        item.fMask |= MIIM_DATA;
        item.dwItemData = static_cast<ULONG_PTR>(i) | ((item.fType & MFT_SEPARATOR) ? 0x80000000u : 0);
        item.fType |= MFT_OWNERDRAW;
        SetMenuItemInfoW(menu, i, TRUE, &item);
    }
}

bool measure_menu_item(HWND window, MEASUREITEMSTRUCT& item) {
    if (item.CtlType != ODT_MENU) return false;
    auto dpi = GetDpiForWindow(window);
    item.itemWidth = MulDiv(270, dpi, 96);
    item.itemHeight = MulDiv((item.itemData & 0x80000000u) ? 6 : 22, dpi, 96);
    return true;
}

namespace {
// menu font for this DPI, cached per DPI
HFONT menu_font(UINT dpi) {
    thread_local UINT cached_dpi = 0;
    thread_local HFONT font = nullptr;
    if (font && cached_dpi == dpi) return font;
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) return font;
    HFONT next = CreateFontIndirectW(&metrics.lfMenuFont);
    if (!next) return font;
    if (font) DeleteObject(font);
    font = next;
    cached_dpi = dpi;
    return font;
}
}

bool paint_menu_item(HWND window, const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_MENU) return false;
    UINT dpi = GetDpiForWindow(window);
    auto scaled = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    auto dc = item.hDC;
    auto bounds = item.rcItem;
    bool selected = (item.itemState & ODS_SELECTED) != 0;
    SetDCBrushColor(dc, selected ? (use_dark ? RGB(37,57,55) : RGB(215,235,232)) : (use_dark ? RGB(0,0,0) : RGB(255,255,255)));
    auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    FillRect(dc, &bounds, brush);
    auto menu = reinterpret_cast<HMENU>(item.hwndItem);
    wchar_t text[160]{};
    MENUITEMINFOW entry{sizeof(entry)};
    entry.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_STATE;
    entry.dwTypeData = text;
    entry.cch = 159;
    GetMenuItemInfoW(menu, static_cast<UINT>(item.itemData & 0x7fffffffu), TRUE, &entry);
    bounds.left += scaled(12);
    bounds.right -= scaled(10);
    if (entry.fType & MFT_SEPARATOR) {
        bounds.top = (bounds.top + bounds.bottom) / 2;
        bounds.bottom = bounds.top + std::max(1, scaled(1));
        SetDCBrushColor(dc, use_dark ? RGB(48,48,48) : RGB(210,210,210));
        FillRect(dc, &bounds, brush);
    } else {
        HFONT font = menu_font(dpi);
        HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, (entry.fState & MFS_DISABLED) ? (use_dark ? RGB(160,160,160) : RGB(110,110,110)) : (use_dark ? RGB(245,248,247) : RGB(20,20,20)));
        if (entry.fState & MFS_CHECKED) DrawTextW(dc, L"\u2713", 1, &bounds, DT_SINGLELINE | DT_VCENTER);
        bounds.left += scaled(22);
        DrawTextW(dc, text, -1, &bounds, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (old) SelectObject(dc, old);
    }
    return true;
}
}


