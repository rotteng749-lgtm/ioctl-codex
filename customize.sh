#!/system/bin/sh
# ioctl-codex — customize.sh
#
# Runs once, at install time, from the module directory. It:
#   1. detects the active root manager / Zygisk implementation,
#   2. creates the persistent config tree under /data/adb/ioctl-codex,
#   3. copies the shipped .so files into that tree (the Zygisk loader reads
#      them from there, NOT from the module dir, so we survive module updates),
#   4. seeds targets.txt and the WebUI-friendly defaults,
#   5. sets sane permissions.
#
# Everything here must be POSIX sh (the installer runs under busybox sh, not
# bash) and must never fail the whole install because one optional step failed.

SKIPUNZIP=1

ui_print " "
ui_print "  ioctl-codex — universal Zygisk instrumentation framework"
ui_print " "

# ---------------------------------------------------------------------------
# Root-manager detection. We support (in this order of preference):
#   * KernelSU-Next / KernelSU  (KSU env markers)
#   * APatch
#   * Magisk (incl. with Zygisk Next / built-in Zygisk)
# The module itself does not care which one is present; this is only used for
# log messages and for choosing the correct WebUI mechanism.
# ---------------------------------------------------------------------------
ROOT_MANAGER="unknown"
if [ -n "$KSU" ] || [ -d /data/adb/ksu ]; then
    ROOT_MANAGER="kernelsu"
elif [ -n "$APATCH" ] || [ -d /data/adb/ap ]; then
    ROOT_MANAGER="apatch"
elif [ -d /data/adb/magisk ]; then
    ROOT_MANAGER="magisk"
fi
ui_print "- root manager: $ROOT_MANAGER"

# Zygisk provider detection (informational only). We look at the installed
# module ids rather than a fixed path, because providers rename their dirs:
#   Zygisk Next, ReZygisk, Zygisk-LSPosed, or a manager's built-in Zygisk.
ZYG="built-in / unknown"
if [ -d /data/adb/modules/zygisksu ] || [ -d /data/adb/modules/zygisk_next ]; then
    ZYG="Zygisk Next"
elif [ -d /data/adb/modules/rezygisk ]; then
    ZYG="ReZygisk"
elif [ -d /data/adb/modules/zygisk-lsposed ] || ls /data/adb/modules 2>/dev/null | grep -qi lsposed; then
    ZYG="LSPosed-bundled"
fi
ui_print "- Zygisk provider: $ZYG"

# ---------------------------------------------------------------------------
# Unpack the payload. $MODPATH is the module dir; the zip contains the
# prebuilt .so files under libs/<abi>/.
# ---------------------------------------------------------------------------
ui_print "- extracting module payload"
unzip -o "$ZIPFILE" -d "$MODPATH" >/dev/null 2>&1

# Pick the ABI that matches the device. arm64 is primary; armeabi-v7a is the
# fallback for 32-bit-only devices.
if [ "$ARCH" = "arm64" ] || [ "$ARCH" = "arm64-v8a" ]; then
    ABI=arm64-v8a
elif [ "$ARCH" = "arm" ] || [ "$ARCH" = "armeabi-v7a" ]; then
    ABI=armeabi-v7a
else
    ABI=arm64-v8a
fi
ui_print "- device ABI: $ABI"

# ---------------------------------------------------------------------------
# Persistent data tree.
# ---------------------------------------------------------------------------
DATA_DIR=/data/adb/ioctl-codex
mkdir -p "$DATA_DIR/libs"
mkdir -p "$DATA_DIR/config"
mkdir -p "$DATA_DIR/scripts"
touch "$DATA_DIR/targets.txt"

# Stage the prebuilt .so files where the Zygisk module looks for them.
if [ -d "$MODPATH/libs/$ABI" ]; then
    cp -f "$MODPATH/libs/$ABI/"*.so "$DATA_DIR/libs/" 2>/dev/null
fi
# Keep a copy of the zygisk loader inside the module dir (Zygisk loads that one
# directly from the module's own zygisk/ directory — see below).
mkdir -p "$MODPATH/zygisk"
if [ -f "$MODPATH/libs/$ABI/libioctl_codex_zygisk.so" ]; then
    cp -f "$MODPATH/libs/$ABI/libioctl_codex_zygisk.so" "$MODPATH/zygisk/$ABI.so"
fi

# ---------------------------------------------------------------------------
# Embedded Frida gadget. We ship the upstream gadget xz-compressed (it is ~7 MB
# raw) under libs/frida/. We only deploy it for arm64: the upstream release is
# arm64-only and the gadget is optional — the Frida tab reports a clean error if
# it is absent. We decompress with the on-device toybox/busybox `xz` if present,
# else `gzip`/`unxz`; if none exist we leave the .xz in place and tell the user
# to run scripts/extract-frida.sh.
# ---------------------------------------------------------------------------
FRIDA_XZ="$MODPATH/libs/frida/frida-gadget-arm64.so.xz"
FRIDA_OUT="$DATA_DIR/libs/libfrida-gadget.so"
if [ "$ABI" = "arm64-v8a" ] && [ -f "$FRIDA_XZ" ] && [ ! -f "$FRIDA_OUT" ]; then
    # Decompressor chain, most-preferred first. Android guarantees toybox
    # (gzip/zcat) but xz is only present if busybox ships it, and even then its
    # applets are often not on PATH — hence the explicit `busybox xz` fallback.
    ok=0
    if command -v xz >/dev/null 2>&1; then
        xz -d -c "$FRIDA_XZ" > "$FRIDA_OUT" 2>/dev/null && ok=1
    fi
    if [ "$ok" = 0 ] && command -v unxz >/dev/null 2>&1; then
        unxz -c "$FRIDA_XZ" > "$FRIDA_OUT" 2>/dev/null && ok=1
    fi
    if [ "$ok" = 0 ] && command -v busybox >/dev/null 2>&1; then
        busybox xz -d -c "$FRIDA_XZ" > "$FRIDA_OUT" 2>/dev/null && ok=1
    fi
    if [ "$ok" = 1 ] && [ -s "$FRIDA_OUT" ]; then
        chmod 755 "$FRIDA_OUT"
        ui_print "- frida gadget deployed ($(wc -c < "$FRIDA_OUT") bytes)"
    else
        rm -f "$FRIDA_OUT"
        ui_print "! no xz decompressor found — run scripts/extract-frida.sh after boot"
    fi
fi

# ---------------------------------------------------------------------------
# Default configuration.
# ---------------------------------------------------------------------------
if [ ! -f "$DATA_DIR/config/config.json" ]; then
    cat > "$DATA_DIR/config/config.json" <<EOF
{
  "enabled": true,
  "dump_root": "/sdcard/panxcz-dumper",
  "log_to_logcat": true,
  "overlay_scale": 2.0
}
EOF
fi

# A starter Frida script the Frida tab will run if present.
if [ ! -f "$DATA_DIR/config/script.js" ]; then
    cat > "$DATA_DIR/config/script.js" <<'EOF'
// ioctl-codex default Frida script.
// Runs inside the target process once the Frida gadget is loaded.
Java.perform(function () {
    console.log("[ioctl-codex] frida script loaded");
    // Enumerate loaded classes lazily — never do this eagerly in production,
    // it is very slow. Left here as a template.
    // Java.enumerateLoadedClasses({ onMatch: function (n) { console.log(n); }, onComplete: function () {} });
});
EOF
fi

# ---------------------------------------------------------------------------
# Permissions. targets.txt must be writable by the KernelSU WebUI (which runs
# as a shell/root context) and readable by the zygote stage. libs/ must be
# readable+executable by every app uid (the menu .so is dlopen'ed from a staged
# copy, but the source must at least be readable).
# ---------------------------------------------------------------------------
chmod 644 "$DATA_DIR/targets.txt"
chmod 755 "$DATA_DIR" "$DATA_DIR/libs" "$DATA_DIR/config" "$DATA_DIR/scripts"
chmod 755 "$DATA_DIR/libs/"*.so 2>/dev/null
chmod 644 "$DATA_DIR/config/"* 2>/dev/null

# ---------------------------------------------------------------------------
# WebUI. KernelSU-Next serves $MODPATH/webroot over its WebView. The page lets
# the user pick a target app; it writes /data/adb/ioctl-codex/targets.txt via
# the KSU exec bridge.
# ---------------------------------------------------------------------------
if [ -d "$MODPATH/webroot" ]; then
    ui_print "- WebUI bundled (open it from the module card)"
else
    ui_print "! webroot/ missing — the WebUI selector will be unavailable"
fi

ui_print "- installed to $DATA_DIR"
ui_print "- add target packages to $DATA_DIR/targets.txt (or use the WebUI)"
ui_print " "
