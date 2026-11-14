#include "util.h"
#include <windows.h>
#include <cstdio>

std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string U(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return s;
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t i = s.find(sep, start);
        if (i == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, i - start));
        start = i + 1;
    }
    return out;
}

std::string exeDir() {
    std::wstring buf(MAX_PATH * 4, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
    buf.resize(n);
    size_t i = buf.find_last_of(L"\\/");
    return U(i == std::wstring::npos ? buf : buf.substr(0, i));
}

std::string expandEnv(const std::string& s) {
    std::wstring in = W(s);
    DWORD n = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
    if (!n) return s;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(in.c_str(), &out[0], n);
    out.resize(n - 1);
    return U(out);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

bool readFileUtf8(const std::string& path, std::string& out, size_t maxBytes) {
    out.clear();
    HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    size_t n = (size_t)sz.QuadPart;
    if (n > maxBytes) n = maxBytes;
    out.resize(n);
    DWORD got = 0;
    BOOL ok = n ? ReadFile(h, &out[0], (DWORD)n, &got, nullptr) : TRUE;
    CloseHandle(h);
    out.resize(got);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return ok != FALSE;
}

static SRWLOCK g_logLock = SRWLOCK_INIT;

void logLine(const std::string& msg) {
    AcquireSRWLockExclusive(&g_logLock);
    std::wstring path = W(exeDir() + "\\usage-monitor.log");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 256 * 1024)
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char head[32];
        snprintf(head, sizeof head, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::string line = head + msg + "\r\n";
        DWORD w;
        WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}
