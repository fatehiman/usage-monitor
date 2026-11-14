#include "report.h"
#include "config.h"
#include "history.h"
#include "tz.h"
#include "util.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace {

struct Series {
    std::string id, label, pre, post;
    int order = 1000;
    std::string pts;  // JSON "[t,v],[t,v]"
};

std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else if (c == '<') o += "\\u003c";  // never close the <script> by accident
        else o += (char)c;
    }
    return o + "\"";
}

// UTC -> "local epoch" (seconds where a UTC clock shows the display-zone wall time).
// The page then only needs UTC date functions; no time zone logic in JavaScript.
time_t localEpoch(time_t utc) {
    SYSTEMTIME s = toZone(utc, appTimeZone() ? *appTimeZone() : TimeZone());
    struct tm t {};
    t.tm_year = s.wYear - 1900; t.tm_mon = s.wMonth - 1; t.tm_mday = s.wDay;
    t.tm_hour = s.wHour; t.tm_min = s.wMinute; t.tm_sec = s.wSecond;
    return _mkgmtime(&t);
}

bool loadTemplate(std::string& out) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(3), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    const char* p = g ? (const char*)LockResource(g) : nullptr;
    if (!p) return false;
    out.assign(p, SizeofResource(nullptr, r));
    return true;
}

void fail(const std::string& msg) {
    logLine("Report: " + msg);
    MessageBoxW(nullptr, W(msg).c_str(), L"Usage Monitor - report", MB_OK | MB_ICONERROR);
}

}  // namespace

int runReport(const std::string& file, bool openBrowser) {
    if (file.empty() || file.find_first_of("\\/:") != std::string::npos) { fail("Bad service file name: " + file); return 1; }
    loadAppSettings();
    const AppSettings& app = appSettings();
    historyCleanup();

    // Service name and indicator order from its .conf file.
    std::string name = file.substr(0, file.rfind('.'));
    std::map<std::string, int> order;
    for (auto& s : loadServices(exeDir() + "\\services"))
        if (s.file == file) {
            name = s.name;
            for (size_t i = 0; i < s.indicators.size(); i++) order[s.indicators[i].id] = (int)i;
        }

    // Read only the files of the last historyDays days (older ones are not processed).
    time_t now = time(nullptr);
    int today = displayDay(now);
    std::vector<Series> series;
    std::string dir = historyDir();
    for (int day = today - app.historyDays; day <= today; day++) {
        std::string text;
        if (!readFileUtf8(dir + "\\" + dayName(day) + ".tsv", text, 64 * 1024 * 1024)) continue;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f = split(line, '\t');
            if (f.size() < 7 || f[1] != file) continue;
            time_t t = (time_t)_atoi64(f[0].c_str());
            if (!t) continue;
            Series* s = nullptr;
            for (auto& x : series) if (x.id == f[2]) s = &x;
            if (!s) {
                series.push_back(Series());
                s = &series.back();
                s->id = f[2];
                auto it = order.find(f[2]);
                if (it != order.end()) s->order = it->second;
            }
            s->pre = f[4];  // the latest line wins (labels can change, e.g. a model name)
            s->post = f[5];
            if (!f[6].empty()) s->label = f[6];
            char pt[64];
            snprintf(pt, sizeof pt, "[%lld,%s]", (long long)localEpoch(t), f[3].c_str());
            if (!s->pts.empty()) s->pts += ',';
            s->pts += pt;
        }
    }
    std::stable_sort(series.begin(), series.end(), [](const Series& a, const Series& b) { return a.order < b.order; });

    std::string data = "{\"name\":" + jsonStr(name) + ",\"file\":" + jsonStr(file) +
                       ",\"now\":" + std::to_string((long long)localEpoch(now)) +
                       ",\"weekStart\":" + std::to_string(app.weekStart) +
                       ",\"days\":" + std::to_string(app.historyDays) + ",\"series\":[";
    for (size_t i = 0; i < series.size(); i++) {
        const Series& s = series[i];
        data += std::string(i ? "," : "") + "{\"id\":" + jsonStr(s.id) + ",\"label\":" + jsonStr(s.label) +
                ",\"pre\":" + jsonStr(s.pre) + ",\"post\":" + jsonStr(s.post) + ",\"pts\":[" + s.pts + "]}";
    }
    data += "]}";

    std::string html;
    if (!loadTemplate(html)) { fail("The report template is missing in the exe."); return 2; }
    const std::string mark = "/*DATA*/null";
    size_t at = html.find(mark);
    if (at == std::string::npos) { fail("The report template has no /*DATA*/ mark."); return 2; }
    html.replace(at, mark.size(), data);

    std::string outDir = exeDir() + "\\reports";
    CreateDirectoryW(W(outDir).c_str(), nullptr);
    std::string out = outDir + "\\" + file.substr(0, file.rfind('.')) + ".html";
    HANDLE h = CreateFileW(W(out).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { fail("Cannot write " + out); return 3; }
    DWORD w = 0;
    BOOL ok = WriteFile(h, html.data(), (DWORD)html.size(), &w, nullptr);
    CloseHandle(h);
    if (!ok || w != html.size()) { fail("Cannot write " + out); return 3; }

    if (openBrowser && (INT_PTR)ShellExecuteW(nullptr, L"open", W(out).c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32) {
        fail("Cannot open " + out + " in the browser.");
        return 4;
    }
    return 0;
}
