#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The in-container half of the HelixPrint plugin host harness. One container
# is one cell: a Moonraker vintage (the image) laid out as one kind of printer.
# scripts/plugin-host-matrix.sh starts the container with `cell.sh start
# LAYOUT` and drives every other verb through docker exec.
#
#   start LAYOUT      lay the printer out, then keep Moonraker running
#   stage             copy the plugin into INSTALL_DIR, keep a copy of the config
#   install           moonraker-plugin/install.sh --no-restart --auto, as the app
#   uninstall         moonraker-plugin/install.sh --no-restart --uninstall-auto
#   sections          how many [helix_print] sections the config tree holds
#   link              the state of <components>/helix_print.py
#   confdiff          the config against the staged copy, backups aside (ours
#                     are *.bak.helix.<ts>; Moonraker keeps its own .moonraker.conf.bkp)
#   backups           the *.bak* files the config dir gained
#   env               the cell's facts, KEY=value
#
# Layouts:
#   plain             a Pi: ~pi/moonraker, ~pi/printer_data, app ~pi/helixscreen
#   legacy            plain, but the app lives in /opt/helixscreen
#   k1                Creality K1: root, the repo nested in /usr/data/moonraker
#   forgex            Forge-X: Moonraker chrooted in /data/.mod/.forge-x, its
#                     moonraker.conf a symlink into the mod's tree including
#                     mod_data/user.moonraker.conf, /opt/config bind-mounted in;
#                     the install step also runs the app's startup relink
#                     twice from the host side, and both must find the link
#                     current
#   modconf           plain, with a mod-owned symlinked moonraker.conf that
#                     includes user.moonraker.conf
#   incsec            plain, with [helix_print] already in an included file
#   remote            plain, but the app runs elsewhere: install and uninstall
#                     are the REMOTE-mode command, remote-install.sh piped
#                     into sh by pi over curl, fetching the plugin from a
#                     release tree served on 127.0.0.1 (no network, no git)
#   relocate          plain, but the first install is made from another
#                     install directory, which then moves to INSTALL_DIR; the
#                     install step passes when the app's startup relink
#                     (install.sh --relink) re-points the dangling link there
#   cli               plain, but install and uninstall are a user's own
#                     install.sh --auto at a shell, without --no-restart:
#                     install.sh restarts Moonraker over its HTTP API alone,
#                     and a sudo, systemctl or service it runs fails the step
#   rofs              k1, but the components directory is a read-only mount;
#                     the install must refuse and change nothing
#   private           Moonraker runs as pi, the app as root under umask 077
#                     from a mode 700 /userdata/helixscreen, which pi cannot
#                     search; the install must refuse and change nothing
#
# Moonraker v0.8 resolves a relative [include] against the directory of the
# symlink's target, later versions against the link's own directory; the mod
# layouts carry a link beside the target so either finds the same file.
#
# A layout whose app runs under systemd (plain, legacy, modconf, incsec) runs
# install.sh the way config/helixscreen.service does: ProtectSystem=strict
# with ReadWritePaths of INSTALL_DIR, its parent and /var/log, and
# NoNewPrivileges, reproduced with bwrap. CELL_STRICT=0 runs it unconfined.
set -eu

STATE=/run/cell.env
PLUGIN_SRC=/plugin-src
PRE=/var/tmp/cell-pre
# The release the remote layout serves, laid out as raw.githubusercontent.com
# serves a tag, and where the remote command finds it.
RELEASE_ROOT=/var/tmp/release
RELEASE_REF=v0.0.0-harness
RELEASE_URL=http://127.0.0.1:8000
MR_SRC=/opt/moonraker-src
FX=/data/.mod/.forge-x
# Where the cli layout's service manager stubs record each call.
SERVICE_CALLS=/var/tmp/cell-service-calls

base_conf() { # <with authorization: yes|no>
    printf '[server]\nhost: 0.0.0.0\nport: 7125\nklippy_uds_address: /tmp/klippy_uds\n\n'
    if [ "$1" = yes ]; then
        printf '[authorization]\ntrusted_clients:\n  127.0.0.0/8\n  0.0.0.0/0\ncors_domains:\n  *\n\n'
    fi
    printf '[machine]\nprovider: none\n'
}

data_dirs() { # <data dir> <owner>
    mkdir -p "$1/config" "$1/gcodes" "$1/logs" "$1/database"
    chown -R "$2:$2" "$1"
}

layout() { # <layout>
    APP_USER=pi APP_HOME=/home/pi INSTALL_DIR=/home/pi/helixscreen STRICT=1 UMASK=022
    CONFIG_DIR=/home/pi/printer_data/config
    COMPONENTS_DIR=/home/pi/moonraker/moonraker/components
    case "$1" in
        plain|legacy|modconf|incsec|remote|relocate|cli|private)
            cp -a "$MR_SRC" /home/pi/moonraker
            chown -R pi:pi /home/pi/moonraker
            data_dirs /home/pi/printer_data pi
            MR_CMD="setpriv --reuid=pi --regid=pi --init-groups env HOME=/home/pi python3 /home/pi/moonraker/moonraker/moonraker.py -d /home/pi/printer_data"
            case "$1" in
                legacy)
                    INSTALL_DIR=/opt/helixscreen ;;
                cli)
                    STRICT=0
                    # Service managers that record every call and refuse it.
                    mkdir -p /var/tmp/cell-cli-bin
                    for tool in sudo systemctl service; do
                        # shellcheck disable=SC2016  # the stub's own $0 and $*
                        printf '#!/bin/sh\necho "$0 $*" >> %s\nexit 1\n' "$SERVICE_CALLS" > "/var/tmp/cell-cli-bin/$tool"
                        chmod 0755 "/var/tmp/cell-cli-bin/$tool"
                    done ;;
                private)
                    APP_USER=root APP_HOME=/root INSTALL_DIR=/userdata/helixscreen STRICT=0 UMASK=077 ;;
                modconf)
                    mkdir -p "$CONFIG_DIR/mod"
                    { base_conf no; printf '\n[include user.moonraker.conf]\n'; } > "$CONFIG_DIR/mod/moonraker.conf"
                    ln -s mod/moonraker.conf "$CONFIG_DIR/moonraker.conf"
                    ln -s ../user.moonraker.conf "$CONFIG_DIR/mod/user.moonraker.conf"
                    base_conf yes | sed -n '/^\[authorization\]/,/^$/p' > "$CONFIG_DIR/user.moonraker.conf" ;;
                incsec)
                    { base_conf yes; printf '\n[include helix.conf]\n'; } > "$CONFIG_DIR/moonraker.conf"
                    printf '[helix_print]\n# kept with the other HelixScreen settings\n' > "$CONFIG_DIR/helix.conf" ;;
            esac
            [ -e "$CONFIG_DIR/moonraker.conf" ] || base_conf yes > "$CONFIG_DIR/moonraker.conf"
            chown -R pi:pi "$CONFIG_DIR" ;;
        k1|rofs)
            APP_USER=root APP_HOME=/root INSTALL_DIR=/usr/data/helixscreen STRICT=0
            CONFIG_DIR=/usr/data/printer_data/config
            COMPONENTS_DIR=/usr/data/moonraker/moonraker/moonraker/components
            mkdir -p /usr/data/moonraker
            cp -a "$MR_SRC" /usr/data/moonraker/moonraker
            data_dirs /usr/data/printer_data root
            base_conf yes > "$CONFIG_DIR/moonraker.conf"
            MR_CMD="python3 /usr/data/moonraker/moonraker/moonraker/moonraker.py -d /usr/data/printer_data"
            if [ "$1" = rofs ]; then
                mount --bind "$COMPONENTS_DIR" "$COMPONENTS_DIR"
                mount -o remount,ro,bind "$COMPONENTS_DIR"
            fi ;;
        forgex)
            APP_USER=root APP_HOME=/root INSTALL_DIR=/opt/config/mod_data/helixscreen STRICT=0
            CONFIG_DIR=$FX/root/printer_data/config
            COMPONENTS_DIR=$FX/root/moonraker/moonraker/components
            mkdir -p "$(dirname "$FX")"
            cp -a /opt/fx-root "$FX"
            mkdir -p "$FX/proc" "$FX/dev" "$FX/tmp" "$FX/root" "$FX/opt/config" /opt/config/mod_data
            chmod 1777 "$FX/tmp"
            mount --bind /proc "$FX/proc"
            mount --bind /dev "$FX/dev"
            mount --bind /opt/config "$FX/opt/config"
            cp -a "$MR_SRC" "$FX/root/moonraker"
            data_dirs "$FX/root/printer_data" root
            mkdir -p "$CONFIG_DIR/mod" "$CONFIG_DIR/mod_data"
            { base_conf no; printf '\n[include mod_data/user.moonraker.conf]\n'; } > "$CONFIG_DIR/mod/moonraker.conf"
            ln -s mod/moonraker.conf "$CONFIG_DIR/moonraker.conf"
            ln -s ../mod_data "$CONFIG_DIR/mod/mod_data"
            base_conf yes | sed -n '/^\[authorization\]/,/^$/p' > "$CONFIG_DIR/mod_data/user.moonraker.conf"
            MR_CMD="chroot $FX python3 /root/moonraker/moonraker/moonraker.py -d /root/printer_data" ;;
        *)
            echo "cell.sh: unknown layout $1" >&2; exit 2 ;;
    esac
    INSTALL_PARENT=$(dirname "$INSTALL_DIR")
    mkdir -p "$INSTALL_DIR"
    chown "$APP_USER:$APP_USER" "$INSTALL_DIR"
    # What mkdir makes under the app's umask.
    chmod "$(printf '%o' $((0777 & ~0$UMASK)))" "$INSTALL_DIR"
    [ "${CELL_STRICT:-1}" = 1 ] || STRICT=0
    {
        printf 'LAYOUT=%s\nAPP_USER=%s\nAPP_HOME=%s\nINSTALL_DIR=%s\nINSTALL_PARENT=%s\n' \
            "$1" "$APP_USER" "$APP_HOME" "$INSTALL_DIR" "$INSTALL_PARENT"
        printf 'CONFIG_DIR=%s\nCOMPONENTS_DIR=%s\nSTRICT=%s\nUMASK=%s\n' "$CONFIG_DIR" "$COMPONENTS_DIR" "$STRICT" "$UMASK"
        printf 'MR_CMD="%s"\n' "$MR_CMD"
    } > "$STATE"
}

# Moonraker exits to restart on some vintages; a service manager starts it
# again, and this loop is that service manager.
supervise() {
    # shellcheck disable=SC1090
    . "$STATE"
    while :; do
        printf '%s cell: starting moonraker\n' "$(date '+%F %T')" >> /var/log/moonraker.out
        # shellcheck disable=SC2086  # MR_CMD is a word list
        $MR_CMD >> /var/log/moonraker.out 2>&1 || true
        sleep 1
    done
}

load() {
    # shellcheck disable=SC1090
    . "$STATE"
}

# Run the REMOTE-mode command as pi over a login-like shell, the way a user
# pastes it: curl piped into sh, on a PATH whose git fails loudly. The CDN
# holds no release here, so the wrapper takes its GitHub path to the tree.
remote_command() { # [--uninstall]
    load
    mkdir -p /var/tmp/cell-bin
    printf '#!/bin/sh\necho "git called: $*" >&2\nexit 127\n' > /var/tmp/cell-bin/git
    chmod 0755 /var/tmp/cell-bin/git
    cd /home/pi
    # shellcheck disable=SC2016  # expands in the user's shell
    setpriv --reuid=pi --regid=pi --init-groups env HOME=/home/pi \
        PATH=/var/tmp/cell-bin:/usr/local/bin:/usr/bin:/bin HELIX_PLUGIN_BASE_URL="$RELEASE_URL" \
        HELIX_PLUGIN_CDN_URL="$RELEASE_URL/no-cdn" \
        sh -c 'url=$1 ref=$2; shift 2; curl -fsSL "$url/$ref/moonraker-plugin/remote-install.sh" | sh -s -- "$ref" "$@"' \
        sh "$RELEASE_URL" "$RELEASE_REF" "$@"
}

# Install from a directory beside INSTALL_DIR, move that tree to INSTALL_DIR,
# and check that the relink the app runs at startup repairs the link.
relocated_install() {
    load
    old="$INSTALL_DIR.old"
    rm -rf "$old"
    mkdir -p "$old"
    mv "$INSTALL_DIR/moonraker-plugin" "$old/moonraker-plugin"
    chown -R "$APP_USER:$APP_USER" "$old"
    ( APP_PLUGIN_DIR="$old/moonraker-plugin" as_app --no-restart --auto ) || return 1
    printf 'installed from %s: %s\n' "$old" "$(readlink "$COMPONENTS_DIR/helix_print.py")"
    mv "$old/moonraker-plugin" "$INSTALL_DIR/moonraker-plugin"
    rmdir "$old"
    if [ -e "$COMPONENTS_DIR/helix_print.py" ]; then
        echo "the link still resolves after the move" >&2
        return 1
    fi
    status=0
    ( as_app --no-restart --relink ) || status=$?
    if [ "$status" -ne 3 ]; then
        echo "relink exited $status, want 3 (re-pointed)" >&2
        return 1
    fi
    case "$(readlink -f "$COMPONENTS_DIR/helix_print.py")" in
        "$INSTALL_DIR"/moonraker-plugin/helix_print.py) ;;
        *) echo "relinked to $(readlink "$COMPONENTS_DIR/helix_print.py")" >&2; return 1 ;;
    esac
    status=0
    ( as_app --no-restart --relink ) || status=$?
    if [ "$status" -ne 0 ]; then
        echo "a second relink exited $status, want 0 (current)" >&2
        return 1
    fi
    touch /var/tmp/cell-relocated
}

# Run the app's startup relink twice: each must find the link the install
# made current (exit 0), or Moonraker restarts at every app start.
relink_stays_current() {
    for run in 1 2; do
        status=0
        ( as_app --no-restart --relink ) || status=$?
        if [ "$status" -ne 0 ]; then
            echo "relink run $run exited $status, want 0 (current)" >&2
            return 1
        fi
    done
}

# Run install.sh with <args> as a user at a shell (the cli layout): it must
# reach Moonraker over HTTP alone, so a call to a service manager fails it.
cli_run() { # <args...>
    rm -f "$SERVICE_CALLS"
    status=0
    ( as_app "$@" ) || status=$?
    if [ -s "$SERVICE_CALLS" ]; then
        echo "install.sh ran a service manager: $(tr '\n' ' ' < "$SERVICE_CALLS")" >&2
        return 1
    fi
    return "$status"
}

# Run install.sh with <args> as the app, confined as its service confines it,
# under the app's umask. APP_PLUGIN_DIR names another copy of the plugin to
# run it from.
as_app() { # <args...>
    load
    path=/usr/local/bin:/usr/bin:/bin
    if [ "$LAYOUT" = cli ]; then path=/var/tmp/cell-cli-bin:$path; fi
    umask "$UMASK"
    set -- env HOME="$APP_HOME" PATH="$path" \
        sh "${APP_PLUGIN_DIR:-$INSTALL_DIR/moonraker-plugin}/install.sh" "$@"
    if [ "$STRICT" = 1 ]; then
        # The bwrap runs as the app user, so the writes it allows are the
        # app user's own; bwrap sets no_new_privs as NoNewPrivileges does.
        set -- bwrap --ro-bind / / --dev-bind /dev /dev --proc /proc \
            --bind "$INSTALL_DIR" "$INSTALL_DIR" --bind "$INSTALL_PARENT" "$INSTALL_PARENT" \
            --bind /var/log /var/log -- "$@"
    fi
    if [ "$APP_USER" != root ]; then
        set -- setpriv --reuid="$APP_USER" --regid="$APP_USER" --init-groups "$@"
    fi
    cd "$INSTALL_DIR"
    exec "$@"
}

case "${1:-}" in
    start)
        layout "${2:?layout}"
        if [ "$2" = remote ]; then
            mkdir -p "$RELEASE_ROOT"
            python3 -m http.server 8000 --bind 127.0.0.1 --directory "$RELEASE_ROOT" \
                > /var/log/release-server.out 2>&1 &
        fi
        supervise ;;
    stage)
        load
        if [ "$LAYOUT" = remote ]; then
            rm -rf "${RELEASE_ROOT:?}/$RELEASE_REF"
            mkdir -p "$RELEASE_ROOT/$RELEASE_REF"
            cp -R "$PLUGIN_SRC" "$RELEASE_ROOT/$RELEASE_REF/moonraker-plugin"
            rm -rf "$RELEASE_ROOT/$RELEASE_REF/moonraker-plugin/tests"
        fi
        rm -rf "$INSTALL_DIR/moonraker-plugin" "$PRE"
        # Copied as the app's own installer copies it, under its umask.
        ( umask "$UMASK"; cp -R "$PLUGIN_SRC" "$INSTALL_DIR/moonraker-plugin" )
        chown -R "$APP_USER:$APP_USER" "$INSTALL_DIR/moonraker-plugin"
        mkdir -p "$PRE"
        cp -a "$CONFIG_DIR/." "$PRE/"
        # Uninstall removes [helix_print] wherever it lives, a section the
        # user placed before the install included, so incsec expects it gone.
        if [ "$LAYOUT" = incsec ]; then : > "$PRE/helix.conf"; fi ;;
    install)
        load
        if [ "$LAYOUT" = remote ]; then remote_command; exit; fi
        if [ "$LAYOUT" = relocate ] && [ ! -e /var/tmp/cell-relocated ]; then
            relocated_install
            exit
        fi
        if [ "$LAYOUT" = forgex ]; then
            ( as_app --no-restart --auto ) || exit 1
            relink_stays_current
            exit
        fi
        if [ "$LAYOUT" = cli ]; then cli_run --auto; else as_app --no-restart --auto; fi ;;
    uninstall)
        load
        if [ "$LAYOUT" = remote ]; then
            remote_command --uninstall
            # Everything the command added goes: its plugin directory too.
            if [ -e /home/pi/helix_print ]; then echo "left behind: /home/pi/helix_print" >&2; exit 1; fi
            exit 0
        fi
        if [ "$LAYOUT" = cli ]; then cli_run --uninstall-auto; else as_app --no-restart --uninstall-auto; fi ;;
    sections)
        load
        # Each file once, however many links lead to it.
        find -L "$CONFIG_DIR" -type f ! -name '*.bak*' ! -name '*.bkp' -exec readlink -f {} + | sort -u |
            while IFS= read -r f; do
                grep -q '^\[helix_print\]' "$f" || continue
                printf '%s %s\n' "$(grep -c '^\[helix_print\]' "$f")" "${f#"$CONFIG_DIR"/}"
            done ;;
    link)
        load
        l="$COMPONENTS_DIR/helix_print.py"
        if [ -L "$l" ]; then printf 'symlink %s\n' "$(readlink "$l")"
        elif [ -e "$l" ]; then echo file
        else echo absent
        fi ;;
    confdiff)
        load
        diff -r --no-dereference -x '*.bak*' -x '*.bkp' "$PRE" "$CONFIG_DIR" ;;
    backups)
        load
        find "$CONFIG_DIR" -name '*.bak*' | sed "s#^$CONFIG_DIR/##" | sort ;;
    env)
        load
        cat "$STATE" ;;
    *)
        sed -n '3,/^set -eu$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2
        exit 2 ;;
esac
