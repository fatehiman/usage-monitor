// Decrypts a keep-sessions blob (E:\www\keep-sessions, extension/background.js):
// key = PBKDF2-SHA256(passphrase, salt, iters, 32 bytes); AES-256-GCM, 12-byte IV,
// 16-byte tag at the end of ct. Uses Windows BCrypt only.
#pragma once
#include <string>

// in: the /api/pull answer ({"ok","found","data":{"domain",..,"enc":{...}}}),
// the blob itself, or the "enc" object.
// A blob without "enc" (no passphrase in the extension) is returned unchanged.
// out: the plaintext JSON ({"cookies":[...],"storage":{...}}).
bool keepSessionsDecrypt(const std::string& in, const std::string& passphrase, std::string& out, std::string& err);
