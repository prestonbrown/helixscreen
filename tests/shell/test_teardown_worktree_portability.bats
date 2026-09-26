#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/teardown-worktree.sh resolves its target with `realpath -m` and
# reports leftover-file owners with `stat -c`. Both are GNU-only: BSD/macOS
# realpath has no -m at all (an unrecognized option, not a different
# behavior), and BSD/macOS stat needs `-f` format strings instead of `-c`.
# The maintainer develops on a Mac, so a script that only works against GNU
# coreutils breaks there outright.
#
# These tests shim `realpath` and `stat` on PATH to reject the GNU-only
# flags the way BSD's do, so the fallback branches run and are checked from
# Linux without needing an actual Mac.

SCRIPT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/teardown-worktree.sh"

setup() {
    load helpers
    MAIN="$BATS_TEST_TMPDIR/mainrepo"
    mkdir -p "$MAIN"
    cd "$MAIN" || return 1
    git init -q -b master .
    git config user.email t@example.com
    git config user.name tester
    echo hello > README.md
    git add README.md
    git commit -qm base --no-verify
}

make_worktree() {
    local name="$1"
    mkdir -p "$MAIN/.worktrees"
    git worktree add -q -b "feature/$name" "$MAIN/.worktrees/$name"
}

# Rejects -m the way BSD/macOS realpath does (its only option is -q), so the
# script's own probe fails and it must fall back to portable resolution.
shim_bsd_realpath() {
    mock_command_script "realpath" '
        for a in "$@"; do
            case "$a" in
                -m|--canonicalize-missing)
                    echo "realpath: illegal option -- m" >&2
                    exit 1
                    ;;
            esac
        done
        exec /usr/bin/realpath "$@"
    '
}

# Rejects -c the way BSD/macOS stat does, translating the -f form the script
# falls back to into a real answer instead of stubbing it out, so the test
# proves the fallback reports the actual owner, not just that it degrades
# without crashing.
shim_bsd_stat() {
    mock_command_script "stat" '
        case "$1" in
            -c)
                echo "stat: illegal option -- c" >&2
                exit 1
                ;;
            -f)
                fmt="$2"; shift 2
                case "$fmt" in
                    "%Su") exec /usr/bin/stat -c "%U" "$@" ;;
                    "%Su:%Sg") exec /usr/bin/stat -c "%U:%G" "$@" ;;
                    *) exit 1 ;;
                esac
                ;;
            *) exec /usr/bin/stat "$@" ;;
        esac
    '
}

@test "a normal teardown still resolves the target without GNU realpath -m" {
    shim_bsd_realpath
    make_worktree normal
    run "$SCRIPT" normal --into master
    [ "$status" -eq 0 ]
    contains "Teardown complete" "$output"
    [ ! -d "$MAIN/.worktrees/normal" ]
}

@test "a fully-removed but unpruned worktree still resolves without GNU realpath -m" {
    shim_bsd_realpath
    make_worktree gone
    # Simulate a prior run whose rm -rf fully succeeded but never reached
    # `git worktree prune` - the directory is gone, the registration is not.
    rm -rf "$MAIN/.worktrees/gone"
    run "$SCRIPT" gone --into master --force
    [ "$status" -eq 0 ]
    run git -C "$MAIN" worktree list --porcelain
    lacks ".worktrees/gone" "$output"
}

# Without -m, a path resolves only while its parent exists, so a pointer into
# the worktree has to be matched before the worktree is deleted.
@test "a shared submodule pointer is restored without GNU realpath -m" {
    shim_bsd_realpath
    make_worktree doomed
    mkdir -p "$MAIN/lib/ftxui" "$MAIN/.worktrees/doomed/lib" "$MAIN/.git/modules/lib/ftxui"
    git config --file "$MAIN/.git/modules/lib/ftxui/config" core.worktree \
        "../../../../.worktrees/doomed/lib/ftxui"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    [ "$(git config --file "$MAIN/.git/modules/lib/ftxui/config" core.worktree)" = "../../../../lib/ftxui" ]
}

@test "the leftover-file report resolves real owners without GNU stat -c" {
    shim_bsd_stat
    make_worktree broken
    echo leftover > "$MAIN/.worktrees/broken/leftover.txt"
    rm -f "$MAIN/.worktrees/broken/.git"

    run "$SCRIPT" broken --into master
    [ "$status" -ne 0 ]

    # The current user, not the "?" that a silently-swallowed stat failure
    # would print - proving the -f fallback actually resolved an owner.
    contains "$(id -un)  $MAIN/.worktrees/broken/leftover.txt" "$output"
    lacks "?  $MAIN/.worktrees/broken/leftover.txt" "$output"
}
