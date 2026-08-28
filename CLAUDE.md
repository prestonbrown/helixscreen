# CLAUDE.md

## Quick Start

**HelixScreen**: LVGL 9.5 touchscreen UI for Klipper 3D printers. XML engine in `lib/helix-xml/` — our own MIT fork of the engine LVGL removed in 9.5, and its own repo ([prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml)), so a fresh clone needs `git submodule update --init --recursive`. Pattern: XML → Subjects → C++.

**Before compiling:** Check for existing build processes (`pgrep -x -d' ' 'make|cc1plus'`, or `ps -eo pid,args | grep '[m]ake -j'`) — concurrent compilations thrash the machine. Never `pgrep -f` here: it matches the checking command's own line, so it always reports a false hit, and in a wait loop it never exits. And `pgrep` takes ONE pattern: `pgrep -x make cc1plus` errors with "only one pattern can be provided", so with stderr suppressed it prints nothing and reads as an all-clear. Use the alternation form above, or one `pgrep -x` per name. **Check `free -h`'s Mem AND Swap rows together:** the `helix-tests` link is gated by memory headroom, not cores, and neither row decides on its own. An exhausted swap row is NOT a throttle signal while `Mem:` `available` is still tens of GB: a full `make -j6` built clean at 80Gi available with 14Mi free swap. The failure case is both tight at once, low `available` *and* swap near 0, where `-j8` dies mid-link with *no* `oom-kill` line while load average still looks healthy. `-j6` clears it. Dying at the *same* step twice **can** be a resource ceiling, but rule out a peer first: a second `make` in the SAME tree deletes your freshly linked binary, because `prune-orphan-test-objs` (`mk/tests.mk`) runs `rm -f $(TEST_BIN)` whenever it finds one orphan object, and it is a *sibling* prerequisite of the link, so `-j` gives them no order. The tell is in the log: `[LD] helix-tests` followed by `✓ Unit test binary ready` and NO `✗ Test linking failed!` means the linker exited 0 and something else removed the output. Every shard then reports `No such file or directory` and the suite reads RED with nothing wrong in your code. Memory is not the cause there - a starved link fails loudly and stops make.

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
scripts/setup-worktree.sh feature/my-branch  # Symlinks deps, builds fast
```

**XML changes need no rebuild:** `ui_xml/*.xml` is loaded at runtime — edit XML, then **relaunch** the binary to see changes (no `make` needed). Better: hot reload is **on by default for native dev builds** (cross-compiled release builds default it off) — the running app re-registers components within ~500ms of a save and rebuilds the active panel/overlay/modal in place. `HELIX_HOT_RELOAD=1`/`0` overrides the default either way. Invalid XML (mid-write truncation, syntax errors) is silently skipped on the polling thread — the existing UI stays live and the next poll retries.

**Screenshots:** Press 'S' in UI, or `./scripts/screenshot.sh helix-screen output-name [token]` (drives a fresh instance via `helix-screen ctl`; token = panel/overlay/`demo` screen from `scripts/screenshot-recipes.sh`).

**Driving the UI (screenshots, debugging, bringing up any panel/overlay/modal):** `helix-screen ctl` remote-controls a running instance — `navigate`/`click`/`ls`/`text`/`geom`/`set_value`/`scroll`/`demo`/`screenshot`, or a `helix-screen repl` REPL. The server auto-starts in `--test` (or `--remote`). See `docs/devel/HELIXCTL.md`. (Replaces the removed `-p`/`--panel` flags.)

> **Always pin the socket — never run a bare `ctl`.** The default path is per-user and
> fixed, so with two instances up, `ctl` silently drives **whichever started first** and
> still reports success. That is how you "verify" a change against another session's app.
> Derive both the socket and the config dir from the worktree so parallel agents can't
> collide — and you need *both*, since `--remote-socket` alone still contends on the
> config flock:
> ```bash
> TREE=$(basename "$(git rev-parse --show-toplevel)")
> export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
> mkdir -p "$HELIX_CONFIG_DIR"   # must exist — the app does not create it, it aborts
> ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
> ./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate settings
> ```
> **An empty `HELIX_CONFIG_DIR` is isolated for the lock and socket, not for the printer
> address.** Finding no `settings.json` there, `Config` bootstraps one from
> `~/.helixscreen/settings.json.backup` — look for `[Config] Config missing — restoring
> from backup:` in the log. That inherits the real `moonraker_host`, so a run **without**
> `--test` opens a WebSocket to the actual printer. Keep `--test` (the mock client ignores
> the host entirely), or name the target explicitly with `--moonraker ws://HOST:7125`, which
> **does** take precedence over the saved `moonraker_host` (the flag is `--moonraker`, not
> `--moonraker-url`; verified 2026-08-27 — a seeded config defaulting to port 7125 connected
> to `ws://127.0.0.1:7199/websocket` when the flag named it).
>
> Prefer `ctl text <name>` / `ctl geom <name>` over reading a screenshot — they are exact,
> and a screenshot only proves what a scroll position happened to expose.

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
| **Doc citations** | Hand-writing the markdown link, or hand-fixing a `:123` line number after moving code | Write the plain backticked citation (`src/printer/printer_state.cpp:625`), then `make regen-doc-links`. Both halves are derived: `scripts/doc_cite_anchors.py` re-pins the line number from a committed content hash of the cited line (so moved code self-heals across every scanned doc, not just the guide), then `scripts/gen_doc_links.py` derives the link URL in `docs/devel/architecture/` from the citation text. `quality-checks.sh` fails a doc that is out of date with either, and the pre-commit hook repairs it in place — re-stage and commit. The one thing you must fix by hand: a cited line whose **own text changed**, which is a hard error because the sentence may no longer be true. |
| **No auto-mock** | `if(!start()) return Mock()` | Check `RuntimeConfig::should_mock_*()` |
| **JSON include** | `#include <nlohmann/json.hpp>` | `#include "hv/json.hpp"` (libhv's bundled version) |
| **Build system** | `cmake`, `ninja` | `make -j` (pure Makefile) |
| **No RTTI** | `dynamic_cast`, `typeid`, `std::type_index`, `any.type()` | `helix::type_tag<T>()` keys, virtual kind queries (`HELIX_CONTEXT_MENU_KIND`), pointer-form `any_cast`. Firmware builds `-fno-rtti`; lint-gated, escape hatch `// RTTI_OK: <reason>` |
| **Bug commits** | Filing an issue just so the commit can cite one | Cite the issue when one already exists: `fix(scope): thing (prestonbrown/helixscreen#123)`. No issue? `fix(scope): thing` is complete on its own — the commit body carries the explanation. |
| **Commit body length** | 3-paragraph Tests / Verification / Mutation essay | Subject + ~4-line paragraph (cf. `feat(z-offset)` 25e1505e7). Reserve the long form for genuine state-machine fixes that touch multiple subsystems (cf. `fix(ams): DRY unload API` 504905a2). |
| **Submodule mods** | Edit `lib/lvgl/...` / `lib/libhv/...` directly | Add/amend `patches/*.patch` — `mk/patches.mk` auto-applies. **Exception: `lib/helix-xml/` is our own submodule** ([prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml)) — edit it directly, commit and push *in the submodule*, then commit the bumped pointer in this repo. Never write a patch for it. A worktree gets its own checkout of it (not a symlink), so engine edits stay in that branch. |

**ALWAYS:** Search the SAME FILE you're editing for similar patterns before implementing.

**Submodule patch workflow** — third-party submodules ONLY (`lib/lvgl/`, `lib/libhv/`, …). **Never run it on `lib/helix-xml/`**: that repo is ours, its edits are meant to be committed, and the `git restore .` below would destroy them.

Edit the file under `lib/<sub>/`, then `cd lib/<sub> && git diff -- <the files you touched> > ../../patches/<name>.patch && git restore -- <those files>`. **Scope the diff** — a bare `git diff` captures every patch currently applied. And if two patches touch the same file (a dozen-plus files are; `src/misc/lv_event.c` has seven) even a scoped diff folds the others in, so use the pristine-file method in `patches/README.md` § "Regenerating a patch whose file is shared". The patch in `patches/` is the source of truth — direct edits get wiped on the next `git submodule update`. Check `mk/patches.mk` (`LVGL_PATCHED_FILES`, `LIBHV_PATCHED_FILES`, etc.) and existing `patches/*.patch` before creating a new one — amend an existing patch when the change is in the same area (e.g., `lvgl_sdl_window.patch` already owns `lv_sdl_window.c`).

---

## CRITICAL RULES - Declarative UI

**DATA in C++, APPEARANCE in XML, Subjects connect them.**

**Absolute for new code.** The tree still has 379 sites that break these rules
(`scripts/check_imperative_ui.py --list`). Some were deliberate pragmatism from when the XML
engine could not express what was needed; some are plain mistakes that got through review.
Both are debt, tracked in prestonbrown/helixscreen#1140 and being ported. **Existing imperative
code is not precedent** — do not imitate a nearby site just because it is there, and do not
opportunistically refactor one as a side effect of an unrelated change. The gate ratchets: the
count may fall, never rise.

| # | Rule | ❌ NEVER | ✅ ALWAYS |
|---|------|----------|----------|
| 1 | **NO lv_obj_add_event_cb()** | `lv_obj_add_event_cb(btn, cb)` | XML `<event_cb trigger="clicked" callback="name"/>` + `lv_xml_register_event_cb()` |
| 2 | **NO imperative visibility** | `lv_obj_add_flag(obj, HIDDEN)` | XML `<bind_flag_if_eq subject="state" flag="hidden" ref_value="0"/>` for cheap show/hide of an already-built subtree. `<if cond="X">…<else/>…</if>` is the structural sibling — use it when the *creation* itself is expensive (a whole card, an alternate layout); it builds only the matching branch instead of both. See `docs/devel/LVGL9_XML_GUIDE.md` § "Structural conditionals with `<if>` / `<else>`". |
| 3 | **NO lv_label_set_text** | `lv_label_set_text(lbl, val)` | Subject binding: `<text_body bind_text="my_subject"/>` |
| 4 | **NO C++ styling** | `lv_obj_set_style_bg_color()` | XML: `style_bg_color="#card_bg"` |
| 5 | **NO manual LVGL cleanup** | `lv_display_delete()`, `lv_group_delete()` | Just `lv_deinit()` - handles everything |
| 6 | **bind_style priority** | `style_bg_color` + `bind_style` | Inline attrs override - use TWO bind_styles |
| 7 | **NO C++ derived subject for compound conditions** | Hand-written observer that combines 2+ subjects (`a \|\| b > c`) | XML `<subject_expr name="x" expr="a or b gt c"/>` or inline `cond="a or b gt c"` on `bind_flag_if`/`bind_state_if`/`bind_style_if` (word forms — `&&`/`<` need XML escaping). |
| 8 | **NO C++ create-and-wire loop for repeated fragments** | `for(int i=0;i<n;i++) { lv_obj_create(...); ... }` in C++ | XML `<repeat count="4">…$i…</repeat>` (fixed) or `<repeat count="a_subject">…${i}…</repeat>` (reactive rebuild on subject change) — see `docs/devel/LVGL9_XML_GUIDE.md` § "Repeating fragments with `<repeat>`". Measured layout, computed callbacks, and data population still belong in C++ — `<repeat>` only replaces the widget-creation loop itself. |

**Structural exceptions — C++ is correct here, permanently:**

| Case | Why |
|------|-----|
| Custom XML widget implementations — the 29 files calling `lv_xml_register_widget` | The file *is* the widget; there is no XML beneath it to bind to |
| `LV_EVENT_DELETE` cleanup, draw hooks (`DRAW_MAIN`/`DRAW_POST`), `SIZE_CHANGED`, gestures/scroll | No declarative equivalent exists |
| Measured layout and computed fonts (`decide_nozzle_layout()`, breakpoint fonts) | Depends on runtime pixel measurement — see rule 8 |
| Widgets created in C++ (`lv_*_create`) — canvas and procedural rendering | Never had an XML layer |
| Per-item payload on generated collections | `lv_obj_set_user_data()` on a `ui_button` overwrites `UiButtonData*` (`temperature_service.cpp:671`) |
| `helix-screen ctl` remote control (`remote_control_server.cpp`) | Its job is reaching into an arbitrary live widget tree on command |
| CLI stdout (`cli_args.cpp`, `detect_printer_cmd.cpp`, `helix_splash.cpp`) | stdout *is* the product there; spdlog is for logging |
| Widget pool recycling, chart data, animations | Churn or per-frame data that a subject would not model |

Genuinely un-declarative site? Annotate it: `// DECLARATIVE_OK: <reason>`.

---

## CRITICAL RULES - Vendor Knowledge Stays Behind an Abstraction

**A vendor, firmware, or mod name may appear in ONE module per capability. Generic code asks
that module a capability question and never names the vendor.**

Generic code is anything whose job is not "support vendor X": `PrinterState` and its
sub-states, the discovery/subscription builder, `Application` startup, panels, widgets,
formatters. When one of those grows an `if (zmod) … else if (creality) …`, the vendor matrix
is now spread across every layer and the next firmware means editing all of them.

| ❌ WRONG | ✅ CORRECT |
|----------|-----------|
| `zmod::parse_persisted_z_offset(status)` in `PrinterMotionState::update_from_status` | `zoffset::read_persisted_offset_microns(status)` — the module owns which firmwares and which schema |
| `if (hw.has_macro("SAVE_ZMOD_DATA")) subs["save_variables"] = nullptr;` in the subscription builder | `for (auto& o : zoffset::required_status_objects(hw)) subs[o] = nullptr;` |
| `api->execute_gcode("SAVE_ZMOD_DATA LOAD_ZOFFSET=1")` in `Application` | `zoffset::persistence_enable_gcode(hw)` — empty string when the printer needs none |

**The test: adding a second firmware with the same capability must touch exactly one file.**
If it would touch the status parser *and* the subscription builder *and* startup, the
abstraction is missing. Model it as a provider table keyed on a detection predicate, with the
capability questions as free functions over it — `include/z_offset_persistence.h` +
`src/printer/z_offset_persistence.cpp` is the reference shape (~40 lines of table, three
questions: what to subscribe, how to read it, how to enable it).

**Naming follows the same rule.** Subjects, accessors, and headers name the *capability*
(`persisted_z_offset`, `firmware_persists_z_offset`), never the vendor (`zmod_z_offset`). A
vendor-named symbol reachable from generic code is the smell even when the call site looks
clean.

Existing vendor-dispatch that already lives behind an interface is the pattern working, not an
exception: `AmsBackend*` (one class per filament system, `AmsState` never names one),
`ZOffsetCalibrationStrategy`, `PrinterDetector` capability lookups. Follow those.

Genuinely unavoidable vendor branch in generic code? Annotate it: `// VENDOR_OK: <reason>`.

---

## Design Tokens (MANDATORY)

| Category | ❌ WRONG | ✅ CORRECT |
|----------|----------|-----------|
| **Colors** | `lv_color_hex(0xE0E0E0)` | `theme_manager_get_color("card_bg")` |
| **Spacing** | `style_pad_all="12"` | `style_pad_all="#space_md"` |
| **Typography** | `<lv_label style_text_font="...">` | `<text_heading>`, `<text_body>`, `<text_small>` |

Note: `theme_manager_get_color()` for tokens, `theme_manager_parse_hex_color()` for hex strings only (NOT tokens).

---

## Threading & Lifecycle

> **Full rules: `docs/devel/THREADING.md`** (routed by the `helix-threading` skill). Read it
> before writing code that crosses a thread boundary, observes a subject, or destroys a widget.
> The invariants below are the always-loaded safety net — each one fails silently at compile
> time and crashes later, usually on a customer's printer.

1. **Never touch LVGL from a background thread.** WebSocket/libhv, HTTP, and timer callbacks
   are background threads; `lv_subject_set_*()` counts, because it fires observers that call
   widget APIs. Route through `ui_queue_update()` (`ui_update_queue.h`). Pattern:
   `printer_state.cpp` `set_*_internal()`.
2. **Never write bare `if (tok.expired()) return;` on a background thread** and then touch
   `this` — TOCTOU use-after-free (L081 Mechanism C, #707). Use `lifetime_.bg_cb(tag, fn)`, or
   `tok.defer(tag, fn)` when you have bg-side parsing worth keeping off the main thread.
   `lifetime_.defer()` is main-thread only. Gate: `scripts/check_l081_anti_pattern.py`.
3. **Never delete synchronously inside a queued callback.** `safe_delete()`,
   `lv_obj_delete()`, and `lv_obj_clean()` corrupt LVGL's event list mid-batch (#776, #190,
   #80). Use `safe_delete_deferred()`, `lv_obj_delete_async()`, `safe_clean_children()`.
   **`lifetime_.defer` does NOT escape the batch** — it fires in the next `process_pending`
   tick, which is still a batch.
4. **If you fetch a `SubjectLifetime`, you must hand it to `observe_*`.** The factories take it
   as a defaulted 4th parameter (`const SubjectLifetime& lifetime = {}`), so omitting it is
   silent: the guard gets no token, never sees the subject die, and `reset()` calls
   `lv_observer_remove()` on freed memory (#705). Local vs member is *not* what decides
   correctness — the `get_*_subject(name, lifetime)` accessors assign the owner's own
   `shared_ptr`, so a caller's copy dying never expires the guard.
5. **A raw `lv_timer_t*` cancelled in `cleanup()` must also be cancelled in the destructor.**
   `StaticPanelRegistry::destroy_all()` runs *before* `lv_deinit()`, so any teardown that
   skips the explicit stop leaves the timer armed on a freed `this` (#1173, twice). Share one
   `cancel_*_timer()` between both paths and use `lv_timer_cancel_safe()` — it self-guards on
   `lv_is_initialized()` and neuters instead of unlinking, so it is safe from a destructor and
   from inside `lv_timer_handler` (#750, #751). A `LifetimeToken`-guarded callback is the other
   valid answer; annotate those `// TIMER_DTOR_OK: <reason>`. Gate:
   `scripts/check_timer_destructor_cancel.py`.

Also: no `std::thread(...).detach()` for one-shot work — `EAGAIN` → `std::terminate` on
AD5M/CC1 (#724, #837); use `HttpExecutor::fast()/slow()` or `BusThread`. `ObserverGuard::reset()`
for all normal cleanup, never `release()` (#579). Every `init_subjects()` self-registers its
`deinit_subjects()` with `StaticSubjectRegistry`.

---

## Patterns

| Pattern | Key Point | Exemplar |
|---------|-----------|----------|
| Subject init order | Register components → init subjects → create XML | `src/application/subject_initializer.cpp`, called from `Application::register_xml_components()` |
| Widget lookup | `lv_obj_find_by_name()` not `lv_obj_get_child()` — indices break when layout changes | any panel; 1100+ call sites |
| Overlays | `NavigationManager::instance().push_overlay(root)` / `.go_back()` (`ui_nav_manager.h`) — pair every push with `register_overlay_instance(root, this)` or `on_deactivate()` never fires (tests abort; `HELIX_STRICT_OVERLAY_CHECK=1`) | `src/ui/ui_settings_safety.cpp` |
| Modals (simple) | `Modal::show("component_name")` / `Modal::hide(dialog)` | `src/ui/ui_job_queue_modal.cpp` |
| Modals (subclass) | Extend `Modal`, implement `get_name()` + `component_name()`, override `on_ok()`/`on_cancel()` | `include/ui_info_qr_modal.h` + `src/ui/ui_info_qr_modal.cpp` (42 + 62 lines — the whole pattern, nothing else) |
| Confirmation dialog | `modal_show_confirmation(title, msg, severity, btn_text, on_confirm, on_cancel, data)` (in `helix::ui`) | `src/ui/ui_panel_macros.cpp:370` |
| Modal buttons (XML) | `<modal_button_row primary_text="Save" primary_callback="on_save"/>` | `ui_xml/bed_mesh_rename_modal.xml` |
| Home-panel widget | Subclass `PanelWidget`; `attach()` + `on_size_changed()`. Instances are **recycled** across rebuilds, so any imperative apply must run from `attach()` too, not only on size change | `src/ui/panel_widgets/motion_widget.cpp` (64 lines) |
| Background → UI | Never touch LVGL off the main thread; `ui_queue_update()` or `tok.defer()` | `src/printer/printer_state.cpp` `set_*_internal()` |

---

## Where Things Live

**Singletons** (classic `::instance()` unless noted):
`SettingsManager` (persistent settings), `NavigationManager` (panel/overlay stack), `UpdateQueue` (thread-safe UI updates), `SoundManager`, `DisplayManager`, `ModalStack`, `ToolState` (multi-tool tracking), `AmsState` (multi-backend filament systems). Two look like singletons but are not: `PrinterState` (all printer data/subjects) is a Meyers singleton reached via `get_printer_state()` (`app_globals.h:149`) — there is no `PrinterState::instance()`; `PrinterDetector` (printer DB + capabilities) is a static class, no instance exists. Full census (76 `::instance()` singletons plus four other access shapes): `docs/devel/architecture/05-printer-state.md`.

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

**Test isolation**: `HelixTestFixture` (`tests/helix_test_fixture.h`) is the base for every test fixture. Ctor + dtor call `reset_all()` which drains `UpdateQueue`, resets `SystemSettingsManager` language, clears `ModalStack`. `LVGLTestFixture` inherits it. `XMLTestFixture` owns per-instance `PrinterState` / `MoonrakerClient` / `MoonrakerAPI` (no more static test state). XML subjects still register into LVGL's global scope — per-test scopes were blocked by LVGL internals; subjects are refreshed by each test's `init_subjects(true)`.

---

## Debugging

**NEVER debug without flags!** Use `-vv` minimum.
Trust debug output. Impossible values = bug is UPSTREAM. Ask "what ELSE?" not "did first fix work?"

**A plain `> file` redirect drops the console log — but `--test` is exempt.** The console
sink attaches for a TTY always, for a PIPE only with an explicit `-v`/`--log-level`, and for a
regular file or socket never (a plain redirect is indistinguishable from the daemon redirect).
`--test` overrides all of it and logs to any stdout kind, which is why most runs never hit
this. A **non-`--test`** background run needs `2>&1 | tee /tmp/x.log` or
`HELIX_LOG_DEST=console` — measured on one binary, identical flags: 645 lines through `tee`,
3 through `>`. Full decision table: `docs/devel/LOGGING.md` § "Console sink".

**Debug bundles**: `--save` writes `debug-bundle-<code>.json` to the **current working directory** — run it from `/tmp` so the bundle never lands in the repo: `cd /tmp && <repo>/scripts/debug-bundle.sh <SHARE_CODE> --save`. Investigate there, never commit a bundle. (If one ends up in the repo, move it to `/tmp`.)

### Drive the UI yourself — `ctl` is not a question for Preston

Anything observable or drivable is yours to do: reaching a panel, clicking a widget, reading a
value, capturing a screenshot. Ask him only for the judgment a human eye has to make ("does
this look right"), and even then drive to the state first and tell him exactly what to look
at. `ctl text` / `state` / `geom` are exact; a screenshot only proves what a scroll position
happened to expose.

**Local mock — always allowed, no permission needed.** Use the pinned-socket recipe in Quick
Start with `--test`, `SDL_VIDEODRIVER=dummy`, and `--sim-speed 4-10` when you need an active
print. This is the default answer to "does my change work".

**Real printer — ask once per session, then keep going.** Get Preston's OK before the first
command that touches a real machine. After that, read-only commands (`ping`, `status`,
`current`, `ls`, `text`, `state`, `geom`, `screenshot`) need no further asking. Anything that
moves the machine or changes a print — gcode, home, heat, move, print/cancel,
`FIRMWARE_RESTART`, emergency stop — is confirmed **every** time; one unconfirmed call cost a
26-minute print. `ctl current` before you navigate and navigate back when you are done: that
is his printer's screen, and he is standing in front of it.

Two shapes, and they are not the same thing:

| Shape | What it is | How |
|-------|-----------|-----|
| desktop UI → real Moonraker | your local build, real printer data, no device walk | `--moonraker ws://HOST:7125` |

Both shapes are verified. Pointing a local `SDL_VIDEODRIVER=dummy` build at the CB1/Voron
discovered its real hardware (`AFC_BoxTurtle Turtle_1`, 4 lanes, 10 AFC objects,
quad_gantry_level) and `ctl` drove the resulting UI, so an AFC question does not need a
device deploy - only the printer's Moonraker port on the LAN.
| `ctl` → app on the device | the app actually running on the printer | ssh, then `<install>/bin/helix-screen ctl <cmd>` |

**The device build gate — check the binary, never the help text.** `ENABLE_REMOTE_CONTROL`
defaults to `yes`, but `HELIX_PACKAGING=1` forces it to `no`, so **an installed release has no
ctl server**. The `--remote*` flags still appear in `--help` on those builds, so they look
supported and buy nothing:

```bash
strings -a <install>/bin/helix-screen | grep -c list_callbacks   # 0 = no server compiled in
```

Verified 2026-08-27: CB1/Voron running packaged 0.99.116 → `0`, and `ctl` was rejected as an
unknown argument. K2 Plus running dev cross-build 0.99.117 → `2`, with `ctl ping` answering
`pong` over `/tmp/helixscreen-control.sock`, plus `navigate`/`ls`/`state`/`geom`/`screenshot`
all working against the live machine. A device also needs `HELIX_REMOTE_CONTROL=1` in its
`helixscreen.env` for the server to listen; `make deploy-*` sets that from the
`.build-features` stamp `mk/rules.mk` writes beside the binary.

---

## Critical Paths (always MAJOR work)

PrinterState, WebSocket/threading, shutdown, DisplayManager, XML processing

---

## Autonomous Sessions

Given autonomous control ("work independently", "minimal interruption"), load the
`autonomous-session` skill — scratchpad workspace, autonomy guidelines, and what still
requires asking first.
