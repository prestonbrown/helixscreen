# CLAUDE.md

## Quick Start

**HelixScreen**: LVGL 9.5 touchscreen UI for Klipper 3D printers. XML engine in `lib/helix-xml/`, our own MIT fork of the engine LVGL removed in 9.5 and its own repo ([prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml)), so a fresh clone needs `git submodule update --init --recursive`. Pattern: XML → Subjects → C++.

**macOS is a development platform only, never a deployment target.** The app ships to embedded Linux (MIPS, ARM, aarch64) and Raspberry Pi. Fix a Mac portability break only when it blocks your own build or test run; never add CI jobs or abstractions to defend macOS as a runtime.

**Planned work lives in GitHub issues** (`gh issue list --milestone Backlog`; milestone = scheduling axis, labels = kind: docs-debt, hw-verify, tech-debt). In-flight plans and specs live in `docs/devel/plans/` and are deleted in the change that ships the work (`docs/CLAUDE.md`).

**Building, testing, landing, worktrees, build hosts or peer sessions: load the `helix-ops` skill** (`.claude/skills/helix-ops/SKILL.md`). Always on, wherever you are:

- **Before compiling, check for a build already running**: `pgrep -x -d' ' 'make|clang++|cc1plus'` (never `pgrep -f`), `free -h` (Mem `available`), `jobpool status`; ask whoever is building (`ListAgents` + `SendMessage`). Plain `make`, never `-jN`.
- **Two `make`s in one tree delete each other's test binary.** Every shard then reports `No such file or directory`; nothing is wrong with your code. Serialize, or use a worktree.
- **Never `pkill helix-screen`** (nor `-x`, nor `-f`): it kills every session's instance. Resolve your PID from your own socket.
- **The main working tree is live.** Commit with `git commit -- <paths>` (new file: `git add -N` first), never `git add` then commit; never let git autostash.

```bash
make                                 # Build ONLY the program binary (NOT tests)
./build/bin/helix-screen --test -vv  # Mock printer + DEBUG logs (-v INFO, -vv DEBUG, -vvv TRACE; default WARN)
HELIX_MOCK_AUTO_PRINT=1 ./build/bin/helix-screen --test --sim-speed 6 -vv
#   ACTIVE PRINT: 4-10x reaches Printing in ~15s; 50x+ also finishes it in ~8s (completion only).
make t F='[tag]'                     # Build, then run ONE tag or case (the inner loop)
make land-gate                       # THE pre-landing gate: ONCE per batch (procedure: helix-ops skill)
scripts/syntax_check.py <file>...    # "does this compile?" in seconds; proves nothing about behaviour
scripts/setup-worktree.sh feature/my-branch  # MAJOR work: .worktrees/, never EnterWorktree
```

**XML changes need no rebuild:** `ui_xml/*.xml` loads at runtime; dev builds hot-reload it ~500ms after a save (`HELIX_HOT_RELOAD=0/1`).

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

**Every expensive check runs once per batch, at the finish, sized to what the batch touched**, when the code it judges has stopped changing: a slow verdict on code about to move again is thrown away, and rerunning a check buys no rigor. A landing is not that moment when more of the same work is coming; the end of the work is. Put this table in every agent brief, because agents default to over-verifying.

| When | Run | Not then |
|---|---|---|
| **While building** - each task, each agent, each review fix | One change: `make t F='[tags the change touches]'` once after it. An independent job in its own worktree tests there as usual. Several worktrees for one feature or batch: each syntax-checks (`scripts/syntax_check.py`) and commits, with no builds or tag runs; merge them, then run the tags once on the merged tree. A new or changed test: break the line it pins once, see it go red, restore, on the merged tree when building is deferred | `mutate-diff`, `unit-sweep`, `full-test-run`, `land-gate`, screenshots, ASAN, re-reviews |
| **Finish** - once per batch (every branch landing together) | 1. One review of the batch; a second round only for a crash, lifetime, data-loss or wrong-hardware-command finding. 2. One screenshot per changed screen. 3. ONE `make land-gate` on the combined result, on the fastest free machine (thelio, then zeus; never a Mac) | a gate per branch; a second full gate for a docs-only or test-only fix; waiting on mutation |
| **Series finished** - when the work has stopped changing, not at each landing | `make mutate-diff MUTATE_ARGS="--base <origin/main before the series>"` once over the whole series, in the background, capped at 45 minutes. A survivor is a follow-up fix (a test that pins it) | after each landing while more of the same work is coming; blocking a merge on it |
| **Gate red** | Flaky or environment stage: `make land-gate STAGES=<stage>` on the same clean HEAD. A real finding: fix it, rerun only that stage's command (`build/land-gate/<stage>.log` names it), then one more `land-gate` only if the fix touched `src/` or `include/` | rerunning the whole gate to "be sure" |
| **Release** | Whole-branch mutation, ASAN on the test host, device checks | |

**Several changes or agents at once: one test plan, owned by one session** (helix-ops skill § "Several agents or sessions on one batch").

---

## Sharing This Tree With Other Sessions

Message the overlapping session before touching its work, claim shared resources with `scripts/helix-claim`, settle it between sessions and bring Preston outcomes, whoever moves main pushes it, one session per printer: the `helix-ops` skill and `docs/devel/SHARED_TREE.md`.

---

## Docs (load when needed)

Full index: **`docs/devel/CLAUDE.md`** (auto-loaded when working in docs/devel/)

| Doc | When |
|-----|------|
| `docs/devel/ARCHITECTURE.md` | Whole-app 15-minute model, routes into the architecture chapters |
| `docs/devel/UI_CONTRIBUTOR_GUIDE.md` | UI/layout: breakpoints, tokens, colors, widgets |
| `docs/devel/LVGL9_XML_GUIDE.md` | XML layouts, widgets, bindings, observer cleanup |
| `docs/devel/MODAL_SYSTEM.md` | Modal architecture: ui_dialog, modal_button_row, Modal pattern |
| `docs/devel/FILAMENT_MANAGEMENT.md` | AMS, AFC, Happy Hare, ACE, AD5X IFS, CFS, Tool Changer |
| `docs/devel/CHAMBER_HEATER.md` | Chamber heaters: backends, discovery, diagnostics, ceiling rules |
| `docs/devel/REVIEW_RUBRIC.md` | Reviewing a change: crash families, silent-failure traps |
| `docs/devel/ENVIRONMENT_VARIABLES.md` | Runtime env vars |
| `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` | Mock printer config for `--test` runs (`HELIX_MOCK_*`, replay) |
| `docs/devel/LOGGING.md` | spdlog levels: info vs debug vs trace |
| `docs/devel/BUILD_SYSTEM.md` | Makefile, cross-compilation, jobpool, worktrees, the test host |
| `docs/devel/SHARED_TREE.md` | Working beside other sessions: helix-claim, hooks, colliding builds |
| `tests/CLAUDE.md` | Test cadence table, land-gate stages, proving a test can fail |
| `scripts/CLAUDE.md` | One line per script; full notes in `docs/devel/SCRIPTS_REFERENCE.md` |

## Path-Scoped Rules (`.claude/rules/`)

They load automatically for matching files, and every one is lint-gated.

| Rule | Loads for | Covers |
|------|-----------|--------|
| `.claude/rules/declarative-ui.md` | `src/ui/`, `ui_xml/`, `include/ui_*.h` | DATA in C++, APPEARANCE in XML; design tokens |
| `.claude/rules/vendor-abstraction.md` | `src/`, `include/` | A vendor name lives in ONE module per capability |
| `.claude/rules/threading.md` | `src/`, `include/`, `tests/unit/` | The five lifecycle invariants; `docs/devel/THREADING.md` is the full text |
| `.claude/rules/submodules.md` | `lib/`, `patches/`, `mk/patches.mk` | `lib/helix-xml/` edited directly; the rest via `patches/` |
| `.claude/rules/filament-backends.md` | `src/printer/ams_*`, `include/ams_*`, `*filament_*` | Which doc to read before touching a filament backend |

---

## Above a Bugfix: Investigate, Then Scope

Features, refactors, new panels/widgets/managers: **scope AFTER investigating.** Map call sites, subjects and tests, read that subsystem's `docs/devel/` doc, find the canonical implementation of each piece, extend the near-fit helper (never fork a twin), and say what you searched and what you are reusing.

---

## Code Standards

| Rule | ❌ WRONG | ✅ CORRECT |
|------|----------|-----------|
| **spdlog only** | `printf()`, `cout`, `LV_LOG_*` | `spdlog::info("temp: {}", t)` |
| **SPDX headers** | 20-line GPL boilerplate | `// SPDX-License-Identifier: GPL-3.0-or-later` |
| **RAII widgets** | `lv_malloc()` / `lv_free()` | `lvgl_make_unique<T>()` + `release()` |
| **Class-based** | `ui_panel_*_init()` functions | Classes: `MotionPanel`, `WiFiManager` |
| **Observer factory** | Static callback + `lv_observer_get_user_data()` | `observe<int>()` from `core/observer_factory.h` |
| **Icon sync** | Add icon, forget fonts | Glyph in `include/ui_icon_codepoints.h` (or an `ICON_*` macro in `include/ui_fonts.h`), then `make regen-fonts`; `validate_icon_fonts.sh` fails a missing one |
| **Formatting** | Manual formatting | Let pre-commit hook (clang-format) fix |
| **Doc citations** | A line number (`src/printer/printer_state.cpp:638`) | `` `src/printer/printer_state.cpp#update_from_status` ``: path + `#` scopes (`scripts/doc_anchors.py`); fix by hand only on a rename |
| **No auto-mock** | `if(!start()) return Mock()` | Check `RuntimeConfig::should_mock_*()` |
| **JSON include** | `#include <nlohmann/json.hpp>` | `#include "hv/json.hpp"`; `json_fwd.h` in a header naming `json` only by reference, pointer or return. Escape hatch `// JSON_HPP_OK: <reason>` |
| **Build system** | `cmake`, `ninja` | `make` (pure Makefile) |
| **No RTTI** | `dynamic_cast`, `typeid`, `std::type_index`, `any.type()` | `helix::type_tag<T>()` keys, virtual kind queries, pointer-form `any_cast`. Escape hatch `// RTTI_OK: <reason>` |
| **No exceptions (firmware)** | `try`/`catch`/`throw`, `j.value("k", d)`, `j.at("k")`, one-arg `json::parse`, `std::stoi`, value-form `any_cast` | `json_util::safe_*`/`as_*`, `json::parse(s, nullptr, false)`, `text_io::parse_leading`, `any_cast<T>(&a)`; `exception_policy.h`. ESP32 builds `-fno-exceptions`: a throw is an abort |
| **Constant names** | `constexpr int MAX_RETRY_COUNT = 3;` | `constexpr int kMaxRetryCount = 3;` (UPPER_SNAKE is for macros). Escape hatch `// CONST_NAME_OK: <reason>` |
| **No std::regex** | `std::regex`, `#include <regex>` | `helix::Regex` (`include/helix_regex.h`): libstdc++'s matcher overflows musl's thread stacks. Escape hatch `// STD_REGEX_OK: <reason>` |
| **Bug commits** | Filing an issue just so the commit can cite one | Cite one that exists: `fix(scope): thing (prestonbrown/helixscreen#123)`; otherwise `fix(scope): thing` |
| **Unproven tests** | "Tests pass" as evidence | A hand break while writing the test, then `make mutate-diff` when the series is done. `tests/CLAUDE.md` § "Proving a test can fail" |
| **Commit body length** | 3-paragraph Tests / Verification / Mutation essay | Subject + ~4-line paragraph |
| **Comment archaeology** | `// unlike the three widgets 3d0875bff fixed` | Present-tense constraint (global CLAUDE.md); `scripts/check_comment_archaeology.py`, escape hatch `ARCHAEOLOGY_OK: <reason>` |
| **Submodule mods** | Edit `lib/lvgl/...` / `lib/libhv/...` directly | `patches/*.patch`; `lib/helix-xml/` is ours and edited directly |

**ALWAYS:** Search the SAME FILE you're editing for similar patterns before implementing.

---

## Patterns

| Pattern | Key Point | Exemplar |
|---------|-----------|----------|
| Subject init order | Register components → init subjects → create XML | `src/application/subject_initializer.cpp` |
| Widget lookup | `lv_obj_find_by_name()` not `lv_obj_get_child()` | any panel |
| Overlays | `NavigationManager::instance().push_overlay(root)` / `.go_back()`; pair every push with `register_overlay_instance(root, this)` or `on_deactivate()` never fires | `src/ui/settings/ui_settings_safety.cpp` |
| Modals (simple) | `helix::ui::ComponentModal::show_owned("component_name")` / `Modal::hide(dialog)`; the bare `Modal::show("name")` factory is lint-refused | `src/ui/settings/ui_settings_fans.cpp` |
| Modals (subclass) | Extend `Modal`, implement `get_name()` + `component_name()`, override `on_ok()`/`on_cancel()` | `include/ui_info_qr_modal.h` + `src/ui/components/ui_info_qr_modal.cpp` |
| Confirmation dialog | `helix::ui::modal_confirm(title, msg, severity, btn_text, on_confirm, ConfirmOptions)`. **Pass `on_dismiss` whenever the caller holds a guard/flag/pending entry the buttons were meant to clear**: backdrop tap, ESC and hot-reload dismiss without a button. `owner_token` gates all three callbacks | `src/ui/printers/ui_change_host_modal.cpp#show_connection_failed_modal` |
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
| `src/ui/` | The UI framework at the top level; everything else in ONE level of group dirs (`settings/`, `print/`, `home/`, `components/`, `panel_widgets/`, ...). The Makefile globs no deeper |
| `src/printer/` | PrinterState, macro/filament managers |
| `src/api/` | The Moonraker client and API facade |
| `src/network/`, `src/platform/` | WiFi, Ethernet, mDNS; display, input, usb |
| `src/mock/` | The mock Moonraker behind `--test` |
| `src/core/` | Update queue, observer factory, static registries, text and HTTP plumbing |
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

Navigating, clicking, reading values and screenshots are yours; ask him only for what a human eye must judge, after driving to the state.

- **Local mock: always allowed.** The pinned recipe above with `--test`, `SDL_VIDEODRIVER=dummy`.
- **Real printer: ask once per session.** After that, read-only commands (`ping`, `status`, `current`, `ls`, `text`, `state`, `geom`, `screenshot`) need no asking. Anything that moves the machine or changes a print (gcode, home, heat, move, print/cancel, `FIRMWARE_RESTART`, e-stop) is confirmed **every** time. `ctl current` before you navigate, and navigate back when done.
- Desktop UI against a real printer: `--moonraker ws://HOST:7125` (the flag; there is no `--moonraker-url`). The app on the device: ssh, then `<install>/bin/helix-screen ctl`.
- **Installed releases have no ctl server** (`HELIX_PACKAGING=1` forces it off). Check the binary: `strings -a <bin> | grep -c list_callbacks` (0 = none). The server also needs `HELIX_REMOTE_CONTROL=1` in `helixscreen.env`. HELIXCTL.md § "Enabling the server".

---

## Critical Paths (always MAJOR work)

PrinterState, WebSocket/threading, shutdown, DisplayManager, XML processing

---

## Superpowers Skills Here

Worktree create/remove steps: the `helix-ops` skill § Worktrees.

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
