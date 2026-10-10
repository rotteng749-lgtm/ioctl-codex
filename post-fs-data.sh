#!/system/bin/sh
# ioctl-codex — post-fs-data.sh
#
# Runs as root in the "post-fs-data" stage, i.e. very early: /data is mounted
# but the framework has not started. We only do cheap, idempotent filesystem
# setup here so that the later Zygisk hooks find a ready data tree. We must NOT
# block: any hang here delays boot.
#
# Note: do NOT put anything slow (network, large copies) here. Anything that can
# be deferred belongs in service.sh.

MODDIR=${0%/*}
DATA_DIR=/data/adb/ioctl-codex

# Create the tree if customize.sh somehow did not (e.g. module enabled without
# reinstall, or after a data wipe of the module metadata).
mkdir -p "$DATA_DIR/libs" "$DATA_DIR/config" "$DATA_DIR/scripts"
[ -f "$DATA_DIR/targets.txt" ] || : > "$DATA_DIR/targets.txt"

# The zygote stage runs as the root/zygote uid and reads these directly; the
# target app never reads the originals (it gets a staged copy), so 0755 on the
# dirs + 0644 on the list is enough and is safer than 0777.
chmod 755 "$DATA_DIR" "$DATA_DIR/libs" "$DATA_DIR/config" "$DATA_DIR/scripts" 2>/dev/null
chmod 644 "$DATA_DIR/targets.txt" 2>/dev/null
chmod 644 "$DATA_DIR/config/"* 2>/dev/null

# SELinux: the module is policy-free by default. If the device is enforcing and
# the companion socket move fails, the user can flip permissive just for the
# dump step from the Settings tab. Nothing to do here.
exit 0
