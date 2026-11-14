#include "json.h"
#include <vector>
#include <cstdlib>

namespace {

typedef const char* P;

void ws(P& p, P e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
}

bool skipString(P& p, P e) {  // p on opening quote
    p++;
    while (p < e) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"') { p++; return true; }
        p++;
    }
    return false;
}

bool skipValue(P& p, P e) {
    ws(p, e);
    if (p >= e) return false;
    if (*p == '"') return skipString(p, e);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < e) {
            char c = *p;
            if (c == '"') { if (!skipString(p, e)) return false; continue; }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') { if (--depth == 0) { p++; return true; } }
            p++;
        }
        return false;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return true;
}

void putUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
}

unsigned hex4(P p) {
    char b[5] = {p[0], p[1], p[2], p[3], 0};
    return (unsigned)strtoul(b, nullptr, 16);
}

std::string unescape(P b, P e) {  // text between the quotes
    std::string o;
    for (P p = b; p < e; p++) {
        if (*p != '\\' || p + 1 >= e) { o += *p; continue; }
        char c = *++p;
        switch (c) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u':
                if (p + 4 < e) {
                    unsigned cp = hex4(p + 1);
                    p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < e && p[1] == '\\' && p[2] == 'u') {
                        unsigned lo = hex4(p + 3);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                    putUtf8(o, cp);
                }
                break;
            default: o += c;  // \" \\ \/
        }
    }
    return o;
}

std::string valueText(P p, P e) {
    ws(p, e);
    if (p >= e) return "";
    if (*p == '"') {
        P s = p;
        if (!skipString(p, e)) return "";
        return unescape(s + 1, p - 1);
    }
    P s = p;
    if (!skipValue(p, e)) return "";
    std::string raw(s, p);
    return raw == "null" ? "" : raw;
}

// p on '{'. On success p is on the member value.
bool findKey(P& p, P e, const std::string& key) {
    ws(p, e);
    if (p >= e || *p != '{') return false;
    p++;
    for (;;) {
        ws(p, e);
        if (p >= e || *p != '"') return false;
        P ks = p;
        if (!skipString(p, e)) return false;
        std::string k = unescape(ks + 1, p - 1);
        ws(p, e);
        if (p >= e || *p != ':') return false;
        p++;
        ws(p, e);
        if (k == key) return true;
        if (!skipValue(p, e)) return false;
        ws(p, e);
        if (p < e && *p == ',') { p++; continue; }
        return false;
    }
}

// p on '['. Calls fn(elementStart) for each element until fn returns true.
template <class F>
bool eachElement(P& p, P e, F fn) {
    ws(p, e);
    if (p >= e || *p != '[') return false;
    p++;
    for (;;) {
        ws(p, e);
        if (p >= e || *p == ']') return false;
        P el = p;
        if (fn(el)) { p = el; return true; }
        if (!skipValue(p, e)) return false;
        ws(p, e);
        if (p < e && *p == ',') { p++; continue; }
        return false;
    }
}

struct Seg { int kind; std::string a, b; size_t idx; };  // 0 key, 1 index, 2 filter a=b

bool parsePath(const std::string& path, std::vector<Seg>& segs) {
    size_t i = 0, n = path.size();
    while (i < n) {
        if (path[i] == '.') { i++; continue; }
        if (path[i] == '[') {
            size_t j = path.find(']', i);
            if (j == std::string::npos) return false;
            std::string in = path.substr(i + 1, j - i - 1);
            size_t eq = in.find('=');
            if (eq != std::string::npos) segs.push_back({2, in.substr(0, eq), in.substr(eq + 1), 0});
            else segs.push_back({1, "", "", (size_t)strtoul(in.c_str(), nullptr, 10)});
            i = j + 1;
            continue;
        }
        size_t j = i;
        while (j < n && path[j] != '.' && path[j] != '[') j++;
        segs.push_back({0, path.substr(i, j - i), "", 0});
        i = j;
    }
    return true;
}

}  // namespace

bool jsonGet(const std::string& doc, const std::string& path, std::string& out) {
    std::vector<Seg> segs;
    if (!parsePath(path, segs)) return false;
    P p = doc.data(), e = doc.data() + doc.size();
    for (const Seg& s : segs) {
        ws(p, e);
        if (s.kind == 0) {
            if (!findKey(p, e, s.a)) return false;
        } else if (s.kind == 1) {
            size_t i = 0;
            if (!eachElement(p, e, [&](P) { return i++ == s.idx; })) return false;
        } else {
            if (!eachElement(p, e, [&](P el) {
                    P q = el;
                    return findKey(q, e, s.a) && valueText(q, e) == s.b;
                })) return false;
        }
    }
    out = valueText(p, e);
    return true;
}
