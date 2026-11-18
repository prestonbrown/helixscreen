#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Record the state a HelixPrint plugin install or uninstall touches on one
# printer, read-only, so two snapshots taken around a change show what it did.
#
#   scripts/plugin-state-snapshot.sh --ssh USER@HOST [options] DEVICE LABEL
#   scripts/plugin-state-snapshot.sh --diff SNAPSHOT_A SNAPSHOT_B
#   scripts/plugin-state-snapshot.sh --emit-remote [--chroot DIR]
#
# Options:
#   --ssh USER@HOST      the printer to read; the HTTP half asks the same host
#   --port N             Moonraker's port (7125; some firmware serves it on 80)
#   --http HOST          ask Moonraker on HOST instead of the ssh host
#   --chroot DIR         Moonraker runs chrooted in DIR (Forge-X). Detected from
#                        /proc/<pid>/root when the ssh user may read it; this
#                        names it when it may not
#   --sshpass-env VAR    password-only host: sshpass reads the password from the
#                        environment variable VAR. A password is never an argument
#   --out DIR            where snapshots go (~/printer-backups)
#   --no-http            skip the HTTP half
#   --root DIR           test hook: read DIR as the printer's "/" with a local
#                        sh instead of ssh
#   --emit-remote        print the script the printer runs, and exit
#
# A snapshot lands in <out>/DEVICE/plugin-test/<timestamp>-LABEL/:
#
#   paths.txt            what install.sh would find, found the way it finds it
#                        (its own discovery functions, run on the printer): the
#                        components dir, config dir, moonraker.conf, the file the
#                        [helix_print] section belongs in, every [include], the
#                        Moonraker and app users, the install dir and version
#   components_ls.txt    ls -l of the components dir, symlink targets included
#   gcodes_ls.txt        the plugin's directories under gcodes
#   backups.txt          *.bak* files in the config dir
#   units.txt            systemctl cat of the moonraker and helixscreen units
#   checksums.txt        sha256 (md5 on a BusyBox without it) of every copied file
#   files/               the copied files at their printer paths: moonraker.conf
#                        (a symlink stays a symlink, its target is copied too),
#                        each include, the section file, printer.cfg,
#                        release_info.json
#   server_info.json, helix_status.json, server_config.json, update_status.json,
#   print_stats.json, moonraker_log_tail.txt, summary.txt
#                        the HTTP half; summary.txt is the stable facts from them
#
# --diff compares two snapshots and exits 0 when they match, 1 when they differ,
# 2 on an error. A changed Moonraker pid or start time is reported as a restart
# and is not a difference.
#
# Nothing on the printer is written: the printer runs read-only commands and
# streams its output and a tar of the files back over the connection. The
# printer-side half is plain sh that BusyBox runs: no pgrep, head -n, an md5sum
# fallback, and Moonraker found by scanning /proc.
set -u

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
INSTALL_SH="${HELIX_PLUGIN_INSTALL_SH:-$SELF_DIR/../moonraker-plugin/install.sh}"

die() { printf 'plugin-state-snapshot: %s\n' "$1" >&2; exit "${2:-2}"; }
usage() { sed -n '3,/^set -u$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }

valid_name() { # <name>
    case "$1" in ''|.|..|*[!A-Za-z0-9._-]*) return 1 ;; esac
}

# Single-quote a word for a remote shell.
shquote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

# --- the printer-side script --------------------------------------------------

# install.sh's discovery block: the vendor path lists and every function from
# components_under to helix_conf_file.
install_sh_discovery() {
    [ -r "$INSTALL_SH" ] || die "cannot read $INSTALL_SH"
    awk '/^MOONRAKER_SRC_PATHS="/ { on = 1 }
         /^running_klipper_sdcard_source\(\) *\{/ { exit }
         on' "$INSTALL_SH"
}

# The helpers install.sh sources (the [include] walk, the running Moonraker),
# so the snapshot lists the files the installer edits. Their
# running_moonraker_root is renamed so the wrapper below can add the --chroot
# override and the --root prefix to it.
conf_sh() {
    _conf_sh="$(dirname "$INSTALL_SH")/helix_print_conf.sh"
    [ -r "$_conf_sh" ] || die "cannot read $_conf_sh"
    sed 's/^running_moonraker_root() {$/install_sh_moonraker_root() {/' "$_conf_sh"
}

# The functions the collector calls; a refactor of install.sh that drops one
# fails the snapshot instead of recording a wrong path.
REQUIRED_FUNCS="components_under running_moonraker_proc running_moonraker_args
install_sh_moonraker_root running_moonraker_arg find_moonraker_components
find_config_dir helix_conf_file moonraker_conf_files moonraker_conf_includes"

emit_remote() {
    printf '%s\n' "SNAP_ROOT=$(shquote "$ROOT")"
    printf '%s\n' "SNAP_CHROOT=$(shquote "$CHROOT")"
    printf '%s\n' "SNAP_REQUIRED=$(shquote "$(printf '%s' "$REQUIRED_FUNCS" | tr '\n' ' ')")"
    conf_sh
    install_sh_discovery
    cat <<'COLLECTOR'

for f in $SNAP_REQUIRED; do
    command -v "$f" >/dev/null 2>&1 || { echo "@@ERROR install.sh discovery lacks $f"; exit 3; }
done

# The start time of the process at /proc directory @1, in clock ticks since
# boot: field 22 of its stat, counted after the last ')' since the command
# name may hold spaces.
proc_start_ticks() {
    sed 's/.*) //' "$1/stat" 2>/dev/null | awk '{ print $20 }'
}

# Whether systemd is the init of the host whose /run is @1, judged by its
# runtime directory there: a systemctl on PATH can be a vendor's stand-in.
systemd_is_init() {
    [ -d "$1/systemd/system" ]
}

HELIX_PROC_ROOT="$SNAP_ROOT/proc"

# The running Moonraker's "/" as a path from here: install.sh's answer, else
# the --chroot the caller named, under the --root prefix.
running_moonraker_root() {
    r=$(install_sh_moonraker_root)
    [ -n "$r" ] || r="$SNAP_CHROOT"
    printf '%s' "$SNAP_ROOT${r%/}"
}

if [ -n "$SNAP_ROOT" ]; then
    MOONRAKER_SRC_PATHS=$(for p in $MOONRAKER_SRC_PATHS; do printf '%s\n' "$SNAP_ROOT$p"; done)
    MOONRAKER_CONFIG_DIRS=$(for p in $MOONRAKER_CONFIG_DIRS; do printf '%s\n' "$SNAP_ROOT$p"; done)
    # The pip fallback asks this machine's python, which is not the fixture's.
    python3() { return 1; }
fi

# A path as the printer names it.
dev() { printf '%s' "${1#"$SNAP_ROOT"}"; }
kv() { printf '%s=%s\n' "$1" "$2"; }
# Command output with the --root prefix taken out of every path.
strip_root() { if [ -n "$SNAP_ROOT" ]; then sed "s#$SNAP_ROOT##g"; else cat; fi; }
section() { printf '@@FILE %s\n' "$1"; }

proc_uid() { # <proc dir>
    awk '/^Uid:/ { print $2; exit }' "$1/status" 2>/dev/null
}
user_of() { # <uid>
    [ -n "$1" ] || return 0
    awk -F: -v u="$1" '$3 == u { print $1; exit }' "$SNAP_ROOT/etc/passwd" 2>/dev/null
}
home_of() { # <user>
    [ -n "$1" ] || return 0
    awk -F: -v n="$1" '$1 == n { print $6; exit }' "$SNAP_ROOT/etc/passwd" 2>/dev/null
}
# The HelixScreen process: argv[0] named helix-screen.
app_proc() {
    for f in "$HELIX_PROC_ROOT"/[0-9]*/cmdline; do
        [ -r "$f" ] || continue
        a0=$(tr '\000' '\n' < "$f" 2>/dev/null | awk 'NR == 1 { print; exit }')
        case "$a0" in */helix-screen|helix-screen) dirname "$f"; return 0 ;; esac
    done
    return 1
}

# --- what install.sh would find -------------------------------------------
mproc=$(running_moonraker_proc) || mproc=""
moonraker_uid=""; moonraker_user=""; moonraker_start=""; mpid=""
if [ -n "$mproc" ]; then
    mpid=${mproc##*/}
    moonraker_uid=$(proc_uid "$mproc")
    moonraker_user=$(user_of "$moonraker_uid")
    moonraker_start=$(proc_start_ticks "$mproc")
fi

aproc=$(app_proc) || aproc=""
app_uid=""; app_user=""; install_dir=""; apid=""
if [ -n "$aproc" ]; then
    apid=${aproc##*/}
    app_uid=$(proc_uid "$aproc")
    app_user=$(user_of "$app_uid")
    exe=$(readlink "$aproc/exe" 2>/dev/null) ||
        exe=$(tr '\000' '\n' < "$aproc/cmdline" 2>/dev/null | awk 'NR == 1 { print; exit }')
    case "$exe" in /*/bin/helix-screen*) install_dir=${exe%/bin/helix-screen*} ;; esac
fi
# install.sh runs as the app, so its $HOME is the app user's.
app_home=$(home_of "$app_user")
[ -n "$app_home" ] || app_home=$(dev "${HOME:-/root}")
HOME="$SNAP_ROOT$app_home"

components_dir=$(find_moonraker_components "")
config_dir=$(find_config_dir)
conf=""; section_file=""
if [ -n "$config_dir" ]; then
    conf="$config_dir/moonraker.conf"
    section_file=$(helix_conf_file "$config_dir")
fi
mroot=$(dev "$(running_moonraker_root)")

data_dir=$(running_moonraker_arg -d --datapath)
if [ -n "$data_dir" ]; then data_dir="$(running_moonraker_root)$data_dir"
elif [ -n "$config_dir" ]; then data_dir=$(dirname "$config_dir")
fi

link=""; link_state=absent; link_target=""; link_resolves=""
if [ -n "$components_dir" ]; then
    link="$components_dir/helix_print.py"
    if [ -L "$link" ]; then
        link_state=symlink
        link_target=$(readlink "$link" 2>/dev/null)
        # The target is named as Moonraker sees it.
        case "$link_target" in
            /*) [ -f "$(running_moonraker_root)$link_target" ] && link_resolves=yes || link_resolves=no ;;
            *) [ -f "$components_dir/$link_target" ] && link_resolves=yes || link_resolves=no ;;
        esac
    elif [ -f "$link" ]; then
        link_state="file"
    fi
fi

version=""
if [ -n "$install_dir" ] && [ -f "$SNAP_ROOT$install_dir/release_info.json" ]; then
    version=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$SNAP_ROOT$install_dir/release_info.json" | awk 'NR == 1')
fi

if systemd_is_init "$SNAP_ROOT/run"; then init=systemd
elif [ -x "$SNAP_ROOT/sbin/procd" ]; then init=procd
else init=sysv
fi

tree=""
[ -z "$conf" ] || tree=$(moonraker_conf_files "$conf" "$(running_moonraker_root)")

section paths.txt
kv moonraker_pid "$mpid"
kv moonraker_start_ticks "$moonraker_start"
kv boot_id "$(cat "$SNAP_ROOT/proc/sys/kernel/random/boot_id" 2>/dev/null)"
kv moonraker_uid "$moonraker_uid"
kv moonraker_user "$moonraker_user"
kv moonraker_root "$mroot"
kv moonraker_argv "$( [ -z "$mproc" ] || running_moonraker_args | tr '\n' ' ' | sed 's/ $//')"
kv app_pid "$apid"
kv app_uid "$app_uid"
kv app_user "$app_user"
kv app_home "$app_home"
kv install_dir "$install_dir"
kv helixscreen_version "$version"
kv init "$init"
kv components_dir "$(dev "$components_dir")"
kv config_dir "$(dev "$config_dir")"
kv data_dir "$(dev "$data_dir")"
kv moonraker_conf "$(dev "$conf")"
kv moonraker_conf_is_symlink "$( [ -n "$conf" ] && [ -L "$conf" ] && echo yes || echo no)"
kv moonraker_conf_real "$( [ -z "$conf" ] || dev "$(readlink -f "$conf" 2>/dev/null)")"
kv section_file "$(dev "$section_file")"
printf '%s\n' "$tree" | awk 'NR > 1 && NF' | while IFS= read -r f; do kv include "$(dev "$f")"; done
kv plugin_link "$(dev "$link")"
kv plugin_link_state "$link_state"
kv plugin_link_target "$link_target"
kv plugin_link_resolves "$link_resolves"

section components_ls.txt
# ls -l, not find: BusyBox find has no -printf, and ls shows symlink targets.
# shellcheck disable=SC2012
[ -z "$components_dir" ] || ls -l "$components_dir" 2>&1 | strip_root

section gcodes_ls.txt
if [ -n "$data_dir" ]; then
    for d in .helix_temp .helix_print helix_print; do
        # shellcheck disable=SC2012
        [ -e "$data_dir/gcodes/$d" ] && ls -la "$data_dir/gcodes/$d" 2>&1 | strip_root | sed "1s#^#$d: #"
    done
fi

section backups.txt
[ -z "$config_dir" ] || find "$config_dir" -name '*.bak*' 2>/dev/null | strip_root | sort

section units.txt
if [ -z "$SNAP_ROOT" ] && [ "$init" = systemd ] && command -v systemctl >/dev/null 2>&1; then
    for u in moonraker helixscreen; do
        systemctl cat "$u" 2>&1
        echo
    done
else
    echo "init: $init (no systemctl cat)"
    for s in "$SNAP_ROOT"/etc/init.d/*; do
        case "${s##*/}" in *[Mm]oonraker*|*[Hh]elix*) printf '/etc/init.d/%s\n' "${s##*/}" ;; esac
    done
fi

copy=""
add() { [ -n "$1" ] && { [ -e "$1" ] || [ -L "$1" ]; } && copy="${copy:+$copy
}$(dev "$1")"; }
if [ -n "$conf" ]; then
    add "$conf"
    [ ! -L "$conf" ] || add "$(readlink -f "$conf" 2>/dev/null)"
fi
for f in $tree; do add "$f"; done
add "$section_file"
[ -z "$config_dir" ] || add "$config_dir/printer.cfg"
[ -z "$install_dir" ] || add "$SNAP_ROOT$install_dir/release_info.json"
copy=$(printf '%s\n' "$copy" | awk 'NF && !seen[$0]++')

section copy-list.txt
printf '%s\n' "$copy"

section checksums.txt
if command -v sha256sum >/dev/null 2>&1; then sum=sha256sum; else sum=md5sum; fi
printf '%s\n' "$copy" | while IFS= read -r f; do
    [ -n "$f" ] && [ -f "$SNAP_ROOT$f" ] && $sum "$SNAP_ROOT$f" | strip_root
done
echo "@@END"
COLLECTOR
}

# The printer-side tar of the files the collector listed, paths relative to "/".
emit_tar() { # <copy list file>
    printf 'cd %s || exit 3\n' "$(shquote "${ROOT:-/}")"
    printf 'exec tar -cf -'
    while IFS= read -r f; do
        [ -n "$f" ] && printf ' %s' "$(shquote ".${f}")"
    done < "$1"
    printf '\n'
}

# Run stdin as a shell script on the printer, or under --root here.
on_printer() {
    if [ -n "$ROOT" ]; then
        sh -s
    elif [ -n "$SSHPASS_ENV" ]; then
        # shellcheck disable=SC2086  # PLUGIN_SNAP_SSH_OPTS is a word list
        DISPLAY='' SSH_ASKPASS='' SSHPASS=$(printenv "$SSHPASS_ENV") \
            sshpass -e ssh -o ConnectTimeout=8 ${PLUGIN_SNAP_SSH_OPTS:-} "$SSH" sh -s
    else
        # shellcheck disable=SC2086
        ssh -o ConnectTimeout=8 -o BatchMode=yes ${PLUGIN_SNAP_SSH_OPTS:-} "$SSH" sh -s
    fi
}

# Split the collector's @@FILE sections into files under <dir>.
split_sections() { # <dir>
    awk -v dir="$1" '
        /^@@ERROR / { sub(/^@@ERROR /, ""); print > "/dev/stderr"; bad = 1; next }
        /^@@END$/ { done = 1; next }
        /^@@FILE / { if (out) close(out); out = dir "/" substr($0, 8); printf "" > out; next }
        out { print >> out }
        END { exit bad ? 3 : (done ? 0 : 4) }'
}

# --- the HTTP half -------------------------------------------------------------

http_get() { # <path> <out file> [curl args...]
    _p=$1 _o=$2; shift 2
    curl -s --max-time 15 "$@" "http://$HTTP_HOST:$PORT$_p" > "$_o" 2>/dev/null || true
}

collect_http() { # <dir>
    command -v curl >/dev/null 2>&1 || { echo "curl not found: no HTTP half" > "$1/summary.txt"; return 0; }
    http_get /server/info "$1/server_info.json"
    http_get /server/helix/status "$1/helix_status.json"
    http_get /server/config "$1/server_config.json"
    http_get '/machine/update/status?refresh=false' "$1/update_status.json"
    http_get '/printer/objects/query?print_stats=state' "$1/print_stats.json"
    http_get /server/files/logs/moonraker.log "$1/moonraker_log.raw" -H 'Range: bytes=-262144'
    tail -n 400 "$1/moonraker_log.raw" > "$1/moonraker_log_tail.txt" 2>/dev/null
    rm -f "$1/moonraker_log.raw"
    write_summary "$1"
}

# The facts a before/after comparison is about, from the HTTP half.
write_summary() { # <dir>
    command -v jq >/dev/null 2>&1 || { echo "jq not found: no summary" > "$1/summary.txt"; return 0; }
    {
        jq -r '.result // {} |
            "moonraker_version=\(.moonraker_version // "")",
            "api_version=\(.api_version_string // (.api_version // [] | map(tostring) | join(".")))",
            "klippy_state=\(.klippy_state // "")",
            "components=\(.components // [] | sort | join(" "))",
            "failed_components=\(.failed_components // [] | sort | join(" "))",
            (.warnings // [] | .[] | "warning=\(.)")' "$1/server_info.json" 2>/dev/null ||
            echo "server_info=unreadable"
        if jq -e '.result' "$1/helix_status.json" >/dev/null 2>&1; then
            echo "helix_status=$(jq -c '.result' "$1/helix_status.json")"
        else
            echo "helix_status=absent"
        fi
        jq -r '.result.config // {} | to_entries[] |
            select(.key == "helix_print" or (.key | startswith("update_manager"))) |
            "config.\(.key)=\(.value | tojson)"' "$1/server_config.json" 2>/dev/null
        jq -r '.result.version_info // {} | keys[] | "update_manager_entry=\(.)"' "$1/update_status.json" 2>/dev/null
        echo "print_state=$(jq -r '.result.status.print_stats.state // "unknown"' "$1/print_stats.json" 2>/dev/null)"
        grep -E 'HelixPrint|helix_print|Unable to load component|Unparsed config|Failed to load' \
            "$1/moonraker_log_tail.txt" 2>/dev/null | sed 's/^/log: /' | tail -n 20
    } > "$1/summary.txt"
}

# --- diff ----------------------------------------------------------------------

# Keys that change on every restart or app start; they say a restart happened.
VOLATILE='^(moonraker_pid|moonraker_start_ticks|app_pid|boot_id)='

diff_snapshots() { # <a> <b>
    a=$1 b=$2
    for d in "$a" "$b"; do
        [ -f "$d/paths.txt" ] || die "$d is not a snapshot (no paths.txt)"
    done
    rc=0
    for k in moonraker_pid moonraker_start_ticks boot_id; do
        va=$(sed -n "s/^$k=//p" "$a/paths.txt") vb=$(sed -n "s/^$k=//p" "$b/paths.txt")
        [ "$va" = "$vb" ] || printf 'restart: %s %s -> %s\n' "$k" "${va:-none}" "${vb:-none}"
    done
    for f in paths.txt summary.txt components_ls.txt gcodes_ls.txt backups.txt units.txt checksums.txt; do
        [ -e "$a/$f" ] || [ -e "$b/$f" ] || continue
        if ! out=$(diff -u --label "a/$f" --label "b/$f" \
                    "$( [ -e "$a/$f" ] && echo "$a/$f" || echo /dev/null)" \
                    "$( [ -e "$b/$f" ] && echo "$b/$f" || echo /dev/null)" |
                    grep -v -E "^[-+]${VOLATILE#^}" | grep -E '^[-+][^-+]|^[-+]$'); then
            continue
        fi
        printf '=== %s\n%s\n' "$f" "$out"
        rc=1
    done
    if [ -d "$a/files" ] || [ -d "$b/files" ]; then
        mkdir -p "$a/files" "$b/files" 2>/dev/null
        if ! out=$(diff -ru --no-dereference "$a/files" "$b/files" 2>&1); then
            printf '=== files/\n%s\n' "$out"
            rc=1
        fi
    fi
    [ "$rc" -eq 0 ] && echo "no differences"
    return "$rc"
}

# --- arguments -------------------------------------------------------------------

SSH="" PORT=7125 HTTP_HOST="" CHROOT="" SSHPASS_ENV="" OUT="" ROOT="" NO_HTTP=false
MODE=snapshot
[ $# -gt 0 ] || usage
need() { if [ $# -lt 2 ] || [ -z "$2" ]; then die "$1 needs a value"; fi; }
while [ $# -gt 0 ]; do
    case "$1" in
        --ssh) need "$@"; SSH=$2; shift 2 ;;
        --port) need "$@"; PORT=$2; shift 2 ;;
        --http) need "$@"; HTTP_HOST=$2; shift 2 ;;
        --chroot) need "$@"; CHROOT=$2; shift 2 ;;
        --sshpass-env) need "$@"; SSHPASS_ENV=$2; shift 2 ;;
        --out) need "$@"; OUT=$2; shift 2 ;;
        --root) need "$@"; ROOT=$2; shift 2 ;;
        --no-http) NO_HTTP=true; shift ;;
        --diff) MODE="diff"; shift ;;
        --emit-remote) MODE=emit; shift ;;
        -h|--help) usage ;;
        --) shift; break ;;
        -*) die "unknown option $1" ;;
        *) break ;;
    esac
done

case "$PORT" in ''|*[!0-9]*) die "--port takes a number" ;; esac
case "$CHROOT" in ''|/*) ;; *) die "--chroot takes an absolute path" ;; esac
CHROOT=${CHROOT%/}

case "$MODE" in
    diff)
        [ $# -eq 2 ] || die "--diff takes two snapshot directories"
        diff_snapshots "$1" "$2"
        exit $? ;;
    emit)
        emit_remote
        exit 0 ;;
esac

[ $# -eq 2 ] || die "expected DEVICE LABEL (see --help)"
DEVICE=$1 LABEL=$2
valid_name "$DEVICE" || die "DEVICE must be letters, digits, . _ -"
valid_name "$LABEL" || die "LABEL must be letters, digits, . _ -"

if [ -n "$ROOT" ]; then
    [ -d "$ROOT" ] || die "--root $ROOT is not a directory"
    ROOT=$(cd "$ROOT" && pwd -P)
    [ -n "$SSH" ] && die "--root reads a local tree; it takes no --ssh"
else
    [ -n "$SSH" ] || die "--ssh USER@HOST is required"
fi
if [ -n "$SSHPASS_ENV" ]; then
    case "$SSHPASS_ENV" in [A-Za-z_]*) ;; *) die "--sshpass-env takes a variable name" ;; esac
    case "$SSHPASS_ENV" in *[!A-Za-z0-9_]*) die "--sshpass-env takes a variable name" ;; esac
    printenv "$SSHPASS_ENV" >/dev/null || die "--sshpass-env: $SSHPASS_ENV is not set"
    command -v sshpass >/dev/null 2>&1 || die "--sshpass-env needs sshpass"
fi
[ -n "$HTTP_HOST" ] || { HTTP_HOST=${SSH#*@}; HTTP_HOST=${HTTP_HOST%%:*}; }
[ -n "$HTTP_HOST" ] || NO_HTTP=true

DIR="${OUT:-$HOME/printer-backups}/$DEVICE/plugin-test/$(date +%Y%m%d-%H%M%S)-$LABEL"
[ ! -e "$DIR" ] || die "$DIR already exists"
mkdir -p "$DIR" || die "cannot create $DIR"

emit_remote | on_printer | split_sections "$DIR"
rc=$?
[ "$rc" -eq 0 ] || die "the printer-side half failed (status $rc); partial snapshot in $DIR" 1

if [ -s "$DIR/copy-list.txt" ]; then
    mkdir -p "$DIR/files"
    emit_tar "$DIR/copy-list.txt" | on_printer | tar -xf - -C "$DIR/files" ||
        echo "plugin-state-snapshot: copying files failed; see $DIR/copy-list.txt" >&2
fi

$NO_HTTP || collect_http "$DIR"
echo "$DIR"
