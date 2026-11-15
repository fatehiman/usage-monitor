#include "runner.h"
#include "crypto.h"
#include "http.h"
#include "json.h"
#include "util.h"
#include <windows.h>
#include <wincred.h>
#include <map>
#include <regex>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

struct Ctx {
    const Service* s;
    std::map<std::string, std::string> vars;
    std::string err;
};

// ---------- variables ----------

bool readCred(const std::string& target, bool user, std::string& out, std::string& err) {
    PCREDENTIALW cr = nullptr;
    if (!CredReadW(W(target).c_str(), CRED_TYPE_GENERIC, 0, &cr)) {
        err = "Credential not found: " + target + " (save it with tools\\set-secret.ps1)";
        return false;
    }
    if (user) {
        out = cr->UserName ? U(cr->UserName) : "";
    } else if (cr->CredentialBlobSize % 2 == 0) {  // cmdkey stores UTF-16
        out = U(std::wstring((const wchar_t*)cr->CredentialBlob, cr->CredentialBlobSize / 2));
    } else {
        out.assign((const char*)cr->CredentialBlob, cr->CredentialBlobSize);
    }
    SecureZeroMemory(cr->CredentialBlob, cr->CredentialBlobSize);
    CredFree(cr);
    out = trim(out);
    return true;
}

bool resolve(Ctx& c, const std::string& name, std::string& v) {
    if (startsWith(name, "env:")) {
        std::wstring n = W(name.substr(4));
        DWORD len = GetEnvironmentVariableW(n.c_str(), nullptr, 0);
        if (!len) { c.err = "Environment variable not set: " + name.substr(4); return false; }
        std::wstring buf(len, L'\0');
        len = GetEnvironmentVariableW(n.c_str(), &buf[0], len);
        buf.resize(len);
        v = U(buf);
        return true;
    }
    if (startsWith(name, "cred.user:")) return readCred(name.substr(10), true, v, c.err);
    if (startsWith(name, "cred:")) return readCred(name.substr(5), false, v, c.err);
    if (startsWith(name, "file:")) {
        std::string p = expandEnv(name.substr(5));
        if (!readFileUtf8(p, v, 1024 * 1024)) { c.err = "Cannot read file: " + p; return false; }
        v = trim(v);
        return true;
    }
    auto it = c.vars.find(name);
    if (it == c.vars.end()) { c.err = "Unknown variable: " + name; return false; }
    v = it->second;
    return true;
}

bool expand(Ctx& c, const std::string& in, std::string& out) {
    out.clear();
    size_t i = 0;
    for (;;) {
        size_t a = in.find("{{", i);
        if (a == std::string::npos) { out += in.substr(i); return true; }
        out += in.substr(i, a - i);
        size_t b = in.find("}}", a + 2);
        if (b == std::string::npos) { c.err = "Missing }} in: " + in; return false; }
        std::vector<std::string> parts = split(in.substr(a + 2, b - a - 2), '|');
        std::string v;
        if (!resolve(c, trim(parts[0]), v)) return false;
        for (size_t k = 1; k < parts.size(); k++) {
            std::string f = lower(trim(parts[k]));
            if (f == "url") v = urlEncode(v);
            else if (f == "trim") v = trim(v);
            else if (startsWith(f, "or:")) {  // {{a|or:b}} = a, or b when a is empty
                if (trim(v).empty() && !resolve(c, trim(parts[k]).substr(3), v)) return false;
            }
            else { c.err = "Unknown filter: " + f; return false; }
        }
        out += v;
        i = b + 2;
    }
}

// ---------- extractors ----------

bool headerValue(const std::string& headers, const std::string& name, std::string& out) {
    std::string want = lower(name) + ":";
    for (auto& line : split(headers, '\n')) {
        std::string l = trim(line);
        if (startsWith(lower(l), want)) { out = trim(l.substr(want.size())); return true; }
    }
    return false;
}

// Returns 1 found, 0 not found, -1 error (c.err set).
int extract(Ctx& c, const std::string& spec, const std::string& body, int status, const std::string& headers, std::string& out) {
    if (spec == "body") { out = trim(body); return 1; }
    if (spec == "status") { out = std::to_string(status); return 1; }
    if (startsWith(spec, "header:")) return headerValue(headers, trim(spec.substr(7)), out) ? 1 : 0;
    if (startsWith(spec, "json:")) return jsonGet(body, trim(spec.substr(5)), out) ? 1 : 0;
    if (startsWith(spec, "between:")) {
        std::string arg = spec.substr(8);
        size_t bar = arg.find('|');
        if (bar == std::string::npos) { c.err = "between: needs start|end"; return -1; }
        std::string a = arg.substr(0, bar), b = arg.substr(bar + 1);
        size_t i = body.find(a);
        if (i == std::string::npos) return 0;
        i += a.size();
        size_t j = body.find(b, i);
        if (j == std::string::npos) return 0;
        out = trim(body.substr(i, j - i));
        return 1;
    }
    if (startsWith(spec, "regex:")) {
        try {
            std::regex re(spec.substr(6));
            std::smatch m;
            if (!std::regex_search(body, m, re)) return 0;
            out = trim(m.size() > 1 && m[1].matched ? m[1].str() : m[0].str());
            return 1;
        } catch (const std::exception&) {
            c.err = "Bad regex: " + spec.substr(6);
            return -1;
        }
    }
    c.err = "Unknown extractor: " + spec;
    return -1;
}

// Returns 1 pass, 0 fail, -1 error.
int check(Ctx& c, const std::string& spec, const std::string& v) {
    if (spec == "notempty") return v.empty() ? 0 : 1;
    if (startsWith(spec, "oneof:")) {
        for (auto& o : split(spec.substr(6), ','))
            if (lower(trim(o)) == lower(trim(v))) return 1;
        return 0;
    }
    if (startsWith(spec, "regex:")) {
        try {
            return std::regex_search(v, std::regex(spec.substr(6))) ? 1 : 0;
        } catch (const std::exception&) {
            c.err = "Bad regex: " + spec.substr(6);
            return -1;
        }
    }
    c.err = "Unknown check: " + spec;
    return -1;
}

bool statusAllowed(const std::string& expect, int st) {
    if (trim(expect).empty()) return st >= 200 && st < 400;
    for (auto& s : split(expect, ','))
        if (atoi(trim(s).c_str()) == st) return true;
    return false;
}

// ---------- steps ----------

bool runStep(Ctx& c, HttpSession& http, const Section& st) {
    const std::string tag = "step " + st.id + ": ";
    std::string url;
    if (!expand(c, st.get("url"), url)) return false;
    url = trim(url);
    if (url.empty()) { c.err = tag + "url is missing"; return false; }

    std::string body, headers;
    int status = 0;
    if (startsWith(lower(url), "file:")) {
        std::string p = expandEnv(url.substr(5));
        if (!readFileUtf8(p, body, c.s->maxBody)) { c.err = tag + "cannot read " + p; return false; }
        status = 200;
    } else {
        HttpReq rq;
        rq.method = upper(trim(st.get("method", "GET")));
        rq.url = url;
        rq.timeoutSec = c.s->timeoutSec;
        rq.maxBody = c.s->maxBody;
        rq.followRedirects = st.get("follow_redirects", "1") != "0";
        std::string form;
        bool hasType = false;
        for (auto& kv : st.kv) {
            std::string v;
            if (startsWith(kv.key, "header.")) {
                if (!expand(c, kv.val, v)) return false;
                std::string hn = kv.key.substr(7);
                if (hn == "content-type") hasType = true;
                rq.headers.push_back({hn, v});
            } else if (startsWith(kv.key, "form.")) {
                if (!expand(c, kv.val, v)) return false;
                if (!form.empty()) form += '&';
                form += urlEncode(kv.key.substr(5)) + "=" + urlEncode(v);
            }
        }
        if (st.find("body")) {
            if (!expand(c, st.get("body"), rq.body)) return false;
        } else if (!form.empty()) {
            rq.body = form;
            if (!hasType) rq.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        }
        HttpResp rs;
        bool sent = http.send(rq, rs);
        if (c.s->debug)
            logLine(c.s->file + " " + tag + rq.method + " " + url.substr(0, url.find('?')) + " -> " +
                    std::to_string(rs.status) + " " + std::to_string(rs.body.size()) + " bytes" +
                    (rs.error.empty() ? "" : " " + rs.error) + "\r\n" + rs.body.substr(0, 2048));
        if (!sent) { c.err = tag + rs.error; return false; }
        status = rs.status;
        std::string cf;
        if (status == 403 && headerValue(rs.headers, "cf-mitigated", cf)) {
            c.err = tag + "blocked by a Cloudflare browser check (" + cf + ")";
            return false;
        }
        if (!statusAllowed(st.get("expect_status"), status)) {
            const KV* msg = st.find("fail." + std::to_string(status));
            c.err = msg ? msg->val : tag + "HTTP " + std::to_string(status);
            return false;
        }
        body.swap(rs.body);
        headers.swap(rs.headers);
    }

    if (const KV* d = st.find("decrypt")) {
        if (lower(trim(d->val)) != "keep-sessions") { c.err = tag + "unknown decrypt type: " + d->val; return false; }
        std::string pass, plain, derr;
        if (!expand(c, st.get("passphrase"), pass)) return false;
        bool ok = keepSessionsDecrypt(body, pass, plain, derr);
        SecureZeroMemory(&pass[0], pass.size());
        SecureZeroMemory(&body[0], body.size());
        if (!ok) { c.err = tag + derr; return false; }
        body.swap(plain);
    }
    struct Wipe { std::string& s; ~Wipe() { SecureZeroMemory(&s[0], s.size()); } } wipeBody{body};

    for (auto& kv : st.kv) {
        bool opt = startsWith(kv.key, "get?.");
        if (!opt && !startsWith(kv.key, "get.")) continue;
        std::string var = kv.key.substr(opt ? 5 : 4), v;
        int r = extract(c, kv.val, body, status, headers, v);
        if (r < 0) { c.err = tag + c.err; return false; }
        if (r == 0) {
            if (!opt) {
                const KV* msg = st.find("fail.notfound");
                c.err = msg ? msg->val : tag + "value not found: " + var;
                return false;
            }
            v.clear();
        }
        c.vars[var] = v;
    }
    for (auto& kv : st.kv) {
        if (!startsWith(kv.key, "require.")) continue;
        std::string var = kv.key.substr(8), v;
        if (!resolve(c, var, v)) return false;
        int r = check(c, kv.val, v);
        if (r < 0) { c.err = tag + c.err; return false; }
        if (r == 0) {
            c.err = "Blocked: " + var + " = " + (v.empty() ? "(empty)" : v.substr(0, 40));
            return false;
        }
    }
    return true;
}

// ---------- indicators ----------

// First number in the text. Accepts Persian / Arabic-Indic digits and separators.
bool parseNumber(const std::string& in, double& out) {
    std::string s;
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char a = in[i], b = i + 1 < in.size() ? (unsigned char)in[i + 1] : 0;
        if (a == 0xDB && b >= 0xB0 && b <= 0xB9) { s += (char)('0' + b - 0xB0); i++; }       // ۰-۹
        else if (a == 0xD9 && b >= 0xA0 && b <= 0xA9) { s += (char)('0' + b - 0xA0); i++; }  // ٠-٩
        else if (a == 0xD9 && b == 0xAB) { s += '.'; i++; }                                  // ٫
        else if (a == 0xD9 && b == 0xAC) { i++; }                                            // ٬
        else if (a == ',') {}
        else s += (char)a;
    }
    for (size_t i = 0; i < s.size(); i++) {
        bool digit = s[i] >= '0' && s[i] <= '9';
        bool start = digit || ((s[i] == '-' || s[i] == '.') && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9');
        if (start) {
            char* end = nullptr;
            out = strtod(s.c_str() + i, &end);
            return end != s.c_str() + i;
        }
    }
    return false;
}

std::string fmtNum(double v, int dec, bool stripZeros) {
    char b[64];
    snprintf(b, sizeof b, "%.*f", dec, v);
    std::string s = b;
    if (stripZeros && s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

// "1843870.5" -> "1,843,870.5"
std::string addThousands(const std::string& s) {
    size_t start = (!s.empty() && s[0] == '-') ? 1 : 0;
    size_t end = s.find('.');
    if (end == std::string::npos) end = s.size();
    std::string out = s;
    for (size_t i = end; i > start + 3; i -= 3) out.insert(i - 3, ",");
    return out;
}

std::string fmtBytes(double bytes, int dec) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int u = 0;
    double v = bytes;
    while (std::fabs(v) >= 1024 && u < 5) { v /= 1024; u++; }
    return fmtNum(v, dec, true) + " " + units[u];
}

double scaleFactor(const std::string& scale) {
    std::string s = lower(trim(scale));
    if (s == "bytes" || s == "b") return 1;
    if (s == "kb") return 1024.0;
    if (s == "mb") return 1024.0 * 1024;
    if (s == "gb") return 1024.0 * 1024 * 1024;
    if (s == "tb") return 1024.0 * 1024 * 1024 * 1024;
    return 0;
}

// API time -> UTC time_t (0 = not a time). Step 1 of 2: every time becomes UTC here;
// step 2 (shortTime) converts UTC to the display zone once. So no time is converted twice.
//   epoch seconds/ms                      -> already UTC
//   ISO with "Z" or "+03:30" / "-05:00"   -> that offset is used
//   ISO without offset ("2026-11-17 04:00") -> read in the service's timezone (src)
time_t parseTime(const std::string& in, const TimeZone& src) {
    std::string s = trim(in);
    if (s.empty()) return 0;
    if (s.find_first_not_of("0123456789") == std::string::npos) {
        double d = atof(s.c_str());
        return (time_t)(d > 1e12 ? d / 1000 : d);
    }
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%d%*1[T ]%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 5) return 0;
    size_t tpos = s.find_first_of("T ", 8);
    size_t z = s.find_first_of("+-Z", tpos == std::string::npos ? s.size() : tpos);
    if (z == std::string::npos) {  // no offset: wall time of the service zone
        SYSTEMTIME st{};
        st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D;
        st.wHour = (WORD)h; st.wMinute = (WORD)m; st.wSecond = (WORD)sec;
        return fromZone(st, src);
    }
    struct tm tm {};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = sec;
    time_t t = _mkgmtime(&tm);
    if (s[z] != 'Z') {
        int oh = 0, om = 0;
        sscanf(s.c_str() + z + 1, "%d:%d", &oh, &om);
        int off = oh * 3600 + om * 60;
        t -= s[z] == '+' ? off : -off;
    }
    return t;
}

// Reads an indicator key as a number. Returns 1 ok, 0 key missing/empty, -1 error.
int num(Ctx& c, const Section& ind, const std::string& key, double& out) {
    const KV* kv = ind.find(key);
    if (!kv) return 0;
    std::string v;
    if (!expand(c, kv->val, v)) return -1;
    if (trim(v).empty()) return 0;
    if (!parseNumber(v, out)) { c.err = "indicator " + ind.id + ": " + key + " is not a number: '" + v.substr(0, 30) + "'"; return -1; }
    double f;
    std::string fs = ind.get("factor");
    if (!fs.empty() && parseNumber(fs, f)) out *= f;
    return 1;
}

int levelOf(const Section& ind, double v) {
    double t;
    if (parseNumber(ind.get("alert_below", "x"), t) && v <= t) return LV_ALERT;
    if (parseNumber(ind.get("alert_above", "x"), t) && v >= t) return LV_ALERT;
    if (parseNumber(ind.get("warn_below", "x"), t) && v <= t) return LV_WARN;
    if (parseNumber(ind.get("warn_above", "x"), t) && v >= t) return LV_WARN;
    return LV_NORMAL;
}

bool formatIndicator(Ctx& c, const Section& ind, IndVal& out) {
    std::string type = lower(trim(ind.get("type", "value")));
    std::string decS = ind.get("decimals");
    std::string prefix = ind.get("prefix"), suffix = ind.get("suffix"), unit = ind.get("unit");
    double scale = scaleFactor(ind.get("scale"));
    std::string text = ind.get("text");
    std::string reset;
    if (const KV* r = ind.find("reset")) {
        std::string v;
        if (!expand(c, r->val, v)) return false;
        time_t t = parseTime(v, c.s->tz);
        // Display zone: the app zone when set, else the service zone.
        const TimeZone& show = appTimeZone() ? *appTimeZone() : c.s->tz;
        reset = t ? U(shortTime(t, show, true, true)) : trim(v);
    }

    std::string val;
    double remain = 0, used = 0, total = 0, value = 0, pct = 0;
    int hr, hu, ht, hp;
    if ((hr = num(c, ind, "remain", remain)) < 0 || (hu = num(c, ind, "used", used)) < 0 ||
        (ht = num(c, ind, "total", total)) < 0)
        return false;

    if (type == "percent") {
        if ((hp = num(c, ind, "percent", pct)) < 0) return false;
        if (!hp) {
            if (ht && total != 0 && hr) pct = remain * 100 / total;
            else if (ht && total != 0 && hu) pct = used * 100 / total;
            else { c.err = "indicator " + ind.id + ": percent needs 'percent' or remain/used + total"; return false; }
        }
        if (ind.get("invert") == "1") pct = 100 - pct;
        int dec = decS.empty() ? 0 : atoi(decS.c_str());
        val = prefix + fmtNum(pct, dec, false) + "%" + suffix;
        out.num = pct;
        out.post = "%";
        out.level = levelOf(ind, pct);
        out.pie = pct;
    } else if (type == "ratio") {
        if (!ht || (!hr && !hu)) { c.err = "indicator " + ind.id + ": ratio needs total and remain or used"; return false; }
        if (!hr) remain = total - used;
        if (scale > 0) {
            int dec = decS.empty() ? 1 : atoi(decS.c_str());
            val = fmtBytes(remain * scale, dec) + " / " + fmtBytes(total * scale, dec);
            out.num = remain * scale;
            out.post = "B";
        } else {
            int dec = decS.empty() ? 0 : atoi(decS.c_str());
            std::string u = unit.empty() ? "" : " " + unit;
            val = prefix + fmtNum(remain, dec, true) + u + " / " + prefix + fmtNum(total, dec, true) + u + suffix;
            out.num = remain;
            out.pre = prefix;
            out.post = u + suffix;
        }
        out.level = levelOf(ind, total != 0 ? remain * 100 / total : 0);
        if (total != 0) out.pie = remain * 100 / total;
    } else if (type == "value") {
        int hv = num(c, ind, "value", value);
        if (hv < 0) return false;
        if (!hv && ht && hu) { value = total - used; hv = 1; }  // e.g. credits bought - credits used
        if (!hv) { c.err = "indicator " + ind.id + ": value (or total + used) is missing or empty"; return false; }
        if (scale > 0) {
            val = fmtBytes(value * scale, decS.empty() ? 1 : atoi(decS.c_str()));
            out.num = value * scale;
            out.post = "B";
        } else {
            std::string n = fmtNum(value, decS.empty() ? 2 : atoi(decS.c_str()), false);
            if (ind.get("thousands") == "1") n = addThousands(n);
            val = prefix + n + (unit.empty() ? "" : " " + unit) + suffix;
            out.num = value;
            out.pre = prefix;
            out.post = (unit.empty() ? "" : " " + unit) + suffix;
        }
        out.level = levelOf(ind, value);
        double mx;
        if (num(c, ind, "max", mx) == 1 && mx != 0) out.pie = value * 100 / mx;  // pie needs a maximum
    } else if (type == "days") {
        // Days left until a date (plan end / renewal), e.g. "23 days (12/7)".
        const KV* d = ind.find("date");
        std::string v;
        if (!d) { c.err = "indicator " + ind.id + ": days needs 'date'"; return false; }
        if (!expand(c, d->val, v)) return false;
        time_t t = parseTime(v, c.s->tz);
        if (!t) { c.err = "indicator " + ind.id + ": date is not a time: '" + trim(v).substr(0, 30) + "'"; return false; }
        double days = (double)(t - time(nullptr)) / 86400.0;
        if (days < 0) val = "expired";
        else if (days < 1) val = std::to_string((int)(days * 24)) + " h";
        else val = std::to_string((int)days) + ((int)days == 1 ? " day" : " days");
        if (reset.empty()) {
            const TimeZone& show = appTimeZone() ? *appTimeZone() : c.s->tz;
            reset = U(shortTime(t, show, true, true));
        }
        out.num = days;
        out.post = " days";
        out.history = false;  // a countdown, not usage
        out.level = levelOf(ind, days);
        double mx;
        if (num(c, ind, "max", mx) == 1 && mx != 0) out.pie = (days < 0 ? 0 : days) * 100 / mx;  // max = plan length in days
    } else {
        c.err = "indicator " + ind.id + ": unknown type '" + type + "'";
        return false;
    }

    std::string label;
    if (!expand(c, ind.get("label"), label)) return false;
    // Short form: "<value> <label> (<reset>)", e.g. "64% Fable (Tue 4:00)".
    out.text = val + (text.empty() ? "" : " " + text) + (label.empty() ? "" : " " + label) +
               (reset.empty() ? "" : " (" + reset + ")");
    out.id = ind.id;
    out.label = label;
    out.tray = ind.get("tray") == "1";
    double su;
    if (parseNumber(ind.get("spike_used", "x"), su) && su > 0) {
        out.spikeUsed = su;
        out.spikeMinutes = atoi(ind.get("spike_minutes", "1").c_str());
        if (out.spikeMinutes < 1) out.spikeMinutes = 1;
    }
    if (ind.get("pie") == "0") out.pie = -1;
    if (out.pie > 100) out.pie = 100;
    if (out.pie < 0 && out.pie != -1) out.pie = 0;
    return true;
}

}  // namespace

bool runService(const Service& s, Result& r) {
    Ctx c;
    c.s = &s;
    bool ok = true;
    {
        HttpSession http(s.userAgent, s.proxy);
        for (auto& st : s.steps)
            if (!runStep(c, http, st)) { ok = false; break; }
    }
    std::vector<IndVal> inds;
    if (ok) {
        for (auto& ind : s.indicators) {
            if (ind.get("optional") == "1") {  // skip when its main value is empty
                std::string v;
                const char* keys[] = {"percent", "value", "remain", "used", "date"};
                bool empty = true;
                for (const char* k : keys)
                    if (const KV* kv = ind.find(k)) { empty = expand(c, kv->val, v) && trim(v).empty(); break; }
                if (empty) continue;
            }
            IndVal v;
            if (!formatIndicator(c, ind, v)) { ok = false; break; }
            inds.push_back(v);
        }
    }
    // Wipe values read from secrets before the map is freed.
    for (auto& kv : c.vars) SecureZeroMemory(&kv.second[0], kv.second.size());

    if (ok) {
        r.inds.swap(inds);
        r.status = "OK";
        r.ok = true;
        r.stale = false;
        r.updated = time(nullptr);
        logLine(s.file + ": OK");
    } else {
        r.status = c.err.empty() ? "Unknown error" : c.err;
        r.ok = false;
        r.stale = !r.inds.empty();
        logLine(s.file + ": " + r.status);
    }
    return ok;
}
