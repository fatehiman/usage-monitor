// Thin WinHTTP wrapper. One HttpSession = one cookie jar (used for one service run).
#pragma once
#include <string>
#include <vector>
#include <utility>

struct HttpReq {
    std::string method = "GET";
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutSec = 20;
    size_t maxBody = 2 * 1024 * 1024;
    bool followRedirects = true;
};

struct HttpResp {
    int status = 0;
    std::string body;
    std::string headers;  // raw, CRLF separated
    std::string error;
};

class HttpSession {
public:
    // proxy: "system", "none" or "host:port"
    HttpSession(const std::string& userAgent, const std::string& proxy);
    ~HttpSession();
    bool send(const HttpReq& rq, HttpResp& rs);
private:
    void* h_ = nullptr;
};
