#include "config.h"
#include "util.h"
#include <windows.h>
#include <algorithm>
#include <cstdlib>

// Honest UA. Tested 2026-11-13: Cloudflare (claude.ai) shows a browser challenge to
// WinHTTP when it claims to be Chrome, but lets "UsageMonitor/0.1" through.
static const char* kDefaultUA = "UsageMonitor/0.1 (Windows)";

const KV* Section::find(const std::string& key) const {
    for (auto& k : kv) if (k.key == key) return &k;
    return nullptr;
}

std::string Section::get(const std::string& key, const std::string& def) const {
    const KV* k = find(key);
    return k ? k->val : def;
}

static std::string unquote(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
    return v;
}

static void parseFile(const std::string& path, Service& s) {
    s.name = s.file.substr(0, s.file.rfind('.'));  // shown even when parsing fails
    std::string text;
    if (!readFileUtf8(path, text, 1024 * 1024)) { s.error = "Cannot read file"; return; }

    Section svc;
    Section* cur = nullptr;
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        lineNo++;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            if (line.back() != ']') { s.error = "line " + std::to_string(lineNo) + ": missing ]"; return; }
            std::string name = lower(trim(line.substr(1, line.size() - 2)));
            if (name == "service") cur = &svc;
            else if (startsWith(name, "step.")) { s.steps.push_back({name.substr(5), lineNo, {}}); cur = &s.steps.back(); }
            else if (startsWith(name, "indicator.")) { s.indicators.push_back({name.substr(10), lineNo, {}}); cur = &s.indicators.back(); }
            else { s.error = "line " + std::to_string(lineNo) + ": unknown section [" + name + "]"; return; }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || !cur) { s.error = "line " + std::to_string(lineNo) + ": expected key = value"; return; }
        cur->kv.push_back({lower(trim(line.substr(0, eq))), unquote(trim(line.substr(eq + 1))), lineNo});
    }

    std::string stem = s.file.substr(0, s.file.rfind('.'));
    s.name = svc.get("name", stem);
    s.icon = svc.get("icon");
    s.site = svc.get("site");
    s.userAgent = svc.get("user_agent", kDefaultUA);
    s.proxy = svc.get("proxy", "system");
    s.enabled = svc.get("enabled", "1") != "0";
    s.debug = svc.get("debug", "0") == "1";
    s.order = atoi(svc.get("order", "100").c_str());
    s.intervalMin = std::max(1, atoi(svc.get("interval", "60").c_str()));
    s.timeoutSec = std::max(3, atoi(svc.get("timeout", "20").c_str()));
    s.maxBody = (size_t)std::max(1024.0, atof(svc.get("max_body", "2097152").c_str()));
    std::string tzErr;
    if (!findTimeZone(svc.get("timezone"), s.tz, tzErr)) s.error = tzErr;
    else if (s.steps.empty()) s.error = "no [step.*] section";
    else if (s.indicators.empty()) s.error = "no [indicator.*] section";
}

std::vector<Service> loadServices(const std::string& dir) {
    std::vector<Service> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(W(dir + "\\*.conf").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string name = U(fd.cFileName);
            if (name.empty() || name[0] == '_' || lower(name).size() < 5 || lower(name).substr(name.size() - 5) != ".conf") continue;
            Service s;
            s.file = name;
            parseFile(dir + "\\" + name, s);
            if (!s.error.empty()) logLine(name + ": config error: " + s.error);
            if (s.enabled || !s.error.empty()) out.push_back(std::move(s));
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(out.begin(), out.end(), [](const Service& a, const Service& b) {
        if (a.order != b.order) return a.order < b.order;
        return lower(a.name) < lower(b.name);
    });
    return out;
}
