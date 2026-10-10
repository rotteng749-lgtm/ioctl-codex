#!/system/bin/sh
# ioctl-codex — scripts/extract-frida.sh
#
# Installs a Frida gadget .so into the module's libs/ tree so the "Frida JS
# runner" capability can dlopen it. The framework ships WITHOUT a gadget (it is
# a ~40 MB binary and is GPL-ish in licensing terms), so the user supplies one:
#
#   * preferred:  a frida-gadget-<version>-android-<abi>.so from
#                 https://github.com/frida/frida/releases
#   * optional:   a .xz / .gz compressed copy of the same file
#
# Usage (as root, e.g. from a KernelSU shell or `su -c`):
#   sh /data/adb/modules/ioctl-codex/scripts/extract-frida.sh /sdcard/gadget.so.xz
#   sh /data/adb/modules/ioctl-codex/scripts/extract-frida.sh /sdcard/frida-gadget.so
#
# With no argument it scans /sdcard/Download and /sdcard for a matching file.

set -e

DATA_DIR=/data/adb/ioctl-codex
DEST="$DATA_DIR/libs/libfrida-gadget.so"

# ---------------------------------------------------------------------------
# Figure out the device ABI so we can name the destination correctly and warn
# on an obvious mismatch.
# ---------------------------------------------------------------------------
ABI=""
for p in $(getprop ro.product.cpu.abi) $(getprop ro.product.cpu.abilist | tr ',' ' '); do
    case "$p" in
        arm64-v8a) ABI=arm64; break ;;
        armeabi-v7a|armeabi) [ -z "$ABI" ] && ABI=arm ;;
    esac
done
[ -z "$ABI" ] && ABI=arm64
echo "ioctl-codex: device ABI = $ABI"

SRC="$1"

# ---------------------------------------------------------------------------
# No argument: look for a plausible gadget in the usual drop spots.
# ---------------------------------------------------------------------------
if [ -z "$SRC" ]; then
    for d in /sdcard/Download /sdcard/Downloads /sdcard; do
        [ -d "$d" ] || continue
        # Prefer a file whose name mentions frida and the right arch.
        for f in "$d"/frida-gadget*"$ABI"*.so.xz "$d"/frida-gadget*"$ABI"*.so.gz \
                 "$d"/frida-gadget*"$ABI"*.so "$d"/libfrida-gadget.so \
                 "$d"/frida-gadget*.so.xz "$d"/frida-gadget*.so; do
            if [ -f "$f" ]; then SRC="$f"; break; fi
        done
        [ -n "$SRC" ] && break
    done
fi

if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
    echo "ioctl-codex: no gadget file found." >&2
    echo "  Download one from https://github.com/frida/frida/releases" >&2
    echo "  (e.g. frida-gadget-16.x.x-android-arm64.so.xz) and re-run:" >&2
    echo "    sh $0 /path/to/frida-gadget-*.so.xz" >&2
    exit 1
fi

echo "ioctl-codex: source = $SRC"

# ---------------------------------------------------------------------------
# Decompress if needed. Android guarantees toybox gzip/zcat; xz is only present
# if busybox ships it (and its applets may not be on PATH), so try each tool.
# ---------------------------------------------------------------------------
TMP="$DATA_DIR/libs/.frida.tmp.$$"
unxz_any() {
    # $1 = source, $2 = dest
    if command -v xz >/dev/null 2>&1; then xz -dc "$1" > "$2" && return 0; fi
    if command -v unxz >/dev/null 2>&1; then unxz -c "$1" > "$2" && return 0; fi
    if command -v busybox >/dev/null 2>&1; then busybox xz -dc "$1" > "$2" && return 0; fi
    return 1
}
case "$SRC" in
    *.xz)  echo "  decompressing xz ..."
           if ! unxz_any "$SRC" "$TMP"; then
               echo "ioctl-codex: no xz decompressor available (install busybox xz)" >&2
               exit 1
           fi ;;
    *.gz)  echo "  decompressing gz ...";  gzip -dc "$SRC" > "$TMP" ;;
    *)     cp -f "$SRC" "$TMP" ;;
esac

# ---------------------------------------------------------------------------
# Sanity check: must be a shared object for the right machine.
# ---------------------------------------------------------------------------
if command -v file >/dev/null 2>&1; then
    echo "  $(file "$TMP")"
fi
if ! head -c 4 "$TMP" | od -An -tx1 | grep -q "7f 45 4c 46"; then
    echo "ioctl-codex: $TMP is not an ELF file (bad download?)" >&2
    rm -f "$TMP"
    exit 1
fi

mv -f "$TMP" "$DEST"
chmod 755 "$DEST"
echo "ioctl-codex: installed gadget -> $DEST"
echo "  Open the overlay, go to Settings > Custom Libs, and Reload."
echo "  The Frida tab will then load it and listen on 127.0.0.1:27042"
echo "  (or run \$DATA_DIR/config/script.js if that file exists)."
