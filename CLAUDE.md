# CLAUDE.md

## Quick Start

**HelixScreen**: LVGL 9.5 touchscreen UI for Klipper 3D printers. XML engine in `lib/helix-xml/` — our own MIT fork of the engine LVGL removed in 9.5, and its own repo ([prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml)), so a fresh clone needs `git submodule update --init --recursive`. Pattern: XML → Subjects → C++.

**macOS is a development platform only — never a deployment target.** The app ships to embedded Linux (MIPS, ARM, aarch64) and Raspberry Pi. A Mac exists to build, run `--test`, and iterate on UI; nothing is released for it and no user runs it there. Host-portability work is justified by *a developer being able to build and run the suite*, never by production correctness — so fix a portability break when it blocks your own iteration, and don't add CI jobs or abstractions to defend macOS as a runtime.

**Planned work lives in GitHub issues** (`gh issue list --milestone Backlog`; milestone = scheduling axis, labels = kind: docs-debt, hw-verify, tech-debt). In-flight plans and specs live in `docs/devel/plans/`: point-in-time scaffolding, deleted in the change that ships the work (convention: `docs/CLAUDE.md`).

**Before compiling, check for a build already running** — concurrent compilations thrash the machine:

```bash
pgrep -x -d' ' 'make|clang++|cc1plus'   # ONE pattern. Never pgrep -f: it matches its own command line
#   The compiler is auto-detected in `Makefile`'s `origin CXX` block: clang++
#   when it can link, else g++; mk/cross.mk uses $(CROSS_COMPILE)g++. Native
#   builds on this box show `clang++` and NOT `cc1plus`; cross builds show
#   `cc1plus`. A pattern naming only one of them reads 0 against a box at full
#   tilt. A 0 does not prove idle either: everything is wrapped in ccache, and
#   a cache hit finishes in milliseconds, so sampling between translation units
#   legitimately catches nothing mid-build. `make` is the process that stays up
#   for the whole build; treat it as the signal, compilers as corroboration.
free -h                          # read the Mem row: `available` is the only number that matters
#   A nearly-full Swap row is NORMAL here and is NOT a problem by itself. Linux
#   never reclaims swap it has already written, so `used` only ratchets up over
#   an uptime measured in weeks; those pages are cold and cost nothing. If ANY
#   swap is free and `available` is healthy, the box is fine: do not throttle,
#   do not wait for it to drain, do not narrate it.
ps -eo pid,etime,time,pcpu,comm --sort=-time | head   # abandoned spinners
#   A helix-tests left behind by a deleted worktree can hold a core at 100% for
#   a day: high TIME, high %CPU, and `readlink /proc/<pid>/cwd` ends in
#   "(deleted)". Kill that PID by number - never by name, it is shared.
```

- **Plain `make` (or `make -j`) picks the `-j` for you; do not hand-pick a `-jN`.** It takes this session's fair share from `scripts/helix-claim jobs`: the cores split across the trees building right now, capped by `available`. An idle box gets all 32; four trees building get about 6 each. An explicit `-jN` still passes through untouched, for the rare case you own the box.
- The unit sweep caps how many shards run at once from the same share (`SHARD_CONCURRENCY` overrides). Every make renices itself to 10, so its compilers and test shards yield to the desktop: thelio's system76-scheduler drops `make` to nice 19 `SCHED_IDLE` only when it happens to notice it, and never lists `helix-tests`. `HELIX_NICE=0` opts out.
- Dying at the same step twice **can** be a resource ceiling, but rule out a peer first: a second `make` in the SAME tree deletes your freshly linked binary (`prune-orphan-test-objs` in `mk/tests.mk` runs `rm -f $(TEST_BIN)` as a sibling prerequisite of the link, so `-j` gives them no order). The tell: `[LD] helix-tests`, then `✓ Unit test binary ready`, NO `✗ Test linking failed!`, then every shard reports `No such file or directory`. Nothing is wrong with your code; a starved link fails loudly and stops make.
- **A build here goes minutes at a time printing nothing, and that is normal.** Judge liveness by the log growing, and compare its mtime against `date` in the SAME command before calling it stale - an `etime` and an mtime are not comparable by eye. A parent `make` in `do_wait` and a sub-make in `poll_schedule_timeout` are a make waiting on children and a jobserver poll, not a deadlock. Nothing short of a log that has not grown across two checks minutes apart justifies killing someone's build.
- Who else is building, and in which tree, is a question you ask them: `ListAgents` + `SendMessage`, not a `pgrep` guess.
- **The commit hook builds too.** `scripts/quality-checks.sh` verifies an incremental build of the app at the `helix-claim jobs` share, so N sessions committing never become N unbounded builds; `HELIX_QC_JOBS` overrides it. `scripts/qc_timing.py [--staged-only]` runs the gate and prints where its time went, which is how you find out whether you are waiting on that build or on a check.

```bash
make                                 # Build ONLY the program binary (NOT tests), at a fair -j
./build/bin/helix-screen --test -vv  # Mock printer + DEBUG logs
# ALWAYS use verbosity: -v=INFO, -vv=DEBUG, -vvv=TRACE (default=WARN)

# Verifying anything that needs an ACTIVE PRINT — use --sim-speed, don't wait.
# A default mock run sits in Preparing ~95s before reaching Printing.
HELIX_MOCK_AUTO_PRINT=1 ./build/bin/helix-screen --test --sim-speed 6 -vv
#   --sim-speed <1.0-1000.0> fast-forwards the simulated clock (--test only).
#   4-10x = reach Printing in ~15s, still slow enough to observe async UI work.
#   50x+ = the print STARTS AND FINISHES in ~8s, outrunning async loads (e.g. the
#   print-status gcode preview) — only use high factors to reach print completion.
#   Confirm via log: "[MoonrakerManager] Creating MOCK client (<printer>, <n>x speed)"

make test                            # Build tests only (does NOT run them)
make t F='[tag]'                     # Build, then run ONE tag or case (the inner loop)
./build/bin/helix-tests '[tag]'      # Same run WITHOUT make's dependency scan (0.05-0.6s).
#   Correct only when you have not edited code since the last `make test`:
#   `make -j` builds the app alone, so after an edit the bare binary reports the
#   PREVIOUS build's numbers. `make t` costs 5-18s and buys exactly that guarantee.
make unit-sweep                      # C++ unit tests only, sharded (~50s idle)
make full-test-run                   # unit-sweep + the bats suite (~2m) - the completion gate
#   Nothing else runs bats locally: not the commit hook, not test-xml. Without this
#   the shell suite reaches CI unrun. [.] and [slow] stay outside it deliberately -
#   quality-checks.sh runs [.] on any staged code change, nightly CI runs [slow].
#   `make test-run` runs nothing: it prints which of these fits the question
#   you have and exits non-zero. Cadence table: tests/CLAUDE.md.

make dev-timing                      # What the dev loop costs, measured (ledger from transcripts)
#   Medians for every build, suite and test run, so "is this worth running" is
#   answered from data. Derived + gitignored; rebuilding it takes a few seconds.

scripts/syntax_check.py <file>...    # "does this compile?" in seconds
#   Takes the file's own flags from compile_commands.json and runs -fsyntax-only,
#   instead of the minutes `make test` spends re-linking. Proves nothing about
#   behaviour: run the tag for that. A brand-new file borrows a sibling's flags.
./build/bin/helix-tests "[tag]"      # Run specific test tags
make pi-test                         # Build on thelio + deploy + run

# Building on a remote host: send the DELTA, not the tree.
make remote-test TAG='[ams]'         # build tests on $REMOTE_HOST, run one tag
make remote-native                   # build the app there
#   The host clones and fetches from GitHub itself; your link carries one patch
#   (unpushed commits AND uncommitted edits) plus untracked files — a few KB.
#   `make remote-sync` rsyncs the whole tree and is for the Docker cross targets
#   only; a fresh REMOTE_DIR costs ~260MB, so never make one per branch.

scripts/zeus-run.sh mutate --tests '[tag]'   # mutation gate on zeus
scripts/zeus-run.sh asan '[tag]'            # AddressSanitizer on zeus
scripts/zeus-run.sh sweep                   # make unit-sweep on zeus
#   bats stays on thelio (`make test-shell`): the container runs as root with no
#   shellcheck, so about 190 shell tests fail there on the environment alone.
#   All three are expensive and non-interactive, so they belong on the idle
#   72-core box. `zeus-run.sh test` with no tag runs the suite in ONE process,
#   where cross-test contamination fails cases no branch touched: not a gate.
#   ASAN especially: thelio's /etc/ld.so.preload makes ASAN's runtime load
#   second, so the binary produces NO test output and exits 0 - a pass that ran
#   nothing. The container has no ld.so.preload and its image matches CI's.
#   The commit has to be pushed; the container fetches it, it does not take your
#   tree. zeus is memory-bound, not core-bound (ZFS ARC holds most of its 251GB),
#   so the script caps the ARC for the run and sizes -j from what is then free.

# Worktrees — MUST use for MAJOR work. Always in .worktrees/ (project root).
scripts/setup-worktree.sh feature/my-branch  # Symlinks shared deps, builds fast
#   lib/lvgl, lib/libhv, lib/lua and lib/helix-xml get a PRIVATE checkout per worktree
#   (patches/ is per-branch); everything else in lib/ is a symlink shared with
#   the main tree. Also writes .claude/settings.local.json with PROJECT_DIR set
#   to the MAIN tree so claude-recall writes lessons and stats there.
#   The directory is named after the branch's LAST segment, so it refuses an
#   existing .worktrees/<name>: a re-setup needs --setup-only, the same branch,
#   and no other session's live worktree: claim on it.
#   A worktree the harness makes on its own (EnterWorktree → .claude/worktrees/)
#   gets NONE of this: no lib/ symlinks, no submodules, no build. Prefer this
#   script; if you are already in one, `git submodule update --init --recursive`
#   and a full build before trusting anything it produces.

scripts/teardown-worktree.sh my-branch       # Remove a finished worktree + branch
scripts/teardown-worktree.sh my-branch -n    # ...or just print the plan
#   `git worktree remove` REFUSES here ("working trees containing submodules
#   cannot be moved or removed") because of those private checkouts, so removal
#   is a guarded rm -rf plus a prune. rm -rf removes a lib/ symlink, never its
#   target, so the main tree's shared copies survive.
#   Refuses to discard anything unique: uncommitted changes (--force overrides),
#   unpushed commits in lib/helix-xml (ours, edited directly), a branch not
#   contained in --into (default main), or a live build/test/git process with
#   its cwd inside the tree. Also releases the tree's helix-claim, which would
#   otherwise read LIVE forever against a directory that no longer exists.
#   If `git branch -d` refuses on a branch this script has CONFIRMED is merged,
#   the cause is almost always that -d also checks the branch's UPSTREAM: the
#   work is on local main but main has not been pushed. Push, or --force-branch.
```

**XML changes need no rebuild:** `ui_xml/*.xml` is loaded at runtime. Hot reload is **on by default for native dev builds** (cross-compiled release builds default it off): the running app re-registers components within ~500ms of a save and rebuilds the active panel/overlay/modal in place. `HELIX_HOT_RELOAD=1`/`0` overrides the default either way. Invalid XML (mid-write truncation, syntax errors) is silently skipped on the polling thread; the existing UI stays live and the next poll retries.

**Screenshots:** Press 'S' in UI, or `./scripts/screenshot.sh helix-screen output-name [token]` (drives a fresh instance via `helix-screen ctl`; token = panel/overlay/`demo` screen from `scripts/screenshot-recipes.sh`).

**Driving the UI (screenshots, debugging, bringing up any panel/overlay/modal):** `helix-screen ctl` remote-controls a running instance — `navigate`/`click`/`ls`/`text`/`geom`/`set_value`/`scroll`/`demo`/`screenshot`, or a `helix-screen repl` REPL. The server auto-starts in `--test` (or `--remote`). See `docs/devel/HELIXCTL.md`.

> **Always pin the socket — never run a bare `ctl`.** The default path is per-user and
> fixed, so with two instances up, `ctl` silently drives **whichever started first** and
> still reports success. Derive both the socket and the config dir from the worktree so
> parallel agents can't collide — you need *both*, since `--remote-socket` alone still
> contends on the config flock:
> ```bash
> TREE=$(basename "$(git rev-parse --show-toplevel)")
> export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
> mkdir -p "$HELIX_CONFIG_DIR"   # must exist — the app does not create it, it aborts
> ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
> ./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate settings
> ```
> **An empty `HELIX_CONFIG_DIR` is isolated for the lock and socket, not for the printer
> address.** Finding no `settings.json` there, `Config` bootstraps one from
> `~/.helixscreen/settings.json.backup` (`[Config] Config missing — restoring from backup:`
> in the log). That inherits the real `moonraker_host`, so a run **without** `--test`
> opens a WebSocket to the actual printer. Keep `--test` (the mock client ignores the host
> entirely), or name the target explicitly with `--moonraker ws://HOST:7125`, which takes
> precedence over the saved host. The flag is `--moonraker`; there is no `--moonraker-url`.
>
> **Writing your own `settings.json` there does NOT preset anything unless it carries
> `config_version`.** A config whose `config_version` is absent or 0 is read as the
> packaged tarball default and replaced *wholesale* from the rolling backup (`[Config]
> Loaded config is a tarball default (no config_version) — restoring from backup:`), which
> is how a Moonraker web update recovers real settings after `rmtree()`. So a hand-written
> file presetting `beta_features`, `display/screensaver_type` or anything else is discarded
> before the app reads it, and every value you thought you set is the backup's. Copy a real
> `settings.json` and edit it, or read the value back from the log rather than trusting the
> file you wrote.
>
> Prefer `ctl text <name>` / `ctl geom <name>` over reading a screenshot — they are exact,
> and a screenshot only proves what a scroll position happened to expose.

---

## Sharing This Tree With Other Sessions

**Message the other session first.** Overlapping work - a claim on a tree you need, a
branch touching your files - gets a message BEFORE you read their diff, pick a winner or
plan a rebase, including before you file or comment publicly on it.

- Keep it short: the ask first, then the branch names and SHAs they need to answer.
- A claim says a resource is taken, never what the holder is doing or how far along. Ask.
- Address a peer by the `message=uds:...sock` in their `helix-claim list` row. Every live
  claim carries one; `ListAgents` names do not map to claims, so guessing one reaches the
  wrong session.
- A successful send is not a delivery and silence is not agreement. Never merge, rebase or
  delete on an unanswered message.

**Settle it between sessions; bring Preston outcomes, not questions.** He runs several
sessions at once, and every question routed back to him stalls all of them.

- Coordination is yours: who builds, who holds a tree, merge order, conflicts, release
  timing. Resolve it session-to-session, then tell him what happened.
- Ask him only for what no session can settle: a product call nobody has made yet, or
  judging what pixels look like.
- Be proactive. When a peer's work affects yours, message them before they have to ask.
  When you learn something another session needs, send it.

What is shared here:

- **The main working tree is live.** Other sessions commit in it. Never let git autostash
  (`-c merge.autoStash=false`), and commit your own edits promptly, with explicit pathspecs.
  Pushing main pushes peers' commits too: read `git log origin/main..main` and push only when their gates are green.
- **Whoever moves main pushes it, in the same claim.** Gate the branch first, then
  `take worktree:main`, merge or fast-forward, push, verify `origin/main == main`, release.
  Never leave main ahead of origin: no other session will push work that is not theirs, so
  an unpushed main stalls every merge behind it until its owner is found.
- **Land in batches and gate once.** Before taking main, message whoever holds it or is
  queued and fold your ready branch into one landing: one session merges every ready branch
  and runs ONE `make full-test-run` on the combined result. A full gate per branch turns N
  landings into N serial 10-minute gates, and parallel gates leave thelio's CPU spinning on
  the same suite. Run the full gate once per branch, at the end: a review fix gets its tags
  (`make t F=...`), and the batch gate covers the rest.
- **`MM` does not mean a peer is mid-commit.** It is ambiguous, and one command settles it:
  `git diff HEAD -- <path>`. Empty means the committed content is what is on disk, only the
  INDEX holds an older copy, and nothing is in flight. A `git commit -- <paths>` whose
  pre-commit hook reformats the file leaves exactly that: the pre-format copy stranded in the
  index. Clear it with `git add <path>`: index becomes HEAD, so there is nothing unpublished
  for a peer to sweep, and it discards nothing. A non-empty `git diff HEAD` is the case worth
  waiting on; confirm with `pgrep -x git` plus each pid's cwd and `helix-claim check
  worktree:main` before concluding anything about who owns it.
- **Do not `git add` in this tree: commit the pathspec directly.** `git add` then `git commit` is not atomic: your change sits in the *shared* index for however long your hook runs (20s for a script, minutes for a staged header), and a peer committing in that window takes it into their commit. `git commit -- <paths>` commits those paths' current content without going through the index, so there is no window, though it takes a peer's hunks in the same file too (check `git diff HEAD -- <path>`), and a new file needs `git add -N <path> && git commit -- <path>`, which exposes only an empty intent-to-add entry. The one exception is the stale-index case above, where the content is already in HEAD and staging it exposes nothing.
- **A live merge and an abandoned one look identical from outside.** `MERGE_HEAD` present, zero `UU` entries, and an index mtime minutes old and not moving describe a `git commit` whose hook is *building* — the index stops the moment the hook starts, and a staged header takes the full-build path. An absent `ListAgents` row is not evidence either. The only discriminator is process state:
  ```bash
  pgrep -x git | while read p; do echo "$p $(readlink /proc/$p/cwd)"; done
  ps -o pid,etime,args --ppid <pid>      # quality-checks.sh + make = still building
  ```
  Run that before concluding anything about a foreign index. Completing someone's merge is non-destructive and aborting is destructive, but both are theirs to run.
- **`build/bin/helix-tests` and `helix-screen` can be one inode across worktrees**: whoever linked last set the bytes both trees run. Compare `stat` inodes before trusting a control run against a sibling tree.
- **The default `ctl` socket is per-user, not per-instance.** Pin it (box above) or you drive a peer's app and it reports success.
- **One session per physical printer at a time.** Ask who holds a device before pointing anything at it. Claim `device:` around a deploy with an EXIT trap (box below) and never kill a deploy mid-phase; the name is a role, so put the IP in `--note`.
- **Claim before you take a shared resource: `scripts/helix-claim`.** Plain shell, no Claude
  dependency — opencode, a human or a script can use it, and `AGENTS.md` is a symlink to this
  file so every agent reads the same rule.

  ```bash
  scripts/helix-claim check worktree:main        # FREE | LIVE | STALE  (exit 1 if LIVE)
  if scripts/helix-claim take device:k2plus deploy --pid $$ --note 192.168.1.50; then  # gate on the exit code; never pipe take
      trap 'scripts/helix-claim release-if-owned-by $$ device:k2plus' EXIT; make deploy-k2plus; fi
  scripts/helix-claim list                       # everything, with derived liveness
  scripts/helix-claim resources                  # memory, load, claims, top RSS, zeus: before heavy work
  scripts/helix-claim run heavy:sweep -- make unit-sweep   # claimed while it runs
  make -j"$(scripts/helix-claim jobs)"           # a fair -j, not a guess
  ```

  Resource names for worktrees are **derived, not trusted**: `worktree:main`,
  `worktree:helixscreen` and a bare `worktree:` all resolve to the same tree, matched by
  directory basename or checked-out branch. Free-form names let two sessions claim one tree
  under two spellings and both read FREE, and an advisory lock must never fail open.
  **`build:<tree>` and `worktree:<tree>` name the same directory and exclude each other**,
  in both directions and for the same reason: writing under a running build corrupts the
  build, and building while files move gives a binary matching no commit. A take consults
  its sibling, so `check worktree:main` reports a live `build:` holder instead of FREE.
  One session may hold both on its own tree; only a different owner blocks.

  Liveness is **derived from process state, never asserted**: a claim records its owner's pid
  and that pid's kernel start-time, so a crashed owner reads STALE on its own, pid reuse
  cannot fake LIVE, and nothing needs cleaning up. Use it for `worktree:<name>` (hold it
  from your FIRST edit until the commit lands - not merely for a merge, rebase or long
  commit: uncommitted files with no claim and an old mtime are indistinguishable from
  abandoned work, and `build:<name>` reserves nothing),
  `build:<name>`, `device:<printer>`, `heavy:<what>`, `gh:issue:<n>` (check it first), `socket:<path>`.

  **Before concluding anything about someone else's work, run `check`.** A merge mid-commit
  and an abandoned one look identical in the tree — same `MERGE_HEAD`, same resolved index,
  same frozen mtime. A `git commit` here can hold the shared tree for 40 minutes while its
  hook builds. FREE is not proof: look for a `make` whose cwd is the tree too.
  A missing claim does not make work free: uncommitted work is someone's until they answer.

  **What the git hooks now do** (`core.hooksPath` is `.githooks`, tracked, so this reaches
  every worktree and every tool — opencode, plain `git`, a human — with no install step):

  | Hook | Behaviour |
  |------|-----------|
  | `pre-commit` | If `MERGE_HEAD` exists, claims this tree under the **git process's** pid so the merge announces itself. Prints nothing unless another session already holds the tree. Then runs the existing quality checks unchanged. |
  | `post-commit` | Releases that claim **only if the claim names this commit's own pid**, so a session claim spanning several commits survives. |

  Ordinary human git use stays silent: the hooks speak only on a real conflict. Advisory by
  design; `HELIX_CLAIM_STRICT=1` makes `pre-commit` refuse instead of warn. `--no-verify`
  bypasses both, as before.

  A merge here can hold the tree for **40 minutes** while the hook builds. Its owner claims it,
  announces a long one to peers, and releases when done.

- **`scripts/helix-claim jobs` is where every default `-j` comes from.** It counts distinct
  trees with live compilers (a raw `cc1plus` count is just one build's `-j`), folds in live
  `build:` claims so an unclaimed builder still counts, skips the makes it was called from so
  a build never counts itself, and caps by `MemAvailable`.
- **Never `pkill helix-screen`**, nor `pkill -x helix-screen`, nor `pkill -f`. The name is shared, so it reaps every other session's instance, not yours. The victim sees only `[Application] SIGTERM — fast exit` with no cause, so a long mock or `ctl` run dies looking like a crash. `$!` can name a parent that forked, so resolve the PID from your own socket: `for p in $(pgrep -x helix-screen); do grep -qz "$HELIX_SOCK" /proc/$p/cmdline && echo $p; done`.

---

## Docs (load when needed)

Full index: **`docs/devel/CLAUDE.md`** (auto-loaded when working in docs/devel/)

Most commonly needed:

| Doc | When |
|-----|------|
| `docs/devel/ARCHITECTURE.md` | Whole-app 15-minute model + routing table into the architecture guide's chapters |
| `docs/devel/UI_CONTRIBUTOR_GUIDE.md` | UI/layout work: breakpoints, tokens, colors, widgets, layout overrides |
| `docs/devel/LVGL9_XML_GUIDE.md` | XML layouts, widgets, bindings, observer cleanup |
| `docs/devel/MODAL_SYSTEM.md` | Modal architecture: ui_dialog, modal_button_row, Modal pattern |
| `docs/devel/FILAMENT_MANAGEMENT.md` | AMS, AFC, Happy Hare, ACE, AD5X IFS, CFS, Tool Changer |
| `docs/devel/CHAMBER_HEATER.md` | Chamber heaters: backends, discovery, diagnostics, ceiling rules |
| `docs/devel/REVIEW_RUBRIC.md` | Reviewing a change: crash families, silent-failure traps, what the gates already cover |
| `docs/devel/ENVIRONMENT_VARIABLES.md` | Runtime env vars |
| `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` | Mock printer config for `--test` runs (`HELIX_MOCK_*`, replay) |
| `docs/devel/LOGGING.md` | spdlog levels: info vs debug vs trace |
| `docs/devel/BUILD_SYSTEM.md` | Makefile, cross-compilation |

## Path-Scoped Rules (`.claude/rules/`)

These load automatically when you work on matching files. They are the contract, not background reading; every one is lint-gated.

| Rule | Loads for | Covers |
|------|-----------|--------|
| `.claude/rules/declarative-ui.md` | `src/ui/`, `ui_xml/`, `include/ui_*.h` | DATA in C++, APPEARANCE in XML: the eight declarative rules, the structural exceptions, design tokens |
| `.claude/rules/vendor-abstraction.md` | `src/`, `include/` | A vendor name appears in ONE module per capability; generic code asks capability questions |
| `.claude/rules/threading.md` | `src/`, `include/`, `tests/unit/` | The five lifecycle invariants; `docs/devel/THREADING.md` is the full text |
| `.claude/rules/submodules.md` | `lib/`, `patches/`, `mk/patches.mk` | `lib/helix-xml/` is ours and edited directly; everything else goes through `patches/` |
| `.claude/rules/filament-backends.md` | `src/printer/ams_*`, `include/ams_*`, `*filament_*` | Which doc to read before touching a filament backend |

---

## Above a Bugfix: Investigate, Then Scope

Features, refactors, new panels/widgets/managers — **scope AFTER investigating, not before.**

- Map what exists (call sites, subjects, tests) and read that subsystem's `docs/devel/` doc
- Find the canonical implementation of each piece you're about to write
- Extend the near-fit helper — never fork a twin. Copy-paste-modify = red flag
- Say what you searched and what you're reusing; scope without that is a guess

---

## Code Standards

| Rule | ❌ WRONG | ✅ CORRECT |
|------|----------|-----------|
| **spdlog only** | `printf()`, `cout`, `LV_LOG_*` | `spdlog::info("temp: {}", t)` |
| **SPDX headers** | 20-line GPL boilerplate | `// SPDX-License-Identifier: GPL-3.0-or-later` |
| **RAII widgets** | `lv_malloc()` / `lv_free()` | `lvgl_make_unique<T>()` + `release()` |
| **Class-based** | `ui_panel_*_init()` functions | Classes: `MotionPanel`, `WiFiManager` |
| **Observer factory** | Static callback + `lv_observer_get_user_data()` | `observe<int>()` from `observer_factory.h` |
| **Icon sync** | Add icon, forget fonts | TWO hand-kept lists: `include/ui_icon_codepoints.h` (name -> codepoint) AND `scripts/regen_mdi_fonts.sh` `MDI_ICONS` (what the font contains) + `make regen-fonts` (regenerates the ESP32 firmware's twins too) + rebuild. `validate_icon_fonts.sh` fails the build if either font set is missing an icon |
| **Formatting** | Manual formatting | Let pre-commit hook (clang-format) fix |
| **Doc citations** | A line number (`src/printer/printer_state.cpp:638`), or a bare `:NNN` with no path | A place: `` `src/printer/printer_state.cpp#update_from_status` `` - path, then a `#` fragment naming the enclosing scopes. `scripts/doc_anchors.py` resolves it to a line on demand (`make check-doc-anchors`, advisory), so code that moves rots nothing. A RENAMED symbol is the one case you fix by hand, because the sentence around it may no longer be true |
| **No auto-mock** | `if(!start()) return Mock()` | Check `RuntimeConfig::should_mock_*()` |
| **JSON include** | `#include <nlohmann/json.hpp>` | `#include "hv/json.hpp"` (libhv's bundled version) |
| **Build system** | `cmake`, `ninja` | `make -j` (pure Makefile) |
| **No RTTI** | `dynamic_cast`, `typeid`, `std::type_index`, `any.type()` | `helix::type_tag<T>()` keys, virtual kind queries (`HELIX_CONTEXT_MENU_KIND`), pointer-form `any_cast`. Firmware builds `-fno-rtti`; lint-gated, escape hatch `// RTTI_OK: <reason>` |
| **No exceptions (firmware)** | `try`/`catch`/`throw`, `j.value("k", d)`, `j.at("k")`, one-arg `json::parse`, `std::stoi`, value-form `any_cast` in firmware-compiled code | `json_util::safe_*`/`as_*`, `json::parse(s, nullptr, false)`, `text_io::parse_leading`, `any_cast<T>(&a)`; `exception_policy.h` for shared code. The ESP32 image builds `-fno-exceptions`, so a throw is an abort; `scripts/check_esp32_app_srcs.py` gates the calls |
| **No std::regex** | `std::regex`, `#include <regex>` in app code | `helix::Regex` (`include/helix_regex.h`, heap backtracking). libstdc++'s matcher recurses per input character and overflows musl's small thread stacks with no crash file. Lint-gated in `tests/shell/test_code_lint.bats`; escape hatch `// STD_REGEX_OK: <reason>` |
| **Bug commits** | Filing an issue just so the commit can cite one | Cite the issue when one already exists: `fix(scope): thing (prestonbrown/helixscreen#123)`. No issue? `fix(scope): thing` is complete on its own — the commit body carries the explanation. |
| **Unproven tests** | "Tests pass" as evidence the change is tested | `make mutate-diff` (reverts each hunk, looks for red) and one line in the commit body naming the mutation. A green suite is not evidence. `tests/CLAUDE.md` § "Proving a test can fail" |
| **Commit body length** | 3-paragraph Tests / Verification / Mutation essay | Subject + ~4-line paragraph. Reserve the long form for genuine state-machine fixes that touch multiple subsystems |
| **Comment archaeology** | `// unlike the three widgets 3d0875bff fixed`, `// this used to memcpy the whole canvas`, `// pre-fix the stream wrote into freed memory` | State the constraint, not the history: `// Invalidating here freezes a fullscreen view the user is watching`. § below |
| **Submodule mods** | Edit `lib/lvgl/...` / `lib/libhv/...` directly | `patches/*.patch`; `lib/helix-xml/` is ours and edited directly. `.claude/rules/submodules.md` |

**ALWAYS:** Search the SAME FILE you're editing for similar patterns before implementing.

### Comments describe the code, not its past

A comment earns its place by helping someone understand the code **as it is now**. How it
got here — what it used to do, which commit changed it, what bug or review or mutation run
prompted it — belongs in the commit message, where `git blame` surfaces it on demand.
**The deletion test:** cut the historical clause. If the comment still explains the code to
a first-time reader, leave it cut. If the sentence collapses, rewrite it as a present-tense
fact about the system.

| Keep — a constraint that still binds | Cut — how we got here |
|---|---|
| `// Invalidating here freezes a fullscreen view the user is watching` | `// unlike the three widgets 3d0875bff fixed` |
| `// The piezo demodulates a duty-modulated carrier as static, so PWM is tone-only` | `// Originally disabled 2026-04 for exactly that starvation` |
| `// A wrapper existing does not prove it persists anything` | `// #1401 grew a probe offset 0.060 -> 2.515mm over five save cycles` |
| `// Rows arriving with no scan pending would accumulate unbounded` | `// this file used to have several data races` |

Almost always archaeology: a commit SHA; `used to`, `previously`, `originally`, `before
this`, `no longer`, `pre-fix`; a narrated issue as opposed to a bare cite; "the bug
where…"; and in tests, a recap of what a review or mutation run discovered. Tests, shell
scripts, gates and Makefiles included.

Issue references are welcome as pointers, not summaries:

- ✅ `// A wrapper existing does not prove it persists anything (prestonbrown/helixscreen#1401)`
- ❌ `// #1401: a Helper-Script box folded the offset into the probe, SAVE_CONFIG restarted klipper, and the boot gcode re-applied it, growing 0.060 -> 2.515mm over five cycles`

`scripts/check_comment_archaeology.py` ratchets the SHA half; the phrasing half is the reviewer's.

---

## Patterns

| Pattern | Key Point | Exemplar |
|---------|-----------|----------|
| Subject init order | Register components → init subjects → create XML | `src/application/subject_initializer.cpp`, called from `Application::register_xml_components()` |
| Widget lookup | `lv_obj_find_by_name()` not `lv_obj_get_child()` — indices break when layout changes | any panel |
| Overlays | `NavigationManager::instance().push_overlay(root)` / `.go_back()` (`ui_nav_manager.h`) — pair every push with `register_overlay_instance(root, this)` or `on_deactivate()` never fires (tests abort; `HELIX_STRICT_OVERLAY_CHECK=1`) | `src/ui/ui_settings_safety.cpp` |
| Modals (simple) | `Modal::show("component_name")` / `Modal::hide(dialog)` | `src/ui/ui_job_queue_modal.cpp` |
| Modals (subclass) | Extend `Modal`, implement `get_name()` + `component_name()`, override `on_ok()`/`on_cancel()` | `include/ui_info_qr_modal.h` + `src/ui/ui_info_qr_modal.cpp` (42 + 62 lines — the whole pattern, nothing else) |
| Confirmation dialog | `modal_confirm(title, msg, severity, btn_text, on_confirm, ConfirmOptions)` (in `helix::ui`) for new code - `std::function` throughout, closes its own dialog; the options struct carries `on_cancel`/`cancel_text`/`on_dismiss`/`owner_token`, and `owner_token` gates **all three** callbacks. All are owned, so a dismissal (backdrop tap, ESC, hot-reload rebuild) reaches `on_dismiss` - **pass it whenever the caller holds a guard/flag/pending entry the buttons were meant to clear**, or that state leaks | `src/ui/ui_change_host_modal.cpp#show_connection_failed_modal`; subclass form: `include/lan_client_auth_router.h` |
| Modal buttons (XML) | `<modal_button_row primary_text="Save" primary_callback="on_save"/>` | `ui_xml/bed_mesh_rename_modal.xml` |
| Home-panel widget | Subclass `PanelWidget`; `attach()` + `on_size_changed()`. Instances are **recycled** across rebuilds, so any imperative apply must run from `attach()` too, not only on size change | `src/ui/panel_widgets/motion_widget.cpp` (64 lines) |
| Background → UI | Never touch LVGL off the main thread; `ui_queue_update()` or `tok.defer()` | `src/printer/printer_state.cpp` `set_*_internal()` |

---

## Where Things Live

**Singletons** (classic `::instance()` unless noted):
`SettingsManager` (persistent settings), `NavigationManager` (panel/overlay stack), `UpdateQueue` (thread-safe UI updates), `SoundManager`, `DisplayManager`, `ModalStack`, `ToolState` (multi-tool tracking), `AmsState` (multi-backend filament systems). Two look like singletons but are not: `PrinterState` (all printer data/subjects) is a Meyers singleton reached via `get_printer_state()` (`include/app_globals.h#get_printer_state`) — there is no `PrinterState::instance()`; `PrinterDetector` (printer DB + capabilities) is a static class, no instance exists. Full census: `docs/devel/architecture/05-printer-state.md`.

`TemperatureController` — single authority for ALL nozzle/bed/chamber target sends (NOT a `::instance()` singleton: owned by `SubjectInitializer`, reached via `get_temperature_controller()` in `app_globals.h`). New temp-setting UI MUST call `TemperatureController::set_target()`, never raw `MoonrakerAPI::set_temperature()` — lint-enforced by `tests/shell/test_code_lint.bats`. See the TemperatureController section of `docs/devel/architecture/05-printer-state.md`.

**Entry flow**: `main.cpp` → `Application` → `DisplayManager` → panels via `NavigationManager`

**Key directories**:
| Path | Contents |
|------|----------|
| `src/ui/` | All UI code — flat dir, prefixed: `ui_panel_*.cpp`, `ui_overlay_*.cpp`, `ui_modal*.cpp` |
| `src/ui/modals/` | Additional modal implementations |
| `src/printer/` | PrinterState, MoonrakerAPI, macro/filament managers |
| `src/system/` | Config, settings, update checker, sound, telemetry |
| `src/application/` | App lifecycle, display, input, runtime config |
| `ui_xml/` | All XML layouts (loaded at runtime — no rebuild needed) |
| `ui_xml/components/` | Reusable XML components |
| `assets/` | Fonts, images, sounds, printer DB JSON |
| `config/` | Default config files, env templates |

**Runtime config** (on device): `~/helixscreen/config/` — settings.json, printer_database.json, helixscreen.env

**Mock-facing interfaces**: `IMoonrakerAPI` (`include/i_moonraker_api.h`), `helix::IMoonrakerClient` (`include/i_moonraker_client.h`), and the ten sub-API interfaces in `include/i_moonraker_sub_apis.h` are the consumer contract for the Moonraker network layer — consumers depend on these interfaces ONLY, never the concrete classes. The concretes (`MoonrakerAPI`, `helix::MoonrakerClient`, the ten `Moonraker*API` sub-classes) live behind `MoonrakerManager` (`include/moonraker_manager.h`), which owns them via `std::unique_ptr<MoonrakerAPI>` (the concrete façade — the mock inherits it) / `std::unique_ptr<helix::IMoonrakerClient>` and constructs them in `create_api()` / `create_client()`. Mocks still inherit the concretes. Drift protection in `tests/unit/test_interface_drift_*.cpp` (`[compile][drift]` tag). Lint-enforced by `tests/shell/test_code_lint.bats` — naming a concrete type outside the network layer fails CI.

**Test isolation**: `HelixTestFixture` (`tests/helix_test_fixture.h`) is the base for every test fixture. Ctor + dtor call `reset_all()` which drains `UpdateQueue`, resets `SystemSettingsManager` language, clears `ModalStack`. `LVGLTestFixture` inherits it. `XMLTestFixture` owns per-instance `PrinterState` / `MoonrakerClient` / `MoonrakerAPI`. XML subjects still register into LVGL's global scope — per-test scopes were blocked by LVGL internals; subjects are refreshed by each test's `init_subjects(true)`.

---

## Debugging

**NEVER debug without flags!** Use `-vv` minimum.
Trust debug output. Impossible values = bug is UPSTREAM. Ask "what ELSE?" not "did first fix work?"

**A plain `> file` redirect drops the console log, except under `--test`.** The console
sink attaches for a TTY always, for a pipe only with an explicit `-v`/`--log-level`, and for
a regular file or socket never (a plain redirect is indistinguishable from the daemon's).
`--test` logs to any stdout kind. A **non-`--test`** background run needs
`2>&1 | tee /tmp/x.log` or `HELIX_LOG_DEST=console`. Decision table: `docs/devel/LOGGING.md`
§ "Console sink".

**Debug bundles**: `--save` writes `debug-bundle-<code>.json` to the **current working directory** — run it from `/tmp` so the bundle never lands in the repo: `cd /tmp && <repo>/scripts/debug-bundle.sh <SHARE_CODE> --save`. Investigate there, never commit a bundle. (If one ends up in the repo, move it to `/tmp`.)

### Drive the UI yourself — `ctl` is not a question for Preston

Anything observable or drivable is yours to do: reaching a panel, clicking a widget, reading a
value, capturing a screenshot. Ask him only for the judgment a human eye has to make ("does
this look right"), and even then drive to the state first and tell him exactly what to look
at. `ctl text` / `state` / `geom` are exact; a screenshot only proves what a scroll position
happened to expose.

**Local mock — always allowed, no permission needed.** The pinned-socket recipe in Quick
Start with `--test`, `SDL_VIDEODRIVER=dummy`, and `--sim-speed 4-10` when you need an active
print. This is the default answer to "does my change work".

**Real printer — ask once per session, then keep going.** Get Preston's OK before the first
command that touches a real machine. After that, read-only commands (`ping`, `status`,
`current`, `ls`, `text`, `state`, `geom`, `screenshot`) need no further asking. Anything that
moves the machine or changes a print — gcode, home, heat, move, print/cancel,
`FIRMWARE_RESTART`, emergency stop — is confirmed **every** time. `ctl current` before you
navigate and navigate back when you are done: that is his printer's screen, and he may be
standing in front of it.

Two shapes, and they are not the same thing:

| Shape | What it is | How |
|-------|-----------|-----|
| desktop UI → real Moonraker | your local build, real printer data, no device walk | `--moonraker ws://HOST:7125`. Enough for most hardware questions: a local `SDL_VIDEODRIVER=dummy` build pointed at a printer discovers its real hardware (AFC lanes, QGL, …) and `ctl` drives the result |
| `ctl` → app on the device | the app actually running on the printer | ssh, then `<install>/bin/helix-screen ctl <cmd>` |

**The device build gate - check the binary, not just the help text.** `ENABLE_REMOTE_CONTROL`
defaults to `yes`, but `HELIX_PACKAGING=1` forces it to `no`, so **an installed release has no
ctl server**; `--help` on those builds omits `--remote*` and passing one anyway warns
("ignoring unknown argument") instead of being silently accepted. Confirm what actually
shipped rather than trusting either:

```bash
strings -a <install>/bin/helix-screen | grep -c list_callbacks   # 0 = no server compiled in
```

A device also needs `HELIX_REMOTE_CONTROL=1` in its `helixscreen.env` for the server to
listen; `make deploy-*` sets that from the `.build-features` stamp `mk/rules.mk` writes
beside the binary.

---

## Critical Paths (always MAJOR work)

PrinterState, WebSocket/threading, shutdown, DisplayManager, XML processing

---

## Superpowers Skills Here

Worktrees come from `scripts/setup-worktree.sh` and go away with
`scripts/teardown-worktree.sh`. These replace the create step in
`superpowers:using-git-worktrees` (a native or plain `git worktree add` tree has no lib/
symlinks, submodules or build) and the `git worktree remove` step in
`superpowers:finishing-a-development-branch`, which refuses here.

Design work overrides `superpowers:brainstorming` and `superpowers:writing-plans` here:

- **Ask in batches.** Put 4-5 independent questions in one round (AskUserQuestion takes four;
  a companion page can carry more visual ones). Ask one at a time only when an answer changes
  the next question.
- **Explain the signal first.** UI for a hardware reading opens with what each signal measures,
  which hardware and backends publish it, and which printers have none, before any option:
  two readings from one sensor can still answer different questions and need different UI.
- **Measure before suggesting.** `ctl geom` every surface an option will occupy (a 1x1 and 2x1
  home cell, the sidebar card, the path canvas segment, the modal) at 800x480 and the smallest
  breakpoint, and draw options inside those numbers.
- **Plans state interfaces, tests and constraints, not finished code.** Give exact signatures,
  test cases and values; leave the bodies to the implementer. Every header a plan defines gets a
  `scripts/syntax_check.py` pass before execution starts. A plan of uncompiled code is a second
  codebase that drifts from the first.

---

## Autonomous Sessions

Given autonomous control ("work independently", "minimal interruption"), load the
`autonomous-session` skill — scratchpad workspace, autonomy guidelines, and what still
requires asking first.
