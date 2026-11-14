#include "crypto.h"
#include "json.h"
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <cstdlib>

static bool b64decode(const std::string& s, std::vector<unsigned char>& out) {
    static int map[256];
    static bool init = false;
    if (!init) {
        for (int& m : map) m = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) map[(unsigned char)a[i]] = i;
        map['-'] = 62;  // base64url too
        map['_'] = 63;
        init = true;
    }
    out.clear();
    unsigned buf = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        if (map[c] < 0) return false;
        buf = (buf << 6) | (unsigned)map[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((buf >> bits) & 0xFF));
        }
    }
    return true;
}

bool keepSessionsDecrypt(const std::string& raw, const std::string& pass, std::string& out, std::string& err) {
    // /api/pull answers {"ok":true,"found":true,"data":{ blob }}; unwrap "data".
    std::string in, found;
    if (jsonGet(raw, "found", found) && found == "false") { err = "no such domain on the keep-sessions server"; return false; }
    if (!jsonGet(raw, "data", in) || in.empty() || in[0] != '{') in = raw;
    std::string enc;
    if (!jsonGet(in, "enc", enc) || enc.empty() || enc[0] != '{') {
        std::string probe;
        if (jsonGet(in, "ct", probe)) enc = in;      // the enc object itself
        else { out = in; return true; }              // not encrypted
    }
    std::string s64, iv64, ct64, itersS;
    if (!jsonGet(enc, "salt", s64) || !jsonGet(enc, "iv", iv64) || !jsonGet(enc, "ct", ct64)) {
        err = "encrypted blob has no salt/iv/ct";
        return false;
    }
    unsigned long iters = jsonGet(enc, "iters", itersS) ? strtoul(itersS.c_str(), nullptr, 10) : 150000;
    if (!iters) iters = 150000;
    std::vector<unsigned char> salt, iv, ct;
    if (!b64decode(s64, salt) || !b64decode(iv64, iv) || !b64decode(ct64, ct) || ct.size() < 17) {
        err = "encrypted blob: bad base64";
        return false;
    }

    bool ok = false;
    UCHAR key[32];
    BCRYPT_ALG_HANDLE hmac = nullptr, aes = nullptr;
    BCRYPT_KEY_HANDLE hk = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) ||
        !BCRYPT_SUCCESS(BCryptDeriveKeyPBKDF2(hmac, (PUCHAR)pass.data(), (ULONG)pass.size(), salt.data(), (ULONG)salt.size(),
                                              iters, key, sizeof key, 0))) {
        err = "PBKDF2 failed";
    } else if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0)) ||
               !BCRYPT_SUCCESS(BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                                 sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) ||
               !BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(aes, &hk, nullptr, 0, key, sizeof key, 0))) {
        err = "AES-GCM setup failed";
    } else {
        ULONG n = (ULONG)ct.size() - 16;
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
        BCRYPT_INIT_AUTH_MODE_INFO(ai);
        ai.pbNonce = iv.data();
        ai.cbNonce = (ULONG)iv.size();
        ai.pbTag = ct.data() + n;
        ai.cbTag = 16;
        out.assign(n, '\0');
        ULONG got = 0;
        if (BCRYPT_SUCCESS(BCryptDecrypt(hk, ct.data(), n, &ai, nullptr, 0, (PUCHAR)&out[0], n, &got, 0))) {
            out.resize(got);
            ok = true;
        } else {
            out.clear();
            err = "cannot decrypt keep-sessions blob (wrong passphrase?)";
        }
    }
    SecureZeroMemory(key, sizeof key);
    if (hk) BCryptDestroyKey(hk);
    if (aes) BCryptCloseAlgorithmProvider(aes, 0);
    if (hmac) BCryptCloseAlgorithmProvider(hmac, 0);
    return ok;
}
