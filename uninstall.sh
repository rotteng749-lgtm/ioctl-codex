#!/system/bin/sh
# ioctl-codex — uninstall.sh
#
# Runs as root when the module is removed. We deliberately KEEP the user's
# dumps (/sdcard/panxcz-dumper) and their custom libs/config — those are the
# user's data, not ours to delete. We only drop the transient staged copies that
# live inside app data dirs, so no per-app cruft is left behind.

DATA_DIR=/data/adb/ioctl-codex

# Remove staged copies inside every app that was ever a target. We scan
# /data/data and /data/user/0 for our "codex" marker dir.
for base in /data/data /data/user/0; do
    [ -d "$base" ] || continue
    for app in "$base"/*; do
        [ -d "$app/codex" ] && rm -rf "$app/codex" 2>/dev/null
    done
done

# The module dir itself is removed by the manager; clean our data dir too but
# keep dumps on /sdcard.
rm -rf "$DATA_DIR" 2>/dev/null

exit 0
