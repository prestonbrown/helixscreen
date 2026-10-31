#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The build-hosts file (scripts/lib/build_hosts.sh): one parser, which the
# scripts source and make asks through $(shell). The file names the hosts, the
# environment beats the file on both sides, a line the parser cannot take is
# named and skipped, and a make that uses no remote host never runs the parser,
# so a broken file cannot break an ordinary build.

ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LIB="$ROOT/scripts/lib/build_hosts.sh"

setup() {
    load helpers
    cd "$(scratch_tree)" || return 1
    mock_command_script ssh 'touch "$BATS_TEST_TMPDIR/ssh-called"; exit 255'
    # A makefile that prints what a recipe under mk/remote.mk sees.
    SHOW="$BATS_TEST_TMPDIR/show.mk"
    cat > "$SHOW" <<'MK'
show-remote:
	@echo 'host=[$(REMOTE_HOST)] dir=[$(REMOTE_DIR)] user=[$(REMOTE_USER)]'
show-env:
	@echo "env-test-host=[$$HELIX_TEST_HOST] env-build-dir=[$$REMOTE_BUILD_DIR]"
MK
}

hosts() { printf '%s\n' "$@" > "$HELIX_BUILD_HOSTS_FILE"; }
via_make() { run make -s -f mk/remote.mk -f "$SHOW" "$@"; }
via_shell() { run bash -c '. "$1"; echo "host=[${REMOTE_HOST:-}] dir=[${REMOTE_DIR:-}] user=[${REMOTE_USER:-}]"' _ "$LIB"; }

@test "make and the shell read the same values from the file" {
    hosts REMOTE_HOST=fromfile.invalid REMOTE_DIR=/srv/fromfile
    via_make show-remote
    [ "$status" -eq 0 ]
    contains "host=[fromfile.invalid] dir=[/srv/fromfile]" "$output"
    via_shell
    contains "host=[fromfile.invalid] dir=[/srv/fromfile]" "$output"
}

@test "the environment beats the file, through make and in the shell" {
    hosts REMOTE_HOST=fromfile.invalid REMOTE_DIR=/srv/fromfile
    REMOTE_HOST=fromenv.invalid via_make show-remote
    contains "host=[fromenv.invalid] dir=[/srv/fromfile]" "$output"
    REMOTE_HOST=fromenv.invalid via_shell
    contains "host=[fromenv.invalid] dir=[/srv/fromfile]" "$output"
    # The make command line beats both.
    REMOTE_HOST=fromenv.invalid via_make show-remote REMOTE_HOST=cmdline.invalid
    contains "host=[cmdline.invalid]" "$output"
}

@test "make hands recipes the developer's environment, never the file's value for it" {
    hosts HELIX_TEST_HOST=filehost.invalid REMOTE_BUILD_DIR=/file
    HELIX_TEST_HOST=envhost.invalid REMOTE_BUILD_DIR=/env via_make show-env
    [ "$status" -eq 0 ]
    contains "env-test-host=[envhost.invalid] env-build-dir=[/env]" "$output"
}

@test "a make that uses no remote host never runs the parser, so a broken file cannot stop it" {
    hosts 'HELIX_TEST_HOST zeus' 'export REMOTE_HOST=x' '  REMOTE_DIR=/x'
    run make -s -f mk/remote.mk -f "$SHOW" show-env
    [ "$status" -eq 0 ]
    lacks "build-hosts" "$output"
    lacks "missing separator" "$output"
}

@test "a line the parser cannot take is named and skipped; the good lines still apply" {
    hosts 'HELIX_TEST_HOST zeus' 'REMOTE_HOST=good.invalid' 'export REMOTE_USER=x' '  REMOTE_DIR=/indented'
    via_shell
    contains "host=[good.invalid] dir=[] user=[]" "$output"
    contains ":1: not KEY=VALUE" "$output"
    contains ":3: not KEY=VALUE" "$output"
    contains ":4: not KEY=VALUE" "$output"
    via_make show-remote
    contains "host=[good.invalid] dir=[] user=[]" "$output"
}

@test "values are literal: no \$ expansion, and nothing in the file runs" {
    hosts 'REMOTE_DIR=$HOME/src' 'REMOTE_HOST=$(id)' 'REMOTE_USER=`id`'
    via_shell
    contains 'host=[$(id)] dir=[$HOME/src] user=[`id`]' "$output"
    via_make show-remote
    contains 'host=[$(id)] dir=[$HOME/src] user=[`id`]' "$output"
}

@test "a trailing comment or a quoted value is refused, not half-read" {
    hosts 'REMOTE_HOST=zeus.invalid # the big box' 'REMOTE_DIR="/srv/quoted"'
    via_shell
    contains "host=[] dir=[]" "$output"
    contains ":1: REMOTE_HOST: whitespace in the value" "$output"
    contains ":2: REMOTE_DIR: quotes in the value" "$output"
    via_make show-remote
    contains "host=[] dir=[]" "$output"
}

@test "the last of two lines for one key wins, on both sides" {
    hosts REMOTE_HOST=first.invalid REMOTE_HOST=last.invalid
    via_shell
    contains "host=[last.invalid]" "$output"
    via_make show-remote
    contains "host=[last.invalid]" "$output"
}

@test "a CRLF file reads the same as an LF one" {
    printf 'REMOTE_HOST=crlf.invalid\r\nREMOTE_DIR=/srv/crlf\r\n' > "$HELIX_BUILD_HOSTS_FILE"
    via_shell
    contains "host=[crlf.invalid] dir=[/srv/crlf]" "$output"
    via_make show-remote
    contains "host=[crlf.invalid] dir=[/srv/crlf]" "$output"
}

@test "only the documented keys are ever set" {
    hosts "BASH_ENV=$BATS_TEST_TMPDIR/evil" LD_PRELOAD=/evil.so REMOTE_HOST=ok.invalid
    run bash -c '. "$1"; echo "bash_env=[${BASH_ENV:-}] preload=[${LD_PRELOAD:-}] host=[$REMOTE_HOST]"' _ "$LIB"
    contains "bash_env=[] preload=[] host=[ok.invalid]" "$output"
    contains ":1: BASH_ENV is not a build-hosts key" "$output"
    # The allowlist is the parser's own, and it lists what the docs list.
    run "$LIB" --keys
    [ "$status" -eq 0 ]
    for k in HELIX_TEST_HOST HELIX_TEST_CONTAINER HELIX_TEST_WORKDIR HELIX_TEST_TREES_HOST \
             HELIX_TEST_TREES HELIX_TEST_CCACHE HELIX_TEST_LOCK_DIR HELIX_TEST_HOST_AUTO \
             HELIX_TEST_MEMORY REMOTE_HOST REMOTE_USER REMOTE_DIR REMOTE_BUILD_DIR \
             HELIX_LAND_GATE_HOSTS; do
        contains "$k" "$output"
    done
}

@test "the test container's memory limit is a key the file can set" {
    hosts HELIX_TEST_MEMORY=96g
    run bash -c '. "$1"; echo "mem=[${HELIX_TEST_MEMORY:-}]"' _ "$LIB"
    [ "$status" -eq 0 ]
    contains "mem=[96g]" "$output"
    lacks "is not a build-hosts key" "$output"
}

@test "the land-gate hosts pre-push asks are a key the file can set, commas kept" {
    hosts HELIX_LAND_GATE_HOSTS=helix-dev,zeus.local
    run bash -c '. "$1"; echo "gate=[${HELIX_LAND_GATE_HOSTS:-}]"' _ "$LIB"
    [ "$status" -eq 0 ]
    contains "gate=[helix-dev,zeus.local]" "$output"
    lacks "is not a build-hosts key" "$output"
}

@test "a probe with a remote command returns that command's status, 255 for ssh's own failure" {
    mock_command_script ssh 'while [ $# -gt 1 ]; do shift; done
case "$1" in true) exit 0 ;; say) echo said; exit 0 ;; no) echo partial; exit 1 ;; *) echo "ssh: connect to host x port 22: Connection refused" >&2; exit 255 ;; esac'
    run bash -c '. "$1"; probe_build_host h.invalid' _ "$LIB"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    # On success the command's stdout is the probe's; on failure it is not.
    run bash -c '. "$1"; probe_build_host h.invalid say' _ "$LIB"
    [ "$status" -eq 0 ]
    [ "$output" = said ]
    run bash -c '. "$1"; probe_build_host h.invalid no' _ "$LIB"
    [ "$status" -eq 1 ]
    lacks partial "$output"
    run bash -c '. "$1"; probe_build_host h.invalid refuse' _ "$LIB"
    [ "$status" -eq 255 ]
    contains "refused" "$output"
}

# An ssh that saves what each call reads on stdin (the remote scripts
# remote-build.sh sends) and succeeds without reaching anything.
capture_ssh() {
    mock_command_script ssh 'n=$(ls "$BATS_TEST_TMPDIR"/ssh-in-* 2>/dev/null | wc -l | tr -d " ")
case " $* " in *" -O "*) exit 0 ;; esac
cat > "$BATS_TEST_TMPDIR/ssh-in-$n"; exit 0'
    mock_command_script scp 'exit 0'
}

@test "remote-build.sh passes no -j unless JOBS names one, so the remote make sizes its own" {
    capture_ssh
    hosts REMOTE_HOST=ok.invalid
    # It reads only git (a diff, the untracked list) and writes a temp dir, so the
    # real checkout is safe; the scratch tree has no .git for it to read.
    cd "$ROOT"
    run scripts/remote-build.sh test
    [ "$status" -eq 0 ] || fail "remote-build failed: $output"
    build=$(grep -l "JOBS_ARG=" "$BATS_TEST_TMPDIR"/ssh-in-*)
    [ -n "$build" ] || fail "no build script captured: $(ls "$BATS_TEST_TMPDIR")"
    grep -qx 'JOBS_ARG=""' "$build" || fail "$(grep JOBS_ARG= "$build")"

    rm -f "$BATS_TEST_TMPDIR"/ssh-in-*
    JOBS=-j7 run scripts/remote-build.sh test
    [ "$status" -eq 0 ] || fail "remote-build failed: $output"
    build=$(grep -l "JOBS_ARG=" "$BATS_TEST_TMPDIR"/ssh-in-*)
    grep -qx 'JOBS_ARG="-j7"' "$build" || fail "$(grep JOBS_ARG= "$build")"
}

@test "a file that exists but cannot be read says so" {
    [ "$(id -u)" != 0 ] || skip "root reads a mode-000 file anyway"
    hosts REMOTE_HOST=hidden.invalid
    chmod 000 "$HELIX_BUILD_HOSTS_FILE"
    run scripts/remote-build.sh test
    [ "$status" -ne 0 ]
    contains "cannot read $HELIX_BUILD_HOSTS_FILE" "$output"
}

@test "a remote make target with no host refuses in one line, naming the file" {
    run make -s -f mk/remote.mk remote-ssh
    [ "$status" -ne 0 ]
    [ "${#lines[@]}" -eq 1 ]
    contains "REMOTE_HOST is not set" "$output"
    contains "$HELIX_BUILD_HOSTS_FILE" "$output"
    # The rsync targets need the directory too: --delete into "host:/" is what
    # an empty one would mean.
    REMOTE_HOST=h.invalid run make -s -f mk/remote.mk remote-sync
    [ "$status" -ne 0 ]
    contains "REMOTE_DIR is not set" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-called" ]
}

@test "remote-build.sh with no host refuses in one line and reaches nothing" {
    run scripts/remote-build.sh test
    [ "$status" -eq 2 ]
    [ "${#lines[@]}" -eq 1 ]
    contains "REMOTE_HOST is not set" "$output"
    contains "$HELIX_BUILD_HOSTS_FILE" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-called" ]
}

# An ssh that logs each target it is asked for, then answers as the host's
# name says: up.* answers, hang.* never returns, anything else is a name the
# resolver cannot look up.
mock_candidate_ssh() {
    mock_command_script ssh 'for a; do [ "$a" = true ] && break; h=$a; done
echo "$h" >> "$BATS_TEST_TMPDIR/ssh-log"
case "$h" in
    *up.*) exit 0 ;;
    *hang.*) exec sleep 30 ;;
    *) echo "ssh: Could not resolve hostname $h: nodename nor servname provided, or not known" >&2; exit 255 ;;
esac'
}
resolve() { run bash -c '. "$1"; resolve_build_host "$2"; rc=$?; echo "value=[${!2}]"; exit $rc' _ "$LIB" "$@"; }

@test "a single host is used as written and never probed" {
    mock_candidate_ssh
    REMOTE_HOST=dead.invalid resolve REMOTE_HOST
    [ "$status" -eq 0 ]
    contains "value=[dead.invalid]" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-log" ]
}

@test "a candidate list resolves to the first host that answers, and names the pick" {
    mock_candidate_ssh
    hosts HELIX_TEST_HOST=dead.invalid,up.invalid,up2.invalid
    resolve HELIX_TEST_HOST
    [ "$status" -eq 0 ]
    contains "value=[up.invalid]" "$output"
    contains "→ HELIX_TEST_HOST=up.invalid (not answering: dead.invalid: no such name)" "$output"
    [ "$(cat "$BATS_TEST_TMPDIR/ssh-log")" = "$(printf 'dead.invalid\nup.invalid')" ]
}

@test "the remote user goes with every REMOTE_HOST candidate" {
    mock_candidate_ssh
    REMOTE_USER=pb REMOTE_HOST=dead.invalid,up.invalid resolve REMOTE_HOST
    [ "$status" -eq 0 ]
    contains "value=[up.invalid]" "$output"
    [ "$(cat "$BATS_TEST_TMPDIR/ssh-log")" = "$(printf 'pb@dead.invalid\npb@up.invalid')" ]
}

@test "a candidate that hangs is killed at the limit and the next one is tried" {
    mock_candidate_ssh
    start=$(date +%s)
    HELIX_HOST_PROBE_SECS=1 REMOTE_HOST=hang.invalid,up.invalid resolve REMOTE_HOST
    [ "$status" -eq 0 ]
    [ $(( $(date +%s) - start )) -le 4 ]
    contains "value=[up.invalid]" "$output"
    contains "hang.invalid: timeout after 1s" "$output"
}

@test "no candidate answering fails in one line naming every one tried" {
    mock_candidate_ssh
    HELIX_HOST_PROBE_SECS=1 HELIX_TEST_HOST=dead.invalid,hang.invalid resolve HELIX_TEST_HOST
    [ "$status" -ne 0 ]
    contains "✗ HELIX_TEST_HOST: no candidate answered ssh (tried dead.invalid: no such name; hang.invalid: timeout after 1s)" "$output"
    # require_build_host refuses the same way scripts expect of a missing host.
    HELIX_HOST_PROBE_SECS=1 HELIX_TEST_HOST=dead.invalid run bash -c '. "$1"; HELIX_TEST_HOST=dead.invalid,dead2.invalid; require_build_host HELIX_TEST_HOST' _ "$LIB"
    [ "$status" -eq 2 ]
    contains "no candidate answered" "$output"
}

@test "a resolved host is not probed again, in the same shell or a child" {
    mock_candidate_ssh
    run bash -c '. "$1"; REMOTE_HOST=dead.invalid,up.invalid
        resolve_build_host REMOTE_HOST && require_build_host REMOTE_HOST &&
        bash -c ". \"$1\"; resolve_build_host REMOTE_HOST; echo child=[\$REMOTE_HOST]"' _ "$LIB"
    [ "$status" -eq 0 ]
    contains "child=[up.invalid]" "$output"
    [ "$(wc -l < "$BATS_TEST_TMPDIR/ssh-log")" -eq 2 ]
}

@test "--make resolves a host key, and leaves any other key's commas alone" {
    mock_candidate_ssh
    hosts REMOTE_HOST=dead.invalid,up.invalid REMOTE_DIR=/a,b
    run "$LIB" --make REMOTE_HOST
    [ "$status" -eq 0 ]
    [ "${lines[${#lines[@]}-1]}" = up.invalid ]
    run "$LIB" --make REMOTE_DIR
    [ "$output" = "/a,b" ]
    via_make show-remote
    contains "host=[up.invalid] dir=[/a,b]" "$output"
}

@test "a candidate list with whitespace or quotes is refused like any value" {
    mock_candidate_ssh
    hosts 'REMOTE_HOST=up.invalid, dead.invalid' 'HELIX_TEST_HOST="up.invalid,dead.invalid"'
    run bash -c '. "$1"; echo "host=[${REMOTE_HOST:-}] test=[${HELIX_TEST_HOST:-}]"' _ "$LIB"
    contains "host=[] test=[]" "$output"
    contains "REMOTE_HOST: whitespace in the value" "$output"
    contains "HELIX_TEST_HOST: quotes in the value" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-log" ]
}
