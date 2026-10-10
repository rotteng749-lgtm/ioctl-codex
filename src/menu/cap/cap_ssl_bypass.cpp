// cap_ssl_bypass.cpp — capability 5: SSL pinning bypass.
//
// Neutralises certificate verification at the native layer by hooking the
// BoringSSL/OpenSSL verification entry points with ShadowHook:
//   * SSL_get_verify_result  -> always X509_V_OK (0)
//   * X509_verify_cert       -> always success (1)
//   * SSL_CTX_set_verify     -> forces SSL_VERIFY_NONE
// Hooking at this layer defeats most native pinning implementations and works
// regardless of the Java TrustManager, because the final decision still flows
// through these functions.
//
// We do NOT include openssl headers (the NDK does not ship them); the functions
// are declared with opaque pointer types, which is ABI-correct for these
// signatures and avoids a build-time dependency on libssl.

#include "cap_util.h"

namespace codex {

namespace {

// Opaque BoringSSL/OpenSSL types — we only pass them through.
struct ssl_st;
struct ssl_ctx_st;
struct x509_store_ctx_st;

// X509_V_OK == 0.
long (*orig_SSL_get_verify_result)(const ssl_st *) = nullptr;
int  (*orig_X509_verify_cert)(x509_store_ctx_st *) = nullptr;
void (*orig_SSL_CTX_set_verify)(ssl_ctx_st *, int,
                                int (*)(int, x509_store_ctx_st *)) = nullptr;

long my_SSL_get_verify_result(const ssl_st *) { return 0; }
int  my_X509_verify_cert(x509_store_ctx_st *) { return 1; }          // success
void my_SSL_CTX_set_verify(ssl_ctx_st *ctx, int,
                           int (*cb)(int, x509_store_ctx_st *)) {
    // SSL_VERIFY_NONE == 0
    if (orig_SSL_CTX_set_verify) orig_SSL_CTX_set_verify(ctx, 0, cb);
}

} // namespace

void cap_ssl_bypass_apply(const IoctlCodexCtx *ctx) {
    (void)ctx;
    ilog("ssl_bypass", "installing native SSL verification hooks");

    const char *libs[] = {"libssl.so", "libssl.so.3", "libcrypto.so", nullptr};
    int installed = 0;
    for (int i = 0; libs[i]; ++i) {
        if (hook_sym(libs[i], "SSL_get_verify_result",
                     (void *)my_SSL_get_verify_result,
                     (void **)&orig_SSL_get_verify_result) != nullptr) installed++;
        if (hook_sym(libs[i], "X509_verify_cert",
                     (void *)my_X509_verify_cert,
                     (void **)&orig_X509_verify_cert) != nullptr) installed++;
        if (hook_sym(libs[i], "SSL_CTX_set_verify",
                     (void *)my_SSL_CTX_set_verify,
                     (void **)&orig_SSL_CTX_set_verify) != nullptr) installed++;
    }
    ilog("ssl_bypass", "installed %d hook(s)", installed);
}

} // namespace codex
