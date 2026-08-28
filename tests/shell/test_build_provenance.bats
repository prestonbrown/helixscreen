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
    # -v "$(CURDIR)":/src is the marker for "this container builds our tree".
    # The cert-extraction runs (docker run --rm <img> cat /etc/ssl/...) mount
    # nothing and are correctly exempt.
    run bash -c "grep -n 'docker run' mk/cross.mk | grep -F '\"\$(CURDIR)\":/src' | grep -v 'DOCKER_HOST_CONTEXT'"
    [ "$status" -eq 1 ] || {
        echo "docker run sites that build the tree without the host context:" >&2
        echo "$output" >&2
        echo "-> add \$(DOCKER_HOST_CONTEXT); without it a worktree build has no" >&2
        echo "   submodules and stamps HELIX_GIT_HASH \"unknown\"." >&2
        return 1
    }
}

@test "every docker run mounts \$(CURDIR), not \$(PWD)" {
    # $(PWD) is inherited from the invoking SHELL; $(CURDIR) is make's own
    # working directory and is the one that tracks -C. They agree for a plain
    # `make`, so this is invisible until someone runs
    #   make -C .worktrees/<branch> <platform>-docker
    # from a shell sitting somewhere else. Then the container bind-mounts the
    # shell's tree, compiles THAT source into THAT build dir, and leaves the
    # worktree's artifact untouched — while exiting 0 and printing
    # "Build complete!". The binary you then deploy is built from the wrong
    # commit, and nothing in the log says so.
    run bash -c "grep -n 'docker run' mk/cross.mk | grep -F '\$(PWD)'"
    [ "$status" -eq 1 ] || {
        echo "docker run sites mounting \$(PWD) instead of \$(CURDIR):" >&2
        echo "$output" >&2
        echo "-> use \$(CURDIR) so 'make -C <worktree>' builds that worktree." >&2
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

# --- developer vs production remote control ---------------------------------
# Every developer build carries the helixctl server; only the production
# packaging path drops it. Both halves fail SILENTLY if they regress — a dev
# rig you cannot drive reads as "ctl is broken", and a release that ships the
# server exposes a socket that can drive the whole UI on a customer's printer.

@test "a developer cross build gets the remote-control server" {
    run make -n k2-docker
    [ "$status" -eq 0 ]
    [[ "$output" == *'ENABLE_REMOTE_CONTROL=yes'* ]]
}

@test "a production packaging build does not" {
    # `make -n package-*` stops at release-*'s missing order-only binaries, which
    # is expected on a tree that has not cross-built. Asserting the docker line
    # was emitted is what proves make expanded the recipe before it stopped —
    # without it, a makefile that failed EARLIER would read green.
    run make -n package-k2
    [[ "$output" == *'toolchain-k2'* ]]
    [[ "$output" == *'ENABLE_REMOTE_CONTROL=no'* ]]
    [[ "$output" != *'ENABLE_REMOTE_CONTROL=yes'* ]]
}

@test "an explicit ENABLE_REMOTE_CONTROL still wins over the default" {
    run make -n k2-docker ENABLE_REMOTE_CONTROL=no
    [ "$status" -eq 0 ]
    [[ "$output" == *'ENABLE_REMOTE_CONTROL=no'* ]]
    [[ "$output" != *'ENABLE_REMOTE_CONTROL=yes'* ]]
}

@test "package-% marks packaging for every platform, shared docker targets included" {
    # package-k1 and package-ad5x both drive mips-docker. A per-target variable
    # written on the docker rule instead of the package rule would give one of
    # them the developer default.
    for t in package-ad5m package-cc1 package-pi package-pi32 package-k1 \
             package-ad5x package-k1-dynamic package-k2 package-snapmaker-u1 \
             package-x86; do
        run make -n "$t"
        # See the note above on why status is not asserted here.
        [[ "$output" == *'ENABLE_REMOTE_CONTROL=no'* ]] || {
            echo "$t never forwarded the packaging default"
            return 1
        }
        [[ "$output" != *'ENABLE_REMOTE_CONTROL=yes'* ]] || {
            echo "$t forwarded ENABLE_REMOTE_CONTROL=yes into a production build"
            return 1
        }
    done
}

@test "CI's release workflow marks its build as packaging" {
    # release.yml builds through a bare `make PLATFORM_TARGET=...` inside its own
    # container and never runs package-*, so the marker has to be explicit there.
    run grep -F 'make PLATFORM_TARGET=' .github/workflows/release.yml
    [ "$status" -eq 0 ]
    [[ "$output" == *'HELIX_PACKAGING=1'* ]]
}

@test "every release target refuses a binary built with the server" {
    # The stamp is what the binary actually contains; HELIX_PACKAGING is only
    # what the caller intended. release-* copies whatever is already in
    # build/<plat>/bin, so it has to check the former.
    run python3 - <<'PY'
import re, sys
text = open("mk/cross.mk").read()
missing = []
for m in re.finditer(r'^(release-[a-z0-9-]+):\s*\|\s*(.+)$', text, re.M):
    target, prereqs = m.group(1), m.group(2)
    body = text[m.end():]
    body = body[:body.index("\n\n")] if "\n\n" in body else body
    dirs = {d.group(1) for d in re.finditer(r'(build/[a-z0-9-]+/bin)/', prereqs)}
    checked = set(re.findall(r'assert-no-remote-control,(build/[a-z0-9-]+/bin)\)', body))
    if dirs - checked:
        missing.append("%s: unchecked %s" % (target, ", ".join(sorted(dirs - checked))))
if missing:
    print("\n".join(missing))
    sys.exit(1)
PY
    [ "$status" -eq 0 ]
}

@test "the fbdev half of a dual-link build gets the same stamp" {
    # pi-both/pi32-both/x86-both link a second binary into build/<plat>-fbdev/,
    # a directory mk/rules.mk's link rule never touches. deploy-*-fbdev and
    # release-* both read the stamp from the dir they are handling, so an
    # unstamped fbdev binary silently loses its ctl wiring.
    run grep -F 'build-features' mk/pi-dual-link.mk
    [ "$status" -eq 0 ]
    [[ "$output" == *'remote_control'* ]]
}
