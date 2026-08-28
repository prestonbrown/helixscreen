#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Escalation discipline for the atomic-swap path in
# scripts/lib/installer/release.sh and its bundled twin scripts/install.sh.
#
# During a self-update the swap runs under systemd's NoNewPrivileges=true, where
# `sudo` cannot execute at all. So the load-bearing moves — old install aside,
# new install into place, rollback back — must be reached through
# file_sudo/path_sudo, which escalate only when the target's owner actually
# requires it. Escalating unconditionally turns a swap that would have worked on
# a user-owned parent into a failed one, and a failed rollback leaves the box
# with no install at all.
#
# A bare `$SUDO` is legal in exactly one shape: the escalating half of a
# two-attempt idiom, where an unescalated (or path_sudo) attempt already ran and
# failed on the same logical line.
#
# These are POSITIVE invariants over the real function bodies, not a blocklist
# of historical literals. The predecessor of this file greped for five exact
# strings (`$SUDO mkdir -p "${INSTALL_DIR}/config"` and friends) that the
# INSTALL_BACKUP/path_sudo rewrite deleted, so all ten cases had been passing on
# absence rather than on obedience.

load helpers

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
# Overridable so the guard can be proven to FIRE against a hand-broken copy:
#   cp -r scripts "$T"/ && sed -i 's/$(path_sudo "${INSTALL_DIR}") mv/$SUDO mv/' \
#       "$T/scripts/lib/installer/release.sh"
#   HELIX_TEST_RELEASE_SH="$T/scripts/lib/installer/release.sh" bats tests/shell/test_install_file_sudo.bats
RELEASE_SH="${HELIX_TEST_RELEASE_SH:-$WORKTREE_ROOT/scripts/lib/installer/release.sh}"
INSTALL_SH="${HELIX_TEST_INSTALL_SH:-$WORKTREE_ROOT/scripts/install.sh}"

# The swap lives in three functions: extract_release() moves the old install to
# INSTALL_BACKUP and the new one into place, _restore_install_backup() undoes
# that, cleanup_old_install() drops the backup once the new install is proven.
SWAP_FUNCS="extract_release _restore_install_backup cleanup_old_install"

# Operands whose ownership the installer may not assume — the reason file_sudo
# exists. Everything else in these functions works inside TMP_DIR, which the
# installer created itself.
INSTALL_OPERANDS='INSTALL_DIR|INSTALL_BACKUP|new_install|_backup'

# Emit the body of shell function $2 from file $1: full-line comments dropped,
# and backslash continuations folded so `... || \` + `$SUDO mv ...` reads as the
# one logical line it is. Without the fold the escalating half of a two-attempt
# idiom looks exactly like an unguarded bare $SUDO.
fn_body() {
    awk -v fn="$2" '
        index($0, fn "() {") == 1 { inside = 1 }
        inside && $0 !~ /^[[:space:]]*#/ { print }
        inside && /^\}/ { inside = 0 }
    ' "$1" | sed -e :a -e '/\\$/N; s/\\\n[[:space:]]*/ /; ta'
}

# Bare `$SUDO <verb>` on an install path that is NOT the second half of a
# two-attempt idiom. Echoes offending lines, prefixed with their function.
undocumented_bare_sudo() {
    local file=$1 fn body
    for fn in $SWAP_FUNCS; do
        body="$(fn_body "$file" "$fn")"
        if [ -z "$body" ]; then
            echo "${fn}: FUNCTION NOT FOUND in $file"
            continue
        fi
        printf '%s\n' "$body" \
            | grep -E '\$SUDO[[:space:]]+(mv|rm|cp|mkdir)\b' \
            | grep -E "$INSTALL_OPERANDS" \
            | grep -vE '\b(mv|rm|cp|mkdir)\b.*(\|\||&&)[[:space:]!]*\$SUDO[[:space:]]+(mv|rm|cp|mkdir)\b' \
            | sed "s|^|${fn}: |" || true
    done
}

# Fail unless every logical line of ${fn}() matching SELECT_RE reaches VERB
# through file_sudo/path_sudo. Also fails when SELECT_RE matches nothing at all,
# so a rename or a move to another function cannot silently retire the guard —
# which is precisely how the previous version of this file went dead.
assert_escalated() {
    local file=$1 fn=$2 select_re=$3 verb=$4 desc=$5
    local body hits bad

    body="$(fn_body "$file" "$fn")"
    [ -n "$body" ] || fail "assert_escalated: ${fn}() not found in $file"

    hits="$(printf '%s\n' "$body" | grep -E "$select_re" || true)"
    [ -n "$hits" ] || fail "assert_escalated: nothing in ${fn}() of $file matches /${select_re}/ — ${desc} moved or was removed, so this guard is watching nothing"

    bad="$(printf '%s\n' "$hits" | awk -v verb="$verb" '
        { pre = (match($0, verb "[[:space:]]") ? substr($0, 1, RSTART - 1) : $0)
          if (pre !~ /file_sudo|path_sudo/) print }')"
    [ -z "$bad" ] || fail "${desc} must be reached through file_sudo/path_sudo, not directly:
${bad}"
}

# =============================================================================
# Negative invariant: no unguarded escalation anywhere in the swap path
# =============================================================================

@test "release.sh: swap path never escalates without trying unescalated first" {
    run undocumented_bare_sudo "$RELEASE_SH"
    [ "$status" -eq 0 ]
    [ -z "$output" ] || fail "bare \$SUDO with no preceding attempt on the same logical line:
${output}"
}

@test "install.sh: swap path never escalates without trying unescalated first" {
    run undocumented_bare_sudo "$INSTALL_SH"
    [ "$status" -eq 0 ]
    [ -z "$output" ] || fail "bare \$SUDO with no preceding attempt on the same logical line:
${output}"
}

# =============================================================================
# Positive invariants: the load-bearing swap operations are escalation-aware
# =============================================================================

@test "release.sh: moving the old install aside goes through path_sudo" {
    assert_escalated "$RELEASE_SH" extract_release \
        'mv[[:space:]]+"[^"]*INSTALL_DIR[^"]*"[[:space:]]+"[^"]*INSTALL_BACKUP' \
        mv "the mv of INSTALL_DIR to INSTALL_BACKUP"
}

@test "install.sh: moving the old install aside goes through path_sudo" {
    assert_escalated "$INSTALL_SH" extract_release \
        'mv[[:space:]]+"[^"]*INSTALL_DIR[^"]*"[[:space:]]+"[^"]*INSTALL_BACKUP' \
        mv "the mv of INSTALL_DIR to INSTALL_BACKUP"
}

@test "release.sh: moving the new install into place goes through file_sudo" {
    assert_escalated "$RELEASE_SH" extract_release \
        'mv[[:space:]]+"[^"]*new_install[^"]*"[[:space:]]+"[^"]*INSTALL_DIR' \
        mv "the mv of the extracted tree into INSTALL_DIR"
}

@test "install.sh: moving the new install into place goes through file_sudo" {
    assert_escalated "$INSTALL_SH" extract_release \
        'mv[[:space:]]+"[^"]*new_install[^"]*"[[:space:]]+"[^"]*INSTALL_DIR' \
        mv "the mv of the extracted tree into INSTALL_DIR"
}

@test "release.sh: mkdir under INSTALL_DIR goes through file_sudo" {
    assert_escalated "$RELEASE_SH" extract_release \
        'mkdir[[:space:]]+-p[[:space:]]+".*INSTALL_DIR' \
        mkdir "mkdir on the install tree"
}

@test "install.sh: mkdir under INSTALL_DIR goes through file_sudo" {
    assert_escalated "$INSTALL_SH" extract_release \
        'mkdir[[:space:]]+-p[[:space:]]+".*INSTALL_DIR' \
        mkdir "mkdir on the install tree"
}

@test "release.sh: rollback restores the backup via path_sudo" {
    assert_escalated "$RELEASE_SH" _restore_install_backup \
        'mv[[:space:]]+"[^"]*INSTALL_BACKUP[^"]*"[[:space:]]+"[^"]*INSTALL_DIR' \
        mv "the rollback mv of INSTALL_BACKUP back to INSTALL_DIR"
}

@test "install.sh: rollback restores the backup via path_sudo" {
    assert_escalated "$INSTALL_SH" _restore_install_backup \
        'mv[[:space:]]+"[^"]*INSTALL_BACKUP[^"]*"[[:space:]]+"[^"]*INSTALL_DIR' \
        mv "the rollback mv of INSTALL_BACKUP back to INSTALL_DIR"
}

# =============================================================================
# The helpers above are only as good as the functions they can find
# =============================================================================

@test "every swap function this file guards still exists in both scripts" {
    local f fn
    for f in "$RELEASE_SH" "$INSTALL_SH"; do
        for fn in $SWAP_FUNCS; do
            [ -n "$(fn_body "$f" "$fn")" ] || fail "${fn}() not found in $f — the swap was refactored and these guards need repointing"
        done
    done
}
