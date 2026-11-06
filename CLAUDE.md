# CLAUDE.md

## Quick Start

**HelixScreen**: LVGL 9.5 touchscreen UI for Klipper 3D printers. XML engine in `lib/helix-xml/`, our own MIT fork of the engine LVGL removed in 9.5 and its own repo ([prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml)), so a fresh clone needs `git submodule update --init --recursive`. Pattern: XML → Subjects → C++.

**macOS is a development platform only, never a deployment target.** The app ships to embedded Linux (MIPS, ARM, aarch64) and Raspberry Pi. Fix a Mac portability break only when it blocks your own build or test run; never add CI jobs or abstractions to defend macOS as a runtime. **On a Mac build with `gmake`**: bare `make` is Apple's 3.81, which parses the Makefile but silently no-ops every `$(file ...)` and links a stale manifest.

**Planned work lives in GitHub issues** (`gh issue list --milestone Backlog`; milestone = scheduling axis, labels = kind: docs-debt, hw-verify, tech-debt). In-flight plans and specs live in `docs/devel/plans/` and are deleted in the change that ships the work (`docs/CLAUDE.md`).

**Before compiling, check for a build already running**, since concurrent compilations thrash the machine: `pgrep -x -d' ' 'make|clang++|cc1plus'` (never `pgrep -f`), `free -h` (only the Mem row's `available` matters; a full Swap row is normal), `jobpool status`. Who else is building, and in which tree, is a question you ask them (`ListAgents` + `SendMessage`). How to read those numbers, abandoned spinners, and why a silent build is not a dead one: `docs/devel/SHARED_TREE.md` § "Builds that collide".

- **Plain `make` picks its own `-j`; never hand-pick `-jN`.** Where jobpool is installed (thelio, zeus) `make` is its shim and every build on the box shares one pool. `scripts/helix-claim jobs` is make's `-j` only: size docker, ninja or parallel work with `scripts/helix-claim hold` or `scripts/pool-docker.sh`. `docs/devel/BUILD_SYSTEM.md` § "Parallel Compilation".
- **Two `make`s in one tree delete each other's test binary.** Every shard then reports `No such file or directory`; nothing is wrong with your code. Serialize, or use a worktree.
- **The commit hook runs only the fast gates** (seconds). The slow ones run at pre-push and in `land-gate`. `HELIX_QC_ALL_AT_COMMIT=1` runs every gate at commit; `scripts/qc_timing.py` shows where a run's time went.

```bash
make                                 # Build ONLY the program binary (NOT tests)
./build/bin/helix-screen --test -vv  # Mock printer + DEBUG logs (-v INFO, -vv DEBUG, -vvv TRACE; default WARN)
HELIX_MOCK_AUTO_PRINT=1 ./build/bin/helix-screen --test --sim-speed 6 -vv
#   Anything needing an ACTIVE PRINT: 4-10x reaches Printing in ~15s. 50x+ starts AND
#   finishes the print in ~8s, outrunning async loads: use it only to reach completion.

make t F='[tag]'                     # Build, then run ONE tag or case (the inner loop)
./build/bin/helix-tests '[tag]'      # Same, no rebuild: only if you have not edited since `make test`.
#   A stale binary prints `WARNING: helix-tests is stale` on stderr first: believe it.
make full-test-run                   # unit-sweep + bats (~2m): the only local run of bats
make land-gate                       # THE pre-landing gate: ONCE per batch, before anything reaches main
#   Quote the ETA it prints, never a guess. Watch its log for ": FAIL in|: TIMEOUT in" and
#   report a failed stage the moment it lands. A red step: `make land-gate STAGES=<step>`
#   reruns just it on the same clean HEAD. full-test-run is NOT a landing gate: it skips
#   tests/ui and CI's shard layout. Stages and stamps: tests/CLAUDE.md, scripts/CLAUDE.md.
make dev-timing                      # Measured medians for every build, suite and test run
scripts/syntax_check.py <file>...    # "does this compile?" in seconds; proves nothing about behaviour

make remote-test TAG='[ams]'         # Build + run one tag on $REMOTE_HOST (sends a patch, not the tree)
scripts/test-host-run.sh mutate --tests '[tag]'   # mutation on the test host (zeus)
scripts/test-host-run.sh asan '[tag]'             # ASAN: never on thelio, where it runs nothing and exits 0
scripts/test-host-run.sh sweep | test ... | exec [--build] -- cmd
#   Runs your tree as it is on disk, uncommitted edits included. bats stays local.
#   docs/devel/BUILD_SYSTEM.md § "Using your own build/test host".

scripts/setup-worktree.sh feature/my-branch  # MAJOR work: worktrees in .worktrees/, never EnterWorktree
scripts/teardown-worktree.sh my-branch       # `git worktree remove` refuses here; -n prints the plan
#   setup refuses an existing .worktrees/<last-segment>: check that it is not a peer's tree.
#   BUILD_SYSTEM.md § "Git Worktrees" covers what each script does and refuses.
```

**XML changes need no rebuild:** `ui_xml/*.xml` is loaded at runtime, and native dev builds hot-reload it within ~500ms of a save (`HELIX_HOT_RELOAD=0/1` overrides). Invalid XML mid-write is skipped and retried.

**Screenshots:** press 'S' in the UI, or `./scripts/screenshot.sh helix-screen output-name [token]` (tokens in `scripts/screenshot-recipes.sh`).

**Driving the UI:** `helix-screen ctl` (`navigate`/`click`/`ls`/`text`/`geom`/`set_value`/`scroll`/`demo`/`screenshot`) or `helix-screen repl`; `docs/devel/HELIXCTL.md`. Prefer `ctl text`/`ctl geom` over reading a screenshot.

> **Always pin the socket AND the config dir, derived from the tree. Never run a bare `ctl`**: with
> two instances up it drives whichever started first and still reports success.
> ```bash
> TREE=$(basename "$(git rev-parse --show-toplevel)")
> export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
> mkdir -p "$HELIX_CONFIG_DIR"   # must exist: the app aborts otherwise
> ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
> ./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate settings
> ```
> **Keep `--test`.** An empty config dir restores settings from `~/.helixscreen/settings.json.backup`,
> real printer address included, and a hand-written `settings.json` without `config_version` is
> replaced wholesale from that backup. HELIXCTL.md § "Running a fully isolated second instance".

---

## Verification Cadence: rigor once, at the finish

**Every expensive check runs once per batch, at the finish, sized to what the batch touched.**
Running a check again buys no rigor; it only costs the session hours. Put this table in every
agent brief, because agents default to over-verifying.

| When | Run | Not then |
|---|---|---|
| **While building** - each task, each agent, each review fix | `make t F='[tags the change touches]'` once after the change. A new or changed test: break the line it pins once, see it go red, restore | `mutate-diff`, `unit-sweep`, `full-test-run`, `land-gate`, screenshots, ASAN, re-reviews |
| **Finish** - once per batch (every branch landing together) | 1. One review of the batch; a second round only for a crash, lifetime, data-loss or wrong-hardware-command finding. 2. One screenshot per changed screen. 3. ONE `make land-gate` on the combined result, on the fastest free machine (thelio, then zeus; never a Mac) | a gate per branch; a second full gate for a docs-only or test-only fix; waiting on mutation |
| **Landed** - right after the push, every batch, never skipped | `make mutate-diff MUTATE_ARGS="--base <the origin/main it landed on>"` from the landed tree, in the background, capped at 30 minutes. A survivor is a follow-up fix (a test that pins it) | blocking the merge on it |
| **Gate red** | Flaky or environment stage: `make land-gate STAGES=<stage>` on the same clean HEAD. A real finding: fix it, rerun only that stage's command (`build/land-gate/<stage>.log` names it), then one more `land-gate` only if the fix touched `src/` or `include/` | rerunning the whole gate to "be sure" |
| **Release** | Whole-branch mutation, ASAN on the test host, device checks | |

**Several changes or agents at once: one test plan, owned by one session.** The orchestrator
writes it into every brief; agents run their tags and hand back, never mutation, sweeps, gates or
their own reviews. With peer sessions, whoever lands next owns the batch; the others hand over
branch + SHA and stop testing.

---

## Sharing This Tree With Other Sessions

The manual behind these rules (helix-claim, the hooks, reading a tree mid-merge):
**`docs/devel/SHARED_TREE.md`**.

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
- **Never `pkill helix-screen`** (nor `-x`, nor `-f`): it kills every session's instance. Resolve
  your PID from your own socket.
- **`build/bin/helix-tests` and `helix-screen` can be one inode across worktrees.** Compare `stat`
  inodes before trusting a control run against a sibling tree.

---

## Docs (load when needed)

Full index: **`docs/devel/CLAUDE.md`** (auto-loaded when working in docs/devel/)

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
| `docs/devel/BUILD_SYSTEM.md` | Makefile, cross-compilation, jobpool, worktrees, the test host |
| `docs/devel/SHARED_TREE.md` | Working beside other sessions: helix-claim, hooks, colliding builds |
| `tests/CLAUDE.md` | Test cadence table, land-gate stages, proving a test can fail |

## Path-Scoped Rules (`.claude/rules/`)

These load automatically when you work on matching files. They are the contract, not background reading; every one is lint-gated.

| Rule | Loads for | Covers |
|------|-----------|--------|
| `.claude/rules/declarative-ui.md` | `src/ui/`, `ui_xml/`, `include/ui_*.h` | DATA in C++, APPEARANCE in XML: the nine declarative rules, the structural exceptions, design tokens |
| `.claude/rules/vendor-abstraction.md` | `src/`, `include/` | A vendor name appears in ONE module per capability; generic code asks capability questions |
| `.claude/rules/threading.md` | `src/`, `include/`, `tests/unit/` | The five lifecycle invariants; `docs/devel/THREADING.md` is the full text |
| `.claude/rules/submodules.md` | `lib/`, `patches/`, `mk/patches.mk` | `lib/helix-xml/` is ours and edited directly; everything else goes through `patches/` |
| `.claude/rules/filament-backends.md` | `src/printer/ams_*`, `include/ams_*`, `*filament_*` | Which doc to read before touching a filament backend |

---

## Above a Bugfix: Investigate, Then Scope

Features, refactors, new panels/widgets/managers: **scope AFTER investigating, not before.**

- Map what exists (call sites, subjects, tests) and read that subsystem's `docs/devel/` doc
- Find the canonical implementation of each piece you're about to write
- Extend the near-fit helper; never fork a twin. Copy-paste-modify = red flag
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
| **Icon sync** | Add icon, forget fonts | TWO hand-kept lists: `include/ui_icon_codepoints.h` (name -> codepoint) AND `MDI_ICONS` in `scripts/regen_mdi_fonts.sh` (what the font contains), then `make regen-fonts` and rebuild; `validate_icon_fonts.sh` fails the build on a missing one |
| **Formatting** | Manual formatting | Let pre-commit hook (clang-format) fix |
| **Doc citations** | A line number (`src/printer/printer_state.cpp:638`) | `` `src/printer/printer_state.cpp#update_from_status` ``: path + `#` enclosing scopes (`scripts/doc_anchors.py`). Fix by hand only on a rename |
| **No auto-mock** | `if(!start()) return Mock()` | Check `RuntimeConfig::should_mock_*()` |
| **JSON include** | `#include <nlohmann/json.hpp>` | `#include "hv/json.hpp"`; `json_fwd.h` in a header that only names `json` by reference, pointer or return type |
| **Build system** | `cmake`, `ninja` | `make` (pure Makefile) |
| **No RTTI** | `dynamic_cast`, `typeid`, `std::type_index`, `any.type()` | `helix::type_tag<T>()` keys, virtual kind queries, pointer-form `any_cast`. Escape hatch `// RTTI_OK: <reason>` |
| **No exceptions (firmware)** | `try`/`catch`/`throw`, `j.value("k", d)`, `j.at("k")`, one-arg `json::parse`, `std::stoi`, value-form `any_cast` | `json_util::safe_*`/`as_*`, `json::parse(s, nullptr, false)`, `text_io::parse_leading`, `any_cast<T>(&a)`; `exception_policy.h`. ESP32 builds `-fno-exceptions`: a throw is an abort |
| **Constant names** | `constexpr int MAX_RETRY_COUNT = 3;` | `constexpr int kMaxRetryCount = 3;` (UPPER_SNAKE is for macros). Escape hatch `// CONST_NAME_OK: <reason>` |
| **No std::regex** | `std::regex`, `#include <regex>` | `helix::Regex` (`include/helix_regex.h`): libstdc++'s matcher overflows musl's thread stacks. Escape hatch `// STD_REGEX_OK: <reason>` |
| **Bug commits** | Filing an issue just so the commit can cite one | Cite one that exists: `fix(scope): thing (prestonbrown/helixscreen#123)`; otherwise `fix(scope): thing` |
| **Unproven tests** | "Tests pass" as evidence | A hand break while writing the test, then `make mutate-diff` after landing. `tests/CLAUDE.md` § "Proving a test can fail" |
| **Commit body length** | 3-paragraph Tests / Verification / Mutation essay | Subject + ~4-line paragraph |
| **Comment archaeology** | `// unlike the three widgets 3d0875bff fixed`, `// this used to memcpy the whole canvas` | A present-tense constraint: `// Invalidating here freezes a fullscreen view the user is watching`. Bare issue cites are fine (global CLAUDE.md; `scripts/check_comment_archaeology.py`) |
| **Submodule mods** | Edit `lib/lvgl/...` / `lib/libhv/...` directly | `patches/*.patch`; `lib/helix-xml/` is ours and edited directly |

**ALWAYS:** Search the SAME FILE you're editing for similar patterns before implementing.

---

## Patterns

| Pattern | Key Point | Exemplar |
|---------|-----------|----------|
| Subject init order | Register components → init subjects → create XML | `src/application/subject_initializer.cpp` |
| Widget lookup | `lv_obj_find_by_name()` not `lv_obj_get_child()` | any panel |
| Overlays | `NavigationManager::instance().push_overlay(root)` / `.go_back()`; pair every push with `register_overlay_instance(root, this)` or `on_deactivate()` never fires | `src/ui/ui_settings_safety.cpp` |
| Modals (simple) | `Modal::show("component_name")` / `Modal::hide(dialog)` | `src/ui/ui_job_queue_modal.cpp` |
| Modals (subclass) | Extend `Modal`, implement `get_name()` + `component_name()`, override `on_ok()`/`on_cancel()` | `include/ui_info_qr_modal.h` + `src/ui/ui_info_qr_modal.cpp` |
| Confirmation dialog | `helix::ui::modal_confirm(title, msg, severity, btn_text, on_confirm, ConfirmOptions)`. **Pass `on_dismiss` whenever the caller holds a guard/flag/pending entry the buttons were meant to clear**: backdrop tap, ESC and hot-reload dismiss without a button. `owner_token` gates all three callbacks | `src/ui/ui_change_host_modal.cpp#show_connection_failed_modal` |
| Modal buttons (XML) | `<modal_button_row primary_text="Save" primary_callback="on_save"/>` | `ui_xml/bed_mesh_rename_modal.xml` |
| Home-panel widget | Subclass `PanelWidget`; `attach()` + `on_size_changed()`. Instances are **recycled**, so any imperative apply runs from `attach()` too | `src/ui/panel_widgets/motion_widget.cpp` |
| Background → UI | Never touch LVGL off the main thread; `ui_queue_update()` or `tok.defer()` | `src/printer/printer_state.cpp` `set_*_internal()` |

---

## Where Things Live

**Singletons** (`::instance()`): `SettingsManager`, `NavigationManager`, `UpdateQueue`, `SoundManager`, `DisplayManager`, `ModalStack`, `ToolState`, `AmsState`. **Not** `::instance()`: `PrinterState` via `get_printer_state()` (`include/app_globals.h`); `PrinterDetector` is a static class. Census: `docs/devel/architecture/05-printer-state.md`.

**`TemperatureController`** is the single authority for every nozzle/bed/chamber target send (`get_temperature_controller()`). New temp UI calls `TemperatureController::set_target()`, never raw `MoonrakerAPI::set_temperature()`; lint-enforced.

**Moonraker layer:** consumers depend ONLY on `helix::IMoonrakerAPI` (`include/i_moonraker_api.h`), `helix::IMoonrakerClient` and the sub-API interfaces in `include/i_moonraker_sub_apis.h`. The concretes live behind `MoonrakerManager`; naming one outside the network layer fails lint. `docs/devel/ARCHITECTURE.md`.

**Test isolation:** every fixture derives from `HelixTestFixture` (`tests/helix_test_fixture.h`), which drains `UpdateQueue`, resets language and clears `ModalStack` on both ends. `docs/devel/UI_TESTING.md`.

**Entry flow**: `main.cpp` → `Application` → `DisplayManager` → panels via `NavigationManager`

| Path | Contents |
|------|----------|
| `src/ui/` | All UI code: a flat dir, prefixed (`ui_panel_*.cpp`, `ui_overlay_*.cpp`, `ui_modal*.cpp`), plus `modals/`, `panel_widgets/`, `widgets/`, `tour/` |
| `src/printer/` | PrinterState, macro/filament managers |
| `src/api/` | The Moonraker client and API facade |
| `src/network/` | WiFi, Ethernet, mDNS |
| `src/system/` | Config, settings, update checker, sound, telemetry |
| `src/application/` | App lifecycle, display, input, runtime config |
| `ui_xml/`, `ui_xml/components/` | XML layouts and reusable components (runtime-loaded) |
| `assets/`, `config/` | Fonts, images, sounds, printer DB; default configs |

**Runtime config on device**: `~/helixscreen/config/` (settings.json, printer_database.json, helixscreen.env)

---

## Debugging

**NEVER debug without flags!** Use `-vv` minimum. Trust debug output: impossible values mean the bug is UPSTREAM. Ask "what ELSE?", not "did the first fix work?"

**A plain `> file` redirect drops the console log, except under `--test`.** A non-`--test` background run needs `2>&1 | tee /tmp/x.log` or `HELIX_LOG_DEST=console`. `docs/devel/LOGGING.md` § "Console sink".

**Debug bundles**: run `cd /tmp && <repo>/scripts/debug-bundle.sh <SHARE_CODE> --save`; `--save` writes to the cwd, and a bundle never lands in the repo.

### Drive the UI yourself: `ctl` is not a question for Preston

Reaching a panel, clicking, reading a value, capturing a screenshot are yours to do. Ask him only for the judgment a human eye has to make, and drive to the state first.

- **Local mock: always allowed.** The pinned recipe above with `--test`, `SDL_VIDEODRIVER=dummy`.
- **Real printer: ask once per session.** After that, read-only commands (`ping`, `status`, `current`, `ls`, `text`, `state`, `geom`, `screenshot`) need no asking. Anything that moves the machine or changes a print (gcode, home, heat, move, print/cancel, `FIRMWARE_RESTART`, e-stop) is confirmed **every** time. `ctl current` before you navigate, and navigate back when done.
- Desktop UI against a real printer: `--moonraker ws://HOST:7125` (the flag; there is no `--moonraker-url`). The app on the device: ssh, then `<install>/bin/helix-screen ctl`.
- **Installed releases have no ctl server** (`HELIX_PACKAGING=1` forces it off). Check the binary: `strings -a <bin> | grep -c list_callbacks` (0 = none). The server also needs `HELIX_REMOTE_CONTROL=1` in `helixscreen.env`. HELIXCTL.md § "Enabling the server".

---

## Critical Paths (always MAJOR work)

PrinterState, WebSocket/threading, shutdown, DisplayManager, XML processing

---

## Superpowers Skills Here

Worktrees come from `scripts/setup-worktree.sh` and go away with `scripts/teardown-worktree.sh`, replacing the create step in `superpowers:using-git-worktrees` and the `git worktree remove` step in `superpowers:finishing-a-development-branch`.

Design work overrides `superpowers:brainstorming` and `superpowers:writing-plans` here:

- **Ask in batches** of 4-5 independent questions; one at a time only when an answer changes the next question.
- **Explain the signal first.** UI for a hardware reading opens with what each signal measures, which hardware and backends publish it, and which printers have none.
- **Measure before suggesting.** `ctl geom` every surface an option will occupy at 800x480 and the smallest breakpoint, and draw options inside those numbers.
- **Plans state interfaces, tests and constraints, not finished code.** Exact signatures, test cases and values; bodies left to the implementer. Every header a plan defines gets a `scripts/syntax_check.py` pass first.

Execution overrides `superpowers:subagent-driven-development`, `executing-plans`, `requesting-code-review` and `test-driven-development`: § Verification Cadence wins. No per-task review; one review at the finish; "verify" means the tags. TDD still holds: the new test goes red first.

---

## Autonomous Sessions

Given autonomous control ("work independently", "minimal interruption"), load the
`autonomous-session` skill: scratchpad workspace, autonomy guidelines, and what still
requires asking first.
