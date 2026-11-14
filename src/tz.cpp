#include "tz.h"
#include "util.h"
#include <cstdio>

// Windows 10 1903+ ships ICU as system32\icu.dll (unversioned C exports).
typedef int (*UcalGetWindowsTimeZoneID)(const wchar_t* id, int len, wchar_t* winid, int cap, int* status);

static bool byWindowsKey(const std::wstring& key, DYNAMIC_TIME_ZONE_INFORMATION& out) {
    DYNAMIC_TIME_ZONE_INFORMATION d;
    for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &d) == ERROR_SUCCESS; i++)
        if (_wcsicmp(d.TimeZoneKeyName, key.c_str()) == 0) { out = d; return true; }
    return false;
}

bool findTimeZone(const std::string& name, TimeZone& out, std::string& err) {
    out = TimeZone();
    std::string n = trim(name);
    if (n.empty() || lower(n) == "local") return true;
    std::wstring wn = W(n);
    if (byWindowsKey(wn, out.dtzi)) { out.valid = true; return true; }

    HMODULE icu = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!icu) { err = "timezone: icu.dll not found (needs Windows 10 1903+); use a Windows name like \"FLE Standard Time\""; return false; }
    auto fn = (UcalGetWindowsTimeZoneID)GetProcAddress(icu, "ucal_getWindowsTimeZoneID");
    wchar_t win[128] = L"";
    int status = 0, len = fn ? fn(wn.c_str(), (int)wn.size(), win, 128, &status) : 0;
    FreeLibrary(icu);  // only needed for this lookup
    if (status > 0 || len <= 0) { err = "unknown timezone: " + n; return false; }
    if (!byWindowsKey(std::wstring(win, len), out.dtzi)) { err = "timezone not on this PC: " + n; return false; }
    out.valid = true;
    return true;
}

SYSTEMTIME toZone(time_t utc, const TimeZone& tz) {
    ULARGE_INTEGER u;
    u.QuadPart = ((ULONGLONG)utc + 11644473600ULL) * 10000000ULL;
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME st, local;
    FileTimeToSystemTime(&ft, &st);
    if (tz.valid) SystemTimeToTzSpecificLocalTimeEx(&tz.dtzi, &st, &local);
    else SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
    return local;
}

static int dayNumber(const SYSTEMTIME& s) {  // days since a fixed point, for "same day" tests
    FILETIME ft;
    SYSTEMTIME d = s;
    d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0;
    SystemTimeToFileTime(&d, &ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return (int)(u.QuadPart / (10000000ULL * 86400));
}

std::wstring shortTime(time_t utc, const TimeZone& tz, bool withWeekday, bool roundMinute) {
    static const wchar_t* days[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    SYSTEMTIME t = toZone(roundMinute ? utc + 30 : utc, tz);
    SYSTEMTIME n = toZone(time(nullptr), tz);
    int diff = dayNumber(t) - dayNumber(n);
    wchar_t b[48];
    if (diff == 0) swprintf(b, 48, L"%d:%02d", t.wHour, t.wMinute);
    else if (withWeekday && diff > 0 && diff < 7) swprintf(b, 48, L"%ls %d:%02d", days[t.wDayOfWeek], t.wHour, t.wMinute);
    else swprintf(b, 48, L"%d/%d %d:%02d", t.wMonth, t.wDay, t.wHour, t.wMinute);
    return b;
}

time_t fromZone(const SYSTEMTIME& local, const TimeZone& tz) {
    SYSTEMTIME utc;
    BOOL ok = tz.valid ? TzSpecificLocalTimeToSystemTimeEx(&tz.dtzi, &local, &utc)
                       : TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    if (!ok) return 0;
    FILETIME ft;
    SystemTimeToFileTime(&utc, &ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return (time_t)(u.QuadPart / 10000000ULL - 11644473600ULL);
}

static TimeZone g_appTz;
static bool g_appTzSet = false;

void setAppTimeZone(const TimeZone& tz) {
    g_appTz = tz;
    g_appTzSet = true;
}

const TimeZone* appTimeZone() { return g_appTzSet ? &g_appTz : nullptr; }
