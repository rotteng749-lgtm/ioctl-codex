// cap_crypto_intercept.cpp — capability 7: crypto key interception.
//
// Captures symmetric keys and IVs at the point they are used, by hooking the
// common OpenSSL/BoringSSL cipher entry points. Keys are dumped (hex) to the
// overlay log so they can be reused to decrypt captured traffic offline.
//   * AES_set_encrypt_key / AES_set_decrypt_key   -> key bytes
//   * EVP_CipherInit_ex                           -> key + iv
//   * EVP_EncryptInit_ex / EVP_DecryptInit_ex     -> key + iv

#include "cap_util.h"

#include <mutex>
#include <string>
#include <vector>

namespace codex {

namespace {

struct evp_cipher_ctx_st;

int (*orig_AES_set_encrypt_key)(const unsigned char *, const int, void *) = nullptr;
int (*orig_AES_set_decrypt_key)(const unsigned char *, const int, void *) = nullptr;

std::mutex g_key_mutex;
std::vector<std::string> g_keys;

std::string to_hex(const unsigned char *p, int n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}

void remember(const std::string &what, const unsigned char *p, int n) {
    if (!p || n <= 0) return;
    std::lock_guard<std::mutex> lk(g_key_mutex);
    g_keys.push_back(what + " = " + to_hex(p, n));
    ilog("crypto", "%s = %s", what.c_str(), to_hex(p, n).c_str());
}

int my_AES_set_encrypt_key(const unsigned char *userKey, const int bits, void *key) {
    if (userKey) remember("AES key(enc)", userKey, bits / 8);
    return orig_AES_set_encrypt_key ? orig_AES_set_encrypt_key(userKey, bits, key) : -1;
}
int my_AES_set_decrypt_key(const unsigned char *userKey, const int bits, void *key) {
    if (userKey) remember("AES key(dec)", userKey, bits / 8);
    return orig_AES_set_decrypt_key ? orig_AES_set_decrypt_key(userKey, bits, key) : -1;
}

// EVP_CipherInit_ex(ctx, type, impl, key, iv, enc)
int (*orig_EVP_CipherInit_ex)(evp_cipher_ctx_st *, const void *, void *,
                              const unsigned char *, const unsigned char *, int) = nullptr;
int my_EVP_CipherInit_ex(evp_cipher_ctx_st *c, const void *type, void *impl,
                         const unsigned char *key, const unsigned char *iv, int enc) {
    if (key) remember("EVP key", key, 32); // best-effort length; many are 16/32
    if (iv)  remember("EVP iv", iv, 16);
    return orig_EVP_CipherInit_ex ? orig_EVP_CipherInit_ex(c, type, impl, key, iv, enc) : -1;
}

} // namespace

std::vector<std::string> crypto_intercept_drain() {
    std::lock_guard<std::mutex> lk(g_key_mutex);
    std::vector<std::string> out = g_keys;
    g_keys.clear();
    return out;
}

void cap_crypto_intercept_apply(const IoctlCodexCtx *ctx) {
    (void)ctx;
    ilog("crypto", "installing AES/EVP key hooks");
    int installed = 0;
    const char *libs[] = {"libcrypto.so", "libssl.so", nullptr};
    for (int i = 0; libs[i]; ++i) {
        if (hook_sym(libs[i], "AES_set_encrypt_key", (void *)my_AES_set_encrypt_key,
                     (void **)&orig_AES_set_encrypt_key) != nullptr) installed++;
        if (hook_sym(libs[i], "AES_set_decrypt_key", (void *)my_AES_set_decrypt_key,
                     (void **)&orig_AES_set_decrypt_key) != nullptr) installed++;
        if (hook_sym(libs[i], "EVP_CipherInit_ex", (void *)my_EVP_CipherInit_ex,
                     (void **)&orig_EVP_CipherInit_ex) != nullptr) installed++;
    }
    ilog("crypto", "installed %d hook(s)", installed);
}

} // namespace codex
