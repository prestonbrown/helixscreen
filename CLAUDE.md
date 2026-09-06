# CLAUDE.md

## Quick Start

**HelixScreen**: LVGL 9.5 touchscreen UI for Klipper 3D printers. XML engine in `lib/helix-xml/` — our own MIT fork of the engine LVGL removed in 9.5, and its own repo ([prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml)), so a fresh clone needs `git submodule update --init --recursive`. Pattern: XML → Subjects → C++.

**macOS is a development platform only — never a deployment target.** The app ships to embedded Linux (MIPS, ARM, aarch64) and Raspberry Pi. A Mac exists to build, run `--test`, and iterate on UI; nothing is released for it and no user runs it there. Host-portability work is justified by *a developer being able to build and run the suite*, never by production correctness — so fix a portability break when it blocks your own iteration, and don't add CI jobs or abstractions to defend macOS as a runtime.

**Planned work lives in GitHub issues** (`gh issue list --milestone Backlog`; milestone = scheduling axis, labels = kind: docs-debt, hw-verify, tech-debt). In-flight plans and specs live in `docs/devel/plans/`: point-in-time scaffolding, deleted in the change that ships the work (convention: `docs/CLAUDE.md`).

**Before compiling, check for a build already running** — concurrent compilations thrash the machine:

```bash
pgrep -x -d' ' 'make|cc1plus'   # ONE pattern. Never pgrep -f: it matches its own command line
free -h                          # read the Mem AND Swap rows together
```

- Throttle to `-j6` only when BOTH are tight: low `available` *and* swap near 0. That is the case where `-j8` dies mid-link with no `oom-kill` line while load average looks healthy. Tens of GB `available` beside an exhausted swap row is not a throttle signal. With the box to yourself, `-j` at full `nproc`, and ramp back up the moment a peer finishes.
- Dying at the same step twice **can** be a resource ceiling, but rule out a peer first: a second `make` in the SAME tree deletes your freshly linked binary (`prune-orphan-test-objs` in `mk/tests.mk` runs `rm -f $(TEST_BIN)` as a sibling prerequisite of the link, so `-j` gives them no order). The tell: `[LD] helix-tests`, then `✓ Unit test binary ready`, NO `✗ Test linking failed!`, then every shard reports `No such file or directory`. Nothing is wrong with your code; a starved link fails loudly and stops make.
- Who else is building, and in which tree, is a question you ask them: `ListAgents` + `SendMessage` (global CLAUDE.md § Peer Sessions), not a `pgrep` guess.

```bash
make -j                              # Build ONLY the program binary (NOT tests)
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
make test-run                        # Build AND run tests in parallel
./build/bin/helix-tests "[tag]"      # Run specific test tags
make pi-test                         # Build on thelio + deploy + run

# Worktrees — MUST use for MAJOR work. Always in .worktrees/ (project root).
scripts/setup-worktree.sh feature/my-branch  # Symlinks shared deps, builds fast
#   lib/lvgl, lib/libhv and lib/helix-xml get a PRIVATE checkout per worktree,
#   so patches/ (which is per-branch) reaches no other tree. Everything else in
#   lib/ is symlinked from the main tree and shared.
#   Also writes .claude/settings.local.json (gitignored) with PROJECT_DIR set to
#   the MAIN tree, so claude-recall writes lessons and stats there instead of to
#   a per-worktree .claude-recall/ that `git worktree remove` would discard.
#   A worktree made by hand (plain `git worktree add`), or one the harness makes
#   on its own (EnterWorktree -> .claude/worktrees/), gets NONE of this: no lib/
#   symlinks, no submodules, no build. Prefer this script; if you are already in
#   one, `git submodule update --init --recursive` and a full build before
#   trusting anything it produces.
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
> Prefer `ctl text <name>` / `ctl geom <name>` over reading a screenshot — they are exact,
> and a screenshot only proves what a scroll position happened to expose.

---

## Sharing This Tree With Other Sessions

The protocol is global CLAUDE.md § Peer Sessions. What is shared here:

- **The main working tree is live.** Other sessions commit in it. `git status --short | grep '^MM'` means someone is mid-commit: wait, never merge into that, and never let git autostash (`-c merge.autoStash=false`). Commit your own edits promptly, with explicit pathspecs.
- **`build/bin/helix-tests` and `helix-screen` can be one inode across worktrees**: whoever linked last set the bytes both trees run. Compare `stat` inodes before trusting a control run against a sibling tree.
- **The default `ctl` socket is per-user, not per-instance.** Pin it (box above) or you drive a peer's app and it reports success.
- **One session per physical printer at a time.** Ask who holds a device before pointing anything at it.

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
| **Observer factory** | Static callback + `lv_observer_get_user_data()` | `observe_int_sync<Panel>()` from `observer_factory.h` |
| **Icon sync** | Add icon, forget fonts | `include/ui_icon_codepoints.h` + `make regen-fonts` + rebuild |
| **Formatting** | Manual formatting | Let pre-commit hook (clang-format) fix |
| **Doc citations** | A line number (`src/printer/printer_state.cpp:638`), or a bare `:NNN` with no path | A place: `` `src/printer/printer_state.cpp#update_from_status` `` - path, then a `#` fragment naming the enclosing scopes. `scripts/doc_anchors.py` resolves it to a line on demand (`make check-doc-anchors`, advisory), so code that moves rots nothing. A RENAMED symbol is the one case you fix by hand, because the sentence around it may no longer be true |
| **No auto-mock** | `if(!start()) return Mock()` | Check `RuntimeConfig::should_mock_*()` |
| **JSON include** | `#include <nlohmann/json.hpp>` | `#include "hv/json.hpp"` (libhv's bundled version) |
| **Build system** | `cmake`, `ninja` | `make -j` (pure Makefile) |
| **No RTTI** | `dynamic_cast`, `typeid`, `std::type_index`, `any.type()` | `helix::type_tag<T>()` keys, virtual kind queries (`HELIX_CONTEXT_MENU_KIND`), pointer-form `any_cast`. Firmware builds `-fno-rtti`; lint-gated, escape hatch `// RTTI_OK: <reason>` |
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
| Confirmation dialog | `modal_confirm(title, msg, severity, btn_text, on_confirm, ConfirmOptions)` (in `helix::ui`) for new code - `std::function` throughout, closes its own dialog; the options struct carries `on_cancel`/`cancel_text`/`on_dismiss`/`owner_token`, and `owner_token` gates **all three** callbacks. The `lv_event_cb_t` spellings (`modal_show_confirmation()`/`modal_show_alert()`) are gone. All are owned, so a dismissal (backdrop tap, ESC, hot-reload rebuild) reaches `on_dismiss` - **pass it whenever the caller holds a guard/flag/pending entry the buttons were meant to clear**, or that state leaks | `src/ui/ui_change_host_modal.cpp#show_connection_failed_modal`; subclass form: `include/lan_client_auth_router.h` |
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

**The device build gate — check the binary, never the help text.** `ENABLE_REMOTE_CONTROL`
defaults to `yes`, but `HELIX_PACKAGING=1` forces it to `no`, so **an installed release has no
ctl server**. The `--remote*` flags still appear in `--help` on those builds, so they look
supported and buy nothing:

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

## Autonomous Sessions

Given autonomous control ("work independently", "minimal interruption"), load the
`autonomous-session` skill — scratchpad workspace, autonomy guidelines, and what still
requires asking first.
