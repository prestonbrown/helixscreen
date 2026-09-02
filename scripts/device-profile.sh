#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ask a device which platform profile the INSTALLER would choose for it.
#
# The installer's detection modules are the only implementation of these rules.
# This script ships scripts/lib/installer/{common,host_profile,platform}.sh to
# the device, runs them there, and prints what they decided. It decides nothing
# itself: a wrong answer is a bug in scripts/lib/installer/, and fixing it there
# fixes the mk/cross.mk deploy targets at the same time.
#
# Usage:
#   scripts/device-profile.sh <ssh-target> [ssh-opts...]
#       Query a device. Prints KEY=VALUE lines on stdout, one per answer.
#
#   scripts/device-profile.sh --emit
#       Print the self-contained probe program instead of running it. Used by
#       the tests, and handy for running the probe by hand on a device that
#       cannot be reached over ssh from here.
#
# Output keys:
#   PLATFORM            detect_platform's answer (ad5m, ad5x, k1, pi, ...)
#   MOD_FLAVOR          forge_x | zmod | klipper_mod | stock
#   INSTALL_DIR         where the installer would install
#   INIT_SCRIPT_DEST    the init script it would write (empty on payload hosts)
#   PLATFORM_HOOK_KEY   which assets/config/platform/hooks-<key>.sh applies
#   SERVICE_MECHANISM   systemd | sysv | mod-managed
#
# A key whose answer is empty is still printed, so a caller can tell "the
# installer chose nothing here" from "the probe never ran".

set -u

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
LIB_DIR="$SCRIPT_DIR/lib/installer"

emit_probe() {
    for m in common.sh host_profile.sh platform.sh; do
        if [ ! -f "$LIB_DIR/$m" ]; then
            echo "device-profile: missing $LIB_DIR/$m" >&2
            exit 1
        fi
        cat "$LIB_DIR/$m"
    done

    # These overrides must come AFTER the modules: a shell function is whatever
    # was defined last, so defining them first lets the real implementations
    # replace them -- and the real validate_install_dir ends set_install_paths
    # with `|| exit 1`, so a shadowed stub exits the probe with no output.
    #
    # Silencing the log_* family keeps stdout to KEY=VALUE lines. Neutering the
    # two guards is what makes this a QUERY: host_refuse_mod_owned and
    # validate_install_dir exist to stop an INSTALL touching the wrong place,
    # and we are not installing -- we are asking what it would pick.
    cat <<'PROBE_EOF'

log_info()    { :; }
log_warn()    { :; }
log_error()   { :; }
log_success() { :; }
log_debug()   { :; }
log_step()    { :; }
host_refuse_mod_owned() { :; }
validate_install_dir()  { return 0; }

host_profile_probe >/dev/null 2>&1 || true

_platform=$(detect_platform 2>/dev/null)
# set_install_paths and resolve_platform_hook_key both read AD5M_FIRMWARE as a
# global, exactly as main() sets it up before calling them.
AD5M_FIRMWARE=$(detect_mod_flavor)
MOD_FLAVOR="$AD5M_FIRMWARE"
set_install_paths "$_platform" "$AD5M_FIRMWARE" >/dev/null 2>&1
_hook=$(resolve_platform_hook_key "$_platform")

echo "PLATFORM=$_platform"
echo "MOD_FLAVOR=$MOD_FLAVOR"
echo "INSTALL_DIR=$INSTALL_DIR"
echo "INIT_SCRIPT_DEST=$INIT_SCRIPT_DEST"
echo "PLATFORM_HOOK_KEY=$_hook"
echo "SERVICE_MECHANISM=$HOST_SERVICE_MECHANISM"
PROBE_EOF
}

if [ "${1:-}" = "--emit" ]; then
    emit_probe
    exit 0
fi

if [ $# -lt 1 ]; then
    echo "usage: $0 <ssh-target> [ssh-opts...] | --emit" >&2
    exit 2
fi

SSH_TARGET=$1
shift

# Two steps rather than `... | ssh host sh`: the probe is ~80KB and piping that
# straight into a remote `sh` came back empty from the AD5M. The AD5M also has
# no sftp-server, so scp is not available either -- `cat | ssh 'cat >'` is the
# transfer every AD5M deploy target already uses.
REMOTE_TMP="/tmp/helix-device-profile.$$.sh"

# REMOTE_TMP expanding on the CLIENT side is the intent: $$ is our pid, which is
# what makes the remote filename unique per invocation.
# shellcheck disable=SC2029
if ! emit_probe | ssh "$@" "$SSH_TARGET" "cat > $REMOTE_TMP"; then
    echo "device-profile: could not send the probe to $SSH_TARGET" >&2
    exit 1
fi

# shellcheck disable=SC2029  # REMOTE_TMP is deliberately expanded locally
ssh "$@" "$SSH_TARGET" "sh $REMOTE_TMP; _rc=\$?; rm -f $REMOTE_TMP; exit \$_rc"
