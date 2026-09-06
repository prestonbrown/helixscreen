# scripts/

Build, asset generation, deployment, and developer tooling for HelixScreen. This index says
what each script is for; the script's own header carries the reasoning behind it.

## Categories

### Installation & Deployment
| Script | Purpose |
|--------|---------|
| `install.sh` | **Auto-generated** single-file installer for end users (`curl\|sh`). Do NOT edit directly |
| `install-dev.sh` | Modular dev installer - uses `lib/installer/` modules. Edit this one |
| `uninstall.sh` | **Auto-generated** single-file uninstaller. Do NOT edit directly |
| `bundle-installer.sh` | Bundles `install-dev.sh` + `lib/installer/*` → `install.sh` |
| `bundle-uninstaller.sh` | Bundles uninstall modules → `uninstall.sh` |
| `helix-launcher.sh` | Systemd-launched watchdog wrapper. Sources `helixscreen.env` for runtime config |
| `check-deps.sh` | Validates build dependencies. `--minimal` for cross-compile environments |
| `device-env-set.sh` | Idempotently set one `KEY=VALUE` in a deployed device's `helixscreen.env` over ssh. Deploys exclude that file, so this is the only thing that writes it; `sync-device-features` (`mk/cross.mk`) calls it per `bin/.build-features` |
| `device-env-set-remote.sh` | The on-device half, piped to `sh -s` (POSIX sh: the K1/AD5M/CC1/K2 are BusyBox ash). Its own file so `tests/shell/test_device_env_set.bats` can run it without a printer |

### Release & Packaging
| Script | Purpose |
|--------|---------|
| `dev-release.sh` | Local dev release workflow (build + package + upload) |
| `generate-manifest.sh` | Generates `manifest.json` from release archives. Used by CI and dev-release |
| `android-version-code.sh` | **The single definition of the Android versionCode** - `major*1000000 + minor*1000 + patch` from `VERSION.txt`, plus the lane-overflow guard. `android/app/build.gradle`, `generate-whatsnew.sh` and `.github/workflows/release.yml` all call it; `tests/shell/test_android_version_code.bats` fails on a second copy of the arithmetic |
| `generate-whatsnew.sh` | Extracts the current version's `CHANGELOG.md` section into the Play Store "What's new" file under `android/fastlane/` (≤500 chars, cut on a sentence boundary) |
| `generate-upload-keystore.sh` | One-time generation of the Android upload keystore. Output belongs in `~/.android-keystore/`, never in the tree |

### Asset Generation (`make regen-*`)
| Script | Purpose |
|--------|---------|
| `regen_mdi_fonts.sh` | Regenerate MDI icon fonts from codepoints. Run after adding icons |
| `regen_text_fonts.sh` | Regenerate Noto Sans text fonts for LVGL |
| `regen_images.sh` | Pre-render splash screen images to LVGL binary format |
| `regen_placeholder_images.sh` | Generate placeholder/fallback images |
| `regen_printer_images.sh` | Process printer model images for the printer database |
| `trim_printer_images.sh` | Crop/trim whitespace from printer images |
| `gen_splash_3d.py` | Composite 3D logo onto full-screen splash canvases |
| `generate_gradient_bg.py` | Pre-render gradient backgrounds for print file cards (perf optimization) |
| `LVGLImage.py` | Python library for LVGL binary image format conversion |
| `download_printer_images_headless.py` | Scrape printer images from web sources |

### Icon Pipeline
| Script | Purpose |
|--------|---------|
| `gen_icon_consts.py` | `include/ui_icon_codepoints.h` → `ui_xml/globals.xml` icon string constants |
| `generate_icon_header.py` | Embed PNG icon data into C headers |
| `validate_icon_fonts.sh` | Bidirectional check: codepoints ↔ fonts ↔ XML usage |

### Translation / i18n
| Script | Purpose |
|--------|---------|
| `generate_translations.py` | Main translation generator - YAML → runtime XML packs (`--emit-lv-i18n` for the legacy unlinked C table). A key holding a real newline or tab survives the XML parse only as a `&#10;`/`&#9;` character reference; `escape_xml_attr()` says why |
| `translation_sync.py` | Sync keys (XML/C++ → YAML); also `coverage`, `obsolete`, `glossary` subcommands |
| `translations/yaml_manager.py` | Reads/edits the locale YAMLs by **line splice**, never a re-dump (no dump config reproduces the committed files byte-for-byte). `load_yaml_file()` refuses a file that defines a key twice: a later placeholder would otherwise silently replace the translation. Gated by `tests/shell/test_translation_duplicate_keys.bats` |
| `migrate_xml_translations.py` | Migration tool: inline XML text → translation key references |
| `xml_to_yaml_translations.py` | Extract inline XML strings to YAML format |
| `translations/` | Python package: extractor, YAML manager, coverage, glossary, CLI |

**Terminology consistency:** `translations/GLOSSARY.md` (`make translation-glossary`) pins canonical per-language renderings of recurring terms. Reuse the glossary term when translating, human or agent, and `grep` the locale before coining a new word for an already-translated concept. After changing a canonical term, add its English key to `GLOSSARY_GROUPS` in `translations/glossary.py` and regenerate.

### Telemetry & Crash Analysis
| Script | Purpose |
|--------|---------|
| `telemetry-pull.sh` | Pull events from telemetry worker API. Needs `HELIX_TELEMETRY_ADMIN_KEY` |
| `telemetry-analyze.py` | Adoption, reliability, crash metrics. Output: terminal/JSON/HTML |
| `telemetry-printer-profiles.py` | Printer detection analysis: model distribution, name clustering, candidate heuristics, DB validation |
| `telemetry-update-printer-db.py` | Interactive updater: walks the operator through `printer_database.json` entries and presets from a profile analysis |
| `telemetry-crashes.py` | Resolve ASLR crash addresses → function names, group by signature. `--anomalies` surfaces non-fatal `error_encountered` events. Resolution uses the bundle's `runtime_anchor=` field; older bundles fall back to raw hex |
| `telemetry-backfill.sh` | Backfill Analytics Engine from R2 (90-day retention limit) |
| `resolve-backtrace.sh` | Resolve raw backtrace addresses with the `.sym` files from R2. `--bundle <debug-bundle.json>` (warns when its `crash_report` is not the `crash_txt` crash), `--crash-file`, `--issue <N>` (both worker output shapes; frames only, never register tables). Prints the app-code call spine and a crash-context analysis: crashing thread, `LV_EVENT_*` decode, a warning when the LVGL header fields belong to the main thread rather than the crashing one |
| `debug-bundle.sh` | Fetch and display debug bundles from `crash.helixscreen.org` |
| `freeze-drops.sh` | Aggregate UpdateQueue `DROPPED` events across device logs, debug bundles or files (`--ssh <host>`, `--bundle <file>`), cross-referencing each tag to its source and whether it already uses `defer_critical` |

#### Printer Hardware Profile Pipeline

Composable pipeline for improving `printer_database.json` from real-world telemetry:

```bash
# 1. Pull raw telemetry events to .telemetry-data/events/
./scripts/telemetry-pull.sh --since 2026-01-01

# 2. Analyze: aggregate by model, find discriminators, cluster unknowns
./scripts/telemetry-printer-profiles.py --json --candidates > /tmp/analysis.json

# 3. Interactively update printer DB and generate presets
./scripts/telemetry-update-printer-db.py /tmp/analysis.json
```

**Data flow:** client `hardware_profile` events → Worker R2 → `telemetry-pull.sh` → `.telemetry-data/events/` → `telemetry-printer-profiles.py` (aggregate per detected model) → `telemetry-update-printer-db.py` (diff against `printer_database.json`, walk the operator through changes).

### Quality & Auditing

A gate's header explains what it prevents and why the obvious alternative is wrong. `check_<name>.py`
has its meta-test at `tests/shell/test_<name>_gate.bats`; a row cites the test only when it lives
elsewhere. Rows carry what a gate fails on and its opt-out marker.

| Script | Purpose |
|--------|---------|
| `quality-checks.sh` | Pre-commit and CI quality checks (single source of truth) |
| `check_doc_refs.py` | Doc-reference gate over every `CLAUDE.md`, `.claude/skills/`, `docs/README.md` and `docs/devel/**`: dead backticked paths, dead markdown links, docs/devel index completeness. Proves the FILE exists; whether a `#name` still resolves is `doc_anchors.py`'s question. `--devel [PATHS]`, `--stale` (report-only). |
| `doc_anchors.py` | Resolves a citation that names a place - `` `src/printer/printer_state.cpp#update_from_status` ``, quoted segments for names with spaces or overloads - to today's line, so no doc commits a line number. `--check` is advisory (`make check-doc-anchors`, from `.githooks/pre-push`) and also reports bare `:NNN` refs; `--render <dir>` backs `make docs-pinned`. Tests: `tests/python/test_doc_anchors.py` |
| `check_translation_format_specifiers.py` | Fail if a translated printf/`fmt::format` string (via `lv_tr()`) changes placeholder count/type vs its English source (#1073), or if a stored key/value holds an unresolved C escape (`\xNN`, `\n`, `\"`, ...). Gated on real format-sink usage, so prose `%`/`{}` never false-positive |
| `check_update_queue_leaks.py` | Ratchet on cross-test UpdateQueue leaks (#1166): `[ISOLATION-LEAK]` reports in a test log vs `scripts/update_queue_leak_baseline.txt`, keyed `tag:<producer>` when tagged and `test:<victim>` otherwise, plus a `max-untagged-callbacks:` ceiling. The log needs `2>&1`. Meta-tests: `tests/shell/test_update_queue_leak_gate.bats` |
| `check_asan_leaks.py` | Ratchet on LeakSanitizer at-exit leaks (#1279) vs `scripts/asan_leak_baseline.txt`: direct leaks only, keyed `<file>::<function>` normalized to be compiler-agnostic, plus byte/object ceilings. Refuses a truncated log or one holding a non-leak ASan error. Run from `make test-asan`. Meta-tests: `tests/shell/test_asan_leak_gate.bats` |
| `check_duplicate_xml_names.py` | Fail if one widget `name=` appears twice in a `ui_xml/` file (#1136; `lv_obj_find_by_name()` returns the first match silently) or literally inside a `<repeat>` body. Skips styles, `<if>`/`<else>` pairs, `$i`/`$param` names and trailing-`#` names. Per-name ratchet `DUPLICATE_NAME_BASELINE`; opt out `<!-- DUPLICATE_NAME_OK: reason -->` |
| `check_responsive_token_scope.py` | Fail if a responsive token (`<px>`/`<string>` named `*_micro`..`*_xxlarge`) is declared below the top level of `ui_xml/`, where discovery never reads it (#1211). |
| `check_shipped_spacing_tokens.py` | Fail if a token read by `theme_manager_get_spacing()` is declared only in a dev-only panel that `release-copy-xml-config` (`mk/cross.mk`) strips from shipped `ui_xml/`. Reads the dev-panel list from `mk/cross.mk`. |
| `check_printer_image_invalidation.py` | Fail if code outside `src/system/` calls `invalidate_printer_image_cache()` instead of the `_if_changed` form: a refresh is not an image change, and regenerating costs seconds on the main thread. |
| `check_icon_names.py` | Fail if an icon name written in `ui_xml/` is absent from `include/ui_icon_codepoints.h`. `<icon src="NAME">` resolves at runtime and substitutes `image_broken_variant` on a miss, so an unknown name renders as a deliberate glyph. Covers a direct `src=`, a component `<prop default=>` feeding the `$param`, and the literal handed to that param at an instantiation site; a bare `$param` with no literal is not checkable. `--summary` fails on an EMPTY corpus, since a gate whose pattern stops matching reports zero problems forever and reads exactly like a passing one. `--list` |
| `check_variant_parity.py` | Fail if a `ui_xml/<variant>/` override drops or invents wiring vs its base - widget names, subject bindings, event callbacks, `<api>` props (#1203). Reflow is not compared. Per-file exemptions in `ALLOWED_OMISSIONS`, each with a layout reason |
| `check_raw_this_queue_update.py` | Ratchet on `queue_update([this, …])` in `src/` (#1165): the lambda runs at the next drain whether or not the owner is alive (#1146). Catches `this`, `*this`, `[&]`, `[=]` and wrapped capture lists; silent on `lifetime_.bg_cb` / `tok.defer` / `guard_.defer`. Opt out `// QUEUE_RAW_THIS_OK: reason` |
| `check_hardcoded_pixels.py` | Ratchet on raw pixel literals where a design token belongs: `xml-pad`, `xml-size` (a literal matching a declared token), `xml-tall`, `cpp-pad`. Silent on 0/1, percentages, `#token` and `min_`/`max_` clamps; exempts the dev-only panels and the pre-theme crash screens. Opt out `<!-- SIZE_OK: reason -->` / `// SIZE_OK: reason`. |
| `check_thumbnail_cache_guard.py` | Hard gate: fail if `src/` calls `ThumbnailCache`'s legacy overloads (`fetch(api, path, …)`, `get_if_cached(path, mtime)`), which take no `ThumbnailLoadContext` and let a superseded download overwrite a newer thumbnail. Fails closed on an unprovable call. Opt out `// THUMB_LEGACY_OK: reason`. |
| `check_drm_mmap_lfs.py` | Fail if LVGL's DRM driver can mmap a dumb buffer through a 32-bit `off_t`: `patches/lvgl-drm-mmap64.patch` must exist, be applied by `mk/patches.mk`, and still add `#define _FILE_OFFSET_BITS 64` above the first `#include` plus the `sizeof(off_t)` assertion. A pristine submodule is reported, not failed. |
| `check_gcode_lfs.py` | Fail if the gcode reader loses its 64-bit `off_t`: the target-specific `-D_FILE_OFFSET_BITS=64` in `mk/rules.mk` and the `static_assert` backstop must survive, and the source must not define it itself (the PCH latches the value first, so that form is a no-op). |
| `check_gcode_error_ownership.py` | Hard gate at zero: fail if an `execute_gcode()` call passes an inline error lambda that only logs without declaring `caller_surfaces_errors`; that claim silences `GcodeErrorRouter`, so a rejected macro is reported by nobody. Any UI marker in the body passes. Opt out `// ERROR_OWNERSHIP_OK: reason`. Contract: `docs/devel/RPC_ERROR_OWNERSHIP.md` |
| `check_installer_step_reachability.py` | Fail if a function in `scripts/lib/installer/*.sh` has no production call site (#1343): the bats suites call steps directly, so a green suite cannot prove one is wired in. Never scans the generated bundles. Opt out `# UNCALLED_OK: reason`. |
| `check_printer_images.py` | Fail if `printer_database.json` names a printer image not on disk (a dangling reference silently renders `generic-corexy`). `KNOWN_MISSING` is a ratchet that can only shrink. `--list` |
| `check_pytest_asyncio_deps.py` | Fail if an `async def test_*` exists without pytest-asyncio in `requirements.txt` (plain pytest fails it as a coroutine) or without `@pytest.mark.asyncio` (strict mode SKIPS it). The second rule drops when a pytest config sets `asyncio_mode = auto`. |
| `check_modal_chrome_budget.py` | Fail if a modal's chrome does not match the content cap it budgets against (#1277): everything but a divider and the first button row lives inside the scroll container, or the container opts into `#dialog_content_pinned_max` / `#dialog_content_tall_chrome_max`; a card raised above the shared 85% cap is also flagged. Opt out `MODAL_CHROME_OK: reason` |
| `check_raw_print_job_state.py` | Every read of the raw print wire (`PrintJobState::PRINTING/PAUSED/STANDBY`, `get_print_job_state()`, `get_print_state_enum_subject()`) must carry `// RAW_PRINT_STATE_OK: <real reason>` within 12 lines above; ask `get_print_lifecycle()` / `job_holds_machine()` instead. The derivation layer is allowlisted |
| `check_print_state_cast.py` | Reject hand-casting `lv_subject_get_int()` into `PrintState` or `PrintJobState`: the enums share no numbering past 0, so the wrong subject reads back a different state. Use the typed accessors and observer factories. Opt out `// PRINT_STATE_CAST_OK: reason` |
| `check_patch_drift.py` | Fail if the patches applied into `lib/lvgl` / `lib/libhv` are not the patches now in `patches/`: a sha256 stamp in the submodule's git dir, checked by `mk/patches.mk` before applying (`--pre-apply`) and written after (`--write-stamp`). Never auto-repairs; the only safe repair is `make reapply-patches`. |
| `check_workflow_submodules.py` | Fail if a workflow job runs the shell suite (`make test-shell` or `bats tests/shell/`) without both `./.github/actions/init-submodules` and `make apply-patches`; `test_lvgl_event_code_gate.bats` needs a patched checkout. Gated as `qc_workflow_submodules` |
| `check_comment_archaeology.py` | Ratchet: a comment must not cite a commit SHA (only tokens `git cat-file` resolves in this repo count). The judgment half of CLAUDE.md § "Comments describe the code, not its past" stays with the reviewer. Baseline `comment_archaeology_baseline.txt`; `--list` / `--write-baseline`. |
| `check_touch_rotation_source.py` | Fail if a display backend gates the stored touch range on the `/display/rotate` config key instead of the applied rotation (`display_rotation_degrees()` / `display_is_rotated()`); the key is only the request (#1394). |
| `audit_codebase.sh` | Check for coding standard violations. `--strict` for CI |
| `perf-farm-check.sh` | Pre-release CPU budget check across the physical test farm (run from zeus): idle, a mock print with a LONG filename, and the same with animations off, each on a fresh instance (#1440). Budgets in `config/perf_budgets.json`. Refuses a printer that is printing. Exit 0 in budget, 1 over, 2 no usable sample (not a pass) |
| `format-xml.py` | XML formatter: 2-space indent, attribute wrapping at ~120 chars. Skips generator-owned `ui_xml/translations/` and foreign `android/` XML; LVGL state selectors (`style_bg_opa:checked`) round-trip through a sentinel. A file it cannot parse is an ERROR, never "formatted". `--check` / `--diff`. Meta-tests: `tests/shell/test_format_xml_gate.bats` |
| `verify_mdi_codepoints.py` | Verify MDI codepoint mappings are correct |

### Screenshots & Testing
Screenshots drive a freshly booted instance with `helix-screen ctl` (`docs/devel/HELIXCTL.md`);
each screen maps to a navigation recipe in `screenshot-recipes.sh`, the single source of truth.

| Script | Purpose |
|--------|---------|
| `screenshot.sh` | Boot an instance, drive it to a screen, capture (`./scripts/screenshot.sh helix-screen output [token] [flags]`). Token = a base panel, overlay, or `demo` screen from `screenshot-recipes.sh` |
| `screenshot-recipes.sh` | Token → helix-screen ctl navigation-recipe table (sourced by screenshot.sh + screenshot-all.sh) |
| `screenshot-all.sh` | Capture all documentation screenshots |
| `ad5m-screenshot.sh` | Remote screenshot capture from AD5M printer |
| `generate-screenshots.sh` | Generate screenshots for documentation/marketing |
| `generate-test-data.py` | Generate mock test data for test suites |
| `test_clean_shutdown.sh` | Verify clean shutdown behavior |
| `afc-test.sh` | AFC live smoke test against a real printer |

### Developer Tools
| Script | Purpose |
|--------|---------|
| `setup-worktree.sh` | Create/configure git worktrees in `.worktrees/`. Symlinks the shared `lib/` submodules from the main tree and gives `lib/helix-xml`, `lib/lvgl` and `lib/libhv` a private per-worktree checkout (`LIB_PRIVATE_SUBMODULES`; its comment says why). `--unlink` / `--relink` around a merge or rebase. Gated by `tests/shell/test_worktree_setup.bats` |
| `sync-worktree-mtimes.py` | Give worktree files the main tree's mtime when the content is byte-identical, so the cloned `build/obj/` is usable instead of rebuilding nearly every object. Called from `setup-worktree.sh` |
| `git-stats.sh` | Comprehensive repo statistics with effort estimation |
| `benchmark_hosts.sh` | Benchmark host performance for build optimization |
| `benchmark_neon.sh` | NEON SIMD performance benchmarks |
| `add-spdx-headers.sh` | Add SPDX license headers to source files |
| `add-copyright-headers.sh` | Add copyright headers to source files |
| `debug-ad5m-boot.sh` | AD5M boot diagnostics. `--boot` saves to persistent log |

### Subdirectories
| Path | Purpose |
|------|---------|
| `lib/installer/` | Modular installer components sourced by `install-dev.sh` |
| `lib/lvgl_image_lib.sh` | Shared LVGL image conversion helpers |
| `kiauh/` | KIAUH integration (Klipper installer plugin) |
| `translations/` | Python package for translation extraction, sync, and coverage |

## Key Patterns

- **Auto-generated files**: `install.sh` and `uninstall.sh` are bundled from `install-dev.sh` + `lib/installer/`; edit those, then re-bundle
- **Telemetry credentials**: Scripts auto-load from `.env.telemetry` in project root. Set `HELIX_TELEMETRY_ADMIN_KEY` env var
- **Asset regeneration**: via Makefile targets (`make regen-fonts`, `make regen-images`), not directly
- **Python deps**: telemetry scripts need pandas; install `telemetry-requirements.txt` into the project `.venv/`
