#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <shlobj.h>
#include <cmath>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
enum HB_DWM_WINDOW_CORNER_PREFERENCE { HB_DWMWCP_DEFAULT = 0, HB_DWMWCP_DONOTROUND = 1, HB_DWMWCP_ROUND = 2, HB_DWMWCP_ROUNDSMALL = 3 };

static const wchar_t* kClassName   = L"HotkeyBlockerMainWnd";
static const wchar_t* kAppTitle    = L"Hotkey Blocker";
static const wchar_t* kMutexName   = L"Local\\HotkeyBlocker_SingleInstance_Mutex_9F21";
static const wchar_t* kMsgShowName = L"HotkeyBlocker_ShowWindow_Broadcast_9F21";

#define ID_LIST_HOTKEYS   1001
#define ID_BTN_ADD        1002
#define ID_BTN_REMOVE     1003
#define ID_CHK_TRAY       1004
#define ID_CHK_KEEPAWAKE  1005
#define ID_CHK_RBTRAY     1006
#define ID_STATUS_LABEL   1007
#define ID_CHK_AUTOSTART  1008
#define ID_BTN_EXIT       1009
#define ID_CHK_TRANSPARENCY 1010
#define ID_BTN_MENU       1011
#define ID_TRAY_MENU_SHOW 2001
#define ID_TRAY_MENU_EXIT 2002

#define WM_TRAYICON       (WM_APP + 1)
#define WM_RBTRAY_ICON    (WM_APP + 2)
#define WM_APP_SETOPACITY (WM_APP + 3)
#define TRAY_UID_MAIN     1
#define RBTRAY_UID_BASE   1000

static const COLORREF kColBg        = RGB(24, 24, 27);
static const COLORREF kColBg2       = RGB(32, 32, 36);
static const COLORREF kColText      = RGB(228, 228, 231);
static const COLORREF kColTextDim   = RGB(150, 150, 156);
static const COLORREF kColAccent    = RGB(0, 153, 255);
static const COLORREF kColBorder    = RGB(58, 58, 64);
static const COLORREF kColBtnHover  = RGB(46, 46, 52);

struct HotkeyCombo {
    bool ctrl = false, alt = false, shift = false, win = false;
    UINT vk = 0;
    std::wstring Display() const {
        std::wstring s;
        if (ctrl)  s += L"Ctrl+";
        if (alt)   s += L"Alt+";
        if (shift) s += L"Shift+";
        if (win)   s += L"Win+";
        wchar_t name[64] = {0};
        UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LONG lParamScan = (scan << 16);
        switch (vk) {
            case VK_ESCAPE: wcscpy_s(name, L"Esc"); break;
            case VK_TAB:    wcscpy_s(name, L"Tab"); break;
            case VK_RETURN: wcscpy_s(name, L"Enter"); break;
            case VK_SPACE:  wcscpy_s(name, L"Space"); break;
            case VK_DELETE: lParamScan |= (1 << 24); GetKeyNameTextW(lParamScan, name, 64); break;
            default:
                if (GetKeyNameTextW(lParamScan, name, 64) == 0)
                    swprintf_s(name, L"VK_%02X", vk);
        }
        s += name;
        return s;
    }
};

struct RbTrayEntry {
    HWND hwnd = nullptr;
    UINT uid = 0;
    bool used = false;
};

static std::vector<HotkeyCombo> g_hotkeys;
static bool g_showTrayIcon = false;
static bool g_keepAwake = false;
static bool g_rbTrayEnabled = false;
static bool g_autostartEnabled = false;
static bool g_transparencyEnabled = false;

static HWND g_hMainWnd = nullptr;
static HHOOK g_hKeyboardHook = nullptr;
static HHOOK g_hMouseHook = nullptr;
static HINSTANCE g_hInst = nullptr;
static HFONT g_hFont = nullptr;
static HFONT g_hFontBold = nullptr;
static HBRUSH g_hBrushBg = nullptr;
static HBRUSH g_hBrushBg2 = nullptr;
static HBRUSH g_hBrushBtnHover = nullptr;
static UINT   g_msgShowWindow = 0;
static bool   g_capturing = false;
static bool   g_trayIconVisible = false;
static HANDLE g_keepAwakeStopEvent = nullptr;
static HANDLE g_keepAwakeThread = nullptr;
static std::vector<RbTrayEntry> g_rbEntries;
static HWND g_hListBox = nullptr, g_hBtnAdd = nullptr, g_hBtnRemove = nullptr;
static HWND g_hBtnMenu = nullptr;
static HWND g_hChkKeepAwake = nullptr, g_hChkRbTray = nullptr;
static HWND g_hChkTransparency = nullptr;
static HWND g_hStatusLabel = nullptr;
static HWND g_hTooltip = nullptr;

#define ID_ANIM_TIMER 9001

struct AnimCtrl {
    HWND hwnd = nullptr;
    UINT id = 0;
    bool isCheckbox = false;
    float hover = 0.f;
    float check = 0.f;
    bool hovering = false;
};
static std::vector<AnimCtrl> g_animCtrls;
static bool g_animTimerActive = false;

static AnimCtrl* FindAnim(HWND hwnd) {
    for (auto& a : g_animCtrls) if (a.hwnd == hwnd) return &a;
    return nullptr;
}

static void StartAnimTimer() {
    if (!g_animTimerActive && g_hMainWnd) {
        SetTimer(g_hMainWnd, ID_ANIM_TIMER, 15, nullptr);
        g_animTimerActive = true;
    }
}

static bool GetCheckedStateFor(UINT id);

static COLORREF LerpColor(COLORREF a, COLORREF b, float t) {
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    int r = GetRValue(a) + (int)((GetRValue(b) - GetRValue(a)) * t);
    int g = GetGValue(a) + (int)((GetGValue(b) - GetGValue(a)) * t);
    int b2 = GetBValue(a) + (int)((GetBValue(b) - GetBValue(a)) * t);
    return RGB(r, g, b2);
}

static bool StepTowards(float& cur, float target, float speed) {
    if (fabsf(cur - target) < 0.01f) {
        if (cur != target) { cur = target; return true; }
        return false;
    }
    cur += (target - cur) * speed;
    return true;
}

static LRESULT CALLBACK CtrlSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR uIdSubclass, DWORD_PTR) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        AnimCtrl* a = FindAnim(hwnd);
        if (a && !a->hovering) {
            a->hovering = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            StartAnimTimer();
        }
        break;
    }
    case WM_MOUSELEAVE: {
        AnimCtrl* a = FindAnim(hwnd);
        if (a) { a->hovering = false; StartAnimTimer(); }
        break;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, CtrlSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void ApplyRoundedRegion(HWND hwnd, int radius) {
    RECT rc; GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius, radius);
    SetWindowRgn(hwnd, rgn, TRUE);
}

static void RegisterAnimControl(HWND hwnd, UINT id, bool isCheckbox, int roundRadius) {
    AnimCtrl a; a.hwnd = hwnd; a.id = id; a.isCheckbox = isCheckbox;
    if (isCheckbox) a.check = GetCheckedStateFor(id) ? 1.f : 0.f;
    g_animCtrls.push_back(a);
    SetWindowSubclass(hwnd, CtrlSubclassProc, id, 0);
    if (roundRadius > 0) ApplyRoundedRegion(hwnd, roundRadius);
}

static const wchar_t* kRunKeyPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValueName = L"HotkeyBlocker";

static bool IsAutostartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKeyPath, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS && size > 0;
}

static bool SetAutostart(bool enable) {
    HKEY hKey;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(path) + L"\" --minimized";
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRunKeyPath, 0, nullptr, 0,
                             KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
            return false;
        LONG r = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                                 (const BYTE*)quoted.c_str(),
                                 (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
        return r == ERROR_SUCCESS;
    } else {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKeyPath, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
            return false;
        LONG r = RegDeleteValueW(hKey, kRunValueName);
        RegCloseKey(hKey);
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
}

static void EnsureAutostartCommandUpToDate() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKeyPath, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return;
    wchar_t buf[600] = {0};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG r = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(hKey);
    if (r == ERROR_SUCCESS && type == REG_SZ) {
        if (wcsstr(buf, L"--minimized") == nullptr) {
            SetAutostart(true);
        }
    }
}

static bool GetCheckedStateFor(UINT id) {
    switch (id) {
        case ID_CHK_TRAY: return g_showTrayIcon;
        case ID_CHK_KEEPAWAKE: return g_keepAwake;
        case ID_CHK_RBTRAY: return g_rbTrayEnabled;
        case ID_CHK_AUTOSTART: return g_autostartEnabled;
        case ID_CHK_TRANSPARENCY: return g_transparencyEnabled;
    }
    return false;
}

static std::wstring GetConfigPath() {
    wchar_t path[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path) != S_OK)
        return L"config.ini";
    std::wstring dir = std::wstring(path) + L"\\HotkeyBlocker";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.ini";
}

static void SaveConfig() {
    std::wofstream f(GetConfigPath().c_str());
    if (!f.is_open()) return;
    f << L"ShowTray=" << (g_showTrayIcon ? 1 : 0) << L"\n";
    f << L"KeepAwake=" << (g_keepAwake ? 1 : 0) << L"\n";
    f << L"RbTray=" << (g_rbTrayEnabled ? 1 : 0) << L"\n";
    f << L"Transparency=" << (g_transparencyEnabled ? 1 : 0) << L"\n";
    for (auto& h : g_hotkeys) {
        f << L"Hotkey=" << h.ctrl << L"," << h.alt << L"," << h.shift << L","
          << h.win << L"," << h.vk << L"\n";
    }
}

static void LoadConfig() {
    std::wifstream f(GetConfigPath().c_str());
    if (!f.is_open()) return;
    std::wstring line;
    while (std::getline(f, line)) {
        if (line.rfind(L"ShowTray=", 0) == 0)
            g_showTrayIcon = line.substr(9) == L"1";
        else if (line.rfind(L"KeepAwake=", 0) == 0)
            g_keepAwake = line.substr(10) == L"1";
        else if (line.rfind(L"RbTray=", 0) == 0)
            g_rbTrayEnabled = line.substr(7) == L"1";
        else if (line.rfind(L"Transparency=", 0) == 0)
            g_transparencyEnabled = line.substr(13) == L"1";
        else if (line.rfind(L"Hotkey=", 0) == 0) {
            std::wstringstream ss(line.substr(7));
            std::wstring tok;
            HotkeyCombo h;
            int idx = 0;
            while (std::getline(ss, tok, L',')) {
                int v = _wtoi(tok.c_str());
                switch (idx++) {
                    case 0: h.ctrl = v != 0; break;
                    case 1: h.alt = v != 0; break;
                    case 2: h.shift = v != 0; break;
                    case 3: h.win = v != 0; break;
                    case 4: h.vk = (UINT)v; break;
                }
            }
            if (h.vk != 0) g_hotkeys.push_back(h);
        }
    }
}

static DWORD WINAPI KeepAwakeThreadProc(LPVOID) {
    while (WaitForSingleObject(g_keepAwakeStopEvent, 30000) == WAIT_TIMEOUT) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    }
    return 0;
}

static void StartKeepAwake() {
    if (g_keepAwakeThread) return;
    g_keepAwakeStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    g_keepAwakeThread = CreateThread(nullptr, 0, KeepAwakeThreadProc, nullptr, 0, nullptr);
}

static void StopKeepAwake() {
    if (!g_keepAwakeThread) return;
    SetEvent(g_keepAwakeStopEvent);
    WaitForSingleObject(g_keepAwakeThread, 2000);
    CloseHandle(g_keepAwakeThread);
    CloseHandle(g_keepAwakeStopEvent);
    g_keepAwakeThread = nullptr;
    g_keepAwakeStopEvent = nullptr;
    SetThreadExecutionState(ES_CONTINUOUS);
}

static bool IsModifierVk(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LWIN || vk == VK_RWIN;
}

static bool g_modCtrl = false, g_modAlt = false, g_modShift = false, g_modWin = false;

static int DigitFromEvent(UINT vkCode) {
    if (vkCode >= '0' && vkCode <= '9') return (int)(vkCode - '0');
    return -1;
}

static void ApplyWindowOpacity(int digit, HWND target) {
    if (!target || !IsWindow(target)) return;
    LONG_PTR ex = GetWindowLongPtrW(target, GWL_EXSTYLE);
    if (digit == 0) {
        if (ex & WS_EX_LAYERED) {
            SetWindowLongPtrW(target, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
            RedrawWindow(target, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        return;
    }
    if (!(ex & WS_EX_LAYERED))
        SetWindowLongPtrW(target, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    BYTE alpha = (BYTE)((digit * 255 + 5) / 10);
    SetLayeredWindowAttributes(target, 0, alpha, LWA_ALPHA);
}

static void AddHotkeyFromCapture(const HotkeyCombo& h);

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        switch (p->vkCode) {
            case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
                if (isDown) g_modCtrl = true; else if (isUp) g_modCtrl = false;
                break;
            case VK_LMENU: case VK_RMENU: case VK_MENU:
                if (isDown) g_modAlt = true; else if (isUp) g_modAlt = false;
                break;
            case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
                if (isDown) g_modShift = true; else if (isUp) g_modShift = false;
                break;
            case VK_LWIN: case VK_RWIN:
                if (isDown) g_modWin = true; else if (isUp) g_modWin = false;
                break;
        }

        if (g_capturing) {
            if (isDown) {
                if (!IsModifierVk(p->vkCode)) {
                    HotkeyCombo h;
                    h.ctrl = g_modCtrl; h.alt = g_modAlt; h.shift = g_modShift; h.win = g_modWin;
                    h.vk = p->vkCode;
                    g_capturing = false;
                    AddHotkeyFromCapture(h);
                }
                return 1;
            }
            return 1;
        }

        if (isDown) {
            bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (g_transparencyEnabled && g_modCtrl && shiftHeld) {
                int digit = DigitFromEvent(p->vkCode);
                if (digit >= 0) {
                    HWND fg = GetForegroundWindow();
                    if (fg) PostMessageW(g_hMainWnd, WM_APP_SETOPACITY, (WPARAM)digit, (LPARAM)fg);
                    return 1;
                }
            }
            for (auto& h : g_hotkeys) {
                if (h.vk == p->vkCode && h.ctrl == g_modCtrl && h.alt == g_modAlt &&
                    h.shift == g_modShift && h.win == g_modWin) {
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

static int FindFreeRbSlot() {
    for (size_t i = 0; i < g_rbEntries.size(); i++)
        if (!g_rbEntries[i].used) return (int)i;
    g_rbEntries.push_back(RbTrayEntry());
    return (int)g_rbEntries.size() - 1;
}

static void MinimizeWindowToTray(HWND hwnd) {
    if (!hwnd || hwnd == g_hMainWnd) return;
    for (auto& e : g_rbEntries)
        if (e.used && e.hwnd == hwnd) return;

    int slot = FindFreeRbSlot();
    RbTrayEntry& e = g_rbEntries[slot];
    e.hwnd = hwnd;
    e.uid = RBTRAY_UID_BASE + slot;
    e.used = true;

    DWORD_PTR iconResult = 0;
    SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &iconResult);
    HICON icon = (HICON)iconResult;
    if (!icon) icon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);

    wchar_t title[256] = {0};
    GetWindowTextW(hwnd, title, 256);

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hMainWnd;
    nid.uID = e.uid;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_RBTRAY_ICON;
    nid.hIcon = icon;
    wcsncpy_s(nid.szTip, title[0] ? title : L"Свёрнутое окно", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);

    ShowWindow(hwnd, SW_HIDE);
}

static void RestoreRbWindow(size_t slot) {
    if (slot >= g_rbEntries.size() || !g_rbEntries[slot].used) return;
    RbTrayEntry& e = g_rbEntries[slot];
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hMainWnd;
    nid.uID = e.uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);

    if (IsWindow(e.hwnd)) {
        ShowWindow(e.hwnd, SW_SHOW);
        ShowWindow(e.hwnd, SW_RESTORE);
        SetForegroundWindow(e.hwnd);
    }
    e.used = false;
    e.hwnd = nullptr;
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_rbTrayEnabled && wParam == WM_RBUTTONUP) {
        MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
        HWND hwnd = WindowFromPoint(p->pt);
        if (hwnd) {
            DWORD_PTR hitResult = 0;
            LRESULT sent = SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0,
                MAKELPARAM(p->pt.x, p->pt.y),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &hitResult);
            if (sent != 0 && (LRESULT)hitResult == HTMINBUTTON) {
                HWND top = GetAncestor(hwnd, GA_ROOT);
                if (top && top != g_hMainWnd && (GetWindowLongW(top, GWL_STYLE) & WS_MINIMIZEBOX)) {
                    MinimizeWindowToTray(top);
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

static void UpdateMainTrayIcon() {
    if (g_showTrayIcon && !g_trayIconVisible) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hMainWnd;
        nid.uID = TRAY_UID_MAIN;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
        if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(nid.szTip, kAppTitle);
        Shell_NotifyIconW(NIM_ADD, &nid);
        g_trayIconVisible = true;
    } else if (!g_showTrayIcon && g_trayIconVisible) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hMainWnd;
        nid.uID = TRAY_UID_MAIN;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_trayIconVisible = false;
    }
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_MENU_SHOW, L"Открыть");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_keepAwake ? MF_CHECKED : MF_UNCHECKED),
                ID_CHK_KEEPAWAKE, L"Не давать ПК уходить в сон/выключать экран");
    AppendMenuW(menu, MF_STRING | (g_rbTrayEnabled ? MF_CHECKED : MF_UNCHECKED),
                ID_CHK_RBTRAY, L"Сворачивать окна в трей");
    AppendMenuW(menu, MF_STRING | (g_transparencyEnabled ? MF_CHECKED : MF_UNCHECKED),
                ID_CHK_TRANSPARENCY, L"Горячие клавиши для прозрачности окна");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_MENU_EXIT, L"Выход");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static void ShowTopMenu(HWND hwnd, HWND btn) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_showTrayIcon ? MF_CHECKED : MF_UNCHECKED),
                ID_CHK_TRAY, L"Показывать значок в трее");
    AppendMenuW(menu, MF_STRING | (g_autostartEnabled ? MF_CHECKED : MF_UNCHECKED),
                ID_CHK_AUTOSTART, L"Запускать вместе с Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_BTN_EXIT, L"Выход");
    RECT rc; GetWindowRect(btn, &rc);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, rc.left, rc.bottom + 2, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static void RefreshHotkeyList() {
    SendMessageW(g_hListBox, LB_RESETCONTENT, 0, 0);
    for (auto& h : g_hotkeys)
        SendMessageW(g_hListBox, LB_ADDSTRING, 0, (LPARAM)h.Display().c_str());
}

static void AddHotkeyFromCapture(const HotkeyCombo& h) {
    HotkeyCombo* copy = new HotkeyCombo(h);
    PostMessageW(g_hMainWnd, WM_APP + 51, 0, (LPARAM)copy);
}

static void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int corner = HB_DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
}

static void CenterWindowOnScreen(HWND hwnd) {
    RECT wrc; GetWindowRect(hwnd, &wrc);
    int w = wrc.right - wrc.left;
    int h = wrc.bottom - wrc.top;
    RECT work;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = 0; work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static void ShowMainWindowAnimated(HWND hwnd) {
    CenterWindowOnScreen(hwnd);
    AnimateWindow(hwnd, 180, AW_BLEND);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

static void DrawDarkCheckbox(LPDRAWITEMSTRUCT dis, bool checked) {
    wchar_t text[256] = {0};
    GetWindowTextW(dis->hwndItem, text, 256);

    AnimCtrl* a = FindAnim(dis->hwndItem);
    float hover = a ? a->hover : 0.f;
    float check = a ? a->check : (checked ? 1.f : 0.f);

    FillRect(dis->hDC, &dis->rcItem, g_hBrushBg);

    const int boxSize = 16;
    const int radius = 5;
    int top = dis->rcItem.top + ((dis->rcItem.bottom - dis->rcItem.top) - boxSize) / 2;
    RECT box = { dis->rcItem.left, top, dis->rcItem.left + boxSize, top + boxSize };

    COLORREF fillCol = LerpColor(kColBg2, kColAccent, check);
    COLORREF borderCol = LerpColor(LerpColor(kColBorder, kColAccent, hover * 0.5f), kColAccent, check);

    HBRUSH boxBrush = CreateSolidBrush(fillCol);
    HPEN pen = CreatePen(PS_SOLID, 1, borderCol);
    HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, boxBrush);
    RoundRect(dis->hDC, box.left, box.top, box.right, box.bottom, radius, radius);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(boxBrush);

    if (check > 0.02f) {
        HPEN checkPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ oldP = SelectObject(dis->hDC, checkPen);
        POINT p1 = { box.left + 3, box.top + 8 };
        POINT p2 = { box.left + 6, box.top + 12 };
        POINT p3 = { box.left + 13, box.top + 4 };
        if (check < 0.5f) {
            float seg = check / 0.5f;
            POINT mid = { p1.x + (LONG)((p2.x - p1.x) * seg), p1.y + (LONG)((p2.y - p1.y) * seg) };
            MoveToEx(dis->hDC, p1.x, p1.y, nullptr);
            LineTo(dis->hDC, mid.x, mid.y);
        } else {
            float seg = (check - 0.5f) / 0.5f;
            POINT mid2 = { p2.x + (LONG)((p3.x - p2.x) * seg), p2.y + (LONG)((p3.y - p2.y) * seg) };
            MoveToEx(dis->hDC, p1.x, p1.y, nullptr);
            LineTo(dis->hDC, p2.x, p2.y);
            LineTo(dis->hDC, mid2.x, mid2.y);
        }
        SelectObject(dis->hDC, oldP);
        DeleteObject(checkPen);
    }

    RECT textRect = dis->rcItem;
    textRect.left += boxSize + 8;
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, RGB(255, 255, 255));
    SelectObject(dis->hDC, g_hFont);
    DrawTextW(dis->hDC, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (dis->itemState & ODS_FOCUS) {
        RECT fr = dis->rcItem;
        DrawFocusRect(dis->hDC, &fr);
    }
}

static void DrawDarkButton(LPDRAWITEMSTRUCT dis, const wchar_t* text) {
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    AnimCtrl* a = FindAnim(dis->hwndItem);
    float hover = a ? a->hover : 0.f;

    FillRect(dis->hDC, &dis->rcItem, g_hBrushBg);

    COLORREF bg = pressed ? kColAccent : LerpColor(kColBg2, kColBtnHover, hover);
    COLORREF border = LerpColor(kColBorder, kColAccent, hover * 0.6f + (pressed ? 0.4f : 0.f));

    HBRUSH bgBrush = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, bgBrush);
    RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 10, 10);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(bgBrush);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, pressed ? RGB(255, 255, 255) : kColText);
    SelectObject(dis->hDC, g_hFont);
    DrawTextW(dis->hDC, text, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void CreateControls(HWND hwnd) {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = -15;
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_hFontBold = CreateFontIndirectW(&ncm.lfMessageFont);

    auto setFont = [](HWND h) { SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE); };

    g_hBtnMenu = CreateWindowExW(0, L"BUTTON", L"Меню",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        20, 16, 100, 30, hwnd, (HMENU)ID_BTN_MENU, g_hInst, nullptr);
    setFont(g_hBtnMenu);

    HWND lbl1 = CreateWindowExW(0, L"STATIC", L"Заблокированные сочетания клавиш",
        WS_CHILD | WS_VISIBLE, 20, 64, 400, 22, hwnd, nullptr, g_hInst, nullptr);
    setFont(lbl1);

    g_hListBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
        20, 92, 360, 160, hwnd, (HMENU)ID_LIST_HOTKEYS, g_hInst, nullptr);
    setFont(g_hListBox);

    g_hBtnAdd = CreateWindowExW(0, L"BUTTON", L"Добавить",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        392, 92, 110, 34, hwnd, (HMENU)ID_BTN_ADD, g_hInst, nullptr);
    setFont(g_hBtnAdd);

    g_hBtnRemove = CreateWindowExW(0, L"BUTTON", L"Удалить",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        392, 134, 110, 34, hwnd, (HMENU)ID_BTN_REMOVE, g_hInst, nullptr);
    setFont(g_hBtnRemove);

    g_hStatusLabel = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE, 20, 260, 480, 22, hwnd, (HMENU)ID_STATUS_LABEL, g_hInst, nullptr);
    setFont(g_hStatusLabel);

    g_hChkKeepAwake = CreateWindowExW(0, L"BUTTON", L"Не давать ПК уходить в сон/выключать экран",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        20, 294, 480, 24, hwnd, (HMENU)ID_CHK_KEEPAWAKE, g_hInst, nullptr);
    setFont(g_hChkKeepAwake);

    g_hChkRbTray = CreateWindowExW(0, L"BUTTON", L"Сворачивать окна в трей",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        20, 324, 480, 24, hwnd, (HMENU)ID_CHK_RBTRAY, g_hInst, nullptr);
    setFont(g_hChkRbTray);

    g_hChkTransparency = CreateWindowExW(0, L"BUTTON",
        L"Горячие клавиши для прозрачности окна",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        20, 354, 480, 24, hwnd, (HMENU)ID_CHK_TRANSPARENCY, g_hInst, nullptr);
    setFont(g_hChkTransparency);

    g_hTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, nullptr, g_hInst, nullptr);
    auto addTip = [&](HWND ctrl, const wchar_t* text) {
        TOOLINFOW ti = { sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
        ti.hwnd = hwnd;
        ti.uId = (UINT_PTR)ctrl;
        ti.lpszText = (LPWSTR)text;
        SendMessageW(g_hTooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    };
    addTip(g_hChkRbTray, L"ПКМ на кнопке \"Свернуть\"");
    addTip(g_hChkTransparency, L"Ctrl+Shift+0-9");

    g_autostartEnabled = IsAutostartEnabled();

    RegisterAnimControl(g_hBtnMenu, ID_BTN_MENU, false, 10);
    RegisterAnimControl(g_hBtnAdd, ID_BTN_ADD, false, 10);
    RegisterAnimControl(g_hBtnRemove, ID_BTN_REMOVE, false, 10);
    RegisterAnimControl(g_hChkKeepAwake, ID_CHK_KEEPAWAKE, true, 0);
    RegisterAnimControl(g_hChkRbTray, ID_CHK_RBTRAY, true, 0);
    RegisterAnimControl(g_hChkTransparency, ID_CHK_TRANSPARENCY, true, 0);

    RefreshHotkeyList();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_msgShowWindow) {
        ShowMainWindowAnimated(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        ApplyDarkTitleBar(hwnd);
        CreateControls(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, kColText);
        SetBkColor(hdc, kColBg);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)g_hBrushBg;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, kColText);
        SetBkColor(hdc, kColBg2);
        return (LRESULT)g_hBrushBg2;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_hBrushBg);
        return 1;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        switch (dis->CtlID) {
            case ID_BTN_MENU: DrawDarkButton(dis, L"Меню"); break;
            case ID_BTN_ADD: DrawDarkButton(dis, g_capturing ? L"Отмена" : L"Добавить"); break;
            case ID_BTN_REMOVE: DrawDarkButton(dis, L"Удалить"); break;
            case ID_CHK_KEEPAWAKE: DrawDarkCheckbox(dis, g_keepAwake); break;
            case ID_CHK_RBTRAY: DrawDarkCheckbox(dis, g_rbTrayEnabled); break;
            case ID_CHK_TRANSPARENCY: DrawDarkCheckbox(dis, g_transparencyEnabled); break;
        }
        return TRUE;
    }

    case WM_APP + 50:
        SetWindowTextW(g_hStatusLabel, L"Захват отменён.");
        InvalidateRect(g_hBtnAdd, nullptr, TRUE);
        return 0;

    case WM_APP + 51: {
        HotkeyCombo* h = (HotkeyCombo*)lParam;
        bool dup = false;
        for (auto& e : g_hotkeys)
            if (e.vk == h->vk && e.ctrl == h->ctrl && e.alt == h->alt && e.shift == h->shift && e.win == h->win)
                dup = true;
        if (!dup) g_hotkeys.push_back(*h);
        SetWindowTextW(g_hStatusLabel, dup ? L"Такая комбинация уже добавлена." : L"Комбинация добавлена и заблокирована.");
        delete h;
        RefreshHotkeyList();
        SaveConfig();
        InvalidateRect(g_hBtnAdd, nullptr, TRUE);
        return 0;
    }

    case WM_APP_SETOPACITY:
        ApplyWindowOpacity((int)wParam, (HWND)lParam);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_BTN_MENU && HIWORD(wParam) == BN_CLICKED) {
            ShowTopMenu(hwnd, g_hBtnMenu);
        } else if (id == ID_BTN_ADD && HIWORD(wParam) == BN_CLICKED) {
            if (g_capturing) {
                g_capturing = false;
                SetWindowTextW(g_hStatusLabel, L"Захват отменён.");
            } else {
                g_capturing = true;
                SetWindowTextW(g_hStatusLabel, L"Нажмите нужное сочетание клавиш (кнопка «Отмена» — отменить)...");
            }
            InvalidateRect(g_hBtnAdd, nullptr, TRUE);
        } else if (id == ID_BTN_REMOVE && HIWORD(wParam) == BN_CLICKED) {
            int sel = (int)SendMessageW(g_hListBox, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR && sel < (int)g_hotkeys.size()) {
                g_hotkeys.erase(g_hotkeys.begin() + sel);
                RefreshHotkeyList();
                SaveConfig();
                SetWindowTextW(g_hStatusLabel, L"Комбинация удалена.");
            }
        } else if (id == ID_CHK_TRAY) {
            g_showTrayIcon = !g_showTrayIcon;
            UpdateMainTrayIcon();
            SaveConfig();
        } else if (id == ID_CHK_KEEPAWAKE && HIWORD(wParam) == BN_CLICKED) {
            g_keepAwake = !g_keepAwake;
            InvalidateRect(g_hChkKeepAwake, nullptr, TRUE);
            StartAnimTimer();
            if (g_keepAwake) StartKeepAwake(); else StopKeepAwake();
            SaveConfig();
        } else if (id == ID_CHK_RBTRAY && HIWORD(wParam) == BN_CLICKED) {
            g_rbTrayEnabled = !g_rbTrayEnabled;
            InvalidateRect(g_hChkRbTray, nullptr, TRUE);
            StartAnimTimer();
            SaveConfig();
        } else if (id == ID_CHK_AUTOSTART) {
            bool nc = !g_autostartEnabled;
            if (SetAutostart(nc)) {
                g_autostartEnabled = nc;
                SetWindowTextW(g_hStatusLabel, nc ? L"Добавлено в автозагрузку Windows."
                                                   : L"Убрано из автозагрузки Windows.");
            } else {
                SetWindowTextW(g_hStatusLabel,
                    L"Не удалось изменить автозагрузку — запустите программу от имени администратора.");
            }
        } else if (id == ID_CHK_TRANSPARENCY && HIWORD(wParam) == BN_CLICKED) {
            g_transparencyEnabled = !g_transparencyEnabled;
            InvalidateRect(g_hChkTransparency, nullptr, TRUE);
            StartAnimTimer();
            SaveConfig();
        } else if (id == ID_BTN_EXIT) {
            DestroyWindow(hwnd);
        } else if (id == ID_TRAY_MENU_SHOW) {
            ShowMainWindowAnimated(hwnd);
        } else if (id == ID_TRAY_MENU_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            ShowMainWindowAnimated(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        return 0;

    case WM_RBTRAY_ICON: {
        UINT uid = (UINT)wParam;
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            for (size_t i = 0; i < g_rbEntries.size(); i++)
                if (g_rbEntries[i].used && g_rbEntries[i].uid == uid) RestoreRbWindow(i);
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == ID_ANIM_TIMER) {
            bool anyActive = false;
            for (auto& a : g_animCtrls) {
                float hoverTarget = a.hovering ? 1.f : 0.f;
                bool changed = StepTowards(a.hover, hoverTarget, 0.25f);
                if (a.isCheckbox) {
                    float checkTarget = GetCheckedStateFor(a.id) ? 1.f : 0.f;
                    changed = StepTowards(a.check, checkTarget, 0.28f) || changed;
                }
                if (changed) { InvalidateRect(a.hwnd, nullptr, FALSE); anyActive = true; }
            }
            if (!anyActive) { KillTimer(hwnd, ID_ANIM_TIMER); g_animTimerActive = false; }
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_CLOSE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (g_animTimerActive) { KillTimer(hwnd, ID_ANIM_TIMER); g_animTimerActive = false; }
        if (g_trayIconVisible) {
            NOTIFYICONDATAW nid = {}; nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd; nid.uID = TRAY_UID_MAIN;
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        for (size_t i = 0; i < g_rbEntries.size(); i++) {
            if (g_rbEntries[i].used) {
                NOTIFYICONDATAW nid = {}; nid.cbSize = sizeof(nid);
                nid.hWnd = hwnd; nid.uID = g_rbEntries[i].uid;
                Shell_NotifyIconW(NIM_DELETE, &nid);
                if (IsWindow(g_rbEntries[i].hwnd)) ShowWindow(g_rbEntries[i].hwnd, SW_SHOW);
            }
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInstance;

    bool startMinimized = wcsstr(GetCommandLineW(), L"--minimized") != nullptr;

    g_msgShowWindow = RegisterWindowMessageW(kMsgShowName);
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, g_msgShowWindow, 0, 0);
        return 0;
    }

    LoadConfig();
    EnsureAutostartCommandUpToDate();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    g_hBrushBg = CreateSolidBrush(kColBg);
    g_hBrushBg2 = CreateSolidBrush(kColBg2);
    g_hBrushBtnHover = CreateSolidBrush(kColBtnHover);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hBrushBg;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    RECT rc = {0, 0, 540, 420};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, FALSE);
    DWORD winStyle = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME);
    if (!startMinimized) winStyle |= WS_VISIBLE;
    g_hMainWnd = CreateWindowExW(0, kClassName, kAppTitle, winStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    CenterWindowOnScreen(g_hMainWnd);

    UpdateMainTrayIcon();
    if (g_keepAwake) StartKeepAwake();

    g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);

    if (!startMinimized) {
        AnimateWindow(g_hMainWnd, 180, AW_BLEND);
        ShowWindow(g_hMainWnd, SW_SHOW);
        UpdateWindow(g_hMainWnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hKeyboardHook) UnhookWindowsHookEx(g_hKeyboardHook);
    if (g_hMouseHook) UnhookWindowsHookEx(g_hMouseHook);
    StopKeepAwake();
    DeleteObject(g_hBrushBg);
    DeleteObject(g_hBrushBg2);
    DeleteObject(g_hBrushBtnHover);
    if (g_hFont) DeleteObject(g_hFont);
    if (g_hFontBold) DeleteObject(g_hFontBold);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return (int)msg.wParam;
}