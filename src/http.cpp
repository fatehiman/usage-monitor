#include "http.h"
#include "util.h"
#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 3
#endif

static std::string winErr(const char* what) {
    DWORD e = GetLastError();
    const char* hint = "";
    switch (e) {
        case ERROR_WINHTTP_TIMEOUT: hint = " (timeout)"; break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: hint = " (DNS name not found)"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: hint = " (cannot connect)"; break;
        case ERROR_WINHTTP_SECURE_FAILURE: hint = " (TLS error)"; break;
        case ERROR_WINHTTP_CONNECTION_ERROR: hint = " (connection reset)"; break;
    }
    return std::string(what) + " failed, error " + std::to_string(e) + hint;
}

HttpSession::HttpSession(const std::string& userAgent, const std::string& proxy) {
    std::wstring ua = W(userAgent);
    std::string pl = lower(trim(proxy));
    if (pl.empty() || pl == "system") {
        h_ = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h_)  // older Windows
            h_ = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    } else if (pl == "none") {
        h_ = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
        std::wstring px = W(trim(proxy));
        h_ = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_NAMED_PROXY, px.c_str(), L"<local>", 0);
    }
    if (h_) {
        DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(h_, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof dec);  // fails silently on old Windows
    }
}

HttpSession::~HttpSession() {
    if (h_) WinHttpCloseHandle(h_);
}

bool HttpSession::send(const HttpReq& rq, HttpResp& rs) {
    rs = HttpResp();
    if (!h_) { rs.error = winErr("WinHttpOpen"); return false; }

    std::wstring url = W(rq.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { rs.error = "Bad URL: " + rq.url; return false; }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    int t = rq.timeoutSec * 1000;
    WinHttpSetTimeouts(h_, t, t, t, t);

    HINTERNET con = WinHttpConnect(h_, host.c_str(), uc.nPort, 0);
    if (!con) { rs.error = winErr("Connect"); return false; }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, W(rq.method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { rs.error = winErr("OpenRequest"); WinHttpCloseHandle(con); return false; }

    if (!rq.followRedirects) {
        DWORD v = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &v, sizeof v);
    }
    for (auto& h : rq.headers) {
        std::wstring line = W(h.first + ": " + h.second);
        WinHttpAddRequestHeaders(req, line.c_str(), (DWORD)line.size(), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    bool ok = false;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            rq.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)rq.body.data(),
                            (DWORD)rq.body.size(), (DWORD)rq.body.size(), 0)) {
        rs.error = winErr("Send");
    } else if (!WinHttpReceiveResponse(req, nullptr)) {
        rs.error = winErr("Receive");
    } else {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &sz, WINHTTP_NO_HEADER_INDEX);
        rs.status = (int)code;

        DWORD hl = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hl, WINHTTP_NO_HEADER_INDEX);
        if (hl) {
            std::wstring hb(hl / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &hb[0], &hl, WINHTTP_NO_HEADER_INDEX)) {
                hb.resize(hl / sizeof(wchar_t));
                rs.headers = U(hb);
            }
        }

        ok = true;
        char buf[8192];
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(req, buf, sizeof buf, &got)) { rs.error = winErr("Read"); ok = false; break; }
            if (!got) break;
            size_t room = rq.maxBody - rs.body.size();
            rs.body.append(buf, got < room ? got : room);
            if (rs.body.size() >= rq.maxBody) break;
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    return ok;
}
