---
name: helix-ops
description: Use when building, compiling, running tests or test tags, running land-gate, landing or merging to main, pushing, creating or tearing down a worktree, using the test host or another build host (zeus, thelio, remote-test, test-host-run), sizing parallel or docker jobs, or working beside peer Claude sessions in the shared HelixScreen tree (claims, the main working tree, who lands).
---

# HelixScreen operations: builds, hosts, peers, landing

The always-on rules stay in the root `CLAUDE.md` (check for a running build, two `make`s in one
tree, never `pkill helix-screen`, the verification cadence table). This skill holds the procedure
behind them. Full manuals: `docs/devel/BUILD_SYSTEM.md` (builds, worktrees, the test host),
`docs/devel/SHARED_TREE.md` (claims, hooks, reading a peer's tree), `tests/CLAUDE.md` (cadence,
land-gate stages, stamps), `scripts/CLAUDE.md` (one line per script).

## Before you compile

Check for a build already running, since concurrent compilations thrash the machine:
`pgrep -x -d' ' 'make|clang++|cc1plus'` (never `pgrep -f`), `free -h` (only the Mem row's
`available` matters; a full Swap row is normal), `jobpool status`. Who else is building, and in
which tree, is a question you ask them (`ListAgents` + `SendMessage`). How to read those numbers,
abandoned spinners, and why a silent build is not a dead one: `docs/devel/SHARED_TREE.md`
§ "Builds that collide".

## Build parallelism

- Plain `make` picks its own `-j`. Where jobpool is installed (thelio, zeus) `make` is its shim
  and every build on the box shares one pool. `docs/devel/BUILD_SYSTEM.md` § "Parallel Compilation".
- **`scripts/helix-claim jobs` is make's `-j` only.** With a jobpool live it prints the whole pool,
  which a make draws from and anything else would run on top of. Work outside make (docker, ninja,
  parallel loops) sizes itself with `scripts/helix-claim hold [--min M] -- cmd` (exports
  `JOBPOOL_SLOTS`), and a container goes through `scripts/pool-docker.sh`. Called outside a make
  with a pool live, `jobs` says so on stderr; its stdout is always the bare number, since make and
  `SHARD_CONCURRENCY` parse it.
- **On a Mac build with `gmake`**: bare `make` is Apple's 3.81, which parses the Makefile but
  silently no-ops every `$(file ...)` and links a stale manifest.
- **The commit hook runs only the fast gates** (seconds). The slow ones run at a push to main,
  `release/*` or a tag, and in `land-gate`. `HELIX_QC_ALL_AT_COMMIT=1` runs every gate at commit;
  `scripts/qc_timing.py` shows where a run's time went.
- `make dev-timing` prints measured medians for every build, suite and test run. Quote those,
  never a guess.

## Test and build hosts

```bash
make remote-test TAG='[ams]'         # Build + run one tag on $REMOTE_HOST (sends a patch, not the tree)
scripts/test-host-run.sh mutate --tests '[tag]'   # mutation on the test host (zeus)
scripts/test-host-run.sh asan '[tag]'             # ASAN: never on thelio, where it runs nothing and exits 0
scripts/test-host-run.sh sweep | test ... | exec [--build] -- cmd
#   Runs your tree as it is on disk, uncommitted edits included. bats stays local.
```

`docs/devel/BUILD_SYSTEM.md` § "Using your own build/test host". `./build/bin/helix-tests '[tag]'`
runs without a rebuild only if you have not edited since `make test`; a stale binary prints
`WARNING: helix-tests is stale` on stderr first: believe it. `make full-test-run` (unit sweep +
bats, about 2m) is the only local run of bats, and it is NOT a landing gate: it skips tests/ui
and CI's shard layout.

## Worktrees

`scripts/setup-worktree.sh feature/my-branch` for MAJOR work: worktrees live in `.worktrees/`,
never `EnterWorktree`. `scripts/teardown-worktree.sh my-branch` removes one, since
`git worktree remove` refuses here; `-n` prints the plan. Setup refuses an existing
`.worktrees/<last-segment>`: check that it is not a peer's tree. `docs/devel/BUILD_SYSTEM.md`
§ "Git Worktrees" covers what each script does and refuses. These replace the create step in
`superpowers:using-git-worktrees` and the `git worktree remove` step in
`superpowers:finishing-a-development-branch`.

`build/bin/helix-tests` and `helix-screen` can be one inode across worktrees. Compare `stat`
inodes before trusting a control run against a sibling tree.

## land-gate

`make land-gate` is THE pre-landing gate: once per batch, before anything reaches main, on the
fastest free machine (thelio, then zeus; never a Mac). Quote the ETA it prints, never a guess.
Watch its log for `: FAIL in|: TIMEOUT in` and report a failed stage the moment it lands. A red
step: `make land-gate STAGES=<step>` reruns just it on the same clean HEAD. Stages and stamps:
`tests/CLAUDE.md`, `scripts/CLAUDE.md`.

## Sharing the tree with other sessions

The manual behind these rules (helix-claim, the hooks, reading a tree mid-merge):
`docs/devel/SHARED_TREE.md`.

**Message the other session first.** Overlapping work gets a message BEFORE you read their diff,
pick a winner, plan a rebase, or file or comment publicly. The ask first, then branch names and
SHAs. A claim says a resource is taken, never what the holder is doing: ask. A successful send is
not a delivery and silence is not agreement: never merge, rebase or delete on an unanswered
message.

**Settle it between sessions; bring Preston outcomes, not questions.** Who builds, who holds a
tree, merge order, conflicts, release timing are yours to resolve. Ask him only for a product call
nobody has made, or for judging pixels. When a peer's work affects yours, tell them first.

- **Claim before you take a shared resource: `scripts/helix-claim`.** `check` before concluding
  anything about someone else's work; hold `worktree:<name>` from your FIRST edit until the commit
  lands. Also `build:`, `device:<printer>` (IP in `--note`, released by an EXIT trap),
  `heavy:`, `gh:issue:<n>`, `socket:`. Gate `take` on its exit code; never pipe it.
- **The main working tree is live.** Never let git autostash (`-c merge.autoStash=false`). Commit
  promptly with `git commit -- <paths>`, never `git add` then commit: the shared index leaks your
  change into a peer's commit. A new file: `git add -N <path> && git commit -- <path>`.
- **`MM` does not mean a peer is mid-commit**, and a live merge looks exactly like an abandoned
  one. `git diff HEAD -- <path>` and process state settle it (SHARED_TREE.md). Completing or
  aborting someone's merge is theirs to run.
- **Whoever moves main pushes it, in the same claim.** Gate, `take worktree:main`, merge or
  fast-forward, push, verify `origin/main == main`, release. Pushing main pushes peers' commits
  too: read `git log origin/main..main` first. Never leave main ahead of origin.
- **Land in batches and gate once.** Before taking main, message whoever holds it or is queued and
  fold your ready branch into one landing with ONE `make land-gate`.
- **One session per physical printer.** Ask who holds a device before pointing anything at it;
  never kill a deploy mid-phase.

## Several agents or sessions on one batch

One test plan, owned by one session. The orchestrator writes it into every brief (with the
verification cadence table from root `CLAUDE.md`, because agents default to over-verifying);
agents on one feature or batch syntax-check, commit and hand back, never tag runs, builds,
mutation, sweeps, gates or their own reviews. With peer sessions, whoever lands next owns the
batch; the others hand over branch + SHA and stop testing.
