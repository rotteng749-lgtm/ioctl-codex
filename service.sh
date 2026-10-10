#!/system/bin/sh
# ioctl-codex — service.sh
#
# Runs as root late in boot (after the framework is up). We use it to:
#   * wait for the boot to complete (so /sdcard is mounted and the media_rw
#     context exists),
#   * create the on-/sdcard output root with the right owner so the root
#     companion's chown is a no-op fast path,
#   * start a tiny log heartbeat so the user can confirm the module is alive.
#
# Keep this non-blocking for boot: the wait loop runs in the background.

MODDIR=${0%/*}
DATA_DIR=/data/adb/ioctl-codex
DUMP_ROOT=/sdcard/panxcz-dumper
LOG="$DATA_DIR/log.txt"

# ---------------------------------------------------------------------------
# Wait for boot completion in the background, then prepare the /sdcard root.
# ---------------------------------------------------------------------------
(
    # Poll sys.boot_completed, up to ~120 s. Do not spin hot.
    i=0
    while [ "$(getprop sys.boot_completed 2>/dev/null)" != "1" ] && [ $i -lt 120 ]; do
        sleep 1
        i=$((i + 1))
    done

    # /sdcard is a symlink to /storage/emulated/0; wait for it to appear.
    j=0
    while [ ! -d /storage/emulated/0 ] && [ $j -lt 30 ]; do
        sleep 1
        j=$((j + 1))
    done

    mkdir -p "$DUMP_ROOT" 2>/dev/null
    # media_rw owns /sdcard; matching it avoids a chown on every publish.
    chown media_rw:media_rw "$DUMP_ROOT" 2>/dev/null
    chmod 0775 "$DUMP_ROOT" 2>/dev/null

    {
        echo "$(date '+%Y-%m-%d %H:%M:%S') ioctl-codex service started"
        echo "  data_dir  = $DATA_DIR"
        echo "  dump_root = $DUMP_ROOT"
        echo "  targets   = $(tr '\n' ' ' < "$DATA_DIR/targets.txt" 2>/dev/null)"
    } >> "$LOG" 2>/dev/null
) &

exit 0
