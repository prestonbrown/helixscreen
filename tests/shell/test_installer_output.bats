#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The installer's output layer in common.sh: what reaches the screen, what
# reaches the log file, and how terminal, color and UTF-8 are decided.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    unset HELIX_INSTALL_VERBOSE COLORTERM NO_COLOR HELIX_INSTALL_LOGO LC_TERMINAL TERM_PROGRAM
    export HELIX_INSTALL_TTY=0
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
}

@test "log_info is silent on screen by default" {
    run log_info "detail line"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "log_info prints with HELIX_INSTALL_VERBOSE=1" {
    HELIX_INSTALL_VERBOSE=1
    run log_info "detail line"
    [[ "$output" == *"[INFO] detail line"* ]]
}

@test "log_warn and log_error always print" {
    run log_warn "careful"
    contains "[WARN] careful" "$output"
    run log_error "broken"
    contains "[ERROR] broken" "$output"
}

@test "log_note prints a plain indented line" {
    run log_note "edit config from Mainsail"
    [ "$output" = "    edit config from Mainsail" ]
}

@test "lines before log_open are buffered, then flushed with timestamps" {
    log_info "early one"
    log_warn "early two"
    log_open "$BATS_TEST_TMPDIR/install.log"
    log_info "after open"
    run cat "$BATS_TEST_TMPDIR/install.log"
    [[ "${lines[0]}" =~ ^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\]\ INFO\ early\ one$ ]] || fail "line 0: ${lines[0]}"
    [[ "${lines[1]}" =~ ^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\]\ WARN\ early\ two$ ]] || fail "line 1: ${lines[1]}"
    [[ "${lines[2]}" =~ INFO\ after\ open$ ]]
}

@test "the log file never contains color escapes" {
    HELIX_INSTALL_TTY=1 ui_detect
    log_open "$BATS_TEST_TMPDIR/install.log"
    log_warn "${BOLD}colored${NC} on screen"
    run grep -c "$(printf '\033')" "$BATS_TEST_TMPDIR/install.log"
    [ "$output" = "0" ]
}

@test "no terminal means no color" {
    HELIX_INSTALL_TTY=0 ui_detect
    [ "$UI_TTY" = 0 ]
    [ "$UI_COLOR" = 0 ]
    [ -z "$CYAN" ]
}

@test "a forced terminal with TERM=dumb gets no color" {
    TERM=dumb HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 0 ]
}

@test "NO_COLOR disables color on a terminal" {
    NO_COLOR=1 TERM=xterm-256color HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 0 ]
}

@test "256-color terminals are detected from TERM" {
    TERM=xterm-256color HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 256 ]
    TERM=vt100 HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 16 ]
}

@test "UTF-8 is read from LC_ALL, then LC_CTYPE, then LANG" {
    LC_ALL= LC_CTYPE= LANG=en_US.UTF-8 ui_detect;  [ "$UI_UTF8" = 1 ]
    LC_ALL=C LC_CTYPE= LANG=en_US.UTF-8 ui_detect; [ "$UI_UTF8" = 0 ]
    LC_ALL= LC_CTYPE=C.utf8 LANG= ui_detect;       [ "$UI_UTF8" = 1 ]
    LC_ALL= LC_CTYPE= LANG= ui_detect;             [ "$UI_UTF8" = 0 ]
}

@test "the output layer runs under busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; log_note hi; log_open "$2"; log_warn w; cat "$2"' \
        _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh" "$BATS_TEST_TMPDIR/a.log"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN w"* ]]
}

@test "no terminal: a step prints one numbered line when done" {
    STEP_TOTAL=8
    step "Downloaded"
    run step_done "100 MB, SHA256 verified"
    [ "$output" = "[1/8] Downloaded ... ok (100 MB, SHA256 verified)" ]
}

@test "no terminal: a step with two titles prints the done title when done" {
    STEP_TOTAL=8
    step "Downloading" "Downloaded"
    run step_done "100 MB"
    [ "$output" = "[1/8] Downloaded ... ok (100 MB)" ]
}

@test "no terminal: a step with two titles fails under its running title" {
    STEP_TOTAL=8
    step "Starting HelixScreen" "Started HelixScreen"
    run step_fail
    [ "$output" = "[1/8] Starting HelixScreen ... FAILED" ]
}

@test "terminal: the spinner shows the running title, the done line the done title" {
    NO_COLOR=1 HELIX_INSTALL_TTY=1 LANG=en_US.UTF-8 TERM=vt100 ui_detect
    run _two_title_step
    contains "Starting HelixScreen…" "$output"
    contains "✓ Started HelixScreen" "$output"
}

@test "the log records the running title and the done title" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    step "Downloading" "Downloaded"; step_done "100 MB" >/dev/null 2>&1
    grep -qx '\[[0-9:]*\] STEP Downloading' "$BATS_TEST_TMPDIR/install.log" || fail "no STEP Downloading"
    grep -qx '\[[0-9:]*\] DONE Downloaded (100 MB)' "$BATS_TEST_TMPDIR/install.log" || fail "no DONE Downloaded"
}

@test "no terminal: nothing prints until the step resolves" {
    STEP_TOTAL=8
    run step "Downloaded"
    [ -z "$output" ]
}

@test "no terminal: steps without a total are not numbered" {
    STEP_TOTAL=0
    step "Checked system"
    run step_done
    [ "$output" = "Checked system ... ok" ]
}

@test "no terminal: a failed step says FAILED" {
    STEP_TOTAL=3
    step "Installing libraries"
    run step_fail
    [ "$output" = "[1/3] Installing libraries ... FAILED" ]
}

@test "step_skip prints nothing and does not consume a number" {
    STEP_TOTAL=2
    step "Installing libraries"; step_skip
    step "Downloaded"
    run step_done
    [ "$output" = "[1/2] Downloaded ... ok" ]
}

@test "warnings inside a step are indented under it" {
    STEP_TOTAL=2
    step "Downloaded"
    run log_warn "plain-HTTP mirror"
    [ "$output" = "      [WARN] plain-HTTP mirror" ]
}

@test "terminal + UTF-8: done line uses a check mark and erases the spinner" {
    NO_COLOR=1 HELIX_INSTALL_TTY=1 LANG=en_US.UTF-8 TERM=vt100 ui_detect
    step "Downloaded"
    run step_done "100 MB"
    contains $'\r' "$output"
    contains "✓ Downloaded" "$output"
    contains "100 MB" "$output"
}

@test "terminal without UTF-8 falls back to ASCII marks" {
    NO_COLOR=1 HELIX_INSTALL_TTY=1 LC_ALL=C LANG=C TERM=vt100 ui_detect
    step "Downloaded"
    run step_done
    contains "ok Downloaded" "$output"
    case "$output" in *"✓"*) fail "unicode mark on a non-UTF-8 terminal" ;; esac
}

@test "steps are recorded in the log" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    step "Downloaded"; step_done "100 MB" >/dev/null 2>&1
    run grep -E 'STEP Downloaded|DONE Downloaded \(100 MB\)' "$BATS_TEST_TMPDIR/install.log"
    [ "${#lines[@]}" -eq 2 ]
}

_two_redraws() { step "Downloaded"; log_warn a; log_warn b; }
_two_title_step() { step "Starting HelixScreen" "Started HelixScreen"; step_done; }

@test "terminal + UTF-8: the spinner advances on each redraw" {
    NO_COLOR=1 HELIX_INSTALL_TTY=1 LANG=en_US.UTF-8 TERM=vt100 ui_detect
    run _two_redraws
    contains "⠋" "$output"
    contains "⠙" "$output"
}

@test "terminal without UTF-8: the ASCII spinner advances on each redraw" {
    NO_COLOR=1 HELIX_INSTALL_TTY=1 LC_ALL=C LANG=C TERM=vt100 ui_detect
    run _two_redraws
    contains "|" "$output"
    contains "/" "$output"
}

@test "run_logged is silent on success and returns 0" {
    run run_logged sh -c 'echo hello; echo world >&2'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "run_logged sends all output to the log" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    run_logged sh -c 'echo to-stdout; echo to-stderr >&2'
    run cat "$BATS_TEST_TMPDIR/install.log"
    contains "to-stdout" "$output"
    contains "to-stderr" "$output"
    contains "RUN sh -c" "$output"
}

@test "run_logged keeps the command's exit code" {
    run run_logged sh -c 'exit 100'
    [ "$status" -eq 100 ]
}

@test "run_logged prints the command, exit code and the output tail on failure" {
    RUN_LOGGED_TAIL=2
    run run_logged sh -c 'for i in 1 2 3; do echo "line-$i"; done; exit 7'
    [ "$status" -eq 7 ]
    contains "failed (exit 7)" "$output"
    contains "sh -c" "$output"
    contains "line-2" "$output"
    contains "line-3" "$output"
    case "$output" in *line-1*) fail "tail leaked an earlier line" ;; esac
}

@test "run_logged echoes output live when verbose" {
    HELIX_INSTALL_VERBOSE=1
    run run_logged sh -c 'echo visible'
    contains "visible" "$output"
}

@test "print_failure appends the hint" {
    printf 'oops\n' > "$BATS_TEST_TMPDIR/out"
    run print_failure "thing" 5 "$BATS_TEST_TMPDIR/out" "try again"
    [ "$status" -eq 0 ]
    contains "thing failed (exit 5)" "$output"
    contains "oops" "$output"
    contains "try again" "$output"
}

_set_e_script() {
    printf '%s' "set -e; . '$WORKTREE_ROOT/scripts/lib/installer/common.sh'; HELIX_INSTALL_TTY=0 ui_detect; run_logged sh -c 'echo boom; exit 3'; echo after"
}

@test "run_logged under set -e prints the failure block before the shell exits" {
    run sh -c "$(_set_e_script)"
    contains "failed (exit 3)" "$output"
    contains "boom" "$output"
    case "$output" in *after*) fail "set -e did not stop the caller" ;; esac
}

@test "run_logged under set -e works in busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c "$(_set_e_script)"
    contains "failed (exit 3)" "$output"
    contains "boom" "$output"
}

@test "run_logged exit code survives under busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; run_logged sh -c "exit 42"; echo "rc=$?"' \
        _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    contains "rc=42" "$output"
}

@test "run_logged failure tail shows a last line with no trailing newline" {
    run run_logged sh -c 'printf "a\nlast-%s" nonl; exit 4'
    [ "$status" -eq 4 ]
    contains "last-nonl" "$output"
}

@test "run_logged verbose echo shows a last line with no trailing newline" {
    HELIX_INSTALL_VERBOSE=1
    run run_logged sh -c 'printf "a\nlast-nonl"'
    contains "last-nonl" "$output"
}

@test "run_logged keeps the next log entry on its own line after unterminated output" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    run_logged sh -c 'printf "partial-nonl"'
    log_warn "after"
    run grep -c '^partial-nonl$' "$BATS_TEST_TMPDIR/install.log"
    [ "$output" = 1 ]
}

@test "run_logged unterminated output is handled under busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; log_open "$2"; run_logged sh -c "printf \"a\nlast-nonl\"; exit 4"; log_warn after' \
        _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh" "$BATS_TEST_TMPDIR/ash.log"
    contains "last-nonl" "$output"
    run grep -c '^last-nonl$' "$BATS_TEST_TMPDIR/ash.log"
    [ "$output" = 1 ]
}

@test "run_logged logs RUN and the failure block when mktemp is unavailable" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    TMPDIR=/nonexistent run sh -c "set -e; . '$WORKTREE_ROOT/scripts/lib/installer/common.sh'; INSTALL_LOG='$BATS_TEST_TMPDIR/install.log'; run_logged sh -c 'exit 3'; echo after"
    case "$output" in *after*) fail "set -e did not stop the caller" ;; esac
    run grep -c 'RUN sh -c' "$BATS_TEST_TMPDIR/install.log"
    [ "$output" = 1 ]
}

@test "enabling the service does not print systemctl's own output" {
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '#!/bin/sh\necho "Created symlink /etc/systemd/system/x.wants/helixscreen.service"\n' > "$BATS_TEST_TMPDIR/bin/systemctl"
    chmod +x "$BATS_TEST_TMPDIR/bin/systemctl"
    PATH="$BATS_TEST_TMPDIR/bin:$PATH" SUDO="" SERVICE_NAME=helixscreen
    run _enable_helixscreen_unit
    [ "$status" -eq 0 ]
    case "$output" in *"Created symlink"*) fail "systemctl output reached the screen: $output" ;; esac
}

@test "banner: no terminal prints one plain line" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_TTY=0 ui_detect
    run print_banner v1.1.0-beta.4 beta
    [ "$output" = "HelixScreen installer v1.1.0-beta.4 (beta)" ]
}

@test "banner: UTF-8 color terminal prints the half-block logo" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_TTY=1 LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 TERM=xterm-256color ui_detect
    run print_banner v1.1.0-beta.4 beta
    printf '%s' "$output" | grep -qP '[\x{2580}\x{2584}]'
    plain=$(printf '%s' "$output" | sed 's/\x1b\[[0-9;]*m//g')
    contains "HelixScreen" "$plain"
    [[ "$output" == *"v1.1.0-beta.4"* ]]
}

@test "banner: no UTF-8 drops the art and keeps the logotype" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_TTY=1 LC_ALL=C LANG=C TERM=xterm-256color ui_detect
    run print_banner v1.1.0-beta.4 beta
    plain=$(printf '%s' "$output" | sed 's/\x1b\[[0-9;]*m//g')
    contains "HelixScreen" "$plain"
    ! printf '%s' "$output" | grep -qP '[\x{2580}-\x{259F}]'
}

@test "logo pick: a kitty graphics OK picks kitty over sixel" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    _logo_pick "$(printf '\033_Gi=31;OK\033\\\033[?62;4;22c')"
    [ "$_LOGO_MODE" = kitty ]
}

@test "logo pick: a kitty error is not kitty" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    _logo_pick "$(printf '\033_Gi=31;EINVAL:bad\033\\\033[?62;22c')"
    [ "$_LOGO_MODE" = text ]
}

@test "logo pick: DA1 attribute 4 picks sixel, in any position" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    _logo_pick "$(printf '\033[?62;4;22c')"; [ "$_LOGO_MODE" = sixel ]
    _logo_pick "$(printf '\033[?62;22;4c')"; [ "$_LOGO_MODE" = sixel ]
    _logo_pick "$(printf '\033[?4;6c')"; [ "$_LOGO_MODE" = sixel ]
}

@test "logo pick: DA1 without attribute 4 is text, and 14 or 42 are not 4" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    _logo_pick "$(printf '\033[?64;1;2;6;22c')"; [ "$_LOGO_MODE" = text ]
    _logo_pick "$(printf '\033[?62;14;42c')"; [ "$_LOGO_MODE" = text ]
    _logo_pick ""; [ "$_LOGO_MODE" = text ]
}

@test "logo pick: iTerm2 is named by the environment" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    LC_TERMINAL=iTerm2 _logo_pick "$(printf '\033[?62;4c')"
    [ "$_LOGO_MODE" = iterm ]
    TERM_PROGRAM=iTerm.app _logo_pick ""
    [ "$_LOGO_MODE" = iterm ]
}

@test "logo pick: the cell size comes from CSI 16 t, else 9x18" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    _logo_pick "$(printf '\033[6;20;10t\033[?62c')"
    [ "$_LOGO_CW" = 10 ]
    [ "$_LOGO_CH" = 20 ]
    _logo_pick "$(printf '\033[6;x;10t\033[?62c')"
    [ "$_LOGO_CW" = 9 ]
    [ "$_LOGO_CH" = 18 ]
    _logo_pick "$(printf '\033[6;0;0t')"
    [ "$_LOGO_CW" = 9 ]
    [ "$_LOGO_CH" = 18 ]
}

@test "logo pick: HELIX_INSTALL_LOGO overrides the reply" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_LOGO=sixel _logo_pick "$(printf '\033_Gi=31;OK\033\\')"
    [ "$_LOGO_MODE" = sixel ]
}

@test "banner: sixel draws the image and puts the logotype past it" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_TTY=1 LC_ALL=en_US.UTF-8 TERM=xterm-256color ui_detect
    HELIX_INSTALL_LOGO=sixel run print_banner v1.1.0-beta.4 beta
    contains $'\033P0;1;0q' "$output"
    # 193px at the 9px default cell is 22 columns, plus the indent and gap.
    [[ "$output" == *$'\033[27C'*Helix* ]] || fail "logotype not at column 27"
    contains "v1.1.0-beta.4" "$output"
    img=${output#*$'\033P0;1;0q'}; img=${img%%$'\033\\'*}
    [[ "$img" != *$'\n'* ]]
}

@test "banner: kitty sends one unbroken chunked PNG" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_TTY=1 LC_ALL=en_US.UTF-8 TERM=xterm-256color ui_detect
    HELIX_INSTALL_LOGO=kitty run print_banner v1 beta
    contains $'\033_Ga=T,f=100,r=10,C=1,q=2,m=' "$output"
    # The payload is wrapped in logo.sh; a newline inside it would move the cursor.
    img=${output#*$'\033_Ga=T'}; img=${img%$'\033_Gm=0;'*}
    [[ "$img" != *$'\n'* ]]
}

@test "banner: no color never draws an image" {
    . "$WORKTREE_ROOT/scripts/lib/installer/logo.sh"
    HELIX_INSTALL_TTY=1 NO_COLOR=1 LC_ALL=en_US.UTF-8 TERM=xterm-256color ui_detect
    HELIX_INSTALL_LOGO=kitty run print_banner v1 beta
    lacks $'\033_G' "$output"
    contains "HelixScreen" "$output"
}

@test "logo.sh is exactly what render-installer-logo.sh generates" {
    python3 -c 'import PIL' 2>/dev/null || skip "no Pillow"
    local t="$BATS_TEST_TMPDIR/tree"
    mkdir -p "$t/scripts/lib/installer" "$t/assets/images"
    cp "$WORKTREE_ROOT/scripts/render-installer-logo.sh" "$WORKTREE_ROOT/scripts/installer_logo_art.py" "$t/scripts/"
    cp "$WORKTREE_ROOT/assets/images/helix-icon-256.png" "$t/assets/images/"
    bash "$t/scripts/render-installer-logo.sh" >/dev/null
    cmp "$t/scripts/lib/installer/logo.sh" "$WORKTREE_ROOT/scripts/lib/installer/logo.sh" \
        || fail "logo.sh differs from the generator's output"
}

@test "the generated logo module is in the bundle" {
    grep -q 'logo.sh' "$WORKTREE_ROOT/scripts/bundle-installer.sh"
}

@test "finalize_install_log moves the log into printer_data/logs and keeps one old run" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home"; mkdir -p "$KLIPPER_HOME/printer_data/logs"
    echo old > "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log"
    log_open "$BATS_TEST_TMPDIR/install.log"; log_warn "new run"
    finalize_install_log
    grep -q "new run" "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log"
    [ "$(cat "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log.1")" = old ]
}

@test "finalize_install_log creates printer_data/logs when printer_data has none" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home"; mkdir -p "$KLIPPER_HOME/printer_data/config"
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    log_open "$BATS_TEST_TMPDIR/install.log"; log_warn "no logs dir yet"
    finalize_install_log
    grep -q "no logs dir yet" "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log"
    [ ! -e "$INSTALL_DIR/logs" ] || fail "fell back to the install dir"
}

@test "finalize_install_log on a bare host keeps the log outside the payload, across a swap" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/nohome"; INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    STATE_DIR="" STATE_ROOT=""
    mkdir -p "$INSTALL_DIR"
    log_open "$BATS_TEST_TMPDIR/one.log"; log_warn "first run"
    finalize_install_log
    log_open "$BATS_TEST_TMPDIR/two.log"; log_warn "second run"
    finalize_install_log
    dest=$(install_log_dest)
    case "$dest" in "$INSTALL_DIR"/*) fail "log inside the payload: $dest" ;; esac
    # An update's atomic swap: the payload is moved aside and later deleted.
    mv "$INSTALL_DIR" "$INSTALL_DIR.old"; mkdir -p "$INSTALL_DIR"; rm -rf "$INSTALL_DIR.old"
    grep -q "second run" "$dest" || fail "this run's log was lost"
    grep -q "first run" "$dest.1" || fail "the previous run's log was lost"
}

@test "install_log_dest without printer_data uses the platform's state root" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/nohome"
    INSTALL_DIR=/mnt/UDISK/helixscreen STATE_DIR=/mnt/UDISK/helixscreen-state STATE_ROOT=""
    [ "$(install_log_dest)" = /mnt/UDISK/helixscreen-state/logs/helixscreen-install.log ]
    INSTALL_DIR=/opt/helixscreen STATE_DIR="" STATE_ROOT=/data/.helixscreen
    [ "$(install_log_dest)" = /data/.helixscreen/logs/helixscreen-install.log ]
    INSTALL_DIR=/usr/data/helixscreen STATE_DIR="" STATE_ROOT=""
    [ "$(install_log_dest)" = /usr/data/helixscreen-state/logs/helixscreen-install.log ]
    INSTALL_DIR=/opt/helixscreen
    [ "$(install_log_dest)" = /opt/.helixscreen/logs/helixscreen-install.log ]
}

@test "finalize_install_log twice does not rotate this run's own log away" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home"; mkdir -p "$KLIPPER_HOME/printer_data/logs"
    log_open "$BATS_TEST_TMPDIR/install.log"; log_warn "this run"
    finalize_install_log
    finalize_install_log
    grep -q "this run" "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log"
    [ ! -e "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log.1" ] || fail "rotated its own log"
}

@test "finalize_install_log appends to a destination the caller is writing stderr to" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home"; mkdir -p "$KLIPPER_HOME/printer_data/logs"
    dest="$KLIPPER_HOME/printer_data/logs/helixscreen-install.log"
    echo "caller output" > "$dest"
    log_open "$BATS_TEST_TMPDIR/install.log"; log_note "this run"
    finalize_install_log 2>>"$dest"
    grep -q "caller output" "$dest" || fail "the caller's file was replaced"
    grep -q "this run" "$dest" || fail "this run's log was not appended"
    [ ! -e "$dest.1" ] || fail "the caller's file was rotated"
}

@test "an interrupt during a step resolves it as interrupted" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; STEP_TOTAL=2
        trap "step_fail interrupted; exit 130" INT
        step Downloading; kill -INT $$; sleep 1' _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    contains "Downloading ... FAILED (interrupted)" "$output"
}

# Every installer module, as install-dev.sh sources them. main.sh arms the
# installer's own traps at source time; bats' are put back over them.
_source_installer() {
    local lib="$WORKTREE_ROOT/scripts/lib/installer" m bats_traps
    bats_traps="$(trap -p EXIT INT TERM)"
    for m in $(sed -n 's|^ *\. "\$LIB_DIR/\([a-z_]*\)\.sh".*|\1|p' "$WORKTREE_ROOT/scripts/install-dev.sh"); do
        . "$lib/$m.sh"
    done
    trap - ERR EXIT HUP INT TERM
    eval "$bats_traps"
}

# Run apply_install with every installer function it calls stubbed out, so
# only the step layer and the plan's own decisions act. Arguments are
# assignments applied after sourcing. Sets COUNTED to steps closed / planned.
_count_apply_install_steps() {
    _source_installer
    update_mode=false clean_mode=false local_tarball=""
    [ $# -eq 0 ] || export "$@"
    local fn
    for fn in $(declare -f apply_install install_libraries_step | grep -oE '[a-z_][a-z0-9_]*' | sort -u); do
        case "$fn" in apply_install|install_libraries_step|step|step_done|step_skip|step_fail|plan_*|_is_self_update) continue ;; esac
        [ "$(type -t "$fn")" = function ] && eval "$fn() { :; }"
    done
    platform=pi; version=v1.2.3; TMP_DIR="$BATS_TEST_TMPDIR/none"
    HELIX_INSTALL_TTY=0 ui_detect
    plan_count_steps
    step "Checked system"; step_done
    apply_install >/dev/null 2>&1
    COUNTED="$STEP_NUM/$STEP_TOTAL"
}

@test "steps: a fresh install closes exactly the steps the plan counted" {
    _count_apply_install_steps
    [ "$COUNTED" = "6/6" ] || fail "steps closed/counted: $COUNTED"
}

@test "steps: an update closes exactly the steps the plan counted" {
    _count_apply_install_steps update_mode=true
    [ "$COUNTED" = "6/6" ] || fail "steps closed/counted: $COUNTED"
}

@test "steps: libraries and a competing UI each add one step" {
    _count_apply_install_steps MISSING_RUNTIME_DEPS=libgles2 COMPETING_UIS_FOUND=KlipperScreen
    [ "$COUNTED" = "8/8" ] || fail "steps closed/counted: $COUNTED"
}

@test "steps: a payload install that does not start the UI counts its Finishing setup step" {
    _count_apply_install_steps HOST_SERVICE_MECHANISM=mod-managed
    [ "$COUNTED" = "6/6" ] || fail "steps closed/counted: $COUNTED"
}

@test "apply_install marks what it stops and removes before doing it" {
    mkdir -p "$BATS_TEST_TMPDIR/opt/helixscreen"
    _count_apply_install_steps update_mode=true clean_mode=true \
        COMPETING_UIS_FOUND=KlipperScreen INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    contains HelixScreen "${INSTALL_STOPPED:-}"
    contains KlipperScreen "${INSTALL_STOPPED:-}"
    [ "${INSTALL_REMOVED_OLD:-}" = 1 ] || fail "--clean not marked"
}

@test "apply_install does not mark HelixScreen stopped on a self-update" {
    mkdir -p "$BATS_TEST_TMPDIR/opt/helixscreen"
    _count_apply_install_steps update_mode=true HELIX_SELF_UPDATE=1 \
        INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    lacks HelixScreen "${INSTALL_STOPPED:-}"
}

# Failures under main.sh's real traps. $1 is the shell; $2 runs after the
# confirm point with step 5 of 6 next, and is expected to end the run.
_fail_under_traps() {
    local shell=$1 body=$2
    mkdir -p "$BATS_TEST_TMPDIR/home/printer_data/logs"
    run $shell -c '. "$1/common.sh"; . "$1/main.sh"; HELIX_INSTALL_TTY=0 ui_detect
        KLIPPER_HOME="$2/home"; TMP_DIR="$2/helixscreen-install"; mkdir -p "$TMP_DIR"
        HELIX_CONFIRMED=1; log_open "$TMP_DIR/install.log"; STEP_TOTAL=6; STEP_NUM=4
        '"$body" _ "$WORKTREE_ROOT/scripts/lib/installer" "$BATS_TEST_TMPDIR"
}

LOG_DEST_REL=home/printer_data/logs/helixscreen-install.log

@test "a failure before anything changed: FAILED, nothing changed, and the log in printer_data" {
    _fail_under_traps bash 'step "Installed files"; log_error "boom"; exit 1'
    [ "$status" -eq 1 ]
    contains "[5/6] Installed files ... FAILED" "$output"
    contains "Nothing on your printer was changed after this step." "$output"
    contains "Full log: $BATS_TEST_TMPDIR/$LOG_DEST_REL" "$output"
    grep -q "boom" "$BATS_TEST_TMPDIR/$LOG_DEST_REL"
    [ ! -d "$BATS_TEST_TMPDIR/helixscreen-install" ] || fail "scratch dir left behind"
}

@test "a failure after a competing UI was stopped names it" {
    _fail_under_traps bash 'INSTALL_STOPPED=KlipperScreen; step "Installed files"; exit 1'
    contains "KlipperScreen was stopped; re-run the installer to finish." "$output"
    lacks "Nothing on your printer was changed" "$output"
}

@test "a failed update names HelixScreen and the UIs it stopped" {
    _fail_under_traps bash 'INSTALL_STOPPED="HelixScreen KlipperScreen"; step "Installed files"; exit 1'
    contains "HelixScreen and KlipperScreen were stopped; re-run the installer to finish." "$output"
}

@test "a failed --clean says the old install was removed" {
    _fail_under_traps bash 'INSTALL_REMOVED_OLD=1; step "Installed files"; exit 1'
    contains "The old install was removed; re-run the installer to finish." "$output"
}

@test "a fresh install that fails after the swap does not claim nothing changed" {
    _fail_under_traps bash 'INSTALL_SWAPPED=swapped; step "Set up service"; exit 1'
    contains "[5/6] Set up service ... FAILED" "$output"
    contains "The new install is in place but not set up; re-run the installer to finish." "$output"
    lacks "Nothing on your printer was changed" "$output"
}

@test "an update that fails after the swap names where the previous install is" {
    mkdir -p "$BATS_TEST_TMPDIR/helixscreen.old"
    _fail_under_traps bash 'INSTALL_SWAPPED=swapped INSTALL_BACKUP="$2/helixscreen.old"
        INSTALL_STOPPED=HelixScreen; step "Set up service"; exit 1'
    contains "The new install is in place but not set up; the previous one is kept at $BATS_TEST_TMPDIR/helixscreen.old. Re-run the installer to finish." "$output"
    lacks "Nothing on your printer was changed" "$output"
}

@test "an in-place update that fails midway says the install is partly replaced" {
    _fail_under_traps bash 'INSTALL_DIR=/opt/helixscreen INSTALL_SWAPPED=in-place; step "Installed files"; exit 1'
    contains "HelixScreen at /opt/helixscreen was partly replaced; re-run the installer to finish." "$output"
}

@test "a failure after a rollback says the previous install was put back" {
    _fail_under_traps bash 'INSTALL_BACKUP="$2/helixscreen.old" INSTALL_STOPPED=HelixScreen
        step "Installed files"; exit 1'
    contains "The previous install was put back; HelixScreen was stopped; re-run the installer to finish." "$output"
}

@test "a failure in the last step still says what state the printer is in" {
    mkdir -p "$BATS_TEST_TMPDIR/helixscreen.old"
    _fail_under_traps bash 'INSTALL_SWAPPED=swapped INSTALL_BACKUP="$2/helixscreen.old"
        STEP_NUM=5; step "Starting HelixScreen" "Started HelixScreen"; exit 1'
    contains "[6/6] Starting HelixScreen ... FAILED" "$output"
    contains "the previous one is kept at $BATS_TEST_TMPDIR/helixscreen.old" "$output"
}

@test "a failure in the cleanup after every step prints no state line" {
    _fail_under_traps bash 'INSTALL_SWAPPED=swapped INSTALL_COMPLETE=1; exit 1'
    lacks "re-run the installer" "$output"
    lacks "Nothing on your printer" "$output"
    contains "Full log:" "$output"
}

@test "an interrupt under the installer's traps resolves the step and keeps the log" {
    command -v busybox >/dev/null || skip "no busybox"
    _fail_under_traps "busybox ash" 'STEP_NUM=2; step Downloaded; kill -INT $$; sleep 1; echo AFTER'
    [ "$status" -eq 130 ]
    contains "[3/6] Downloaded ... FAILED (interrupted)" "$output"
    contains "Full log:" "$output"
    lacks AFTER "$output"
    grep -q "FAIL Downloaded (interrupted)" "$BATS_TEST_TMPDIR/$LOG_DEST_REL"
}

# An SSH drop: the terminal is gone (writes fail), then HUP arrives.
_hup_with_dead_terminal() { # shell
    _fail_under_traps "$1" 'set -eu; STEP_NUM=2; step Downloaded
        exec 2>/dev/full 1>/dev/full; kill -HUP $$; sleep 1'
}

@test "a hangup with the terminal gone still keeps the log and cleans up (busybox ash)" {
    command -v busybox >/dev/null || skip "no busybox"
    _hup_with_dead_terminal "busybox ash"
    [ "$status" -eq 129 ] || fail "exit status $status, wanted 129"
    grep -q "FAIL Downloaded (interrupted)" "$BATS_TEST_TMPDIR/$LOG_DEST_REL" || fail "log not kept"
    [ ! -d "$BATS_TEST_TMPDIR/helixscreen-install" ] || fail "scratch dir left behind"
}

@test "a hangup with the terminal gone still keeps the log and cleans up (dash)" {
    command -v dash >/dev/null || skip "no dash"
    _hup_with_dead_terminal dash
    [ "$status" -eq 129 ] || fail "exit status $status, wanted 129"
    grep -q "FAIL Downloaded (interrupted)" "$BATS_TEST_TMPDIR/$LOG_DEST_REL" || fail "log not kept"
    [ ! -d "$BATS_TEST_TMPDIR/helixscreen-install" ] || fail "scratch dir left behind"
}

@test "a failure after the confirm point is reported even when the log could not open" {
    run bash -c '. "$1/common.sh"; . "$1/main.sh"; HELIX_INSTALL_TTY=0 ui_detect
        KLIPPER_HOME="$2/home"; TMP_DIR="$2/none"; HELIX_CONFIRMED=1
        log_open "$2/no/such/dir/install.log" || true; STEP_TOTAL=6; STEP_NUM=4
        step "Installed files"; exit 1' _ "$WORKTREE_ROOT/scripts/lib/installer" "$BATS_TEST_TMPDIR"
    contains "[5/6] Installed files ... FAILED" "$output"
    contains "Nothing on your printer was changed after this step." "$output"
    lacks "Full log:" "$output"
}

@test "bash's ERR handler keeps the log before it removes the scratch dir" {
    mkdir -p "$BATS_TEST_TMPDIR/home/printer_data/logs"
    run bash -c '. "$1/common.sh"; . "$1/main.sh"; HELIX_INSTALL_TTY=0 ui_detect
        KLIPPER_HOME="$2/home"; INSTALL_DIR="$2/opt/helixscreen"
        TMP_DIR="$2/helixscreen-install"; mkdir -p "$TMP_DIR"; CLEANUP_TMP=true
        HELIX_CONFIRMED=1; log_open "$TMP_DIR/install.log"; log_warn "before the error"
        false' _ "$WORKTREE_ROOT/scripts/lib/installer" "$BATS_TEST_TMPDIR"
    [ "$status" -ne 0 ]
    grep -q "before the error" "$BATS_TEST_TMPDIR/$LOG_DEST_REL" || fail "log lost"
    contains "Full log: $BATS_TEST_TMPDIR/$LOG_DEST_REL" "$output"
}

@test "a run that exits 0 before the log opens prints no failure block" {
    run bash -c '. "$1/common.sh"; . "$1/main.sh"; HELIX_INSTALL_TTY=0 ui_detect
        TMP_DIR="$2/none"; exit 0' _ "$WORKTREE_ROOT/scripts/lib/installer" "$BATS_TEST_TMPDIR"
    [ "$status" -eq 0 ]
    [ -z "$output" ] || fail "unexpected output: $output"
}

@test "summary: a fresh install names the config, updates, disabled UI, KIAUH and log" {
    _source_installer
    HELIX_INSTALL_TTY=0 LC_ALL=C ui_detect
    HOME="$BATS_TEST_TMPDIR/home"; KLIPPER_HOME="$HOME"
    conf="$HOME/printer_data/config/moonraker.conf"; mkdir -p "${conf%/*}/helixscreen"
    printf '[update_manager helixscreen]\ntype: web\n' > "$conf"
    find_moonraker_conf() { echo "$conf"; }
    R2_CHANNEL=beta HOST_SERVICE_MECHANISM=systemd
    HELIX_CONFIG_EDITABLE="$HOME/printer_data/config/helixscreen"
    COMPETING_UIS_FOUND=KlipperScreen KIAUH_EXT_ADDED=1
    INSTALL_LOG_KEPT="$HOME/printer_data/logs/helixscreen-install.log"
    run print_summary v1.1.0-beta.4
    contains "HelixScreen v1.1.0-beta.4 is running (beta channel)." "$output"
    contains "Config     ~/printer_data/config/helixscreen  (editable in Mainsail/Fluidd)" "$output"
    contains "Updates    Mainsail/Fluidd update manager, or re-run with --update" "$output"
    contains "Disabled   KlipperScreen  (re-enabled by --uninstall)" "$output"
    contains "KIAUH      restart KIAUH to see the HelixScreen extension" "$output"
    contains "Log        ~/printer_data/logs/helixscreen-install.log" "$output"
}

@test "summary: without an update manager entry, updates come from re-running" {
    _source_installer
    HELIX_INSTALL_TTY=0 ui_detect
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    find_moonraker_conf() { echo ""; }
    unset HELIX_CONFIG_EDITABLE COMPETING_UIS_FOUND KIAUH_EXT_ADDED
    run print_summary v1.2.3
    contains "Config     $INSTALL_DIR/config" "$output"
    contains "Updates    re-run with --update" "$output"
    lacks "Disabled" "$output"
    lacks "KIAUH" "$output"
    lacks "editable" "$output"
}

@test "file_size_text: a missing file prints nothing, on either stream" {
    run file_size_text "$BATS_TEST_TMPDIR/absent.zip"
    [ "$status" -eq 0 ]
    [ -z "$output" ] || fail "printed: $output"
}

@test "file_size_text: MB from a megabyte up, KB below" {
    head -c 2097152 /dev/zero > "$BATS_TEST_TMPDIR/two.bin"
    head -c 3072 /dev/zero > "$BATS_TEST_TMPDIR/small.bin"
    [ "$(file_size_text "$BATS_TEST_TMPDIR/two.bin")" = "2 MB" ]
    [ "$(file_size_text "$BATS_TEST_TMPDIR/small.bin")" = "3 KB" ]
}
