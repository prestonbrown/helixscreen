#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build provenance and deploy wiring.
#
# Both rules here exist because their failure mode is SILENT — the build
# succeeds and the deploy reports success, and you only find out when the
# evidence you collected on a printer turns out to be unattributable, or when
# `helix-screen ctl` cannot reach a device you deliberately built ctl into.

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# --- HELIX_GIT_HASH override -----------------------------------------------
# A cross build compiles inside a container that bind-mounts the source tree
# only. A git WORKTREE's .git is a file pointing at $MAIN/.git/worktrees/<name>,
# a path outside that mount, so git cannot resolve HEAD in there. mk/cross.mk
# resolves it on the host and passes it in; this is the receiving end.

@test "gen-git-hash honours HELIX_GIT_HASH when git cannot resolve HEAD" {
    tmp="$BATS_TEST_TMPDIR/nogit"
    mkdir -p "$tmp"
    cp scripts/gen-git-hash.sh "$tmp/"
    # HOME and cwd both outside any repo: git genuinely cannot answer here.
    run env -u GIT_DIR HELIX_GIT_HASH=deadbee1 BUILD_DIR="$tmp/build" \
        sh -c "cd '$tmp' && ./gen-git-hash.sh"
    [ "$status" -eq 0 ]
    run cat "$tmp/build/generated/helix_git_hash.h"
    [[ "$output" == *'#define HELIX_GIT_HASH "deadbee1"'* ]]
}

@test "gen-git-hash still reports unknown outside a repo with no override" {
    tmp="$BATS_TEST_TMPDIR/nogit2"
    mkdir -p "$tmp"
    cp scripts/gen-git-hash.sh "$tmp/"
    run env -u HELIX_GIT_HASH -u GIT_DIR BUILD_DIR="$tmp/build" \
        sh -c "cd '$tmp' && ./gen-git-hash.sh"
    [ "$status" -eq 0 ]
    run cat "$tmp/build/generated/helix_git_hash.h"
    [[ "$output" == *'#define HELIX_GIT_HASH "unknown"'* ]]
}

@test "gen-git-hash prefers the override over a resolvable git HEAD" {
    # In the repo, where git WOULD answer — the override must still win, since
    # that is exactly the container case: git resolves the wrong thing or
    # nothing, and the host already knows the right answer.
    run env HELIX_GIT_HASH=cafe1234 BUILD_DIR="$BATS_TEST_TMPDIR/b" ./scripts/gen-git-hash.sh
    [ "$status" -eq 0 ]
    run cat "$BATS_TEST_TMPDIR/b/generated/helix_git_hash.h"
    [[ "$output" == *'"cafe1234"'* ]]
}

# --- every source-mounting container gets the host context ------------------

@test "every docker run that mounts the source passes DOCKER_HOST_CONTEXT" {
    # -v "$(PWD)":/src is the marker for "this container builds our tree".
    # The cert-extraction runs (docker run --rm <img> cat /etc/ssl/...) mount
    # nothing and are correctly exempt.
    run bash -c "grep -n 'docker run' mk/cross.mk | grep -F '\"\$(PWD)\":/src' | grep -v 'DOCKER_HOST_CONTEXT'"
    [ "$status" -eq 1 ] || {
        echo "docker run sites that build the tree without the host context:" >&2
        echo "$output" >&2
        echo "-> add \$(DOCKER_HOST_CONTEXT); without it a worktree build has no" >&2
        echo "   submodules and stamps HELIX_GIT_HASH \"unknown\"." >&2
        return 1
    }
}

@test "DOCKER_HOST_CONTEXT carries both the worktree mount and the git hash" {
    run grep -E '^DOCKER_HOST_CONTEXT *=' mk/cross.mk
    [ "$status" -eq 0 ]
    [[ "$output" == *'DOCKER_WORKTREE_MOUNT'* ]]
    [[ "$output" == *'DOCKER_GIT_HASH_ENV'* ]]
}

@test "an inherited HELIX_GIT_HASH wins over the local git lookup" {
    # remote-sync excludes .git, so on the build host there is nothing to look
    # up; the value has to come from the machine that has the checkout.
    run make -n k1-docker HELIX_GIT_HASH=feedface
    [ "$status" -eq 0 ]
    [[ "$output" == *"-e HELIX_GIT_HASH=feedface"* ]]
}

@test "every remote build invocation carries the git hash to the build host" {
    run bash -c "grep -n 'ssh \$(REMOTE_SSH_TARGET) \"cd \$(REMOTE_DIR) && ' mk/remote.mk \
                 | grep -F 'make ' | grep -v 'make clean' | grep -v 'REMOTE_MAKE_ENV'"
    [ "$status" -eq 1 ] || {
        echo "remote build invocations with no HELIX_GIT_HASH:" >&2
        echo "$output" >&2
        echo "-> prefix with \$(REMOTE_MAKE_ENV); the build host has no .git." >&2
        return 1
    }
}

# --- build features stamp ---------------------------------------------------

@test "the link rule records optional subsystems beside the binary" {
    run grep -F 'build-features' mk/rules.mk
    [ "$status" -eq 0 ]
    [[ "$output" == *'remote_control'* ]]
}

@test "every deploy target syncs device features" {
    # deploy-common covers the rsync platforms; the tar/ssh and -bin variants
    # call sync-device-features directly. A target reaching neither deploys a
    # binary with the ctl server compiled in and leaves it unreachable, exactly
    # the way every deploy did before this test existed.
    run python3 - <<'PY'
import re, sys
cur, blocks = None, {}
for line in open("mk/cross.mk"):
    m = re.match(r'^(deploy-[a-z0-9-]+):(.*)', line)
    if m:
        cur = m.group(1)
        blocks[cur] = [m.group(2)]
    elif cur is not None:
        if line.startswith("\t"):
            blocks[cur].append(line)
        elif line.strip() and not line.startswith("#"):
            cur = None

def syncs_directly(name):
    text = "".join(blocks[name][1:])
    return "deploy-common" in text or "sync-device-features" in text

def covered(name, seen=()):
    if name in seen:
        return False
    if syncs_directly(name):
        return True
    # A -fg / -asan-fg variant declares the full deploy as a prerequisite and
    # only restarts afterwards, so the sync its prerequisite ran still stands.
    prereqs = blocks[name][0].split()
    return any(pr in blocks and covered(pr, seen + (name,)) for pr in prereqs)

missing = [name for name in blocks if not covered(name)]

if missing:
    print("deploy targets with no device-feature sync: " + ", ".join(sorted(missing)))
    sys.exit(1)
PY
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }
}
