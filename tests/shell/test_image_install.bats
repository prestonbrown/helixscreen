#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installing into a disk image's root filesystem (--target-root) and finishing
# it on first boot (prestonbrown/helixscreen#1913). The image is a scratch
# directory: a Pi root with a Klipper user, a Moonraker, a dpkg database and,
# where a case needs one, an enabled KlipperScreen. Every way the installer
# could reach the build host's services or the network (systemctl, sudo, apt,
# udevadm, curl, wget, Moonraker, the release CDN) records itself and fails, so
# a case proves a run never asked.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LIB="$WORKTREE_ROOT/scripts/lib/installer"

setup() {
    load helpers
    STUBBIN="$BATS_TEST_TMPDIR/stubbin"
    CALLS="$BATS_TEST_TMPDIR/calls.log"
    mkdir -p "$STUBBIN"
    for c in systemctl sudo apt-get apt apt-cache pkexec udevadm curl wget journalctl; do
        printf '#!/bin/sh\necho "%s $*" >> "%s"\nexit 1\n' "$c" "$CALLS" > "$STUBBIN/$c"
        chmod +x "$STUBBIN/$c"
    done
    # chown is recorded and succeeds: the run is not root, and the ids it
    # hands out are the image's.
    printf '#!/bin/sh\necho "chown $*" >> "%s"\nexit 0\n' "$CALLS" > "$STUBBIN/chown"
    # dpkg-query answers only from the database --admindir names; a query of
    # the build host's own database is a failure.
    cat > "$STUBBIN/dpkg-query" <<STUB
#!/bin/sh
admin=""
for a in "\$@"; do case "\$a" in --admindir=*) admin="\${a#--admindir=}" ;; esac; done
pkg=""
for a in "\$@"; do pkg="\$a"; done
if [ -z "\$admin" ]; then echo "dpkg-query host \$*" >> "$CALLS"; exit 1; fi
grep -qx "Package: \$pkg" "\$admin/status" && printf 'install ok installed'
STUB
    chmod +x "$STUBBIN/chown" "$STUBBIN/dpkg-query"
    PATH="$STUBBIN:$PATH"

    unset INSTALL_DIR TMP_DIR KLIPPER_HOME R2_CHANNEL HELIX_SELF_UPDATE
    export TMPDIR="$BATS_TEST_TMPDIR/tmp"
    mkdir -p "$TMPDIR"
    export HELIX_TTY_DEVICE=/nonexistent HELIX_INSTALL_TTY=0 SUDO=""

    local bats_traps
    bats_traps="$(trap -p EXIT INT TERM HUP)"
    for m in common logo host_profile platform permissions requirements forgex competing_uis desktop printer_seed klipper_include release service audio helix_print_conf moonraker camera remote_screen recovery kiauh image uninstall plan main; do
        . "$LIB/$m.sh"
    done
    trap - ERR EXIT INT TERM HUP
    eval "$bats_traps"

    get_latest_version() { echo "NETWORK get_latest_version" >> "$CALLS"; echo v9.9.9; }
    _url_probe() { echo "NETWORK probe" >> "$CALLS"; echo unknown; }
    _fetch_channel_manifest() { echo "NETWORK manifest" >> "$CALLS"; return 1; }
    download_release() { echo "NETWORK download" >> "$CALLS"; exit 1; }
    restart_moonraker() { echo "NETWORK restart_moonraker" >> "$CALLS"; }
    detect_printer_via_moonraker() { echo "NETWORK detect_printer" >> "$CALLS"; }

    R="$BATS_TEST_TMPDIR/image"
    make_image "$R"
    ARCHIVE="$BATS_TEST_TMPDIR/dl/helixscreen-pi-v1.2.3.tar.gz"
    make_archive "$ARCHIVE"
}

# A MainsailOS-shaped Pi root: user pi (1000) runs Klipper and Moonraker.
make_image() {
    local r=$1
    mkdir -p "$r/etc/systemd/system/multi-user.target.wants" "$r/var/lib/dpkg" \
        "$r/home/pi/printer_data/config" "$r/home/pi/moonraker/moonraker/components/update_manager"
    printf 'root:x:0:0:root:/root:/bin/bash\npi:x:1000:1000::/home/pi:/bin/bash\n' > "$r/etc/passwd"
    printf 'root:x:0:\npi:x:1000:\n' > "$r/etc/group"
    printf '[Service]\nUser=pi\n' > "$r/etc/systemd/system/klipper.service"
    printf '[server]\nhost: 0.0.0.0\n' > "$r/home/pi/printer_data/config/moonraker.conf"
    printf 'klipper\nmoonraker\n' > "$r/home/pi/printer_data/moonraker.asvc"
    printf 'asset_name = config.get("asset_name")\n' \
        > "$r/home/pi/moonraker/moonraker/components/update_manager/net_deploy.py"
    local dep
    for dep in $HELIX_PI_RUNTIME_DEPS; do
        printf 'Package: %s\nStatus: install ok installed\n\n' "$dep" >> "$r/var/lib/dpkg/status"
    done
}

# KlipperScreen installed and enabled, as KIAUH leaves it.
enable_klipperscreen() {
    printf '[Install]\nWantedBy=multi-user.target\n' > "$R/etc/systemd/system/KlipperScreen.service"
    ln -s /etc/systemd/system/KlipperScreen.service "$R/etc/systemd/system/multi-user.target.wants/KlipperScreen.service"
}

# A Pi release in the shipped layout: aarch64 binary, the tracked config/, and
# a bundled install.sh that knows --first-boot.
make_archive() {
    local pkg="$BATS_TEST_TMPDIR/pkg/helixscreen"
    mkdir -p "$pkg/bin" "$pkg/assets" "$pkg/ui_xml" "$(dirname "$1")"
    create_fake_elf "$pkg/bin/helix-screen" 02 b7
    chmod +x "$pkg/bin/helix-screen"
    (cd "$WORKTREE_ROOT" && git ls-files -z config | xargs -0 cp --parents -t "$pkg")
    rm -f "$pkg/config/settings.json"
    mkdir -p "$pkg/assets/config/platform"
    cp "$WORKTREE_ROOT/assets/config/platform/hooks-pi.sh" "$pkg/assets/config/platform/"
    printf '#!/bin/sh\n# handles --first-boot\n' > "$pkg/install.sh"
    printf '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"v1.2.3","asset_name":"helixscreen-pi.zip"}\n' \
        > "$pkg/release_info.json"
    head -c 1100000 /dev/urandom > "$pkg/assets/pad.bin"
    tar -czf "$1" -C "$BATS_TEST_TMPDIR/pkg" helixscreen
}

image_main() { run main --target-root "$R" --platform pi --local "$ARCHIVE" "$@"; }

# A call that reaches a service, a package manager or the network.
reached_out() { grep -qsvE '^chown ' "$CALLS"; }
calls() { cat "$CALLS" 2>/dev/null; }

UNITS=etc/systemd/system
INST=home/pi/helixscreen

@test "an image install lays down the tree, units and updater with the image's own paths, and starts nothing" {
    host_units_before=$(ls -la /etc/systemd/system 2>/dev/null)
    image_main
    [ "$status" -eq 0 ] || fail "install failed ($status): $output"

    [ -x "$R/$INST/bin/helix-screen" ]
    [ -s "$R/$INST/platform/hooks.sh" ]
    unit="$R/$UNITS/helixscreen.service"
    grep -qx 'User=pi' "$unit"
    grep -qx 'Group=pi' "$unit"
    grep -qx 'WorkingDirectory=/home/pi/helixscreen' "$unit"
    grep -qx '# HELIX_PD_HELIX=/home/pi/printer_data/config/helixscreen' "$unit"
    grep -q 'PathChanged=/home/pi/helixscreen/release_info.json' "$R/$UNITS/helixscreen-update.path"
    ph='@@(HELIX_USER|HELIX_GROUP|HELIX_PD_HELIX|INSTALL_DIR|INSTALL_PARENT)@@'
    if grep -rqE "$ph" "$R/$UNITS"; then fail "an unfilled placeholder: $(grep -rnE "$ph" "$R/$UNITS")"; fi
    if grep -rqF "$R" "$R/$UNITS" "$R/home/pi/printer_data"; then
        fail "a build-host path was written into the image: $(grep -rlF "$R" "$R/$UNITS" "$R/home/pi/printer_data")"
    fi

    wants="$R/$UNITS/multi-user.target.wants"
    [ "$(readlink "$wants/helixscreen.service")" = /etc/systemd/system/helixscreen.service ]
    [ "$(readlink "$wants/helixscreen-update.path")" = /etc/systemd/system/helixscreen-update.path ]
    [ "$(readlink "$wants/helixscreen-firstboot.service")" = /etc/systemd/system/helixscreen-firstboot.service ]
    fb="$R/$UNITS/helixscreen-firstboot.service"
    grep -qx 'ConditionPathExists=/home/pi/helixscreen/config/.first_boot_pending' "$fb"
    grep -qx 'ExecStart=/bin/sh /home/pi/helixscreen/install.sh --first-boot' "$fb"
    grep -qx 'Before=helixscreen.service' "$fb"
    [ "$(cat "$R/$INST/config/.first_boot_pending")" = "$(printf 'config-links\npermission-rules\nalsa\nremote-screen\nkiauh')" ]

    conf="$R/home/pi/printer_data/config/moonraker.conf"
    grep -qx '\[update_manager helixscreen\]' "$conf"
    grep -qx 'path: /home/pi/helixscreen' "$conf"
    grep -qx 'helixscreen' "$R/home/pi/printer_data/moonraker.asvc"
    [ "$(cat "$R/$INST/config/.install_source")" = other ]
    grep -qx "chown -Rh 1000:1000 $R/$INST" "$CALLS"

    if reached_out; then fail "the image install reached out: $(calls)"; fi
    [ "$(ls -la /etc/systemd/system 2>/dev/null)" = "$host_units_before" ]
    [ -z "$(ls "$TMPDIR")" ] || fail "scratch left behind: $(ls "$TMPDIR")"
    contains "HelixScreen starts at boot" "$output"
}

@test "an enabled KlipperScreen keeps the display unless --own-display, which the uninstall ledger can undo" {
    enable_klipperscreen
    image_main --source helper-script
    [ "$status" -eq 0 ] || fail "install failed ($status): $output"
    wants="$R/$UNITS/multi-user.target.wants"
    [ -L "$wants/KlipperScreen.service" ]
    [ ! -L "$wants/helixscreen.service" ]
    [ ! -L "$wants/helixscreen-update.path" ]
    [ -L "$wants/helixscreen-firstboot.service" ]
    [ -f "$R/$UNITS/helixscreen.service" ]
    contains "KlipperScreen owns the display" "$output"
    contains "systemctl disable --now KlipperScreen" "$output"
    [ ! -e "$R/$INST/config/.disabled_services" ]
    [ "$(cat "$R/$INST/config/.install_source")" = helper-script ]

    rm -rf "$R"
    make_image "$R"
    enable_klipperscreen
    image_main --own-display
    [ "$status" -eq 0 ] || fail "install failed ($status): $output"
    [ ! -L "$wants/KlipperScreen.service" ]
    [ -f "$R/$UNITS/KlipperScreen.service" ]
    [ -L "$wants/helixscreen.service" ]
    grep -qx 'systemd:KlipperScreen' "$R/$INST/config/.disabled_services"
    if reached_out; then fail "the image install reached out: $(calls)"; fi
}

@test "Moonraker work the image cannot take is deferred to first boot or skipped" {
    rm "$R/home/pi/printer_data/config/moonraker.conf"
    image_main --skip-kiauh-registration
    [ "$status" -eq 0 ] || fail "install failed ($status): $output"
    [ "$(cat "$R/$INST/config/.first_boot_pending")" = "$(printf 'config-links\npermission-rules\nalsa\nremote-screen\nmoonraker')" ]
    [ ! -e "$R/home/pi/printer_data/config/moonraker.conf" ]

    rm -rf "$R"
    make_image "$R"
    um="$R/home/pi/moonraker/moonraker/components/update_manager"
    rm "$um/net_deploy.py"
    : > "$um/zip_deploy.py"
    image_main
    [ "$status" -eq 0 ] || fail "install failed ($status): $output"
    if grep -q 'update_manager helixscreen' "$R/home/pi/printer_data/config/moonraker.conf"; then
        fail "the stanza was armed against a Moonraker that would delete the install"
    fi
    contains "predates release_info.json asset_name support" "$output"
    if reached_out; then fail "the image install reached out: $(calls)"; fi
}

@test "a refused image install writes nothing to the image" {
    snapshot() { (cd "$R" && find . | sort); }
    before=$(snapshot)

    grep -v '^Package: libinput10$' "$R/var/lib/dpkg/status" > "$R/status.new"
    mv "$R/status.new" "$R/var/lib/dpkg/status"
    before=$(snapshot)
    image_main
    [ "$status" -ne 0 ]
    contains "lacks packages HelixScreen needs: libinput10" "$output"
    [ "$(snapshot)" = "$before" ]

    rm -rf "$R"; make_image "$R"; before=$(snapshot)
    image_main --dry-run
    [ "$status" -eq 0 ] || fail "dry run failed ($status): $output"
    contains "Dry run, nothing changed." "$output"
    contains "/home/pi/helixscreen" "$output"
    [ "$(snapshot)" = "$before" ]

    mkdir -p "$R/$INST"; before=$(snapshot)
    image_main
    [ "$status" -ne 0 ]
    contains "already holds /home/pi/helixscreen" "$output"
    [ "$(snapshot)" = "$before" ]

    rm -rf "$R"; make_image "$R"
    sed -i '/^pi:/d' "$R/etc/passwd"; before=$(snapshot)
    image_main
    [ "$status" -ne 0 ]
    contains "not in the image's /etc/passwd" "$output"
    [ "$(snapshot)" = "$before" ]
    if reached_out; then fail "a refused install reached out: $(calls)"; fi
}

@test "a release whose install.sh cannot run first boot is refused and its tree removed" {
    local pkg="$BATS_TEST_TMPDIR/pkg/helixscreen"
    printf '#!/bin/sh\n' > "$pkg/install.sh"
    tar -czf "$ARCHIVE" -C "$BATS_TEST_TMPDIR/pkg" helixscreen
    image_main
    [ "$status" -ne 0 ]
    contains "cannot finish an image install on first boot" "$output"
    [ ! -e "$R/$INST" ]
    [ ! -e "$R/$UNITS/helixscreen.service" ]
}

@test "image-only flags are refused without --target-root, and conflicting modes with it" {
    for args in "--own-display" "--platform pi" "--user pi" \
                "--target-root $R --local $ARCHIVE" \
                "--target-root $R --platform k1 --local $ARCHIVE" \
                "--target-root $R --platform pi" \
                "--target-root $R --platform pi --local $ARCHIVE --update" \
                "--target-root $R --platform pi --local $ARCHIVE --version v1.0.0" \
                "--target-root $R --first-boot"; do
        # shellcheck disable=SC2086  # each case is a word list
        run parse_installer_args $args
        [ "$status" -eq 1 ] || fail "accepted: $args"
    done
}

@test "first boot runs each pending step, then deletes the marker; a failing step keeps it" {
    INSTALL_DIR="$BATS_TEST_TMPDIR/booted/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    printf 'config-links\npermission-rules\nalsa\nremote-screen\nkiauh\nmoonraker\nfuture-step\n' \
        > "$INSTALL_DIR/config/.first_boot_pending"
    detect_init_system() { INIT_SYSTEM=systemd; }
    for f in setup_config_symlink pin_env_file install_permission_rules configure_alsa_default \
             install_remote_screen_route install_kiauh_extension configure_moonraker_updates \
             fix_user_config_ownership; do
        eval "$f() { echo \"STEP $f \$*\" >> '$CALLS'; }"
    done
    run image_first_boot pi
    [ "$status" -eq 0 ] || fail "first boot failed ($status): $output"
    [ ! -e "$INSTALL_DIR/config/.first_boot_pending" ]
    [ "$(grep '^STEP' "$CALLS")" = "$(printf '%s\n' 'STEP setup_config_symlink ' 'STEP pin_env_file ' \
        'STEP install_permission_rules pi' 'STEP configure_alsa_default ' 'STEP install_remote_screen_route ' \
        'STEP install_kiauh_extension false' 'STEP configure_moonraker_updates pi' \
        'STEP setup_config_symlink quiet' 'STEP fix_user_config_ownership ')" ]
    contains "unknown first-boot step: future-step" "$output"

    printf 'permission-rules\nalsa\n' > "$INSTALL_DIR/config/.first_boot_pending"
    install_permission_rules() { return 1; }
    run image_first_boot pi
    [ "$status" -ne 0 ]
    [ -f "$INSTALL_DIR/config/.first_boot_pending" ]

    rm "$INSTALL_DIR/config/.first_boot_pending"
    run image_first_boot pi
    [ "$status" -eq 0 ]
    contains "No first-boot setup is pending" "$output"
}

@test "an uninstall removes the first-boot unit" {
    export HELIX_SYSTEMD_UNIT_DIR="$BATS_TEST_TMPDIR/units"
    mkdir -p "$HELIX_SYSTEMD_UNIT_DIR"
    : > "$HELIX_SYSTEMD_UNIT_DIR/helixscreen-firstboot.service"
    systemctl() { echo "systemctl $*" >> "$CALLS"; }
    remove_first_boot_unit
    [ ! -e "$HELIX_SYSTEMD_UNIT_DIR/helixscreen-firstboot.service" ]
    grep -qx 'systemctl disable helixscreen-firstboot.service' "$CALLS"
    grep -q 'remove_first_boot_unit' "$LIB/uninstall.sh"
}
