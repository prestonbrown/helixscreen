#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the shellcheck gate, scripts/qc/shellcheck.sh.
#
# The gate used to cover config/platform/*.sh and config/helixscreen.init and
# nothing else, so every script under scripts/ — the installer modules, the
# launcher, the release tooling, all of which ship to devices — went unlinted.
# The symptom was silent: staging scripts/helix-launcher.sh printed "No shell
# scripts staged for commit" and the commit sailed through.
#
# scripts/ carries pre-existing findings, so it is a ratchet rather than a hard
# gate: the files in SHELLCHECK_BASELINE report but do not block. The tests that
# matter most here are the two that keep that ratchet honest — a baselined file
# that has since been cleaned must leave the list, and a file outside the list
# must be clean. Without those the baseline silently becomes a permanent
# exemption list and the gate stops meaning anything.

QC="scripts/qc/shellcheck.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
}

# Read the gate's own config so these tests cannot drift from it.
gate_excludes() {
    grep '^SHELLCHECK_SCRIPTS_EXCLUDE=' "$QC" | cut -d'"' -f2
}

gate_baseline() {
    local first
    first=$(grep '^SHELLCHECK_BASELINE="' "$QC" | head -1)
    case "$first" in
        *\")  # opens and closes on one line (the empty baseline)
            # The sed range below cannot read this form: GNU sed starts
            # matching the end regex on the line AFTER the start match, so a
            # one-line value overruns into the next quote-terminated
            # assignment (SHELL_FILES="").
            first=${first#SHELLCHECK_BASELINE=\"}
            printf '%s\n' "${first%\"}"
            ;;
        *)    # multi-line list: opening assignment line, entries, closing quote
            sed -n '/^SHELLCHECK_BASELINE="/,/"$/p' "$QC" \
                | sed 's/^SHELLCHECK_BASELINE="//; s/"$//'
            ;;
    esac
}

# The path filter the staged-file branch uses. Extracted rather than restated,
# and it aborts on an empty result: an empty regex makes `grep -E` match
# everything, which would turn every scope test below into a false pass.
gate_filter() {
    local f
    f=$(grep '^SHELLCHECK_PATH_RE=' "$QC" | head -1 \
        | sed "s/^SHELLCHECK_PATH_RE='//; s/'$//")
    [ -n "$f" ] || { echo "could not extract the gate's path filter from $QC" >&2; return 1; }
    printf '%s\n' "$f"
}

# Does the gate's staged-file filter select $1?
filter_selects() {
    local filter
    filter=$(gate_filter) || return 2
    printf '%s\n' "$1" | grep -qE "$filter"
}

# The exact invocation the gate uses for scripts/.
check_as_gate_does() {
    shellcheck -S warning -e "$(gate_excludes)" "$1" 2>/dev/null
}

# Every scripts/*.sh the gate would actually collect in full-scan mode.
gate_scripts() {
    git ls-files 'scripts/*.sh' 'scripts/**/*.sh' 2>/dev/null | sort -u
}

# ------------------------------------------------------- scope (the actual bug)

@test "the gate's path filter is extractable" {
    # Guards every scope test below: if extraction breaks they would all pass
    # against an empty regex.
    run gate_filter
    [ "$status" -eq 0 ]
    [[ "$output" == *"scripts/"* ]]
}

@test "a staged scripts/*.sh is selected by the gate's file filter" {
    # This is the regression. Before the fix this path matched nothing and the
    # gate reported "No shell scripts staged for commit".
    filter_selects scripts/helix-launcher.sh
}

@test "nested installer modules are selected too" {
    filter_selects scripts/lib/installer/main.sh
}

@test "the config/ tier is still selected" {
    # Widening to scripts/ must not drop the original coverage.
    filter_selects config/platform/foo.sh
    filter_selects config/helixscreen.init
    filter_selects config/creality-backend.init
}

@test "non-shell files are not selected" {
    ! filter_selects docs/devel/BUILD_SYSTEM.md
    ! filter_selects src/ui/controls/ui_panel_macros.cpp
    ! filter_selects ui_xml/globals.xml
}

# ------------------------------------------------------------ ratchet honesty

@test "every baselined path still exists" {
    local missing=""
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        [ -f "$f" ] || missing="$missing $f"
    done <<< "$(gate_baseline)"

    if [ -n "$missing" ]; then
        echo "baseline names files that no longer exist:$missing" >&2
        echo "Remove them from SHELLCHECK_BASELINE in $QC" >&2
        return 1
    fi
}

@test "every baselined file is still actually dirty" {
    # A cleaned file left in the baseline is a silent hole: it would stop being
    # enforced forever. The ratchet only tightens if cleaning a file forces its
    # removal from the list.
    local stale=""
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        [ -f "$f" ] || continue
        if check_as_gate_does "$f" >/dev/null; then
            stale="$stale $f"
        fi
    done <<< "$(gate_baseline)"

    if [ -n "$stale" ]; then
        echo "these files now pass shellcheck and must leave the baseline:$stale" >&2
        echo "Drop them from SHELLCHECK_BASELINE in $QC" >&2
        return 1
    fi
}

@test "the baseline extractor matches the file's shape" {
    # Canary against a silently-broken extractor: with the baseline empty,
    # tests 7-9 above pass vacuously, so the extractor must provably return
    # empty for the one-line form - and provably non-empty while any file is
    # listed.
    local baseline first
    baseline=$(gate_baseline)
    first=$(grep '^SHELLCHECK_BASELINE="' "$QC" | head -1)
    if [ "$first" = 'SHELLCHECK_BASELINE=""' ]; then
        [ -z "$baseline" ]
    else
        [ -n "$baseline" ]
    fi
}

@test "baseline membership is exact, not substring" {
    # A substring test would exempt scripts/install-dev.sh.bak along with
    # scripts/install-dev.sh. The baseline is empty today, so the guard runs
    # against the historically-baselined name's .bak twin and, should a file
    # ever be re-baselined, against every listed entry's .bak twin.
    local baseline entry
    baseline=$(gate_baseline)

    ! printf '%s\n' "$baseline" | grep -Fxq "scripts/install-dev.sh.bak"
    while IFS= read -r entry; do
        [ -z "$entry" ] && continue
        ! printf '%s\n' "$baseline" | grep -Fxq "$entry.bak"
    done <<< "$baseline"
}

# --------------------------------------------------------- severity behaviour

@test "SC3043 (local in POSIX sh) is not fatal" {
    # The installer and launcher target BusyBox ash, which implements local.
    # 375 hits repo-wide; gating on it would be pure noise.
    local f="$BATS_TEST_TMPDIR/uses_local.sh"
    cat > "$f" <<'EOF'
#!/bin/sh
f() {
  local x="$1"
  printf '%s\n' "$x"
}
f hello
EOF
    run check_as_gate_does "$f"
    [ "$status" -eq 0 ]
}

@test "a genuinely dangerous script is caught" {
    # Mutation check: if this passes, the gate is inert.
    local f="$BATS_TEST_TMPDIR/dangerous.sh"
    cat > "$f" <<'EOF'
#!/bin/sh
rm -rf $UNQUOTED_DIR/*
EOF
    run check_as_gate_does "$f"
    [ "$status" -ne 0 ]
    [[ "$output" == *"SC2115"* ]]
}

@test "iterating over ls output is caught" {
    local f="$BATS_TEST_TMPDIR/lsloop.sh"
    cat > "$f" <<'EOF'
#!/bin/sh
for f in $(ls *.txt); do
  cat "$f"
done
EOF
    run check_as_gate_does "$f"
    [ "$status" -ne 0 ]
}

@test "the excluded-code list is exactly the two documented codes" {
    # Widening this list is how a gate quietly dies. Both codes are justified
    # in the comment above SHELLCHECK_SCRIPTS_EXCLUDE; a third needs the same.
    [ "$(gate_excludes)" = "SC3043,SC1091" ]
}

# make land-gate hands the gate its change (HELIX_QC_GATE_CHANGED): each script is
# linted on its own, so the changed ones are the ones to lint.
run_scoped() {
    local tree="$BATS_TEST_TMPDIR/tree" repo="$PWD"
    mkdir -p "$tree/scripts" "$tree/config/platform"
    printf '%s\n' "$@" >"$BATS_TEST_TMPDIR/changed"
    run bash -c 'cd "$1" && . "$2/scripts/qc/_lib.sh" && . "$2/scripts/qc/shellcheck.sh" &&
        QC_TMP=$(mktemp -d) STAGED_ONLY=false HELIX_QC_GATE_CHANGED="$3" QC_COUNTS=/dev/null qc_shellcheck' \
        _ "$tree" "$repo" "$BATS_TEST_TMPDIR/changed"
}

@test "at the land gate only the changed scripts are linted, and a dirty one fails" {
    mkdir -p "$BATS_TEST_TMPDIR/tree/scripts"
    printf '#!/bin/sh\nrm -rf $DIR/*\n' >"$BATS_TEST_TMPDIR/tree/scripts/bad.sh"
    printf '#!/bin/sh\necho ok\n' >"$BATS_TEST_TMPDIR/tree/scripts/clean.sh"
    run_scoped scripts/clean.sh docs/x.md
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" == *"(1 file(s) linted)"* ]] || return 1
    run_scoped scripts/bad.sh scripts/deleted.sh
    [ "$status" -ne 0 ]
    [[ "$output" == *"scripts/bad.sh"* ]]
}

@test "at the land gate a change to the gate itself lints every script, not just the changed paths" {
    local tree="$BATS_TEST_TMPDIR/tree"
    mkdir -p "$tree/assets/config/platform" "$tree/config"
    printf '#!/bin/sh\nrm -rf $DIR/*\n' >"$tree/assets/config/platform/hooks-x.sh"
    printf '#!/bin/sh\necho ok\n' >"$tree/config/x.init"
    run_scoped scripts/qc/shellcheck.sh
    [ "$status" -ne 0 ] || { echo "a gate change linted nothing: $output"; false; }
    [[ "$output" == *"assets/config/platform/hooks-x.sh"* ]] || { echo "$output"; false; }
    run_scoped .shellcheckrc
    [ "$status" -ne 0 ] || { echo "a config change linted nothing: $output"; false; }
}

@test "the full sweep lints the platform hooks under assets/config/platform" {
    local tree="$BATS_TEST_TMPDIR/full" repo="$PWD"
    mkdir -p "$tree/assets/config/platform"
    printf '#!/bin/sh\nrm -rf $DIR/*\n' >"$tree/assets/config/platform/hooks-x.sh"
    run bash -c 'cd "$1" && . "$2/scripts/qc/_lib.sh" && . "$2/scripts/qc/shellcheck.sh" &&
        QC_TMP=$(mktemp -d) STAGED_ONLY=false QC_COUNTS=/dev/null qc_shellcheck' _ "$tree" "$repo"
    [ "$status" -ne 0 ] || { echo "the hooks were not linted: $output"; false; }
    [[ "$output" == *"assets/config/platform/hooks-x.sh"* ]] || { echo "$output"; false; }
}

@test "the gate wakes on every path it lints and on its own script and config" {
    local re
    re="$(bash -c '. scripts/qc/shellcheck.sh; echo "$QC_TRIGGER_qc_shellcheck"')"
    [ -n "$re" ] || { echo "no trigger"; false; }
    for p in config/helixscreen.init assets/config/platform/hooks-ad5m-kmod.sh scripts/x.sh \
        config/restore-user-config.sh scripts/qc/shellcheck.sh .shellcheckrc; do
        [[ "$p" =~ $re ]] || { echo "$p does not wake the gate"; false; }
    done
    [[ ! "docs/devel/X.md" =~ $re ]] || { echo "a doc wakes the gate"; false; }
}
