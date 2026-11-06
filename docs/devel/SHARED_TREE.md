# Sharing the Tree With Other Sessions

Several agent sessions (Claude, opencode, a human) usually work in this repo at once, often in
the main working tree. Root `CLAUDE.md` § "Sharing This Tree" holds the rules. This doc is the
manual behind them: `scripts/helix-claim`, the git hooks that use it, and how to read a tree
someone else may be in the middle of.

## helix-claim

`scripts/helix-claim` is a plain shell script with no Claude dependency, so opencode, a human or
a script can use it. `AGENTS.md` is a symlink to `CLAUDE.md`, so every agent reads the same rule.

```bash
scripts/helix-claim check worktree:main        # FREE | LIVE | STALE  (exit 1 if LIVE)
if scripts/helix-claim take device:k2plus deploy --pid $$ --note 192.168.1.50; then  # gate on the exit code; never pipe take
    trap 'scripts/helix-claim release-if-owned-by $$ device:k2plus' EXIT; make deploy-k2 K2_HOST=192.168.1.50; fi
scripts/helix-claim take worktree:main edit --session   # owner = this session's `claude` process (--pid auto); errors if none is an ancestor
scripts/helix-claim take worktree:main edit --ack-red-main   # worktree:main is refused while Build / Code Quality is red on origin/main; fails open without gh; the merge hook always acks
scripts/helix-claim list                       # everything, with derived liveness
scripts/helix-claim resources                  # memory, load, claims, top RSS, test host: before heavy work
scripts/helix-claim run heavy:sweep -- make unit-sweep   # claimed while it runs
scripts/helix-claim jobs -v                    # make's -j: the jobpool's size, or cores capped by memory
```

Resource kinds: `worktree:<name>`, `build:<name>`, `device:<printer>` (a role; put the IP in
`--note`), `heavy:<what>`, `gh:issue:<n>`, `socket:<path>`.

**Hold `worktree:<name>` from your first edit until the commit lands**, not only around a merge,
rebase or long commit. Uncommitted files with no claim and an old mtime are indistinguishable
from abandoned work, and `build:<name>` reserves nothing on its own.

**Worktree names are derived, not trusted.** `worktree:main`, `worktree:helixscreen` and a bare
`worktree:` all resolve to the same tree, matched by directory basename or checked-out branch.
With free-form names two sessions could claim one tree under two spellings and both read FREE,
and an advisory lock must never fail open.

**`build:<tree>` and `worktree:<tree>` name the same directory and exclude each other**, in both
directions. Writing under a running build corrupts the build, and building while files move
gives a binary matching no commit. A take consults its sibling, so `check worktree:main` reports
a live `build:` holder instead of FREE. One session may hold both on its own tree; only a
different owner blocks.

**Liveness is derived from process state, never asserted.** A claim records its owner's pid and
that pid's start time (from `/proc`, or `ps` on a Mac). A crashed owner reads STALE on its own,
pid reuse cannot fake LIVE, and nothing needs cleaning up.

**Address a peer by the `message=uds:...sock` in their `helix-claim list` row.** Every live claim
carries one. `ListAgents` names do not map to claims, so guessing one reaches the wrong session.

**FREE is not proof that work is free.** Look for a `make` whose cwd is the tree too, and treat
uncommitted work as someone's until they answer.

### `jobs`

`scripts/helix-claim jobs` is where every default `-j` comes from when no jobpool shim decides:
the Makefile's `JOBS`, the sweep's shard concurrency, the commit hook's build and the resource
advisor. With a jobpool daemon live it answers the pool's size; without one, the cores capped
by `MemAvailable`. `JOBPOOL=0` bypasses the pool here too. It is make's `-j` only: size docker,
ninja or GNU parallel work with `helix-claim hold` or `scripts/pool-docker.sh`
(`BUILD_SYSTEM.md` § "Parallel Compilation").

## What the git hooks do

`core.hooksPath` is `.githooks`, tracked, so this reaches every worktree and every tool
(opencode, plain `git`, a human) with no install step.

| Hook | Behaviour |
|------|-----------|
| `pre-commit` | If `MERGE_HEAD` exists, claims this tree under the **git process's** pid so the merge announces itself. Prints nothing unless another session already holds the tree. Then runs the quality checks. |
| `post-commit` | Points any committed path's stale index entry back at HEAD's blob (`.githooks/lib/sync-index-to-head.sh`), then releases that claim **only if the claim names this commit's own pid**, so a session claim spanning several commits survives. |

Ordinary human git use stays silent: the hooks speak only on a real conflict. They are advisory
by design; `HELIX_CLAIM_STRICT=1` makes `pre-commit` refuse instead of warn.

A merge here can hold the tree for 40 minutes while the hook builds. Its owner claims it,
announces a long one to peers, and releases when done.

## Committing in the shared tree

**Commit the pathspec directly; do not `git add`.** `git add` then `git commit` is not atomic:
your change sits in the shared index for however long your hook runs (20s for a script, minutes
for a staged header), and a peer committing in that window takes it into their commit.
`git commit -- <paths>` commits those paths' current content without going through the index,
so there is no window. It does take a peer's hunks in the same file too, so check
`git diff HEAD -- <path>` first. A new file needs `git add -N <path> && git commit -- <path>`,
which exposes only an empty intent-to-add entry.

**`MM` in `git status` does not mean a peer is mid-commit.** One command settles it:
`git diff HEAD -- <path>`. Empty means the committed content is what is on disk and only the
index holds an older copy. A `git commit -- <paths>` whose pre-commit hook reformats the file
leaves exactly that. The post-commit hook clears it; one left by a commit made without the hook
clears with `git add <path>`, which makes the index equal HEAD, so it exposes nothing to a peer
and discards nothing. A non-empty `git diff HEAD` is the case worth waiting on; confirm with
`pgrep -x git` plus each pid's cwd and `helix-claim check worktree:main` before concluding
anything about who owns it.

## Reading someone else's tree

**A live merge and an abandoned one look identical from outside.** `MERGE_HEAD` present, zero
`UU` entries, and an index mtime minutes old and not moving describe a `git commit` whose hook is
building: the index stops changing the moment the hook starts, and a staged header takes the
full-build path. An absent `ListAgents` row is not evidence either. The only discriminator is
process state:

```bash
pgrep -x git | while read p; do echo "$p $(readlink /proc/$p/cwd)"; done
ps -o pid,etime,args --ppid <pid>      # quality-checks.sh + make = still building
```

Completing someone's merge is non-destructive and aborting it is destructive, but both are
theirs to run.

**`build/bin/helix-tests` and `helix-screen` can be one inode across worktrees.** Whoever linked
last set the bytes both trees run. Compare `stat` inodes before trusting a control run against a
sibling tree.

## Running instances

**Never `pkill helix-screen`**, nor `pkill -x helix-screen`, nor `pkill -f`. The name is shared,
so it reaps every other session's instance. The victim sees only `[Application] SIGTERM — fast
exit` with no cause, so a long mock or `ctl` run dies looking like a crash. `$!` can name a
parent that forked, so resolve the PID from your own socket:

```bash
for p in $(pgrep -x helix-screen); do grep -qz "$HELIX_SOCK" /proc/$p/cmdline && echo $p; done
```

The default `ctl` socket is per-user, not per-instance. Pin it (`HELIXCTL.md` § "Running a fully
isolated second instance") or you drive a peer's app and it reports success.

## Builds that collide

A second `make` in the same tree deletes your freshly linked test binary:
`prune-orphan-test-objs` in `mk/tests.mk` runs `rm -f $(TEST_BIN)` as a sibling prerequisite of
the link, so `-j` gives them no order. The tell is `[LD] helix-tests`, then
`✓ Unit test binary ready`, no `✗ Test linking failed!`, then every shard reporting
`No such file or directory`. Nothing is wrong with your code; a link starved of memory fails
loudly and stops make. Serialize builds in one tree, or take a separate worktree.

**A build here goes minutes at a time printing nothing, and that is normal.** Judge liveness by
the log growing, and compare its mtime against `date` in the same command before calling it
stale; an `etime` and an mtime are not comparable by eye. A parent `make` in `do_wait` and a
sub-make in `poll_schedule_timeout` are a make waiting on children and a jobserver poll, not a
deadlock. Nothing short of a log that has not grown across two checks minutes apart justifies
killing someone's build.

A `helix-tests` left behind by a deleted worktree can hold a core at 100% for a day: high TIME,
high %CPU, and `readlink /proc/<pid>/cwd` ending in `(deleted)`. Kill that PID by number, never
by name.

### Checking for a build already running

```bash
pgrep -x -d' ' 'make|clang++|cc1plus'   # ONE pattern. Never pgrep -f: it matches its own command line
free -h                                  # the Mem row's `available` is the number that matters
jobpool status                           # where jobpool is installed
ps -eo pid,etime,time,pcpu,comm --sort=-time | head   # abandoned spinners
```

Native builds on thelio show `clang++` and not `cc1plus` (the `Makefile`'s `origin CXX` block
picks clang++ when it can link); cross builds show `cc1plus`. A pattern naming only one reads 0
against a box at full tilt. A 0 does not prove idle either: everything is wrapped in ccache, and
a cache hit finishes in milliseconds. `make` stays up for the whole build, so treat it as the
signal and the compilers as corroboration.

A nearly full Swap row is normal on a long-uptime box and is not a problem by itself: Linux never
reclaims swap it has already written, so `used` only ratchets up, and those pages are cold. If
any swap is free and `available` is healthy, the box is fine.
