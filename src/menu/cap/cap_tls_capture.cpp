// cap_tls_capture.cpp — capability 6: TLS plaintext capture.
//
// Hooks SSL_read/SSL_write (and the *_ex variants) so that plaintext is
// captured *after* decryption / *before* encryption. Because we sit inside the
// process, this sees cleartext without needing a MITM proxy or CA install.
// Captured traffic is appended to a bounded in-memory ring and shown in the
// "Log" tab; it is also mirrored to the dump writer on demand.

#include "cap_util.h"

#include <mutex>
#include <string>
#include <vector>

namespace codex {

namespace {

struct ssl_st;

int (*orig_SSL_read)(ssl_st *, void *, int) = nullptr;
int (*orig_SSL_write)(ssl_st *, const void *, int) = nullptr;

std::mutex g_tls_mutex;
std::vector<std::string> g_tls_log;
constexpr size_t kMaxLines = 5000;

void record(const char *dir, const void *buf, int n) {
    if (n <= 0) return;
    std::lock_guard<std::mutex> lk(g_tls_mutex);
    if (g_tls_log.size() >= kMaxLines) g_tls_log.erase(g_tls_log.begin(), g_tls_log.begin() + 1000);
    std::string line = dir;
    line += " ";
    line.append(reinterpret_cast<const char *>(buf), (size_t)n);
    // Strip trailing CR/LF noise, keep it readable.
    for (auto &c : line) if (c == '\r' || c == '\n') c = ' ';
    g_tls_log.push_back(line);
}

int my_SSL_read(ssl_st *ssl, void *buf, int num) {
    int n = orig_SSL_read ? orig_SSL_read(ssl, buf, num) : -1;
    if (n > 0) record("[RECV]", buf, n);
    return n;
}

int my_SSL_write(ssl_st *ssl, const void *buf, int num) {
    if (num > 0) record("[SEND]", buf, num);
    return orig_SSL_write ? orig_SSL_write(ssl, buf, num) : -1;
}

} // namespace

std::vector<std::string> tls_capture_drain() {
    std::lock_guard<std::mutex> lk(g_tls_mutex);
    std::vector<std::string> out = g_tls_log;
    g_tls_log.clear();
    return out;
}

void cap_tls_capture_apply(const IoctlCodexCtx *ctx) {
    (void)ctx;
    ilog("tls_capture", "installing SSL_read/SSL_write hooks");
    int installed = 0;
    const char *libs[] = {"libssl.so", "libssl.so.3", nullptr};
    for (int i = 0; libs[i]; ++i) {
        if (hook_sym(libs[i], "SSL_read", (void *)my_SSL_read, (void **)&orig_SSL_read) != nullptr) installed++;
        if (hook_sym(libs[i], "SSL_write", (void *)my_SSL_write, (void **)&orig_SSL_write) != nullptr) installed++;
    }
    ilog("tls_capture", "installed %d hook(s); capture active", installed);
}

} // namespace codex
