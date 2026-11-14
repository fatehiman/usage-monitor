#include "history.h"
#include "tz.h"
#include "util.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>

static AppSettings g_app;

std::wstring iniPath() { return W(exeDir() + "\\usage-monitor.ini"); }

const AppSettings& appSettings() { return g_app; }

void loadAppSettings() {
    std::wstring ini = iniPath();
    wchar_t buf[128] = L"";
    // Display time zone for all times (reset times, Updated, the report).
    GetPrivateProfileStringW(L"app", L"timezone", L"", buf, 128, ini.c_str());
    std::string name = trim(U(buf)), err;
    TimeZone tz;
    if (name.empty()) {}
    else if (findTimeZone(name, tz, err)) setAppTimeZone(tz);
    else logLine("usage-monitor.ini: " + err + " (times use the service zones)");

    int d = (int)GetPrivateProfileIntW(L"app", L"history_days", 30, ini.c_str());
    g_app.historyDays = d < 1 ? 1 : d > 366 ? 366 : d;

    GetPrivateProfileStringW(L"app", L"week_start", L"sat", buf, 128, ini.c_str());
    std::string ws = lower(trim(U(buf))).substr(0, 3);
    static const char* days[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    for (int i = 0; i < 7; i++)
        if (ws == days[i]) g_app.weekStart = i;

    wchar_t snd[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"app", L"spike_sound", L"", snd, MAX_PATH, ini.c_str());
    g_app.spikeSound = expandEnv(trim(U(snd)));
}

std::string historyDir() { return exeDir() + "\\history"; }

static int dayOf(int y, int m, int d) {
    struct tm t {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    return (int)(_mkgmtime(&t) / 86400);
}

int displayDay(time_t utc) {
    SYSTEMTIME s = toZone(utc, appTimeZone() ? *appTimeZone() : TimeZone());
    return dayOf(s.wYear, s.wMonth, s.wDay);
}

std::string dayName(int day) {
    time_t t = (time_t)day * 86400;
    struct tm g {};
    gmtime_s(&g, &t);
    char b[16];
    snprintf(b, sizeof b, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    return b;
}

static std::string clean(const std::string& s) {  // no tabs / new lines inside a field
    std::string o = s;
    for (auto& c : o) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return o;
}

void historyAppend(const std::string& file, const std::vector<IndVal>& inds, time_t when) {
    if (inds.empty()) return;
    std::string dir = historyDir();
    CreateDirectoryW(W(dir).c_str(), nullptr);
    std::string text;
    for (auto& v : inds) {
        if (!v.history) continue;
        char num[40];
        snprintf(num, sizeof num, "%.10g", v.num);
        text += std::to_string((long long)when) + "\t" + clean(file) + "\t" + clean(v.id) + "\t" + num + "\t" +
                clean(v.pre) + "\t" + clean(v.post) + "\t" + clean(v.label) + "\n";
    }
    if (text.empty()) return;
    std::wstring path = W(dir + "\\" + dayName(displayDay(when)) + ".tsv");
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { logLine("Cannot write history file " + U(path)); return; }
    DWORD w;
    WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr);
    CloseHandle(h);
}

void historyCleanup() {
    int oldest = displayDay(time(nullptr)) - g_app.historyDays;  // keep today and the last N days
    std::string dir = historyDir();
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(W(dir + "\\*.tsv").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        int y = 0, m = 0, d = 0;
        std::string n = U(fd.cFileName);
        if (sscanf(n.c_str(), "%4d-%2d-%2d.tsv", &y, &m, &d) != 3) continue;
        if (dayOf(y, m, d) < oldest) {
            DeleteFileW(W(dir + "\\" + n).c_str());
            logLine("History: deleted old " + n);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}
