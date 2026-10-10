// ioctl_codex.h — stable C ABI shared by every ioctl-codex component.
//
// This header is the *contract* between:
//   * the Zygisk module  (libioctl_codex_zygisk.so)
//   * the overlay/menu   (libioctl_codex_menu.so)
//   * every custom lib  (e.g. libioctl_codex_dumper.so, user libs in
//                        /data/adb/ioctl-codex/libs/*.so)
//
// It is deliberately plain C, version-tagged, and free of C++/STL types so
// that a custom lib built with a different compiler, NDK, or even a different
// language (Rust/Zig via C ABI) can be loaded at runtime.
//
// ABI RULES (do not break these):
//   1. Only POD structs passed by pointer, and only `int`/`void` returns.
//   2. Never remove or reorder existing fields — only append and bump
//      IOCTL_CODEX_API_VERSION.
//   3. All callbacks are invoked on the render thread unless documented
//      otherwise. They must not block for long.
//
// The host (menu) fills an IoctlCodexCtx and passes a const pointer to the
// custom lib's ioctl_codex_custom_init(). The custom lib MUST copy whatever
// it needs out of the ctx; the ctx is only guaranteed valid for the duration
// of the init call (and of any host callback).

#ifndef IOCTL_CODEX_H
#define IOCTL_CODEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Marks a symbol as part of the stable ABI. Required because the build uses
// -fvisibility=hidden: without this the linker would hide the entry points and
// neither Zygisk nor the custom-lib loader could find them.
#if defined(__GNUC__)
#  define IOCTL_CODEX_EXPORT __attribute__((visibility("default")))
#else
#  define IOCTL_CODEX_EXPORT
#endif

// Bump when the IoctlCodexCtx layout or semantics change incompatibly.
#define IOCTL_CODEX_API_VERSION 1

// Capability bits. The overlay exposes these as independent toggles in the
// "Hooks" tab. A custom lib can read ctx->enabled_caps to know which features
// the user has switched on. Bits are stable forever once assigned.
enum IoctlCodexCapability {
    IOCTL_CODEX_CAP_JAVA_DUMP       = 1u << 0,  // 1. Java introspection dump
    IOCTL_CODEX_CAP_NATIVE_DUMP     = 1u << 1,  // 2. Native module dump
    IOCTL_CODEX_CAP_STRING_DUMP     = 1u << 2,  // 3. String dump
    IOCTL_CODEX_CAP_MEM_SCAN        = 1u << 3,  // 4. Memory scanner + refinement
    IOCTL_CODEX_CAP_SSL_BYPASS      = 1u << 4,  // 5. SSL pinning bypass
    IOCTL_CODEX_CAP_TLS_CAPTURE     = 1u << 5,  // 6. TLS plaintext capture
    IOCTL_CODEX_CAP_CRYPTO_INTERCEPT= 1u << 6,  // 7. Crypto key interception
    IOCTL_CODEX_CAP_STORAGE_DUMP    = 1u << 7,  // 8. Storage dump
    IOCTL_CODEX_CAP_DETECTION_BYPASS= 1u << 8,  // 9. Root/emulator/frida bypass
    IOCTL_CODEX_CAP_FRIDA_RUNNER    = 1u << 9,  // 10. Custom Frida JS runner
    IOCTL_CODEX_CAP_IL2CPP_DUMP     = 1u << 10, // 11. IL2CPP / Unity dumper
};

// Log levels for IoctlCodexCtx.log. The overlay prefixes each line with the
// tag and colour-codes by level in the "Log" tab.
enum IoctlCodexLogLevel {
    IOCTL_CODEX_LOG_DEBUG = 0,
    IOCTL_CODEX_LOG_INFO  = 1,
    IOCTL_CODEX_LOG_WARN  = 2,
    IOCTL_CODEX_LOG_ERROR = 3,
};

// A growable output buffer handed to a custom lib so it can stream dump text
// back to the overlay without allocating across the ABI boundary.
//
// Usage from a custom lib:
//     ioctl_codex_writer_write(w, line, len);
// The host owns the storage; the custom lib must never free w.
typedef struct IoctlCodexWriter {
    void *ctx;   // opaque, host-owned
    // Append `len` bytes. Returns 0 on success, non-zero on failure.
    int (*write)(void *ctx, const char *data, size_t len);
} IoctlCodexWriter;

// The context the host passes into ioctl_codex_custom_init().
//
// `struct_size` is always set by the host to sizeof(IoctlCodexCtx) at the time
// it was compiled. A custom lib built against a newer header MUST check that
// struct_size covers the fields it wants before touching them:
//     if (ctx->struct_size >= offsetof(IoctlCodexCtx, some_new_field) + sizeof(...))
// This is what makes forward/backward compatibility possible without
// versioning every entry point.
typedef struct IoctlCodexCtx {
    // --- versioning -------------------------------------------------------
    int         api_version;    // == IOCTL_CODEX_API_VERSION of the host
    size_t      struct_size;    // sizeof(IoctlCodexCtx) as seen by the host

    // --- target identity --------------------------------------------------
    const char *package_name;   // e.g. "com.example.game"  (never NULL)
    const char *version_name;   // versionName, may be NULL
    int64_t     version_code;   // versionCode, -1 if unknown
    const char *app_data_dir;   // app-private dir, e.g. /data/user/0/<pkg>
    int         target_uid;     // uid of the target process

    // --- logging ----------------------------------------------------------
    // `tag` is a short subsystem label, `msg` is the message. Both are copied
    // synchronously; the custom lib does not need to keep them alive.
    void (*log)(int level, const char *tag, const char *msg);

    // --- capability state -------------------------------------------------
    uint32_t    enabled_caps;   // bitwise-or of IoctlCodexCapability

    // --- host services ----------------------------------------------------
    // Ask the overlay to repaint on the next frame. Cheap and thread-safe.
    void (*request_redraw)(void);

    // Return a writer that appends to the overlay's dump text buffer. `title`
    // is a short label shown as the tab heading. Returns NULL on failure.
    IoctlCodexWriter *(*open_writer)(const char *title);

    // Close a writer obtained from open_writer(). Safe to call with NULL.
    void (*close_writer)(IoctlCodexWriter *w);

    // Absolute path of the dump output root, e.g. /sdcard/panxcz-dumper.
    // Provided so a custom lib can honour the same layout. May be NULL.
    const char *dump_root;

    // Writable staging root inside the app data dir, e.g.
    // /data/data/<pkg>/codex/dump. The sandboxed app CANNOT write /sdcard, so
    // a custom lib must write here; the host then asks the root companion to
    // move the finished tree under dump_root. May be NULL.
    const char *stage_root;

    // Publish a staged directory to persistent storage. `stage_path` is an
    // absolute path under stage_root; `final_rel` is the path relative to
    // dump_root (e.g. "com.foo.bar/1.2.3"). The host asks the root companion to
    // move it and chmod/chown it for /sdcard. Returns 0 on success. If root IPC
    // is unavailable the host leaves the files staged and returns non-zero.
    int (*publish_dir)(const char *stage_path, const char *final_rel);

    // --- extension space (append here, bump api_version) ------------------
    // Reserved for future fields; the host zero-fills them.
    void *reserved[4];
} IoctlCodexCtx;

// ---------------------------------------------------------------------------
// Custom lib entry points. A lib that wants to be loaded from
// /data/adb/ioctl-codex/libs/ must export ioctl_codex_custom_init (and
// optionally ioctl_codex_custom_deinit). Everything else is up to the lib.
// ---------------------------------------------------------------------------

// Called once, on the render thread, right after dlopen succeeds.
// Return 0 to indicate success. A non-zero return is surfaced as a failure in
// the overlay log and the lib is dlclose()d again.
IOCTL_CODEX_EXPORT int ioctl_codex_custom_init(const IoctlCodexCtx *ctx);

// Called before dlclose(), e.g. during hot-swap. Optional (may be absent).
// Must release any resources the lib acquired in init.
IOCTL_CODEX_EXPORT void ioctl_codex_custom_deinit(void);

// ---------------------------------------------------------------------------
// Menu entry points — implemented by libioctl_codex_menu.so and called by the
// Zygisk module after it decides the current process is a target.
// ---------------------------------------------------------------------------

// Bring up the overlay for the given package. Must be called on a thread that
// can block (it starts the render/input loops). Returns 0 on success.
//
// `root_socket_fd` is a Unix socket connected to the module's root companion
// (see module.cpp), or -1 if root IPC is unavailable. The menu uses it for the
// storage bridge (stage in app_data_dir, then ask root to move to /sdcard).
IOCTL_CODEX_EXPORT int ioctl_codex_menu_init(const char *package_name, const char *version_name,
                          int64_t version_code, const char *app_data_dir,
                          int uid, int root_socket_fd);

// Tear the overlay down (used on hot-swap / process teardown).
IOCTL_CODEX_EXPORT void ioctl_codex_menu_shutdown(void);

// ---------------------------------------------------------------------------
// Dumper entry points — implemented by libioctl_codex_dumper.so.
// ---------------------------------------------------------------------------

// Locate the IL2CPP metadata for `package_name`, parse it, and write
// dump.cs / bypass.h / il2cpp.h under <dump_root>/<package>/<version>/.
// Returns 0 on success. Progress is reported through ctx->log and the writer.
IOCTL_CODEX_EXPORT int ioctl_codex_dumper_run(const IoctlCodexCtx *ctx);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // IOCTL_CODEX_H
