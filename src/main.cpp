// Usage Monitor - tray app. Plain Win32, two threads (UI + one worker).
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <ctime>
#include "config.h"
#include "draw.h"
#include "history.h"
#include "report.h"
#include "runner.h"
#include "spike.h"
#include "tz.h"
#include "util.h"

#define WM_APP_TRAY   (WM_APP + 1)
#define WM_APP_UPDATE (WM_APP + 2)  // a result changed
#define WM_APP_RELOAD (WM_APP + 3)  // the service list changed
#define WM_APP_SPIKE  (WM_APP + 4)  // a usage spike was found: play the sound

enum { ID_OPEN = 101, ID_SETTINGS, ID_TOPMOST, ID_EXIT };
enum { TIMER_RESUME = 1, TIMER_SPIN = 2 };
static const wchar_t* kMainClass = L"UsageMonitorMain";
static const wchar_t* kTableClass = L"UsageMonitorTable";
static const wchar_t* kTitle = L"Usage Monitor";
// No minimize box: closing (or a tray click) sends the window to the tray.
static const DWORD kTableStyle = WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX;

static HINSTANCE g_inst;
static HWND g_main, g_table, g_list, g_tip;
static bool g_topmost = false;  // tray menu "Always on top", saved in usage-monitor.ini [window] topmost
static UINT g_msgTaskbarCreated;
static NOTIFYICONDATAW g_nid;
static std::string g_dir;  // services folder

// Shared between worker and UI. Protected by g_lock.
static SRWLOCK g_lock = SRWLOCK_INIT;
static std::vector<Service> g_services;
static std::vector<Result> g_results;
static bool g_refreshAll = false;
static std::vector<std::string> g_refreshFiles;  // services the user asked to refresh

static HANDLE g_stopEv, g_refreshEv, g_worker;

// Table window state (only while it is open).
enum { CC_NORMAL, CC_WARN, CC_ALERT, CC_GREY, CC_GOOD };
struct Cell {
    int color = CC_NORMAL;
    double pie = -1;  // -1 = draw text the normal way
};
static HFONT g_font, g_boldFont;
static HIMAGELIST g_imgs;
static int g_indCols = 1;
static std::vector<std::vector<Cell>> g_cells;  // [row][column]
static std::vector<char> g_rowRunning;
static std::vector<HICON> g_rowIcons;  // service icon per row (drawn by us, with alpha)
static int g_iconSize = 16;
static int g_tipCount = 0;
static std::vector<std::wstring> g_rowTip;  // tooltip text of the Updated cell per row
static std::vector<char> g_rowSpike;        // row shows the "!" spike badge next to the Updated time
static int g_hotRow = -1, g_hotCol = -1;  // button (row, column) under the mouse
static double g_spin = 0;

static int colUpdated() { return 1 + g_indCols; }  // last update time, green = OK, red = error (tooltip)
static int colRefresh() { return 2 + g_indCols; }
static int colReport() { return 3 + g_indCols; }  // opens the usage report (separate process)
static int colCount() { return 4 + g_indCols; }
static bool isButtonCol(int c) { return c == colRefresh() || c == colReport(); }
static bool isIndCol(int c) { return c >= 1 && c <= g_indCols; }

// ---------------------------------------------------------------- worker

static void trimMemory() {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static DWORD WINAPI Worker(LPVOID) {
    std::vector<Service> svcs;
    std::vector<ULONGLONG> due;
    int cleanDay = 0;  // day of the last history cleanup (old log files are deleted once a day)

    auto reload = [&]() {
        svcs = loadServices(g_dir);
        std::vector<Result> nr(svcs.size());
        AcquireSRWLockExclusive(&g_lock);
        for (size_t i = 0; i < svcs.size(); i++)
            for (size_t j = 0; j < g_services.size(); j++)
                if (g_services[j].file == svcs[i].file) { nr[i] = g_results[j]; nr[i].running = false; }
        for (size_t i = 0; i < svcs.size(); i++)
            if (!svcs[i].error.empty()) { nr[i].status = "Config error: " + svcs[i].error; nr[i].ok = false; }
        g_services = svcs;
        g_results.swap(nr);
        ReleaseSRWLockExclusive(&g_lock);
        due.assign(svcs.size(), 0);
        logLine("Loaded " + std::to_string(svcs.size()) + " service(s)");
        PostMessageW(g_main, WM_APP_RELOAD, 0, 0);
    };

    CreateDirectoryW(W(g_dir).c_str(), nullptr);
    reload();
    HANDLE change = FindFirstChangeNotificationW(W(g_dir).c_str(), FALSE,
                                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);

    for (;;) {
        bool ran = false;
        for (size_t i = 0; i < svcs.size(); i++) {
            if (!svcs[i].error.empty() || due[i] > GetTickCount64()) continue;
            if (WaitForSingleObject(g_stopEv, 0) == WAIT_OBJECT_0) goto done;
            Result r;
            AcquireSRWLockExclusive(&g_lock);
            g_results[i].running = true;
            r = g_results[i];
            ReleaseSRWLockExclusive(&g_lock);
            PostMessageW(g_main, WM_APP_UPDATE, 0, 0);

            bool ok = runService(svcs[i], r);
            r.running = false;
            bool spike = false;
            if (ok) {  // usage history: one line per indicator in history\YYYY-MM-DD.tsv
                historyAppend(svcs[i].file, r.inds, r.updated);
                int day = displayDay(time(nullptr));
                if (day != cleanDay) { historyCleanup(); cleanDay = day; }
                spike = spikeCheck(svcs[i], r);
            }
            AcquireSRWLockExclusive(&g_lock);
            if (!spike) {  // keep the badge as it is now (the user may have cleared it during the run)
                r.spikeAt = g_results[i].spikeAt;
                r.spikeText = g_results[i].spikeText;
            }
            g_results[i] = r;
            ReleaseSRWLockExclusive(&g_lock);
            PostMessageW(g_main, WM_APP_UPDATE, 0, 0);
            if (spike) PostMessageW(g_main, WM_APP_SPIKE, 0, 0);

            ULONGLONG interval = (ULONGLONG)svcs[i].intervalMin * 60000;
            if (!ok && interval > 5 * 60000) interval = 5 * 60000;  // retry sooner after an error
            if (r.blocked) interval = 30000;  // paused (wrong exit country): look again soon
            due[i] = GetTickCount64() + interval;
            ran = true;
        }
        if (ran) trimMemory();

        ULONGLONG next = (ULONGLONG)-1, now = GetTickCount64();
        for (size_t i = 0; i < svcs.size(); i++)
            if (svcs[i].error.empty() && due[i] < next) next = due[i];
        DWORD wait = INFINITE;
        if (next != (ULONGLONG)-1) wait = next <= now ? 0 : (DWORD)(next - now > 86400000ULL ? 86400000ULL : next - now);

        HANDLE hs[3] = {g_stopEv, g_refreshEv, change};
        DWORD n = change != INVALID_HANDLE_VALUE ? 3 : 2;
        DWORD w = WaitForMultipleObjects(n, hs, FALSE, wait);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) {
            AcquireSRWLockExclusive(&g_lock);
            for (size_t i = 0; i < svcs.size(); i++) {
                bool want = g_refreshAll;
                for (auto& f : g_refreshFiles) if (f == svcs[i].file) want = true;
                if (want) due[i] = 0;
            }
            g_refreshAll = false;
            g_refreshFiles.clear();
            ReleaseSRWLockExclusive(&g_lock);
        } else if (w == WAIT_OBJECT_0 + 2) {
            if (WaitForSingleObject(g_stopEv, 500) == WAIT_OBJECT_0) break;  // let the editor finish writing
            FindNextChangeNotification(change);
            reload();
        }
    }
done:
    if (change != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change);
    return 0;
}

// row < 0 = all services
static void requestRefresh(int row) {
    AcquireSRWLockExclusive(&g_lock);
    if (row < 0) {
        g_refreshAll = true;
    } else if (row < (int)g_services.size() && g_services[row].error.empty()) {
        g_refreshFiles.push_back(g_services[row].file);
        g_results[row].running = true;  // spin at once, also while it waits in the queue
    }
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_refreshEv);
    PostMessageW(g_main, WM_APP_UPDATE, 0, 0);
}

// The user clicked the "!" badge: the spike is seen, hide it.
static void clearSpike(int row) {
    AcquireSRWLockExclusive(&g_lock);
    if (row >= 0 && row < (int)g_results.size()) {
        g_results[row].spikeAt = 0;
        g_results[row].spikeText.clear();
    }
    ReleaseSRWLockExclusive(&g_lock);
    PostMessageW(g_main, WM_APP_UPDATE, 0, 0);
}

// [app] spike_sound = <file.wav> in usage-monitor.ini, else the Windows "Exclamation" sound.
// winmm.dll is loaded only when a .wav file is set, and only for the moment of the sound.
static void playSpikeSound() {
    std::wstring wav = W(appSettings().spikeSound);
    if (!wav.empty()) {
        typedef BOOL(WINAPI * PlaySoundFn)(LPCWSTR, HMODULE, DWORD);
        if (HMODULE m = LoadLibraryW(L"winmm.dll")) {
            auto play = (PlaySoundFn)(void*)GetProcAddress(m, "PlaySoundW");
            // Synchronous on purpose: the DLL is freed after it. The sound is short.
            bool ok = play && play(wav.c_str(), nullptr, 0x00020000 /*SND_FILENAME*/ | 0x0002 /*SND_NODEFAULT*/);
            FreeLibrary(m);
            if (ok) return;
        }
        logLine("Cannot play spike_sound " + appSettings().spikeSound + ", using the Windows sound");
    }
    MessageBeep(MB_ICONEXCLAMATION);
}

// The report runs in its own short-lived process: "usage-monitor.exe --report <file>".
// It reads the history logs, writes reports\<service>.html, opens the browser and exits,
// so this process never holds the history in memory.
static void openReport(int row) {
    std::string file;
    AcquireSRWLockShared(&g_lock);
    if (row >= 0 && row < (int)g_services.size()) file = g_services[row].file;
    ReleaseSRWLockShared(&g_lock);
    if (file.empty()) return;
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --report \"" + W(file) + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);  // so the browser can come to the front
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        logLine("Cannot start the report process, error " + std::to_string(GetLastError()));
    }
}

// ---------------------------------------------------------------- helpers

static RECT workAreaUnderCursor() {
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcWork;
}

static void openServicesFolder() {
    CreateDirectoryW(W(g_dir).c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", W(g_dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- tray

static HICON loadAppIcon(int cx, int cy) {
    return (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
}

// Gray copy of the app icon (same alpha), shown while fetching is paused by [app] exit_country.
static HICON makeGrayIcon(HICON src) {
    ICONINFO ii{};
    if (!src || !GetIconInfo(src, &ii)) return nullptr;
    HICON out = nullptr;
    BITMAP bm{};
    if (GetObjectW(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<DWORD> px((size_t)bm.bmWidth * bm.bmHeight);
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, ii.hbmColor, 0, bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS)) {
            for (auto& p : px) {
                DWORD a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                DWORD y = (r * 30 + g * 59 + b * 11) / 100;
                y = 70 + y * 120 / 255;  // soft gray, not too dark
                p = (a << 24) | (y << 16) | (y << 8) | y;
            }
            SetDIBits(dc, ii.hbmColor, 0, bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS);
            out = CreateIconIndirect(&ii);
        }
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    return out;
}

static HICON g_iconColor, g_iconGray;
static bool g_paused = false;  // some service is paused: exit IP is not in [app] exit_country

static HICON trayIcon() {
    if (!g_iconColor) g_iconColor = loadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (g_paused && !g_iconGray) g_iconGray = makeGrayIcon(g_iconColor);
    return g_paused && g_iconGray ? g_iconGray : g_iconColor;
}

static void addTray() {
    g_nid = NOTIFYICONDATAW{};
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = trayIcon();
    wcscpy_s(g_nid.szTip, kTitle);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void updateTip() {
    std::wstring tip = kTitle;
    bool paused = false;
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; i < g_services.size(); i++) {
        const Result& r = g_results[i];
        std::string line;
        for (auto& v : r.inds)
            if (v.tray) line += (line.empty() ? "" : ", ") + v.text;
        paused = paused || r.blocked;
        if (r.blocked) line += (line.empty() ? "" : " - ") + std::string("paused");
        else if (!r.ok && r.status != "Waiting") line += (line.empty() ? "" : " - ") + std::string("error");
        if (r.spikeAt) line = "(!) " + line;  // usage spike, not cleared yet
        if (!line.empty()) tip += L"\n" + W(g_services[i].name + ": " + line);
    }
    ReleaseSRWLockShared(&g_lock);
    if (tip.size() > 127) tip = tip.substr(0, 124) + L"...";
    wcscpy_s(g_nid.szTip, tip.c_str());
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    if (paused != g_paused) {  // gray icon while paused
        g_paused = paused;
        g_nid.hIcon = trayIcon();
        g_nid.uFlags |= NIF_ICON;
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void showTrayMenu(int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Open");
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settings");
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : MF_UNCHECKED), ID_TOPMOST, L"Always on top");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    SetMenuDefaultItem(m, ID_OPEN, FALSE);
    SetForegroundWindow(g_main);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------- table window

static int dpiScale(int v) { return MulDiv(v, GetDpiForWindow(g_table), 96); }

static void setFont(UINT dpi) {
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    HFONT f = CreateFontIndirectW(&ncm.lfMessageFont);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)f, TRUE);
    if (g_font) DeleteObject(g_font);
    g_font = f;
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    if (g_boldFont) DeleteObject(g_boldFont);
    g_boldFont = CreateFontIndirectW(&ncm.lfMessageFont);
}

static void setCell(int row, int col, const std::wstring& text) {
    LVITEMW it{};
    it.iSubItem = col;
    it.pszText = (LPWSTR)text.c_str();
    SendMessageW(g_list, LVM_SETITEMTEXTW, row, (LPARAM)&it);
}

static RECT cellRect(int row, int col) {
    RECT r{};
    ListView_GetSubItemRect(g_list, row, col, LVIR_BOUNDS, &r);
    return r;
}

static int pieSize(const RECT& cell) {
    int s = dpiScale(14), h = cell.bottom - cell.top - 4;
    return s < h ? s : h;
}

static int buttonSize(const RECT& cell) {
    int s = dpiScale(20), h = cell.bottom - cell.top - 2;
    return s < h ? s : h;
}

static int badgeSize() { return dpiScale(15); }

// The "!" badge right of the Updated time (the text is left-aligned, so it follows the text width).
static RECT badgeRect(int row) {
    RECT c = cellRect(row, colUpdated());
    wchar_t buf[64] = L"";
    ListView_GetItemText(g_list, row, colUpdated(), buf, 64);
    int s = badgeSize();
    int x = c.left + dpiScale(6) + ListView_GetStringWidth(g_list, buf) + dpiScale(5);
    int y = c.top + ((c.bottom - c.top) - s) / 2;
    return RECT{x, y, x + s, y + s};
}

static RECT buttonRect(int row, int col) {
    RECT c = cellRect(row, col);
    int s = buttonSize(c);
    int x = c.left + ((c.right - c.left) - s) / 2, y = c.top + ((c.bottom - c.top) - s) / 2;
    return RECT{x, y, x + s, y + s};
}

// Three tooltip tools per row: id row*3 = refresh button, row*3+1 = Updated cell,
// row*3+2 = report button (text by callback, so it follows the latest result without rebuilding).
static void updateTips() {
    if (!g_tip) return;
    for (int i = 0; i < g_tipCount; i++) {
        TOOLINFOW ti{sizeof ti};
        ti.hwnd = g_list;
        ti.uId = i;
        SendMessageW(g_tip, TTM_DELTOOL, 0, (LPARAM)&ti);
    }
    int rows = ListView_GetItemCount(g_list);
    g_tipCount = 0;
    for (int i = 0; i < rows; i++) {
        for (int k = 0; k < 3; k++) {
            TOOLINFOW ti{sizeof ti};
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = g_list;
            ti.uId = i * 3 + k;
            ti.rect = k == 0 ? buttonRect(i, colRefresh()) : k == 1 ? cellRect(i, colUpdated()) : buttonRect(i, colReport());
            ti.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        g_tipCount += 3;
    }
}

static bool tipText(NMHDR* nh) {  // TTN_GETDISPINFOW; true when handled
    if (nh->hwndFrom != g_tip || nh->code != TTN_GETDISPINFOW) return false;
    static std::wstring text;
    NMTTDISPINFOW* di = (NMTTDISPINFOW*)nh;
    int row = (int)(di->hdr.idFrom / 3), kind = (int)(di->hdr.idFrom % 3);
    text.clear();
    if (kind != 1) {
        AcquireSRWLockShared(&g_lock);
        if (row < (int)g_services.size())
            text = kind == 0 ? L"Refresh " + W(g_services[row].name) + L" now"
                             : L"Usage report of " + W(g_services[row].name) + L" (opens in the browser)";
        ReleaseSRWLockShared(&g_lock);
    } else if (row < (int)g_rowTip.size()) {
        text = g_rowTip[row];
    }
    di->lpszText = (LPWSTR)text.c_str();
    return true;
}

static void autoSizeColumns() {
    int pad = dpiScale(14);
    HWND hdr = ListView_GetHeader(g_list);
    RECT c0 = cellRect(0, 0);
    int pieExtra = (ListView_GetItemCount(g_list) ? pieSize(c0) : dpiScale(14)) + dpiScale(2);
    for (int c = 0; c < colRefresh(); c++) {
        ListView_SetColumnWidth(g_list, c, LVSCW_AUTOSIZE);
        int w = ListView_GetColumnWidth(g_list, c);
        wchar_t buf[64] = L"";
        HDITEMW hi{};
        hi.mask = HDI_TEXT;
        hi.pszText = buf;
        hi.cchTextMax = 64;
        Header_GetItem(hdr, c, &hi);
        int hw = ListView_GetStringWidth(g_list, buf) + pad;
        if (isIndCol(c)) w += pieExtra;
        if (c == 0) w += g_iconSize + dpiScale(6);
        if (c == colUpdated())
            for (char sp : g_rowSpike)
                if (sp) { w += badgeSize() + dpiScale(6); break; }
        ListView_SetColumnWidth(g_list, c, (w > hw ? w : hw) + pad / 4);
    }
    ListView_SetColumnWidth(g_list, colRefresh(), dpiScale(20) + dpiScale(10));
    ListView_SetColumnWidth(g_list, colReport(), dpiScale(20) + dpiScale(14));
}

// rebuild = new columns/rows (service list changed); otherwise update texts only.
static void fillTable(bool rebuild) {
    if (!g_list) return;
    AcquireSRWLockShared(&g_lock);
    std::vector<Service> svcs = g_services;
    std::vector<Result> res = g_results;
    ReleaseSRWLockShared(&g_lock);

    int maxInds = 1;
    for (auto& s : svcs) if ((int)s.indicators.size() > maxInds) maxInds = (int)s.indicators.size();
    if (!rebuild && (maxInds != g_indCols || ListView_GetItemCount(g_list) != (int)svcs.size())) rebuild = true;

    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    if (rebuild) {
        ListView_DeleteAllItems(g_list);
        while (ListView_DeleteColumn(g_list, 0)) {}
        g_indCols = maxInds;
        std::vector<std::wstring> heads = {L"Service", L"Remained"};
        for (int i = 1; i < g_indCols; i++) heads.push_back(L"");
        heads.push_back(L"Updated");
        heads.push_back(L"");  // refresh buttons
        heads.push_back(L"");  // report buttons
        for (size_t c = 0; c < heads.size(); c++) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = (LPWSTR)heads[c].c_str();
            col.cx = 80;
            col.iSubItem = (int)c;
            SendMessageW(g_list, LVM_INSERTCOLUMNW, c, (LPARAM)&col);
        }

        UINT dpi = GetDpiForWindow(g_table);
        int cx = GetSystemMetricsForDpi(SM_CXSMICON, dpi), cy = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
        g_iconSize = cx;
        // The image list only makes the rows taller (room for the refresh buttons).
        // Icons are drawn by us in customDraw, so alpha edges look right.
        if (g_imgs) ImageList_Destroy(g_imgs);
        int rowH = MulDiv(24, dpi, 96);
        g_imgs = ImageList_Create(1, rowH > cy ? rowH : cy, ILC_COLOR32, 0, 1);
        ListView_SetImageList(g_list, g_imgs, LVSIL_SMALL);
        for (HICON h : g_rowIcons) if (h) DestroyIcon(h);
        g_rowIcons.clear();
        HICON def = loadAppIcon(cx, cy);

        for (size_t i = 0; i < svcs.size(); i++) {
            HICON h = nullptr;
            if (!svcs[i].icon.empty())
                h = (HICON)LoadImageW(nullptr, W(g_dir + "\\" + svcs[i].icon).c_str(), IMAGE_ICON, cx, cy, LR_LOADFROMFILE);
            g_rowIcons.push_back(h ? h : CopyIcon(def));
            std::wstring name = W(svcs[i].name);
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = (int)i;
            it.pszText = (LPWSTR)name.c_str();
            SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        }
        DestroyIcon(def);
    }

    g_cells.assign(svcs.size(), std::vector<Cell>(colCount()));
    g_rowRunning.assign(svcs.size(), 0);
    g_rowTip.assign(svcs.size(), L"");
    g_rowSpike.assign(svcs.size(), 0);
    bool anyRunning = false;
    for (size_t i = 0; i < svcs.size(); i++) {
        const Result& r = res[i];
        int row = (int)i;
        for (int k = 0; k < g_indCols; k++) {
            bool has = k < (int)r.inds.size();
            setCell(row, 1 + k, has ? W(r.inds[k].text) : L"");
            Cell& c = g_cells[i][1 + k];
            c.color = !has ? CC_NORMAL : r.stale ? CC_GREY : r.inds[k].level;
            c.pie = has ? r.inds[k].pie : -1;
        }
        // Updated: time of the last successful run (PC time zone); green = last run OK,
        // red = last run failed (the error is in the tooltip).
        bool failed = !r.ok && r.status != "Waiting";
        std::wstring when = r.updated ? shortTime(r.updated, appTimeZone() ? *appTimeZone() : TimeZone(), false, false) : L"";
        setCell(row, colUpdated(), !when.empty() ? when : failed ? L"Error" : L"-");
        g_cells[i][colUpdated()].color = r.ok ? CC_GOOD : failed ? CC_ALERT : CC_NORMAL;
        if (failed) g_rowTip[i] = W(r.status) + (when.empty() ? L"" : L"\r\nLast good update: " + when);
        else if (r.ok) g_rowTip[i] = L"OK, updated " + when;
        if (r.spikeAt) {
            g_rowSpike[i] = 1;
            std::wstring at = shortTime(r.spikeAt, appTimeZone() ? *appTimeZone() : TimeZone(), true, false);
            g_rowTip[i] = L"Usage spike at " + at + L":\r\n" + W(r.spikeText) + L"\r\nClick to clear.\r\n\r\n" + g_rowTip[i];
        }
        g_rowRunning[i] = r.running;
        anyRunning |= r.running;
    }
    autoSizeColumns();  // texts change length (e.g. "-" -> "22:40")
    updateTips();
    if (anyRunning) SetTimer(g_table, TIMER_SPIN, 60, nullptr);
    else { KillTimer(g_table, TIMER_SPIN); g_spin = 0; }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
}

// ---- remembered window position (usage-monitor.ini next to the exe, section [window])
static bool g_havePos = false, g_posMax = false;
static RECT g_posRect{};

static void loadWindowPos() {
    std::wstring ini = iniPath();
    g_topmost = GetPrivateProfileIntW(L"window", L"topmost", 0, ini.c_str()) != 0;
    int v[4];
    const wchar_t* keys[] = {L"left", L"top", L"right", L"bottom"};
    for (int i = 0; i < 4; i++) {
        v[i] = (int)GetPrivateProfileIntW(L"window", keys[i], -999999, ini.c_str());
        if (v[i] == -999999) return;
    }
    g_posRect = RECT{v[0], v[1], v[2], v[3]};
    g_posMax = GetPrivateProfileIntW(L"window", L"maximized", 0, ini.c_str()) != 0;
    g_havePos = true;
}

static void saveWindowPos() {
    if (!g_table) return;
    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(g_table, &wp);
    g_posMax = wp.showCmd == SW_SHOWMAXIMIZED;
    if (IsZoomed(g_table) || IsIconic(g_table)) g_posRect = wp.rcNormalPosition;  // size before maximize
    else GetWindowRect(g_table, &g_posRect);
    g_havePos = true;
    std::wstring ini = iniPath();
    const wchar_t* keys[] = {L"left", L"top", L"right", L"bottom"};
    LONG v[4] = {g_posRect.left, g_posRect.top, g_posRect.right, g_posRect.bottom};
    for (int i = 0; i < 4; i++) WritePrivateProfileStringW(L"window", keys[i], std::to_wstring(v[i]).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"window", L"maximized", g_posMax ? L"1" : L"0", ini.c_str());
}

// Tray menu "Always on top": only a window flag, no extra memory.
static void toggleTopmost() {
    g_topmost = !g_topmost;
    WritePrivateProfileStringW(L"window", L"topmost", g_topmost ? L"1" : L"0", iniPath().c_str());
    if (g_table)
        SetWindowPos(g_table, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Work area of the remembered monitor; false when there is no saved position or
// that monitor is not connected now.
static bool savedWorkArea(RECT& wa) {
    if (!g_havePos) return false;
    HMONITOR m = MonitorFromRect(&g_posRect, MONITOR_DEFAULTTONULL);
    if (!m) return false;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(m, &mi);
    wa = mi.rcWork;
    return true;
}

// Size fits the data each time. Position: the last top-left on the last monitor,
// else centered on the monitor under the mouse.
static void sizeAndPlace() {
    UINT dpi = GetDpiForWindow(g_table);
    int w = 0;
    for (int c = 0; c < colCount(); c++) w += ListView_GetColumnWidth(g_list, c);
    w += GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) + MulDiv(4, dpi, 96);
    int rows = ListView_GetItemCount(g_list);
    if (rows < 3) rows = 3;
    DWORD view = ListView_ApproximateViewRect(g_list, -1, -1, rows);
    int h = HIWORD(view) + MulDiv(8, dpi, 96);

    RECT r{0, 0, w, h};
    AdjustWindowRectExForDpi(&r, kTableStyle, FALSE, 0, dpi);
    RECT wa;
    bool keep = savedWorkArea(wa);
    if (!keep) wa = workAreaUnderCursor();
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int maxW = (wa.right - wa.left) * 9 / 10, maxH = (wa.bottom - wa.top) * 9 / 10;
    if (ww > maxW) ww = maxW;
    if (wh > maxH) wh = maxH;
    if (ww < MulDiv(300, dpi, 96)) ww = MulDiv(300, dpi, 96);
    int x, y;
    if (keep) {  // keep it inside the work area (16 px slack for the invisible resize borders)
        int slack = MulDiv(16, dpi, 96);
        x = g_posRect.left;
        y = g_posRect.top;
        if (x + ww > wa.right + slack) x = wa.right + slack - ww;
        if (y + wh > wa.bottom + slack) y = wa.bottom + slack - wh;
        if (x < wa.left - slack) x = wa.left - slack;
        if (y < wa.top) y = wa.top;
    } else {
        x = wa.left + ((wa.right - wa.left) - ww) / 2;
        y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    }
    SetWindowPos(g_table, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void openSite(int row) {
    std::string site;
    AcquireSRWLockShared(&g_lock);
    if (row >= 0 && row < (int)g_services.size()) site = g_services[row].site;
    ReleaseSRWLockShared(&g_lock);
    if (!site.empty()) ShellExecuteW(nullptr, L"open", W(site).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static COLORREF textColor(int c) {
    switch (c) {
        case CC_WARN: return RGB(196, 110, 0);
        case CC_ALERT: return RGB(200, 30, 30);
        case CC_GREY: return RGB(140, 140, 140);
        case CC_GOOD: return RGB(0x2f, 0x7d, 0x4f);
    }
    return GetSysColor(COLOR_WINDOWTEXT);
}

static COLORREF pieColor(int c) {
    switch (c) {
        case CC_WARN: return RGB(0xE0, 0x9A, 0x2B);
        case CC_ALERT: return RGB(0xD9, 0x4F, 0x4F);
        case CC_GREY: return RGB(0xA6, 0xAA, 0xB3);
    }
    return RGB(0x4C, 0xA8, 0x6E);
}

static void paintIndicatorCell(HDC dc, int row, int col, const Cell& cell) {
    RECT rc = cellRect(row, col);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    int ps = pieSize(rc), x = rc.left + dpiScale(6);
    drawPie(dc, x, rc.top + ((rc.bottom - rc.top) - ps) / 2, ps, cell.pie, pieColor(cell.color), RGB(0xE4, 0xE7, 0xEC));
    wchar_t buf[512] = L"";
    ListView_GetItemText(g_list, row, col, buf, 512);
    RECT tr = rc;
    tr.left = x + ps + dpiScale(2);  // small gap between pie and text
    tr.right -= dpiScale(4);
    HGDIOBJ old = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textColor(cell.color));
    DrawTextW(dc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void paintNameCell(HDC dc, int row) {
    RECT rc = cellRect(row, 0);  // for column 0 this is the whole row
    rc.right = rc.left + ListView_GetColumnWidth(g_list, 0);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    int s = g_iconSize, x = rc.left + dpiScale(4);
    if (row < (int)g_rowIcons.size() && g_rowIcons[row])
        DrawIconEx(dc, x, rc.top + ((rc.bottom - rc.top) - s) / 2, g_rowIcons[row], s, s, 0, nullptr, DI_NORMAL);
    wchar_t buf[256] = L"";
    ListView_GetItemText(g_list, row, 0, buf, 256);
    RECT tr = rc;
    tr.left = x + s + dpiScale(2);  // small gap between icon and name
    tr.right -= dpiScale(4);
    HGDIOBJ old = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    DrawTextW(dc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void paintButtonCell(HDC dc, int row, int col) {
    RECT rc = cellRect(row, col);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    RECT b = buttonRect(row, col);
    bool hot = row == g_hotRow && col == g_hotCol;
    if (col == colReport()) {
        drawReportButton(dc, b.left, b.top, b.right - b.left, hot);
    } else {
        bool running = row < (int)g_rowRunning.size() && g_rowRunning[row];
        drawRefreshButton(dc, b.left, b.top, b.right - b.left, running ? g_spin : 0, hot || running);
    }
}

// Updated time + red "!" badge (usage spike). Only used for rows with a spike.
static void paintUpdatedCell(HDC dc, int row, const Cell& cell) {
    RECT rc = cellRect(row, colUpdated());
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    wchar_t buf[64] = L"";
    ListView_GetItemText(g_list, row, colUpdated(), buf, 64);
    RECT tr = rc;
    tr.left += dpiScale(6);
    HGDIOBJ old = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textColor(cell.color));
    DrawTextW(dc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    RECT b = badgeRect(row);
    drawPie(dc, b.left, b.top, b.right - b.left, 100, RGB(0xD9, 0x3B, 0x3B), GetSysColor(COLOR_WINDOW));
    SelectObject(dc, g_boldFont);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L"!", 1, &b, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, old);
}

// pt in list client coords. Returns the row and sets col (a button column), or -1 = no button there.
static int buttonAt(POINT pt, int& col) {
    LVHITTESTINFO ht{};
    ht.pt = pt;
    col = -1;
    if (ListView_SubItemHitTest(g_list, &ht) < 0 || !isButtonCol(ht.iSubItem)) return -1;
    RECT b = buttonRect(ht.iItem, ht.iSubItem);
    if (!PtInRect(&b, pt)) return -1;
    col = ht.iSubItem;
    return ht.iItem;
}

// Very light grid: a line under each row and at the right edge of each column.
static void paintGrid(HDC dc, int row) {
    static HBRUSH br = CreateSolidBrush(RGB(0xE6, 0xE6, 0xE6));
    RECT rr = cellRect(row, 0);  // whole row
    RECT h{rr.left, rr.bottom - 1, rr.right, rr.bottom};
    FillRect(dc, &h, br);
    int x = rr.left;
    for (int c = 0, n = colCount(); c < n; c++) {
        x += ListView_GetColumnWidth(g_list, c);
        RECT v{x - 1, rr.top, x, rr.bottom};
        FillRect(dc, &v, br);
    }
}

static LRESULT customDraw(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPOSTPAINT: paintGrid(cd->nmcd.hdc, (int)cd->nmcd.dwItemSpec); return CDRF_DODEFAULT;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            int row = (int)cd->nmcd.dwItemSpec, col = cd->iSubItem;
            if (row < 0 || row >= (int)g_cells.size() || col >= (int)g_cells[row].size()) return CDRF_DODEFAULT;
            const Cell& cell = g_cells[row][col];
            if (col == 0) { paintNameCell(cd->nmcd.hdc, row); return CDRF_SKIPDEFAULT; }
            if (isButtonCol(col)) { paintButtonCell(cd->nmcd.hdc, row, col); return CDRF_SKIPDEFAULT; }
            if (isIndCol(col) && cell.pie >= 0) { paintIndicatorCell(cd->nmcd.hdc, row, col, cell); return CDRF_SKIPDEFAULT; }
            if (col == colUpdated() && row < (int)g_rowSpike.size() && g_rowSpike[row]) {
                paintUpdatedCell(cd->nmcd.hdc, row, cell);
                return CDRF_SKIPDEFAULT;
            }
            cd->clrText = textColor(cell.color);
            cd->clrTextBk = GetSysColor(COLOR_WINDOW);
            return CDRF_DODEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

static LRESULT CALLBACK ListSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY && tipText((NMHDR*)lp)) return 0;
    if (msg == WM_MOUSEMOVE) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int col, hot = buttonAt(pt, col);
        if (hot != g_hotRow || col != g_hotCol) {
            int oldRow = g_hotRow, oldCol = g_hotCol;
            g_hotRow = hot;
            g_hotCol = col;
            if (oldRow >= 0) { RECT r = cellRect(oldRow, oldCol); InvalidateRect(hwnd, &r, FALSE); }
            if (hot >= 0) { RECT r = cellRect(hot, col); InvalidateRect(hwnd, &r, FALSE); }
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
    } else if (msg == WM_MOUSELEAVE && g_hotRow >= 0) {
        RECT r = cellRect(g_hotRow, g_hotCol);
        g_hotRow = g_hotCol = -1;
        InvalidateRect(hwnd, &r, FALSE);
    } else if (msg == WM_SETCURSOR && g_hotRow >= 0) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    } else if (msg == WM_LBUTTONDOWN) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int col, row = buttonAt(pt, col);
        if (row >= 0) {  // do not start a selection
            if (col == colReport()) openReport(row);
            else requestRefresh(row);
            return 0;
        }
        LVHITTESTINFO ht{};  // click on an Updated cell with the "!" badge = spike seen
        ht.pt = pt;
        if (ListView_SubItemHitTest(g_list, &ht) >= 0 && ht.iSubItem == colUpdated() &&
            ht.iItem < (int)g_rowSpike.size() && g_rowSpike[ht.iItem]) {
            clearSpike(ht.iItem);
            return 0;
        }
    } else if (msg == WM_LBUTTONDBLCLK) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int col;
        if (buttonAt(pt, col) >= 0) return 0;  // a fast double click on a button is not "open site"
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK TableProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_table = hwnd;
            g_list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                     0, 0, 100, 100, hwnd, (HMENU)1, g_inst, nullptr);
            ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SetWindowSubclass(g_list, ListSubclass, 1, 0);
            g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                    0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
            SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, MulDiv(420, GetDpiForWindow(hwnd), 96));  // wrap long errors
            setFont(GetDpiForWindow(hwnd));
            fillTable(true);
            return 0;
        }
        case WM_SIZE:
            if (g_list) { MoveWindow(g_list, 0, 0, LOWORD(lp), HIWORD(lp), TRUE); updateTips(); }
            return 0;
        case WM_SETFOCUS:
            if (g_list) SetFocus(g_list);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_SPIN) {
                g_spin += 0.35;
                for (int i = 0; i < (int)g_rowRunning.size(); i++)
                    if (g_rowRunning[i]) { RECT r = cellRect(i, colRefresh()); InvalidateRect(g_list, &r, FALSE); }
            }
            return 0;
        case WM_DPICHANGED: {
            setFont(HIWORD(wp));
            fillTable(true);
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nh = (NMHDR*)lp;
            if (tipText(nh)) return 0;
            if (nh->code == HDN_ITEMCHANGEDW || nh->code == HDN_ITEMCHANGEDA) { updateTips(); break; }
            if (nh->hwndFrom != g_list) break;
            switch (nh->code) {
                case NM_CUSTOMDRAW: return customDraw((NMLVCUSTOMDRAW*)lp);
                case LVN_ITEMCHANGING:  // read-only table: no row selection
                    return (((NMLISTVIEW*)lp)->uNewState & LVIS_SELECTED) ? TRUE : FALSE;
                case LVN_ENDSCROLL: updateTips(); return 0;
                case LVN_KEYDOWN: {
                    WORD vk = ((NMLVKEYDOWN*)lp)->wVKey;
                    if (vk == VK_F5) requestRefresh(-1);
                    if (vk == VK_ESCAPE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
                case NM_DBLCLK: openSite(((NMITEMACTIVATE*)lp)->iItem); return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            saveWindowPos();
            KillTimer(hwnd, TIMER_SPIN);
            if (g_list) RemoveWindowSubclass(g_list, ListSubclass, 1);
            if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
            g_tipCount = 0;
            g_hotRow = g_hotCol = -1;
            g_table = nullptr;
            g_list = nullptr;
            g_cells.clear();
            g_cells.shrink_to_fit();
            if (g_imgs) { ImageList_Destroy(g_imgs); g_imgs = nullptr; }
            for (HICON h : g_rowIcons) if (h) DestroyIcon(h);
            g_rowIcons.clear();
            if (g_font) { DeleteObject(g_font); g_font = nullptr; }
            if (g_boldFont) { DeleteObject(g_boldFont); g_boldFont = nullptr; }
            trimMemory();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void openTable() {
    if (g_table) {
        ShowWindow(g_table, SW_RESTORE);
        SetForegroundWindow(g_table);
        return;
    }
    RECT wa;  // create on the right monitor, so the DPI is right
    if (!savedWorkArea(wa)) wa = workAreaUnderCursor();
    // Owned by the hidden main window: an owned window gets no taskbar button.
    CreateWindowExW(g_topmost ? WS_EX_TOPMOST : 0, kTableClass, kTitle, kTableStyle, wa.left, wa.top, 600, 300, g_main, nullptr, g_inst, nullptr);
    if (!g_table) return;
    sizeAndPlace();
    ShowWindow(g_table, g_posMax && savedWorkArea(wa) ? SW_SHOWMAXIMIZED : SW_SHOW);
    SetForegroundWindow(g_table);
}

// ---------------------------------------------------------------- main window (hidden)

static void doExit() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    SetEvent(g_stopEv);
    if (g_table) DestroyWindow(g_table);
    if (g_worker && WaitForSingleObject(g_worker, 3000) != WAIT_OBJECT_0)
        logLine("Worker still busy at exit, leaving it");
    DestroyWindow(g_main);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        addTray();
        updateTip();
        return 0;
    }
    switch (msg) {
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:  // left-click toggles: open -> close to tray, closed/minimized -> open
                    if (g_table && !IsIconic(g_table)) DestroyWindow(g_table);
                    else openTable();
                    break;
                case WM_CONTEXTMENU: showTrayMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); break;
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_OPEN: openTable(); break;
                case ID_SETTINGS: openServicesFolder(); break;
                case ID_TOPMOST: toggleTopmost(); break;
                case ID_EXIT: doExit(); break;
            }
            return 0;
        case WM_APP_UPDATE:
            updateTip();
            fillTable(false);
            return 0;
        case WM_APP_RELOAD:
            updateTip();
            fillTable(true);
            return 0;
        case WM_APP_SPIKE:
            playSpikeSound();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) SetTimer(hwnd, TIMER_RESUME, 30000, nullptr);  // wait for the network
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_RESUME) { KillTimer(hwnd, TIMER_RESUME); requestRefresh(-1); }
            return 0;
        case WM_CLOSE:  // also sent by stop-app.ps1
            doExit();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    {   // "usage-monitor.exe --report <service file>": build the HTML report and exit (no tray, no mutex).
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 3 && lstrcmpiW(argv[1], L"--report") == 0) {
            std::string file = U(argv[2]);
            bool open = !(argc >= 4 && lstrcmpiW(argv[3], L"--no-open") == 0);  // --no-open: only write the file (tests)
            LocalFree(argv);
            return runReport(file, open);
        }
        if (argv) LocalFree(argv);
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\UsageMonitorSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Usage Monitor is already running.", kTitle, MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    g_inst = inst;
    loadWindowPos();
    // usage-monitor.ini [app]: display time zone, history_days, week_start (read at startup).
    loadAppSettings();
    g_dir = exeDir() + "\\services";

    // See res/app.rc: the manifest is resource ID 2, so activate it by hand.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ACTCTXW actx{sizeof actx};
    actx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
    actx.hModule = inst;
    actx.lpResourceName = MAKEINTRESOURCEW(2);
    HANDLE hActx = CreateActCtxW(&actx);
    ULONG_PTR actxCookie = 0;
    if (hActx != INVALID_HANDLE_VALUE) ActivateActCtx(hActx, &actxCookie);

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW tc{sizeof tc};
    tc.lpfnWndProc = TableProc;
    tc.hInstance = inst;
    tc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    tc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    tc.lpszClassName = kTableClass;
    tc.hIcon = loadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    tc.hIconSm = loadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    RegisterClassExW(&tc);

    // Hidden top-level window (not message-only) so WM_CLOSE from stop-app.ps1 reaches us.
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"Usage Monitor (hidden)", WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addTray();

    g_stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_refreshEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    logLine("Started");
    g_worker = CreateThread(nullptr, 4 * 1024 * 1024, Worker, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    logLine("Exit");
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
