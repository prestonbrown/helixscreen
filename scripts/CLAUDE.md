# scripts/

Build, asset generation, deployment, and developer tooling for HelixScreen. One line per script:
what it is for. The full per-script notes (flags, exit codes, opt-out markers, where its tests live)
are in `docs/devel/SCRIPTS_REFERENCE.md`, and the script's own header carries the reasoning.

## Categories

### Installation & Deployment
| Script | Purpose |
|--------|---------|
| `install-dev.sh` | Modular dev installer - uses `lib/installer/` modules. Edit this one |
| `bundle-installer.sh` | Bundles `lib/installer/*` into the end-user `install.sh` (`make installer`); never committed |
| `install.sh` | Bootstrap for the old raw-GitHub URL: fetches and runs the release-bundled installer |
| `bundle-uninstaller.sh` | Bundles uninstall modules → `uninstall.sh` (`make installer` → `build/installer/`) |
| `render-installer-logo.sh` | Regenerates the installer banner `lib/installer/logo.sh` from the app icon (needs Pillow) |
| `helix-launcher.sh` | Systemd-launched watchdog wrapper. Sources `helixscreen.env` for runtime config |
| `check-deps.sh` | Validates build dependencies. `--minimal` for cross-compile environments |
| `device-env-set.sh` | Set one `KEY=VALUE` in a deployed device's `helixscreen.env` over ssh |
| `device-env-set-remote.sh` | The on-device half of `device-env-set.sh`, piped to `sh -s` (BusyBox-safe) |
| `deploy-stop-app-remote.sh` | On-device stop step of `deploy-common`: stops the units, refuses to kill a respawning one |

### Release & Packaging
| Script | Purpose |
|--------|---------|
| `release-gate.sh` | `make release-gate`: what a tag push builds and per-push CI does not; run before tagging |
| `release-health.sh` | Newest beta vs stable in telemetry; WARN lines only, always exits 0 |
| `latest-green-sha.sh` | The commit `/release` cuts from: newest push whose required workflows all passed |
| `dev-release.sh` | Local dev release workflow (build + package + upload) |
| `generate-manifest.sh` | Generates `manifest.json` from release archives. Used by CI and dev-release |
| `package-helix-plugin.sh` | Builds the HelixPrint plugin tarball the remote installer unpacks |
| `ci/gpu_libs_hidden_smoke.sh` | CI smoke: a packaged binary with its GPU libraries missing must still reach the framebuffer |
| `esp32_package_release.sh` | Bundles a built K-Touch firmware into the release zip |
| `distribution_partners.py` | Partner installers and the release assets each fetches; a release step fails on a missing one |
| `version-compare.sh` | Semver precedence for shell consumers (`A B` prints -1/0/1) |
| `r2-prune-plan.sh` | Which R2 release versions a prune may delete; never one a channel manifest names |
| `android-version-code.sh` | The single definition of the Android versionCode |
| `generate-whatsnew.sh` | Writes the Play Store "What's new" file from `android/whatsnew/<version>.txt` |
| `changelog-section.sh` | Prints one version's `CHANGELOG.md` section; the one extractor every publisher uses |
| `generate-upload-keystore.sh` | One-time Android upload keystore generation; output in `~/.android-keystore/`, never the tree |

### Performance Measurement
| Script | Purpose |
|--------|---------|
| `screensaver-perf/` | Screensaver cost on real boards (CPU, presented frames, load gate); see its README |

### Asset Generation (`make regen-*`)
| Script | Purpose |
|--------|---------|
| `regen_mdi_fonts.sh` | Regenerate MDI icon fonts from the list `mdi_icon_codepoints.py` computes. Run after adding icons |
| `regen_text_fonts.sh` | Regenerate Noto Sans text fonts for LVGL |
| `regen_images.sh` | Pre-render splash screen images to LVGL binary format |
| `regen_placeholder_images.sh` | Generate placeholder/fallback images |
| `regen_printer_images.sh` | Process printer model images for the printer database |
| `trim_printer_images.py` | Crop printer images to visible content and remap `regions.json`; `--check` in quality-checks |
| `gen_splash_3d.py` | Composite 3D logo onto full-screen splash canvases |
| `generate_gradient_bg.py` | Pre-render gradient backgrounds for print file cards (perf optimization) |
| `LVGLImage.py` | Python library for LVGL binary image format conversion |
| `download_printer_images_headless.py` | Scrape printer images from web sources |

### Icon Pipeline
| Script | Purpose |
|--------|---------|
| `build_custom_icon_font.py` | Custom SVG glyphs MDI lacks into `helix-custom-icons.ttf` (checked in) |
| `mdi_icon_codepoints.py` | The icon fonts' codepoint list, read from the icon header and `ICON_*` macros |
| `gen_icon_consts.py` | `include/ui_icon_codepoints.h` → `ui_xml/globals.xml` icon string constants |
| `generate_icon_header.py` | Embed PNG icon data into C headers |
| `validate_icon_fonts.sh` | Bidirectional check: codepoints ↔ fonts ↔ XML usage |

### Translation / i18n
| Script | Purpose |
|--------|---------|
| `generate_translations.py` | Main translation generator: YAML to runtime XML packs |
| `translation_packs.py` | The `ui_xml/translations/` files no locale loads (merged file, identity pack) |
| `translation_sync.py` | Sync keys (XML/C++ → YAML); also `coverage`, `obsolete`, `glossary` subcommands |
| `translations/cjk_filename_fonts.py` | CJK glyphs and pixel sizes a print filename can need, for the filename font bins |
| `translations/yaml_manager.py` | Edits locale YAMLs by line splice, never a re-dump; refuses duplicate keys |
| `translations/` | Python package: extractor, YAML manager, coverage, glossary, CLI |

**Terminology consistency:** `translations/GLOSSARY.md` (`make translation-glossary`) pins canonical per-language renderings of recurring terms. Reuse the glossary term when translating, human or agent, and `grep` the locale before coining a new word for an already-translated concept. After changing a canonical term, add its English key to `GLOSSARY_GROUPS` in `translations/glossary.py` and regenerate.

### Telemetry & Crash Analysis
| Script | Purpose |
|--------|---------|
| `telemetry-pull.sh` | Pull events from telemetry worker API. Needs `HELIX_TELEMETRY_ADMIN_KEY` |
| `telemetry-analyze.py` | Adoption, reliability, crash metrics. Output: terminal/JSON/HTML |
| `telemetry-printer-profiles.py` | Printer detection analysis: model distribution, name clustering, candidate heuristics, DB validation |
| `telemetry-update-printer-db.py` | Interactive `printer_database.json` updater from a profile analysis |
| `telemetry-printer-issues.py` | Weekly Action: one GitHub issue per undetected hardware cluster of 5+ devices |
| `telemetry-crashes.py` | Resolve ASLR crash addresses to functions and group by signature |
| `resolve-backtrace.sh` | Resolve raw backtrace addresses with the R2 `.sym` files; crash-context analysis |
| `lvgl_event_name.py` | The `LV_EVENT_*` name a crash record's event code means |
| `debug-bundle.sh` | Fetch and display debug bundles from `crash.helixscreen.org` |
| `freeze-drops.sh` | Aggregate UpdateQueue `DROPPED` events across logs and bundles |

**Printer hardware profile pipeline** (`telemetry-pull.sh` → `telemetry-printer-profiles.py` → `telemetry-update-printer-db.py`): `docs/devel/SCRIPTS_REFERENCE.md` § Telemetry & Crash Analysis.

### Quality & Auditing

A gate's header explains what it prevents and why the obvious alternative is wrong. `check_<name>.py`
has its meta-test at `tests/shell/test_<name>_gate.bats`; a row cites the test only when it lives
elsewhere. Rows carry what a gate fails on and its opt-out marker.

| Script | Purpose |
|--------|---------|
| `quality-checks.sh` | Pre-commit and CI quality checks driver: one `qc/<gate>.sh` per gate, result cache |
| `source_strip.py` | The one comment and literal blanker every pattern-matching gate imports |
| `ratchet.py` | The shared under-baseline verdict for count ratchets: lenient at commit, fails at push (`HELIX_RATCHET_STRICT=1`) |
| `bats_select.py` | Which `tests/shell/*.bats` files a run needs |
| `run_suite_report.sh` | The shared `--success` Catch2 report the vacuous-test and order-dependence gates read |
| `check_doc_refs.py` | Doc-reference gate: dead paths, links, `make` targets, docs/devel index, shipped counts |
| `doc_anchors.py` | Resolves `path#scope` doc citations to today's line; `--check` is advisory |
| `check_translation_format_specifiers.py` | Fail on translated format strings whose placeholders differ from English |
| `check_cjk_font_staleness.sh` + `check_cjk_font_coverage.py` | Fail if the runtime CJK fonts miss a codepoint translations or filenames need |
| `check_update_queue_leaks.py` | Ratchet on cross-test UpdateQueue leaks vs a baseline |
| `check_asan_leaks.py` | Ratchet on LeakSanitizer at-exit leaks vs a baseline |
| `tsan_resplit_shard.sh` | `make test-tsan` retry that re-splits a shard killed by TSan's reporter abort |
| `triage_shard.sh` | Unit sweep post-mortem for a failed shard: alone, pinned, repeated |
| `check_required_names.py` | Fail if a `find_required()` name is missing from the XML its unit creates |
| `check_orphan_callbacks.py` | Ratchet on XML callbacks nobody registers, registrations nobody uses, double registrations |
| `check_esp32_xml_bindings.py` | XML subjects and callbacks the desktop registers but the ESP32 firmware does not |
| `check_overlay_boilerplate.py` | Shrink-only ratchet on overlay boilerplate `OverlayBase` replaces |
| `check_duplicate_xml_names.py` | Fail on a widget `name=` used twice in one XML file |
| `check_responsive_token_scope.py` | Fail on a responsive token declared below the top level of `ui_xml/` |
| `check_shipped_spacing_tokens.py` | Fail on a spacing token declared only in a dev-only panel stripped from release |
| `check_icon_names.py` | Fail on an XML icon name absent from `include/ui_icon_codepoints.h` |
| `check_raw_this_queue_update.py` | Ratchet on `queue_update([this, ...])` in `src/` |
| `check_hardcoded_pixels.py` | Ratchet on raw pixel literals where a design token belongs |
| `check_thumbnail_cache_guard.py` | Fail on calls to `ThumbnailCache`'s legacy overloads without a load context |
| `check_lfs_sites.py` | Fail if a site that needs 64-bit `off_t` on 32-bit targets loses it |
| `check_link_parity.py` | Fail if the fbdev or EGL variant link lacks an object the main link has |
| `check_gpu_lib_callers.py` | Build-time gate: only reviewed objects may call the dlopened GPU libraries |
| `gpu_lib_return_kinds.py` | Build step: return-type table for the GPU library stand-ins |
| `check_gcode_error_ownership.py` | Hard gate: an inline `execute_gcode()` error lambda must not silently swallow errors |
| `check_installer_step_reachability.py` | Fail on an installer function with no production call site |
| `check_printer_images.py` | Fail on a printer image the database names but the disk lacks |
| `check_pytest_asyncio_deps.py` | Fail on async pytest tests that would be skipped or fail without pytest-asyncio |
| `check_modal_chrome_budget.py` | Fail if a modal's chrome does not match the content cap it budgets against |
| `check_source_rules.py` | One table of C++ shape rules the compiler accepts and no unit test reaches |
| `check_cached_widget_pointers.py` | Ratchet on raw `lv_obj_t*` data members |
| `check_ams_xml_mirror.py` | Fail if AmsState's re-entry mirror and `init_subjects()` publish different XML names |
| `apply_submodule_patch.sh` | The one apply/already-applied/refuse verdict every `mk/patches.mk` stanza uses |
| `own_git_dir.sh` | Print a directory's git dir only when it tops its own checkout |
| `check_patch_drift.py` | Fail if the patches applied into a submodule are not the ones now in `patches/` |
| `check_patch_markers.py` | Fail the build when a wired patch's marker line is absent from the checkout |
| `check_workflow_submodules.py` | Fail on a workflow running the shell suite without submodules and patches |
| `check_vendor_branches.py` | Ratchet: generic code must not branch on a vendor |
| `check_hold_ends_press.py` | Fail on a `long_pressed` handler that neither ends its press nor opts out |
| `check_hand_rolled_swatch.py` | Ratchet on colour swatches drawn by hand instead of the swatch painter |
| `check_test_budget.py` | A branch does not grow the C++ test suite, TestAccess or friends |
| `check_growth_ratchets.py` | A branch does not grow files, functions or duplication past the caps |
| `check_layering.py` | A branch does not add an include that points up the layer stack |
| `code_lint.sh` | The tree-wide code lints too small for a script of their own |
| `check_comment_archaeology.py` | Ratchet on commit SHAs in comments, plus history phrases at commit |
| `check_feature_gates.py` | Every `HELIX_HAS_*` gate has its one default in `include/helix_features.h` |
| `check_platform_manifest.py` | Advisory: where `platforms.json` and its derived consumers disagree |
| `platform_manifest.py` | Answers derived questions about a platform from `platforms.json` |
| `rename_observers.py` | Rewrites retired observer factory names onto `observe<V>()` |
| `rename_update_queue_drain.py` | Rewrites `UpdateQueueTestAccess` drains onto the queue's own methods |
| `rename_constants.py` | Renames `UPPER_SNAKE_CASE` C++ constants to `kCamelCase` (plan, then apply) |
| `check_constant_naming.py` | Fail on a new `UPPER_SNAKE_CASE` constant in added lines |
| `audit_codebase.sh` | Check for coding standard violations. `--strict` for CI |
| `check_no_mocks.py` | The land-gate `no-mocks` row: mock code a packaged binary would carry |
| `esp32_syntax_check.py` | `-fsyntax-only` of every firmware unit a commit range reaches, in the IDF image |
| `check_toolhead_census.py` | Commit-time version of the machine-moving control census tests |
| `check_theme_artifacts.py` | Fail if the generated theme token table or its golden is stale |
| `check_old_toolchain.py` | Syntax-check `src/` against the older cross toolchains' compiler (g++-10) |
| `perf-farm-check.sh` | Pre-release CPU budget check across the physical test farm |
| `perf-farm-scheduled.sh` | `perf-farm-check.sh` unattended: locked, logs kept, trend report |
| `format-xml.py` | XML formatter: 2-space indent, attribute wrapping at ~120 chars |
| `verify_mdi_codepoints.py` | Verify each icon's claimed name (from `mdi_icon_codepoints.py --table`) against MDI's own metadata cache |

### Screenshots & Testing
Screenshots drive a freshly booted instance with `helix-screen ctl` (`docs/devel/HELIXCTL.md`);
each screen maps to a navigation recipe in `screenshot-recipes.sh`, the single source of truth.

| Script | Purpose |
|--------|---------|
| `screenshot.sh` | Boot an instance, drive it to a screen token, capture |
| `screenshot-recipes.sh` | Token → helix-screen ctl navigation-recipe table (sourced by screenshot.sh + screenshot-all.sh) |
| `screenshot-all.sh` | Capture all documentation screenshots |
| `ad5m-screenshot.sh` | Remote screenshot capture from AD5M printer |
| `esp32_serial_snapshot.py` | Screenshot an ESP32 panel over its serial console |
| `detokenize_log.py` | Turn an ESP32 console capture's token records back into log text |
| `esp32_logtok_db.py` | Firmware build step: the token database of an ELF's tokenized log strings |
| `generate-screenshots.sh` | Generate screenshots for documentation/marketing |
| `generate-test-data.py` | Generate mock test data for test suites |
| `test_clean_shutdown.sh` | Verify clean shutdown behavior |
| `afc-test.sh` | AFC live smoke test against a real printer |
| `plugin-state-snapshot.sh` | Read-only record of what a plugin install or uninstall touches on a printer |
| `plugin-host-matrix.sh` | Plugin install/uninstall against real Moonrakers in containers, per printer layout |

### Developer Tools
| Script | Purpose |
|--------|---------|
| `setup-worktree.sh` | Create a worktree in `.worktrees/` with shared and private `lib/` submodules |
| `teardown-worktree.sh` | Remove a finished worktree and its branch, refusing unsafe cases |
| `helix-claim` | Cross-session claim registry and the default make `-j` (`jobs`, `hold`) |
| `pool-docker.sh` | Every docker cross build runs through it, inside the machine's job pool |
| `resource-advisor.sh` | `PreToolUse` hook: suggests where a heavy command should run; never blocks |
| `subagent-brief.sh` | `SubagentStart` hook: injects the shared-machine rules into every subagent |
| `compile_command.py` | Which compile command describes a source today, for `-fsyntax-only` replays |
| `tree_stamp.sh` | Content stamp of sources differing from a base; the stale helix-tests warning |
| `test_link_sources.py` | The source files a link is built from, for the stale stamp |
| `sync-worktree-mtimes.py` | Give unchanged worktree files the main tree's mtime so `build/obj/` is reusable |
| `keep-unchanged-mtimes.sh` | Run a command, then restore mtimes on files it left byte-identical |
| `remote-build.sh` | Build on a remote host by sending a patch, not the working tree |
| `mutate-diff.sh` | `make mutate-diff` dispatcher: local with reflinks, else the test host |
| `test-host-run.sh` | Run sweeps, ASAN, TSAN, mutation and tag runs in the test host's container |
| `test-host-setup.sh` | Stand up a test host over ssh |
| `lib/build_hosts.sh` | The one parser for the build-hosts file |
| `ccache-shared-config.sh` | The one writer of shared-ccache settings across trees |
| `lib/mem_available.sh` | MemAvailable bounded by the cgroup cap, for sizing `-j` |
| `lib/ccache_shared.sh` | Runs this tree's `ccache-shared-config.sh` in the test container |
| `full-test-run.sh` | `make full-test-run`: unit sweep plus bats, local or on the test host |
| `land-gate.sh` | `make land-gate`: the pre-landing gate, CI's shard layout on one clean HEAD |
| `../.githooks/lib/land-gate-stamp.sh` | Writes and reads the land-gate pass stamp |
| `lib/sweep_location.sh` | Local or test host for a gate's C++ sweep |
| `git-stats.sh` | Comprehensive repo statistics with effort estimation |
| `benchmark_hosts.sh` | Benchmark host performance for build optimization |
| `benchmark_neon.sh` | NEON SIMD performance benchmarks |
| `add-spdx-headers.sh` | Add SPDX license headers to source files |
| `debug-ad5m-boot.sh` | AD5M boot diagnostics. `--boot` saves to persistent log |

### Cloud Sessions
| Script | Purpose |
|--------|---------|
| `cloud/env-setup.sh` | Cloud environment setup: build deps, seed clone, Python venv |
| `cloud/session-start.sh` | `SessionStart` hook finishing the per-session cloud setup |

### Subdirectories
| Path | Purpose |
|------|---------|
| `lib/installer/` | Modular installer components sourced by `install-dev.sh` |
| `lib/lvgl_image_lib.sh` | Shared LVGL image conversion helpers |
| `lib/worktree_lib.sh` | Path canonicalization and shared-module pointer repair for the worktree scripts |
| `kiauh/` | KIAUH integration (Klipper installer plugin) |
| `translations/` | Python package for translation extraction, sync, and coverage |

## Key Patterns

- **`helix-claim jobs` is make's `-j` only**: anything outside make sizes itself with `helix-claim hold` or `pool-docker.sh` (the `helix-ops` skill)
- **Generated installers**: `install.sh`/`uninstall.sh` are bundled from `lib/installer/` into `build/installer/`, not in git; edit the modules. The tracked `scripts/install.sh` is only the bootstrap
- **Telemetry credentials**: Scripts auto-load from `.env.telemetry` in project root. Set `HELIX_TELEMETRY_ADMIN_KEY` env var
- **Asset regeneration**: via Makefile targets (`make regen-fonts`, `make regen-images`), not directly
- **Python deps**: telemetry scripts need pandas; install `telemetry-requirements.txt` into the project `.venv/`
