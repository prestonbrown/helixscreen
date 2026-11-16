#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# moonraker-plugin/remote-install.sh: the command the app's REMOTE-mode install
# modal shows. It fetches one release's plugin and runs its install.sh, so these
# cases serve a release from fixture trees through stub fetchers and check what
# lands on a printer-shaped HOME: the CDN's tarball from CDN_ROOT, GitHub's raw
# files from RELEASE_ROOT.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"
WRAPPER="${HELIX_PLUGIN_REMOTE_SH:-$REPO_ROOT/moonraker-plugin/remote-install.sh}"
BASE_URL="http://release.test"
CDN_URL="http://cdn.test"

setup() {
    RELEASE_ROOT="$BATS_TEST_TMPDIR/release"
    CDN_ROOT="$BATS_TEST_TMPDIR/cdn"
    FETCH_LOG="$BATS_TEST_TMPDIR/fetch.log"
    GIT_LOG="$BATS_TEST_TMPDIR/git.log"
    export RELEASE_ROOT CDN_ROOT FETCH_LOG GIT_LOG
    # Every case, piped and BusyBox ones included, reads the CDN from CDN_ROOT,
    # which holds nothing until a case publishes there.
    export HELIX_PLUGIN_CDN_URL="$CDN_URL"
    mkdir -p "$CDN_ROOT"
    # The stub wget answers every Moonraker probe yet no plugin ever loads, so
    # the post-restart wait runs to its limit. Nothing here tests that wait.
    export HELIX_MOONRAKER_UP_SECS=1
    publish_release v1.2.3
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    stub_fetcher curl real
    stub_fetcher wget
    stub git 'printf "%s\n" "$*" >> "$GIT_LOG"; exit 1'
    stub sudo 'exit 0'
    stub service 'exit 0'
}

# The file list remote-install.sh fetches, read from the script itself.
plugin_files() {
    sed -n 's/^PLUGIN_FILES="\(.*\)"$/\1/p' "$REPO_ROOT/moonraker-plugin/remote-install.sh"
}

# Lay out release $1's plugin files the way raw.githubusercontent.com serves a
# tag: <ref>/moonraker-plugin/<file>, the wrapper itself included.
publish_release() {
    local dir="$RELEASE_ROOT/$1/moonraker-plugin"
    mkdir -p "$dir"
    for f in $(plugin_files); do
        mkdir -p "$(dirname "$dir/$f")"
        cp "$REPO_ROOT/moonraker-plugin/$f" "$dir/$f"
    done
    cp "$WRAPPER" "$dir/remote-install.sh"
}

# Publish release $1's plugin tarball where the release workflow puts it on
# the CDN.
publish_cdn() {
    mkdir -p "$CDN_ROOT/releases/$1"
    "$REPO_ROOT/scripts/package-helix-plugin.sh" "$CDN_ROOT/releases/$1/helix-print-plugin.tar.gz"
}

stub() {
    printf '#!/bin/sh\n%s\n' "$2" > "$BATS_TEST_TMPDIR/bin/$1"
    chmod +x "$BATS_TEST_TMPDIR/bin/$1"
}

# A curl or wget serving $BASE_URL from RELEASE_ROOT, failing as an HTTP error
# for a missing file, and answering anything else (Moonraker's /server/info)
# with success. `curl real` is curl proper; `curl impostor` is the Creality K1's
# libhv client, which rejects every flag the wrapper passes.
stub_fetcher() {
    local name="$1" kind="${2:-}"
    if [ "$kind" = impostor ]; then
        stub curl 'case "$1" in --version) echo "curl version 1.0.0"; exit 0;; esac
printf "impostor %s\n" "$*" >> "$FETCH_LOG"; echo "usage: curl url" >&2; exit 1'
        return
    fi
    cat > "$BATS_TEST_TMPDIR/bin/$name" <<'EOF'
#!/bin/sh
[ "$1" = --version ] && { echo "curl 8.5.0 (x86_64-pc-linux-gnu)"; exit 0; }
out="" url=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o|-O) out="$2"; shift 2 ;;
        -T|-A|--connect-timeout|--max-time|-U|--timeout) shift 2 ;;
        -*) shift ;;
        *) url="$1"; shift ;;
    esac
done
case "$url" in
    http://release.test/*)
        printf '%s %s\n' "$(basename "$0")" "$url" >> "$FETCH_LOG"
        src="$RELEASE_ROOT/${url#http://release.test/}"
        [ -f "$src" ] || exit 22
        cp "$src" "$out" ;;
    http://cdn.test/*)
        printf '%s %s\n' "$(basename "$0")" "$url" >> "$FETCH_LOG"
        src="$CDN_ROOT/${url#http://cdn.test/}"
        [ -f "$src" ] || exit 22
        cp "$src" "$out" ;;
esac
exit 0
EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/$name"
}

setup_home() {
    local home="$1"
    mkdir -p "$home/moonraker/moonraker/components" "$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0\n' > "$home/printer_data/config/moonraker.conf"
}

run_wrapper() {
    local home="$1"
    shift
    run env HOME="$home" HELIX_PLUGIN_BASE_URL="$BASE_URL" sh "$WRAPPER" "$@"
}

@test "remote-install.sh fetches every plugin module the plugin ships" {
    local listed f name
    listed=" $(plugin_files) "
    for f in "$REPO_ROOT"/moonraker-plugin/*.py "$REPO_ROOT"/moonraker-plugin/klippy/*.py; do
        name=${f#"$REPO_ROOT"/moonraker-plugin/}
        case "$listed" in
            *" $name "*) ;;
            *) echo "missing from PLUGIN_FILES: $name"; return 1 ;;
        esac
    done
}

@test "remote-install.sh passes POSIX syntax check" {
    sh -n "$WRAPPER"
}

@test "the wrapper installs the pinned release's plugin with its own install.sh" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"

    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"

    [ "$(readlink "$home/moonraker/moonraker/components/helix_print.py")" = "$home/helix_print/helix_print.py" ] ||
        fail "link: $(readlink "$home/moonraker/moonraker/components/helix_print.py")"
    cmp -s "$REPO_ROOT/moonraker-plugin/helix_print.py" "$home/helix_print/helix_print.py"
    grep -qx '\[helix_print\]' "$home/printer_data/config/moonraker.conf"
    ! grep -q 'update_manager' "$home/printer_data/config/moonraker.conf" || fail "wrote an updater entry"
    grep -q 'release.test/v1.2.3/moonraker-plugin/install.sh' "$FETCH_LOG" || fail "fetched: $(cat "$FETCH_LOG")"
    ! grep -v '/v1.2.3/' "$FETCH_LOG" || fail "fetched outside the pinned release"
}

@test "a release on the CDN installs from its tarball with no GitHub fetch" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    publish_cdn v1.2.3

    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"

    [ "$(readlink "$home/moonraker/moonraker/components/helix_print.py")" = "$home/helix_print/helix_print.py" ]
    cmp -s "$REPO_ROOT/moonraker-plugin/helix_print.py" "$home/helix_print/helix_print.py"
    grep -qx "curl $CDN_URL/releases/v1.2.3/helix-print-plugin.tar.gz" "$FETCH_LOG" ||
        fail "fetched: $(cat "$FETCH_LOG")"
    ! grep -q 'release.test' "$FETCH_LOG" || fail "fetched from GitHub: $(cat "$FETCH_LOG")"
}

@test "a release the CDN does not hold installs through that release's own wrapper on GitHub" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    # The release's wrapper differs from this one, so running it shows.
    sed -i.orig 's/info "Downloading the HelixPrint plugin/info "release wrapper"; &/' \
        "$RELEASE_ROOT/v1.2.3/moonraker-plugin/remote-install.sh"

    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"

    grep -q "cdn.test/releases/v1.2.3/helix-print-plugin.tar.gz" "$FETCH_LOG" || fail "never asked the CDN"
    grep -q 'release.test/v1.2.3/moonraker-plugin/remote-install.sh' "$FETCH_LOG" ||
        fail "fetched: $(cat "$FETCH_LOG")"
    [ "$(printf '%s\n' "$output" | grep -c 'release wrapper')" -eq 1 ] || fail "output: $output"
    cmp -s "$REPO_ROOT/moonraker-plugin/helix_print.py" "$home/helix_print/helix_print.py"
}

@test "a CDN tarball that is not the plugin is not installed" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    mkdir -p "$CDN_ROOT/releases/v1.2.3" "$BATS_TEST_TMPDIR/bogus"
    printf 'not the plugin\n' > "$BATS_TEST_TMPDIR/bogus/helix_print.py"
    tar -czf "$CDN_ROOT/releases/v1.2.3/helix-print-plugin.tar.gz" -C "$BATS_TEST_TMPDIR/bogus" helix_print.py

    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"
    grep -q 'release.test/v1.2.3/moonraker-plugin/helix_print.py' "$FETCH_LOG" ||
        fail "did not fall back to GitHub: $(cat "$FETCH_LOG")"
    cmp -s "$REPO_ROOT/moonraker-plugin/helix_print.py" "$home/helix_print/helix_print.py"
}

@test "a CDN tarball cut off mid-stream is not installed" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    mkdir -p "$CDN_ROOT/releases/v1.2.3"
    # The gzip stream ends early, right after a whole member: tar alone sees a
    # shorter archive that holds the files the content check reads, and only
    # gzip can tell it was cut.
    python3 - "$REPO_ROOT/moonraker-plugin" "$CDN_ROOT/releases/v1.2.3/helix-print-plugin.tar.gz" <<'PY'
import gzip, io, sys, tarfile
src, out = sys.argv[1], sys.argv[2]
buf = io.BytesIO()
t = tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT)
for name in ("install.sh", "helix_print_conf.sh", "helix_print.py"):
    t.add(f"{src}/{name}", arcname=name)
data = buf.getvalue()
open(out, "wb").write(gzip.compress(data)[:-8])
PY

    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"
    grep -q 'release.test/v1.2.3/moonraker-plugin/helix_print.py' "$FETCH_LOG" ||
        fail "did not fall back to GitHub: $(cat "$FETCH_LOG")"
    for f in $(plugin_files); do
        [ -f "$home/helix_print/$f" ] || fail "$f missing from the install"
    done
}

@test "a release the CDN holds uninstalls from its tarball" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    publish_cdn v1.2.3
    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"

    run_wrapper "$home" v1.2.3 --uninstall
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"
    [ ! -e "$home/helix_print" ] || fail "the plugin directory stayed"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
    ! grep -q 'release.test' "$FETCH_LOG" || fail "fetched from GitHub: $(cat "$FETCH_LOG")"
}

@test "a version without its v and with build metadata still fetches the release tag" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"

    run_wrapper "$home" 1.2.3+abc1234
    [ "$status" -eq 0 ] || fail "install failed: $output"
    grep -q '/v1.2.3/moonraker-plugin/helix_print.py' "$FETCH_LOG"
}

@test "a ref that is not a version pins main, as the app's own rule does" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    publish_release main

    run_wrapper "$home" dev
    [ "$status" -eq 0 ] || fail "install failed: $output"
    grep -q '/main/moonraker-plugin/helix_print.py' "$FETCH_LOG" || fail "fetched: $(cat "$FETCH_LOG")"
    ! grep -q 'cdn.test' "$FETCH_LOG" || fail "asked the CDN for main: $(cat "$FETCH_LOG")"
}

@test "the plugin tarball holds every file the wrapper fetches, at its top level" {
    local tgz="$BATS_TEST_TMPDIR/plugin.tar.gz"
    run "$REPO_ROOT/scripts/package-helix-plugin.sh" "$tgz"
    [ "$status" -eq 0 ] || fail "package failed: $output"
    [ "$(tar -tzf "$tgz" | sort)" = "$(plugin_files | tr ' ' '\n' | sort)" ] ||
        fail "tarball: $(tar -tzf "$tgz" | tr '\n' ' ')"
}

@test "the plugin tarball's files are owned by root, whoever packed it" {
    local tgz="$BATS_TEST_TMPDIR/plugin.tar.gz"
    "$REPO_ROOT/scripts/package-helix-plugin.sh" "$tgz"
    run python3 -c 'import sys, tarfile; print(sorted({(m.uid, m.gid) for m in tarfile.open(sys.argv[1])}))' "$tgz"
    [ "$output" = "[(0, 0)]" ] || fail "owners: $output"
}

@test "packaging refuses a plugin missing a file the wrapper fetches" {
    local root="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$root/scripts" "$root/moonraker-plugin"
    cp "$REPO_ROOT/scripts/package-helix-plugin.sh" "$root/scripts/"
    cp -R "$REPO_ROOT/moonraker-plugin/." "$root/moonraker-plugin/"
    rm "$root/moonraker-plugin/helix_plr.py"

    run "$root/scripts/package-helix-plugin.sh" "$BATS_TEST_TMPDIR/plugin.tar.gz"
    [ "$status" -ne 0 ] || fail "packaged an incomplete plugin"
    [[ "$output" == *helix_plr.py* ]] || fail "output: $output"
    [ ! -e "$BATS_TEST_TMPDIR/plugin.tar.gz" ] || fail "left a tarball behind"
}

@test "an uninstall that stops on an unexpected status keeps the plugin directory" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"
    printf '#!/bin/sh\nexit 130\n' > "$RELEASE_ROOT/v1.2.3/moonraker-plugin/install.sh"

    run_wrapper "$home" v1.2.3 --uninstall
    [ "$status" -ne 0 ] || fail "an interrupted uninstall reported success"
    [ -f "$home/helix_print/helix_print.py" ] || fail "the plugin directory went"
}

@test "the wrapper piped into sh runs to the end without git or bash" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    # bash and git are not on this PATH: only the stubs and coreutils.
    stub bash 'echo bash >> "$GIT_LOG"; exit 1'

    run env HOME="$home" HELIX_PLUGIN_BASE_URL="$BASE_URL" sh -c 'sh -s -- v1.2.3 < "$1"' _ "$WRAPPER"
    [ "$status" -eq 0 ] || fail "install failed: $output"

    [ -L "$home/moonraker/moonraker/components/helix_print.py" ]
    [ ! -e "$GIT_LOG" ] || fail "ran: $(cat "$GIT_LOG")"
}

@test "the wrapper runs under BusyBox sh" {
    command -v busybox >/dev/null 2>&1 || skip "busybox not installed"
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"

    run env HOME="$home" HELIX_PLUGIN_BASE_URL="$BASE_URL" busybox sh "$WRAPPER" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"
    [ -L "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "an impostor curl falls through to wget" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    stub_fetcher curl impostor

    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"

    ! grep -q '^impostor' "$FETCH_LOG" || fail "passed flags to the impostor: $(cat "$FETCH_LOG")"
    grep -q '^wget http://release.test/v1.2.3/' "$FETCH_LOG" || fail "fetched: $(cat "$FETCH_LOG")"
    [ -L "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "a moonraker.conf with no trailing newline keeps its last line" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    printf '[server]\nhost: 0.0.0.0' > "$home/printer_data/config/moonraker.conf"

    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"

    grep -qx 'host: 0.0.0.0' "$home/printer_data/config/moonraker.conf" ||
        fail "last line damaged: $(cat "$home/printer_data/config/moonraker.conf")"
    grep -qx '\[helix_print\]' "$home/printer_data/config/moonraker.conf"
}

@test "on a Creality K1 the plugin links into the nested Moonraker the process names" {
    local root="$BATS_TEST_TMPDIR/k1"
    local pkg="$root/usr/data/moonraker/moonraker/moonraker"
    local data="$root/usr/data/printer_data"
    mkdir -p "$pkg/components" "$data/config" "$root/root" "$root/proc/1444"
    : > "$pkg/moonraker.py"
    printf '[server]\nhost: 0.0.0.0\n' > "$data/config/moonraker.conf"
    printf '%s\0' "$root/usr/data/moonraker/moonraker-env/bin/python" "$pkg/moonraker.py" \
        -d "$data" > "$root/proc/1444/cmdline"
    stub_fetcher curl impostor

    run env HOME="$root/root" HELIX_PROC_ROOT="$root/proc" HELIX_PLUGIN_BASE_URL="$BASE_URL" \
        sh "$WRAPPER" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"

    [ "$(readlink "$pkg/components/helix_print.py")" = "$root/root/helix_print/helix_print.py" ]
    grep -qx '\[helix_print\]' "$data/config/moonraker.conf"
}

@test "a release that is missing a file installs nothing and keeps the plugin in place" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"
    cp -R "$home/printer_data/config" "$BATS_TEST_TMPDIR/config.before"

    publish_release v1.2.4
    rm "$RELEASE_ROOT/v1.2.4/moonraker-plugin/helix_3mf.py"
    run_wrapper "$home" v1.2.4
    [ "$status" -ne 0 ]
    contains "helix_3mf.py" "$output"

    [ -f "$home/helix_print/helix_3mf.py" ] || fail "the installed plugin lost a file"
    [ -L "$home/moonraker/moonraker/components/helix_print.py" ]
    diff -r "$BATS_TEST_TMPDIR/config.before" "$home/printer_data/config" || fail "config changed"
    [ -z "$(find "$home" -maxdepth 1 -name '.helix_print.stage.*')" ] || fail "left a staging dir"
}

@test "uninstall through the wrapper removes everything the install added" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    printf '[server]\nhost: 0.0.0.0\n\n[octoprint_compat]\n' > "$home/printer_data/config/moonraker.conf"
    cp "$home/printer_data/config/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"

    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"
    run_wrapper "$home" v1.2.3 --uninstall
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"

    [ ! -e "$home/helix_print" ] || fail "plugin dir left: $(ls -A "$home/helix_print")"
    [ ! -L "$home/moonraker/moonraker/components/helix_print.py" ] || fail "link left"
    cmp -s "$BATS_TEST_TMPDIR/conf.before" "$home/printer_data/config/moonraker.conf" ||
        fail "moonraker.conf differs: $(diff "$BATS_TEST_TMPDIR/conf.before" "$home/printer_data/config/moonraker.conf")"
    [ -z "$(find "$home" -maxdepth 1 -name '.helix_print*')" ] || fail "left a staging dir"
}

# What an older remote-install.sh left: a sparse git clone of main in
# ~/helix_print, linked from its moonraker-plugin/ subdir, and a commented
# [helix_print] plus a git updater entry appended to moonraker.conf.
setup_git_clone_install() {
    local home="$1"
    local c="$home/printer_data/config/moonraker.conf"
    mkdir -p "$home/helix_print/.git" "$home/helix_print/moonraker-plugin"
    cp "$REPO_ROOT/moonraker-plugin/helix_print.py" "$home/helix_print/moonraker-plugin/"
    ln -s "$home/helix_print/moonraker-plugin/helix_print.py" \
        "$home/moonraker/moonraker/components/helix_print.py"
    cat >> "$c" <<EOF

#~# --- HelixPrint Plugin ---
[helix_print]
# enabled: True
# temp_dir: .helix_temp
# symlink_dir: .helix_print
# cleanup_delay: 86400

[update_manager helix_print]
type: git_repo
origin: https://github.com/prestonbrown/helixscreen.git
path: $home/helix_print
primary_branch: main
managed_services: moonraker
EOF
}

@test "installing over a git-clone install replaces the clone and drops its updater entry" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    setup_git_clone_install "$home"

    run_wrapper "$home" v1.2.3
    [ "$status" -eq 0 ] || fail "install failed: $output"

    [ ! -e "$home/helix_print/.git" ] || fail "the clone is still there"
    [ "$(readlink "$home/moonraker/moonraker/components/helix_print.py")" = "$home/helix_print/helix_print.py" ]
    local c="$home/printer_data/config/moonraker.conf"
    ! grep -q '^\[update_manager helix_print\]' "$c" || fail "updater entry left: $(cat "$c")"
    [ "$(grep -c '^\[helix_print\]' "$c")" -eq 1 ] || fail "sections: $(cat "$c")"
}

@test "an install.sh that fails over a git-clone install puts the clone back" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    setup_git_clone_install "$home"
    local link="$home/moonraker/moonraker/components/helix_print.py"
    publish_release v1.2.4
    printf '#!/bin/sh\necho "install refused" >&2\nexit 1\n' > "$RELEASE_ROOT/v1.2.4/moonraker-plugin/install.sh"

    run_wrapper "$home" v1.2.4
    [ "$status" -ne 0 ] || fail "a failed install.sh reported success"

    [ -e "$link" ] || fail "the plugin link dangles: $(readlink "$link")"
    [ -d "$home/helix_print/.git" ] || fail "the clone was not put back: $(ls -A "$home/helix_print")"
    [ -z "$(find "$home" -maxdepth 1 -name '.helix_print.stage.*' -o -maxdepth 1 -name 'helix_print.old.*')" ] ||
        fail "left: $(ls -A "$home")"
}

@test "an install that fails on a config write after its checks leaves the clone's link resolving" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    setup_git_clone_install "$home"
    local link="$home/moonraker/moonraker/components/helix_print.py"
    local before
    before=$(readlink "$link")
    # The updater entry's removal cannot write the config.
    local real
    real=$(command -v awk)
    stub awk "case \"\$*\" in *'skip = 1'*) [ -f /dev/stdout ] && exit 1 ;; esac
exec $real \"\$@\""

    run_wrapper "$home" v1.2.3
    [ "$status" -ne 0 ] || fail "a failed install reported success"

    [ "$(readlink "$link")" = "$before" ] || fail "link re-pointed to $(readlink "$link")"
    [ -e "$link" ] || fail "the plugin link dangles: $(readlink "$link")"
}

# Moonraker's /server/info never answers; every other URL goes to the fetcher
# stubs. The restart's waits run on a fake clock, so they cost no real time.
silence_moonraker() {
    local name
    for name in curl wget; do
        mv "$BATS_TEST_TMPDIR/bin/$name" "$BATS_TEST_TMPDIR/bin/$name.real"
        stub "$name" "case \"\$*\" in *moonraker.test*) exit 7 ;; esac
exec \"$BATS_TEST_TMPDIR/bin/$name.real\" \"\$@\""
    done
    mock_fake_clock
}

@test "a Moonraker that is down after the restart is reported, and the finished install kept" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    setup_git_clone_install "$home"
    local link="$home/moonraker/moonraker/components/helix_print.py"
    silence_moonraker

    run env HOME="$home" HELIX_PLUGIN_BASE_URL="$BASE_URL" MOONRAKER_URL=http://moonraker.test:7125 \
        sh "$WRAPPER" v1.2.3
    [ "$status" -eq 0 ] || fail "a slow restart failed the install: $output"

    contains "Moonraker is not running" "$output"
    [ "$(readlink "$link")" = "$home/helix_print/helix_print.py" ] || fail "link: $(readlink "$link")"
    [ -e "$link" ] || fail "the plugin link dangles"
    [ ! -e "$home/helix_print/.git" ] || fail "the old clone was restored over the new install"
}

@test "a failed fresh install does not claim a previous plugin is back in place" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    publish_release v1.2.4
    printf '#!/bin/sh\nexit 1\n' > "$RELEASE_ROOT/v1.2.4/moonraker-plugin/install.sh"

    run_wrapper "$home" v1.2.4
    [ "$status" -ne 0 ]
    contains "Install failed" "$output"
    ! contains "is back in place" "$output" || fail "claimed a restore: $output"
}

@test "a failed install over a previous plugin says the previous one is back in place" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    setup_git_clone_install "$home"
    publish_release v1.2.4
    printf '#!/bin/sh\nexit 1\n' > "$RELEASE_ROOT/v1.2.4/moonraker-plugin/install.sh"

    run_wrapper "$home" v1.2.4
    [ "$status" -ne 0 ]
    contains "the previous $home/helix_print is back in place" "$output"
}

@test "uninstalling a git-clone install leaves moonraker.conf as it was before it" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    cp "$home/printer_data/config/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"
    setup_git_clone_install "$home"

    run_wrapper "$home" v1.2.3 --uninstall
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"

    [ ! -e "$home/helix_print" ] || fail "the clone is still there"
    [ ! -L "$home/moonraker/moonraker/components/helix_print.py" ] || fail "link left"
    cmp -s "$BATS_TEST_TMPDIR/conf.before" "$home/printer_data/config/moonraker.conf" ||
        fail "moonraker.conf differs: $(diff "$BATS_TEST_TMPDIR/conf.before" "$home/printer_data/config/moonraker.conf")"
}

@test "the wrapper refuses a ~/helix_print that is not the plugin" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_home "$home"
    mkdir -p "$home/helix_print"
    : > "$home/helix_print/notes.txt"

    run_wrapper "$home" v1.2.3
    [ "$status" -ne 0 ]
    [ -f "$home/helix_print/notes.txt" ]
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "the wrapper fetches every file the release ships in moonraker-plugin" {
    local listed shipped
    listed=$(sed -n 's/^PLUGIN_FILES="\(.*\)"$/\1/p' "$WRAPPER")
    [ -n "$listed" ] || fail "no PLUGIN_FILES in $WRAPPER"
    for shipped in "$REPO_ROOT"/moonraker-plugin/*.py "$REPO_ROOT"/moonraker-plugin/*.sh \
            "$REPO_ROOT"/moonraker-plugin/klippy/*.py; do
        shipped=${shipped#"$REPO_ROOT"/moonraker-plugin/}
        [ "$shipped" = remote-install.sh ] && continue
        case " $listed " in
            *" $shipped "*) ;;
            *) fail "$shipped ships in moonraker-plugin but the wrapper does not fetch it" ;;
        esac
    done
}

@test "the wrapper carries the main installer's fetch helpers unchanged" {
    local fn a b src
    for fn in _has_real_curl:release.sh _has_python:common.sh _py_get:release.sh; do
        src="$REPO_ROOT/scripts/lib/installer/${fn#*:}"
        fn="${fn%%:*}"
        a=$(sed -n "/^$fn() {\$/,/^}\$/p" "$WRAPPER")
        b=$(sed -n "/^$fn() {\$/,/^}\$/p" "$src")
        [ -n "$a" ] || fail "no $fn in $WRAPPER"
        [ "$a" = "$b" ] || fail "$fn differs from $src"
    done
    for line in '_py_download() { _py_get "$1" "$2" "${3:-300}"; }' '_INSTALLER_UA="helixscreen-installer/1.0"'; do
        grep -qxF "$line" "$WRAPPER" || fail "missing: $line"
        grep -qxF "$line" "$REPO_ROOT/scripts/lib/installer/release.sh" || fail "release.sh lacks: $line"
    done
}
