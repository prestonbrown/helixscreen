#!/bin/sh /etc/rc.common
# SPDX-License-Identifier: GPL-3.0-or-later
#
# K2 (Tina Linux / procd) init script for ustreamer — a static MJPEG camera
# server that replaces the stock proprietary WebRTC pipeline (which HelixScreen
# and fluidd cannot consume). procd's boot iterator only invokes scripts with
# the `#!/bin/sh /etc/rc.common` shebang AND a DEPEND directive; plain SysV
# scripts are silently skipped at boot (see [L086]).
#
# START=95 / STOP=05 so ustreamer comes up BEFORE helixscreen (START=99) and is
# torn down after it. USE_PROCD=1 + procd_set_param respawn gives a supervised,
# auto-restarting daemon (ustreamer itself has no watchdog).
#
# Installed + enabled by install_camera_k2() in scripts/lib/installer/camera.sh.
# Keep the invocation here in sync with that module's verification probe.

START=95
STOP=05
DEPEND=done
USE_PROCD=1

# --- Editable configuration -------------------------------------------------
# The UVC camera node. Single-stream device — ustreamer needs exclusive access,
# so the stock cam_app/WebRTC pipeline must be stopped first (the installer does
# this and records the disable for reversal on uninstall).
DEVICE="/dev/video0"
# HTTP listen port. fluidd + HelixScreen are pointed at http://<lan-ip>:PORT/.
PORT="8080"
# Capture resolution and frame rate. 640x480@15 is a safe default for the K2's
# UVC camera and keeps CPU/bandwidth modest on the embedded SoC.
RESOLUTION="640x480"
FPS="15"
# ustreamer binary (shipped in the release bundle, installed to INSTALL_DIR/bin).
# install_camera_k2() rewrites this line to the install root it resolved, so an
# install that is not at the default below still finds the binary.
USTREAMER_BIN="/mnt/UDISK/helixscreen/bin/ustreamer"
# ----------------------------------------------------------------------------

start_service() {
    procd_open_instance
    # Reclaim the camera before every launch. The stock cam_app grabber is
    # hotplug-launched by procd on USB enumeration and holds DEVICE exclusively
    # (single-stream node). The installer kills it once at install time, but it
    # returns on every reboot — and re-enumerates ahead of START=95 — so without
    # this, ustreamer loses the race and loops on "CAP: Can't set input channel",
    # serving its NO LIVE VIDEO placeholder. Killing cam_app inside the respawned
    # command (then exec'ing ustreamer in the same PID, so procd's respawn
    # tracking is preserved) makes every boot and every respawn reclaim the node.
    procd_set_param command /bin/sh -c \
        "killall cam_app cam_sub_app 2>/dev/null; \
         exec '$USTREAMER_BIN' --device '$DEVICE' --format MJPEG \
            --resolution '$RESOLUTION' --desired-fps '$FPS' \
            --host 0.0.0.0 --port '$PORT'"
    # Restart automatically if ustreamer dies (it has no built-in watchdog).
    procd_set_param respawn
    procd_set_param stdout 1
    procd_set_param stderr 1
    procd_close_instance
}
