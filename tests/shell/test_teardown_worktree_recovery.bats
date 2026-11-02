#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/teardown-worktree.sh resolves its target by combining a filesystem
# path guess with a check against `git worktree list --porcelain`. When a
# worktree's OWN .git pointer is already gone - a previous teardown that got
# partway through rm -rf before root-owned leftovers (a docker build, say)
# stopped it - every `git -C "$WT_ABS"` call in the script discovers upward
# past that missing pointer and silently answers for whatever repository it
# finds next, which for a worktree living under the main tree is the main
# tree itself. The script then reports the main tree's branch and uncommitted
# files as the worktree's own, and --force on it attempts to delete the main
# tree's own checked-out branch - saved only by git's own refusal to delete
# a branch checked out somewhere.
#
# These tests pin that a worktree missing its .git pointer is refused with a
# clear message naming only its own leftover files, never the main repo's,
# and that a normal teardown (no corruption) still completes.

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
    echo '.cache/' > .gitignore
    git add README.md .gitignore
    git commit -qm base --no-verify
}

make_worktree() {
    local name="$1"
    mkdir -p "$MAIN/.worktrees"
    git worktree add -q -b "feature/$name" "$MAIN/.worktrees/$name"
}

@test "a normal worktree tears down cleanly" {
    make_worktree normal
    run "$SCRIPT" normal --into master
    [ "$status" -eq 0 ]
    contains "Teardown complete" "$output"
    [ ! -d "$MAIN/.worktrees/normal" ]
    run git -C "$MAIN" worktree list --porcelain
    lacks ".worktrees/normal" "$output"
}

# A branch made by setup-worktree.sh tracks origin/<default>, and `git branch -d`
# checks containment in that ref. It moves only on a fetch or on a push made by
# remote NAME, so these two cases pin the difference between a ref that is merely
# stale and work that is genuinely not on the remote.
make_remote_worktree() {
    local name="$1"
    REMOTE_PATH="$BATS_TEST_TMPDIR/remote.git"
    git init -q --bare -b master "$REMOTE_PATH"
    git -C "$MAIN" remote add origin "$REMOTE_PATH"
    git -C "$MAIN" push -q origin master
    mkdir -p "$MAIN/.worktrees"
    git -C "$MAIN" worktree add -q -b "feature/$name" "$MAIN/.worktrees/$name" origin/master
    echo work > "$MAIN/.worktrees/$name/work.txt"
    git -C "$MAIN/.worktrees/$name" add work.txt
    git -C "$MAIN/.worktrees/$name" commit -qm work --no-verify
    git -C "$MAIN" merge -q --no-ff -m merge "feature/$name"
}

@test "a stale tracking ref does not report pushed work as unpushed" {
    make_remote_worktree pushed
    # The remote has the work and origin/master does not, as after a push from
    # another machine. A push by URL from here would also move origin/master on
    # git 2.56 and later, so the stale ref is set back by hand.
    local stale
    stale=$(git -C "$MAIN" rev-parse origin/master)
    git -C "$MAIN" push -q "$REMOTE_PATH" master:master
    git -C "$MAIN" update-ref refs/remotes/origin/master "$stale"
    run git -C "$MAIN" merge-base --is-ancestor feature/pushed origin/master
    [ "$status" -ne 0 ]

    run "$SCRIPT" pushed --into master
    [ "$status" -eq 0 ]
    contains "was stale" "$output"
    run git -C "$MAIN" branch --list feature/pushed
    [ -z "$output" ]
}

@test "work the remote does not have keeps its branch" {
    make_remote_worktree unpushed

    run "$SCRIPT" unpushed --into master
    [ "$status" -eq 0 ]
    # The refusal has to be the reason, not a silent skip that happens to leave
    # the branch behind.
    contains "git refused -d" "$output"
    run git -C "$MAIN" branch --list feature/unpushed
    contains "feature/unpushed" "$output"
}

@test "a worktree missing its .git pointer is refused, not misattributed to the main tree" {
    make_worktree broken
    echo leftover > "$MAIN/.worktrees/broken/leftover.txt"
    rm -f "$MAIN/.worktrees/broken/.git"

    # Untracked and never committed, so it exists ONLY in the main tree's own
    # working directory - never checked out into any worktree. A script that
    # escapes to the main tree via `git -C <worktree> status` would report it;
    # one that does not can never mention it.
    echo only-in-main-tree > "$MAIN/main-tree-uncommitted.txt"

    run "$SCRIPT" broken --into master
    [ "$status" -ne 0 ]

    # The whole point: never names the main tree's own state.
    lacks "main-tree-uncommitted.txt" "$output"
    lacks "branch:   master" "$output"

    # It does explain what is actually left in the worktree.
    contains "leftover.txt" "$output"
    contains "no .git of its own" "$output"

    # Refusing must not have touched anything - directory and registration
    # are both still there for the operator (or --force) to act on.
    [ -d "$MAIN/.worktrees/broken" ]
    run git -C "$MAIN" worktree list --porcelain
    contains ".worktrees/broken" "$output"

    # And the main tree's own untracked marker is still just sitting there -
    # nothing tried to report on it, let alone touch it.
    [ -f "$MAIN/main-tree-uncommitted.txt" ]
}

@test "--force finishes a worktree missing its .git pointer without touching the main tree's branch" {
    make_worktree broken2
    echo leftover > "$MAIN/.worktrees/broken2/leftover.txt"
    rm -f "$MAIN/.worktrees/broken2/.git"

    run "$SCRIPT" broken2 --into master --force
    [ "$status" -eq 0 ]

    # Recovery mode skips branch cleanup outright rather than guess at
    # containment from a repo it can no longer safely query - it must never
    # even ATTEMPT a branch deletion.
    lacks "Deleting branch" "$output"

    [ ! -d "$MAIN/.worktrees/broken2" ]
    run git -C "$MAIN" worktree list --porcelain
    lacks ".worktrees/broken2" "$output"

    # The main tree's own branch and tracked file are untouched.
    run git -C "$MAIN" branch --show-current
    contains "master" "$output"
    [ -f "$MAIN/README.md" ]

    run git -C "$MAIN" branch --list "feature/broken2"
    contains "feature/broken2" "$output"
}

@test "an unregistered directory is refused, never treated as a worktree" {
    mkdir -p "$BATS_TEST_TMPDIR/not-a-worktree"
    run "$SCRIPT" "$BATS_TEST_TMPDIR/not-a-worktree" --into master
    [ "$status" -ne 0 ]
    contains "does not list that path as a worktree" "$output"
}

@test "the main tree itself is refused even when named directly" {
    run "$SCRIPT" "$MAIN" --into master
    [ "$status" -ne 0 ]
    contains "that is the main tree" "$output"
}

# ---------------------------------------------------------- submodule pointers
#
# .git/modules/<name> is common to every worktree, so initializing a submodule
# inside one aims that shared core.worktree at it. Teardown has to aim it back
# at the main checkout before deleting the directory, or every other worktree
# symlinking that submodule fails `git status` with "cannot chdir". The two
# module depths carry different ../ runs and are restored independently, and a
# pointer aimed anywhere else is not the script's to touch.

fake_module_pointer() {
    mkdir -p "$MAIN/.git/modules/$1"
    printf '[core]\n\tworktree = %s\n' "$2" > "$MAIN/.git/modules/$1/config"
}

module_pointer() {
    git config --file "$MAIN/.git/modules/$1/config" --get core.worktree
}

@test "a submodule pointer aimed into the removed worktree is restored" {
    make_worktree doomed
    mkdir -p "$MAIN/lib/glm"
    fake_module_pointer glm "../../../.worktrees/doomed/lib/glm"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    [ "$(module_pointer glm)" = "../../../lib/glm" ]
}

@test "a restored pointer keeps the deeper lib/ module ../ run" {
    make_worktree doomed
    mkdir -p "$MAIN/lib/ftxui"
    fake_module_pointer lib/ftxui "../../../../.worktrees/doomed/lib/ftxui"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    [ "$(module_pointer lib/ftxui)" = "../../../../lib/ftxui" ]
}

# The worktree's lib/ftxui is a symlink into the main tree, as setup leaves it,
# and a dry run still sees the pointer through it.
@test "a dry run names the pointer it would restore and writes nothing" {
    make_worktree doomed
    mkdir -p "$MAIN/lib/ftxui" "$MAIN/.worktrees/doomed/lib"
    ln -s "$MAIN/lib/ftxui" "$MAIN/.worktrees/doomed/lib/ftxui"
    fake_module_pointer lib/ftxui "../../../../.worktrees/doomed/lib/ftxui"
    # --force only gets past the untracked symlink; -n still changes nothing.
    run "$SCRIPT" doomed --into master --force -n
    [ "$status" -eq 0 ]
    contains "ftxui: points into this worktree, would restore to ../../../../lib/ftxui" "$output"
    [ "$(module_pointer lib/ftxui)" = "../../../../.worktrees/doomed/lib/ftxui" ]
    [ -d "$MAIN/.worktrees/doomed" ]
}

@test "a pointer into a sibling whose name extends the worktree's is left alone" {
    make_worktree doomed
    mkdir -p "$MAIN/.worktrees/doomed-2/lib"
    fake_module_pointer lib/x "../../../../.worktrees/doomed-2/lib/x"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    [ "$(module_pointer lib/x)" = "../../../../.worktrees/doomed-2/lib/x" ]
}

@test "a pointer aimed outside the removed worktree is left alone" {
    make_worktree doomed
    mkdir -p "$MAIN/lib/spdlog"
    fake_module_pointer spdlog "../../../lib/spdlog"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    [ "$(module_pointer spdlog)" = "../../../lib/spdlog" ]
}

# A tree whose owner is reading code rather than building runs no compiler, holds
# no commits and reports a clean status, so every other guard passes it. The
# advisory claim is the only signal that phase leaves behind.
make_claimed_worktree() {
    local name="$1"
    mkdir -p "$MAIN/scripts"
    cp "$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/helix-claim" "$MAIN/scripts/helix-claim"
    chmod +x "$MAIN/scripts/helix-claim"
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"
    make_worktree "$name"
    "$MAIN/scripts/helix-claim" take "worktree:$name" "reading code" >/dev/null 2>&1
}

@test "teardown refuses a worktree whose claim has a live owner" {
    make_claimed_worktree claimed
    run "$SCRIPT" claimed --into master
    [ "$status" -eq 1 ]
    contains "is claimed and its owner is alive" "$output"
    [ -d "$MAIN/.worktrees/claimed" ]
}

@test "--force removes a worktree despite a live claim" {
    make_claimed_worktree forced
    run "$SCRIPT" forced --into master --force
    [ "$status" -eq 0 ]
    contains "removing anyway" "$output"
    [ ! -d "$MAIN/.worktrees/forced" ]
}

@test "a released claim lets teardown proceed" {
    make_claimed_worktree released
    "$MAIN/scripts/helix-claim" release "worktree:released" >/dev/null 2>&1
    run "$SCRIPT" released --into master
    [ "$status" -eq 0 ]
    contains "Teardown complete" "$output"
    [ ! -d "$MAIN/.worktrees/released" ]
}

# A branch carrying commits of its own is what --force-branch exists for: git
# refuses -d on it, so the script must either keep it or discard it on request,
# and must never describe the discarding case as lossless.
make_diverged_worktree() {
    local name="$1"
    make_worktree "$name"
    echo diverged > "$MAIN/.worktrees/$name/own.txt"
    git -C "$MAIN/.worktrees/$name" add own.txt
    git -C "$MAIN/.worktrees/$name" commit -qm "work only on this branch" --no-verify
}

@test "a branch --into does not contain survives teardown by default" {
    make_diverged_worktree diverged
    run "$SCRIPT" diverged --into master
    [ "$status" -eq 0 ]
    contains "NOT contained" "$output"
    contains "--force-branch" "$output"
    run git -C "$MAIN" branch --list feature/diverged
    contains "feature/diverged" "$output"
}

@test "--force-branch deletes a branch --into does not contain" {
    make_diverged_worktree diverged
    run "$SCRIPT" diverged --into master --force-branch
    [ "$status" -eq 0 ]
    contains "discards" "$output"
    [ ! -d "$MAIN/.worktrees/diverged" ]
    run git -C "$MAIN" branch --list feature/diverged
    lacks "feature/diverged" "$output"
}

# The busy guard finds processes by command name (make among them) and by cwd. A
# copy of sleep named make stands in for a build parked in a given directory.
park_fake_build() {
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    cp "$(command -v sleep)" "$BATS_TEST_TMPDIR/bin/make"
    (cd "$1" && exec "$BATS_TEST_TMPDIR/bin/make" 300) &
    PARKED_PID=$!
}

teardown() {
    if [[ -n "${PARKED_PID:-}" ]]; then kill "$PARKED_PID" 2>/dev/null || true; fi
}

@test "a build in a sibling whose name extends the worktree's does not block teardown" {
    make_worktree doomed
    make_worktree doomed-1.0
    park_fake_build "$MAIN/.worktrees/doomed-1.0"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    lacks "processes are running" "$output"
    [ ! -d "$MAIN/.worktrees/doomed" ]
    [ -d "$MAIN/.worktrees/doomed-1.0" ]
}

@test "a build inside the worktree blocks teardown" {
    make_worktree doomed
    mkdir -p "$MAIN/.worktrees/doomed/sub"
    park_fake_build "$MAIN/.worktrees/doomed/sub"
    run "$SCRIPT" doomed --into master
    [ "$status" -ne 0 ]
    contains "processes are running inside that worktree" "$output"
    contains " $PARKED_PID" "$output"
    [ -d "$MAIN/.worktrees/doomed" ]
}

# The tree's mirror on the test host goes with it, best effort: a host that is
# unreachable must never stop a teardown, and with none configured nothing is asked.
stub_test_host_run() { # <exit code>
    mkdir -p "$MAIN/scripts"
    printf '#!/bin/sh\necho "$*" >> "%s"\nexit %s\n' "$BATS_TEST_TMPDIR/host-calls" "$1" > "$MAIN/scripts/test-host-run.sh"
    chmod +x "$MAIN/scripts/test-host-run.sh"
}

@test "teardown drops the tree's test-host mirror" {
    make_worktree mirrored
    stub_test_host_run 0
    HELIX_TEST_HOST=testhost.invalid run "$SCRIPT" mirrored --into master
    [ "$status" -eq 0 ]
    [ "$(cat "$BATS_TEST_TMPDIR/host-calls")" = "--drop mirrored" ]
}

@test "with no test host configured, teardown asks for no mirror drop" {
    make_worktree unhosted
    stub_test_host_run 0
    run "$SCRIPT" unhosted --into master
    [ "$status" -eq 0 ]
    [ ! -e "$BATS_TEST_TMPDIR/host-calls" ]
    lacks "mirror" "$output"
}

@test "an unreachable test host warns and the teardown still completes" {
    make_worktree offline
    stub_test_host_run 1
    HELIX_TEST_HOST=testhost.invalid run "$SCRIPT" offline --into master
    [ "$status" -eq 0 ]
    contains "mirror on testhost.invalid" "$output"
    contains "Teardown complete" "$output"
    [ ! -d "$MAIN/.worktrees/offline" ]
}

@test "a test host that never answers cannot stall the teardown" {
    make_worktree hung
    mkdir -p "$MAIN/scripts"
    printf '#!/bin/sh\nexec sleep 60\n' > "$MAIN/scripts/test-host-run.sh"
    chmod +x "$MAIN/scripts/test-host-run.sh"
    local t0=$SECONDS
    HELIX_TEST_HOST=testhost.invalid HELIX_TEST_HOST_DROP_TIMEOUT=1 run "$SCRIPT" hung --into master
    [ "$status" -eq 0 ]
    [ $((SECONDS - t0)) -lt 15 ]
    contains "mirror on testhost.invalid" "$output"
}

@test "a tree name that can have no mirror tears down without a warning" {
    make_worktree plain
    stub_test_host_run 2
    HELIX_TEST_HOST=testhost.invalid run "$SCRIPT" plain --into master
    [ "$status" -eq 0 ]
    [ "$(cat "$BATS_TEST_TMPDIR/host-calls")" = "--drop plain" ]
    lacks "mirror" "$output"
}

# A running clangd keeps writing its index under the tree's own .cache/ while
# the removal runs; that directory is disposable and must not fail the teardown.
@test "a .cache that keeps being written during removal is retried until gone" {
    make_worktree cached
    local wt="$MAIN/.worktrees/cached"
    mkdir -p "$wt/.cache/clangd/index"
    echo x > "$wt/.cache/clangd/index/f0.idx"
    # The writer runs inside the script's retry pause, which is exactly when a
    # live clangd lands an index write after rm -rf: the first two pauses each
    # recreate .cache, so removal needs a third attempt. Driving it from the
    # pause, not a background loop, makes the overlap independent of load.
    local shim="$BATS_TEST_TMPDIR/shim"
    mkdir -p "$shim"
    cat > "$shim/sleep" <<SHIM
#!/usr/bin/env bash
n=\$(cat "$shim/writes" 2>/dev/null || echo 0)
if [ -d "$wt" ] && [ ! -e "$wt/.cache" ] && [ "\$n" -lt 2 ]; then
    mkdir -p "$wt/.cache/clangd/index"
    echo x > "$wt/.cache/clangd/index/f\$n.idx"
    echo \$((n + 1)) > "$shim/writes"
fi
exec $(command -v sleep) "\$@"
SHIM
    chmod +x "$shim/sleep"
    PATH="$shim:$PATH" HELIX_TEARDOWN_CACHE_RETRY_SLEEP=0 run "$SCRIPT" cached --into master
    [ "$status" -eq 0 ]
    [ "$(cat "$shim/writes")" -eq 2 ]
    contains "Teardown complete" "$output"
    lacks "files remain" "$output"
    contains ".cache/ was still being written; removed it on attempt 3" "$output"
    [ ! -d "$wt" ]
}

half_remove() {
    find "$1" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
}

@test "a tree holding only .git is finished when its branch is contained in --into" {
    make_worktree half
    local wt="$MAIN/.worktrees/half"
    half_remove "$wt"
    run git -C "$wt" status --porcelain
    contains " D README.md" "$output"

    run "$SCRIPT" half --into master
    [ "$status" -eq 0 ]
    contains "half-removed tree" "$output"
    contains "Teardown complete" "$output"
    [ ! -d "$wt" ]
}

@test "a half-removed tree whose branch is not contained still refuses" {
    make_worktree halfwork
    local wt="$MAIN/.worktrees/halfwork"
    echo more > "$wt/more.txt"
    git -C "$wt" add more.txt
    git -C "$wt" commit -qm unmerged --no-verify
    half_remove "$wt"

    run "$SCRIPT" halfwork --into master
    [ "$status" -ne 0 ]
    contains "uncommitted change" "$output"
    [ -d "$wt" ]
}

@test "real modifications next to deletions still refuse" {
    make_worktree dirty
    local wt="$MAIN/.worktrees/dirty"
    echo more >> "$wt/README.md"
    echo scratch > "$wt/untracked.txt"
    run "$SCRIPT" dirty --into master
    [ "$status" -ne 0 ]
    contains "uncommitted change" "$output"
    [ -f "$wt/untracked.txt" ]
}

@test "a half-removed tree with a lone deleted file among live ones is refused" {
    make_worktree live
    local wt="$MAIN/.worktrees/live"
    echo second > "$wt/second.txt"
    git -C "$wt" add second.txt
    git -C "$wt" commit -qm second --no-verify
    git -C "$MAIN" merge -q feature/live
    rm "$wt/README.md"
    run "$SCRIPT" live --into master
    [ "$status" -ne 0 ]
    contains "uncommitted change" "$output"
    [ -f "$wt/second.txt" ]
}

@test "deletions mixed with a staged modification are refused" {
    make_worktree mixed
    local wt="$MAIN/.worktrees/mixed"
    rm "$wt/.gitignore"
    echo more >> "$wt/README.md"
    git -C "$wt" add README.md
    run "$SCRIPT" mixed --into master
    [ "$status" -ne 0 ]
    contains "uncommitted change" "$output"
}

# A private submodule's git directory lives under the worktree's admin dir and
# outlives the deleted checkout; the commits in it are what a prune destroys.
# Builds one with a pushed commit (on a remote-tracking ref) and one on top.
make_module_gitdir() {
    local name="$1" unpushed="$2" admin src
    admin="$(git -C "$MAIN" rev-parse --path-format=absolute --git-common-dir)/worktrees/$name"
    src="$BATS_TEST_TMPDIR/modsrc-$name"
    mkdir -p "$src"
    git -C "$src" init -q -b main .
    git -C "$src" config user.email t@example.com
    git -C "$src" config user.name tester
    echo a > "$src/a.txt"; git -C "$src" add a.txt; git -C "$src" commit -qm pushed --no-verify
    git -C "$src" update-ref refs/remotes/origin/main HEAD
    if [ "$unpushed" = yes ]; then
        echo b > "$src/b.txt"; git -C "$src" add b.txt; git -C "$src" commit -qm unpushed-work --no-verify
    fi
    mkdir -p "$admin/modules/lib"
    mv "$src/.git" "$admin/modules/lib/helix-xml"
}

@test "unpushed commits in a submodule gitdir refuse the teardown while the checkout exists" {
    make_worktree withmod
    make_module_gitdir withmod yes
    run "$SCRIPT" withmod --into master
    [ "$status" -ne 0 ]
    contains "lib/helix-xml has 1 commit" "$output"
    contains "unpushed-work" "$output"
    [ -d "$MAIN/.worktrees/withmod" ]
}

@test "a half-removed tree cannot finish while its submodule gitdir holds unpushed commits" {
    make_worktree halfmod
    make_module_gitdir halfmod yes
    half_remove "$MAIN/.worktrees/halfmod"
    run "$SCRIPT" halfmod --into master
    [ "$status" -ne 0 ]
    contains "unpushed-work" "$output"
    [ -e "$MAIN/.worktrees/halfmod/.git" ]
}

@test "--force discards the submodule commits it listed" {
    make_worktree forcemod
    make_module_gitdir forcemod yes
    run "$SCRIPT" forcemod --into master --force
    [ "$status" -eq 0 ]
    contains "DISCARDED" "$output"
    [ ! -d "$MAIN/.worktrees/forcemod" ]
}

@test "a submodule gitdir with everything pushed does not block" {
    make_worktree pushedmod
    make_module_gitdir pushedmod no
    run "$SCRIPT" pushedmod --into master
    [ "$status" -eq 0 ]
    contains "Teardown complete" "$output"
}

@test "submodule commits the --into branch already pins are pushed, whatever the tracking refs say" {
    make_worktree pinnedmod
    make_module_gitdir pinnedmod yes
    local admin pin
    admin="$(git -C "$MAIN" rev-parse --path-format=absolute --git-common-dir)/worktrees/pinnedmod"
    pin="$(git --git-dir="$admin/modules/lib/helix-xml" rev-parse HEAD)"
    git -C "$MAIN" update-index --add --cacheinfo "160000,$pin,lib/helix-xml"
    git -C "$MAIN" commit -qm "pin helix-xml" --no-verify
    run "$SCRIPT" pinnedmod --into master
    [ "$status" -eq 0 ]
    lacks "unpushed-work" "$output"
    contains "Teardown complete" "$output"
}

@test "only lib/helix-xml is checked: a third-party module's local history never blocks" {
    make_worktree luamod
    make_module_gitdir luamod yes
    local admin
    admin="$(git -C "$MAIN" rev-parse --path-format=absolute --git-common-dir)/worktrees/luamod"
    mv "$admin/modules/lib/helix-xml" "$admin/modules/lib/lua"
    run "$SCRIPT" luamod --into master
    [ "$status" -eq 0 ]
    lacks "lib/lua has" "$output"
    contains "Teardown complete" "$output"
}
