#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build on a remote host without pushing the source tree across your link.
#
# WHY THIS EXISTS
#   `make remote-sync` rsyncs the working tree. rsync is delta-based, but it
#   still exchanges metadata for every file under lib/ and assets/ before it can
#   decide nothing changed, and a fresh destination directory transfers the whole
#   ~260 MB. On a high-latency link (a hotel, a phone tether) that dominates the
#   build.
#
#   The build host has its own internet. So it fetches the committed history from
#   GitHub itself, at its own speed, and the only thing that crosses YOUR link is
#   a patch of what is not pushed yet — usually a few KB.
#
# WHAT CROSSES YOUR LINK
#   1. `git diff <newest-commit-origin-has>` — every local commit AND every
#      uncommitted edit, as one patch.
#   2. A tar of untracked files (new tests, new sources).
#   Nothing else. Submodules, assets and history come from GitHub to the remote.
#
# USAGE
#   scripts/remote-build.sh [make targets...]        # default: test
#   REMOTE_HOST=zeus scripts/remote-build.sh -j32 test
#   scripts/remote-build.sh --run '[cfs][homing]'    # build, then run one tag
#
set -euo pipefail

REMOTE_HOST="${REMOTE_HOST:-thelio}"
REMOTE_DIR="${REMOTE_DIR:-\$HOME/helix-remote}"
JOBS="${JOBS:-}"
RUN_TAG=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --run) RUN_TAG="$2"; shift 2 ;;
        --host) REMOTE_HOST="$2"; shift 2 ;;
        --dir) REMOTE_DIR="$2"; shift 2 ;;
        *) break ;;
    esac
done
MAKE_ARGS=("$@")
[[ ${#MAKE_ARGS[@]} -eq 0 ]] && MAKE_ARGS=(test)

# One TCP+SSH handshake for the whole run, reused by every ssh/scp below. On a
# high-latency link the handshakes cost more than the payload does.
CTL="${TMPDIR:-/tmp}/helix-rb-%r@%h:%p"
SSH_OPTS=(-o ControlMaster=auto -o "ControlPath=$CTL" -o ControlPersist=60s -o Compression=yes)
ssh_() { ssh "${SSH_OPTS[@]}" "$@"; }
cleanup_ctl() { ssh "${SSH_OPTS[@]}" -O exit "$REMOTE_HOST" 2>/dev/null || true; }

say() { printf '\033[36m→\033[0m %s\n' "$*"; }
die() { printf '\033[31m✗\033[0m %s\n' "$*" >&2; exit 1; }

git rev-parse --git-dir >/dev/null 2>&1 || die "not a git repository"
ORIGIN_URL=$(git remote get-url origin) || die "no origin remote"

# The newest commit origin already has. Everything after it — committed or not —
# travels as a patch. `git rev-list HEAD --not --remotes=origin` lists local-only
# commits newest-first, so the oldest one's parent is what the remote can fetch.
OLDEST_LOCAL=$(git rev-list HEAD --not --remotes=origin | tail -1)
if [[ -n "$OLDEST_LOCAL" ]]; then
    BASE=$(git rev-parse "${OLDEST_LOCAL}^")
else
    BASE=$(git rev-parse HEAD)
fi
say "base (fetched from GitHub by $REMOTE_HOST): $(git rev-parse --short "$BASE")"

# Submodule pins travel as text, not content: the remote checks them out itself.
SUBMODULE_PINS=$(git ls-tree HEAD lib/ | awk '$2 == "commit" { print $4" "$3 }')

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"; cleanup_ctl' EXIT

# Tracked delta. Submodule gitlinks are excluded — `git apply` cannot act on them
# and the pins are handled separately above.
git diff --binary "$BASE" -- . ':(exclude)lib' ':(exclude).claude-recall' \
    > "$WORK/local.patch" || true
PATCH_BYTES=$(wc -c < "$WORK/local.patch" | tr -d ' ')

# Untracked files the patch cannot carry (a brand-new test or source file).
git ls-files --others --exclude-standard \
    -- . ':(exclude)build' ':(exclude).claude-recall' > "$WORK/untracked.txt"
UNTRACKED_COUNT=$(wc -l < "$WORK/untracked.txt" | tr -d ' ')
if [[ "$UNTRACKED_COUNT" -gt 0 ]]; then
    tar czf "$WORK/untracked.tgz" -T "$WORK/untracked.txt"
else
    : > "$WORK/untracked.tgz"
fi
TAR_BYTES=$(wc -c < "$WORK/untracked.tgz" | tr -d ' ')

say "sending $((PATCH_BYTES / 1024)) KB patch + $((TAR_BYTES / 1024)) KB of $UNTRACKED_COUNT untracked file(s)"

# One-time clone happens ON the remote, from GitHub — not over your link.
ssh_ "$REMOTE_HOST" bash -se <<REMOTE_SETUP
set -euo pipefail
DIR="$REMOTE_DIR"
if [ ! -d "\$DIR/.git" ]; then
    echo "→ first run: cloning on \$(hostname) from GitHub (not over your link)"
    rm -rf "\$DIR"
    git clone --recurse-submodules "$ORIGIN_URL" "\$DIR"
fi
REMOTE_SETUP

# The payload goes over scp, NOT piped into ssh: the build script below is
# itself delivered on stdin via heredoc, and one stdin cannot carry both.
PAYLOAD_REMOTE="/tmp/helix-remote-build-$$.tar"
tar czf "$WORK/payload.tar" -C "$WORK" local.patch untracked.tgz
scp -q "${SSH_OPTS[@]}" "$WORK/payload.tar" "$REMOTE_HOST:$PAYLOAD_REMOTE"

ssh_ "$REMOTE_HOST" bash -se <<REMOTE_BUILD
set -euo pipefail
DIR="$REMOTE_DIR"
cd "\$DIR"

TMP=\$(mktemp -d); trap 'rm -rf "\$TMP" "$PAYLOAD_REMOTE"' EXIT
tar xzf "$PAYLOAD_REMOTE" -C "\$TMP"

git fetch --quiet origin
git rev-parse --verify --quiet $BASE >/dev/null || {
    echo "✗ $BASE not on origin yet — commit and push, or it cannot be fetched" >&2
    exit 1
}
# reset --hard, not checkout: the previous run left its patch applied as tracked
# modifications, and checkout refuses to discard those. This is a scratch clone
# whose only content is what this script puts there.
git reset --quiet --hard $BASE
git clean -qfd -e build -e .ccache || true

while read -r path sha; do
    [ -n "\$path" ] || continue
    git -C "\$path" fetch --quiet origin 2>/dev/null || true
    git -C "\$path" checkout --quiet "\$sha" 2>/dev/null || echo "⚠ \$path: no \$sha"
done <<'PINS'
$SUBMODULE_PINS
PINS

if [ -s "\$TMP/local.patch" ]; then
    git apply --whitespace=nowarn "\$TMP/local.patch" || {
        echo "✗ patch did not apply — the remote base may be stale" >&2; exit 1; }
fi
if [ -s "\$TMP/untracked.tgz" ]; then
    tar xzf "\$TMP/untracked.tgz" -C "\$DIR"
fi

# libhv's configure output is host-specific, is never regenerated once
# libhv.a exists, and is written by the build itself — so a TU can include it
# mid-write and die on "unterminated #ifndef". Settle it BEFORE make, and treat
# a file without its closing #endif as absent rather than trusting existence.
if ! tail -1 lib/libhv/hconfig.h 2>/dev/null | grep -q '#endif'; then
    (cd lib/libhv && ./configure --with-http-client >/dev/null 2>&1) \
        || echo "⚠ libhv configure failed"
fi

JOBS_ARG="${JOBS:--j\$(nproc)}"
echo "→ building on \$(hostname): make \$JOBS_ARG ${MAKE_ARGS[*]}"
make \$JOBS_ARG ${MAKE_ARGS[*]}
RC=\$?
echo "BUILD_RC=\$RC"
[ \$RC -eq 0 ] || exit \$RC

if [ -n "$RUN_TAG" ]; then
    echo "→ ./build/bin/helix-tests '$RUN_TAG'"
    ./build/bin/helix-tests "$RUN_TAG"
fi
REMOTE_BUILD
