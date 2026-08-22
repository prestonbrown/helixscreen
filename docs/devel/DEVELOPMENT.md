# Development Guide

The daily-workflow reference for HelixScreen development: running the app, test
mode, logging, config, screenshots, and the contribution process.

New here? The entry path is [CONTRIBUTING.md](../../CONTRIBUTING.md) →
[ONBOARDING.md](ONBOARDING.md) (environment setup + build + the 15-minute mental
model) → [YOUR_FIRST_CONTRIBUTION.md](YOUR_FIRST_CONTRIBUTION.md). This doc is the
reference you return to; each topic lives in exactly one place:

| Topic | Lives in |
|-------|----------|
| Environment setup, first build | [ONBOARDING.md](ONBOARDING.md) |
| Makefile internals, cross-compilation, worktrees, fonts/icons | [BUILD_SYSTEM.md](BUILD_SYSTEM.md) |
| Architecture, subsystem deep dives | [ARCHITECTURE.md](ARCHITECTURE.md) + [architecture/](architecture/README.md) |
| Installer scripts | [INSTALLER.md](INSTALLER.md) |

## Quick Start

```bash
make -j
./build/bin/helix-screen --test -vv  # Mock printer + DEBUG logs
```

Environment setup and dependencies per OS: → [ONBOARDING.md](ONBOARDING.md).
Build targets beyond `make -j` (`make build`, `make dev`, `make V=1`, clean
targets, cross-compilation): → [BUILD_SYSTEM.md](BUILD_SYSTEM.md).

## Running the Application

```bash
./build/bin/helix-screen                    # Production mode
./build/bin/helix-screen --test             # Full mock mode (no hardware)
./build/bin/helix-screen --test --real-wifi # Mix real WiFi + mock printer
./build/bin/helix-screen --dark             # Force dark theme
./build/bin/helix-screen --light            # Force light theme
./build/bin/helix-screen -d 1 -s small      # Display 1, small size
```

### Test Mode Flags

| Flag | Effect |
|------|--------|
| `--test` | Enable test mode (required for mocks) |
| `--real-wifi` | Use real WiFi instead of mock |
| `--real-ethernet` | Use real Ethernet instead of mock |
| `--real-moonraker` | Connect to real printer |
| `--real-files` | Use real printer files |
| `--real-ams` | Use a real AMS backend instead of mock |
| `--real-sensors` | Use real sensor data instead of mock |
| `--no-ams` | Disable mock AMS (for runout modal testing) |
| `--disconnected` | Simulate a disconnected printer |

**Test mode keyboard shortcuts** (SDL builds): S=screenshot, M=memory stats,
D=toggle dark/light, Z=cycle screensavers, Cmd/Win+Q=quit; test-mode-only:
A=test action prompt, N=test notification, P=cycle configured printers,
F=filament-runout simulation.

### Wizard Flags

| Flag | Effect |
|------|--------|
| `-w`, `--wizard` | Force the first-run configuration wizard |
| `--wizard-step <step>` | Jump to a specific wizard step (0-12) for testing |

`--wizard` is **destructive to wizard-managed config** — it is meant to fully re-run setup, not just re-open it. On startup with `--wizard`, the app clears all wizard-managed state in `settings.json` so a stale or wrong install-time seed is recoverable:

- Clears the **preset marker** via `Config::clear_preset()` (erases the top-level `"preset"` key). Because `has_preset()` is then false, the re-run becomes a *full* wizard — the identify and hardware pages reappear instead of being skipped.
- Clears the active printer's `moonraker_host` (set to `""`), so the connection step is re-entered.
- Clears the cached hardware snapshot and the wizard's heater/sensor/fan/LED/filament-sensor selections, so stale hardware choices don't trigger false hardware-health warnings.

This is the recovery path when an install-time auto-seed (see [INSTALLER.md](INSTALLER.md)) picked the wrong printer: re-run with `--wizard` to drop the seed and reconfigure from scratch.

### Printer Detection (`--detect-printer`)

A headless one-shot that queries a running Moonraker over its REST API, runs the same detection logic the wizard uses, prints a JSON verdict to stdout, and exits. No UI is started. Useful for debugging which preset a given printer matches, or for scripting install-time detection.

| Flag | Argument | Default | Effect |
|------|----------|---------|--------|
| `--detect-printer` | (none) | — | Run headless detection, print JSON, exit |
| `--host <addr>` | host/IP | `127.0.0.1` | Moonraker host to query |
| `--port <n>` | 1-65535 | `7125` | Moonraker port to query |

```bash
# Detect the local printer
./build/bin/helix-screen --detect-printer

# Detect a printer on another host
./build/bin/helix-screen --detect-printer --host 192.168.1.74 --port 7125
```

Internally it does three REST GETs against `http://HOST:PORT` (3-second timeout each):
`/printer/objects/list`, `/printer/info` (hostname), and
`/printer/objects/query?configfile=settings` (kinematics + build volume from the
`stepper_x/y/z` config). The resulting hardware profile is fed to
`PrinterDetector::auto_detect()`.

**Exit codes:** `0` on success (verdict printed). `1` if Moonraker's object list is
unavailable at the given host/port (a warning is logged; no JSON is printed).

**JSON output shape** — one compact line, terminated by a newline:

```json
{"model":"Creality K1 Max","preset":"k1_max","confidence":92,"runner_up_preset":"qidi_q2","runner_up_confidence":43}
```

| Key | Type | Meaning |
|-----|------|---------|
| `model` | string | Human-readable detected printer name (`PrinterDetectionResult::type_name`) |
| `preset` | string or `null` | Platform preset id for the match; `null` when the result has no preset |
| `confidence` | integer (0-100) | Detection confidence for the top match |
| `runner_up_preset` | string or `null` | Preset id of the second-place candidate; `null` when there is none |
| `runner_up_confidence` | integer (0-100) | Confidence score of the runner-up |

When there is no credible second candidate, the runner-up fields collapse:

```json
{"model":"FlashForge AD5M","preset":"ad5m","confidence":88,"runner_up_preset":null,"runner_up_confidence":0}
```

The installer's Tier-2 detection (see [INSTALLER.md](INSTALLER.md)) parses exactly this
output and applies its confidence gate to `confidence` and the
`confidence - runner_up_confidence` margin.

For cross-compilation, patches, and advanced options, see **[BUILD_SYSTEM.md](BUILD_SYSTEM.md)**.

## Logging

### Verbosity Levels

| Level | Flag | Use For |
|-------|------|---------|
| WARN | (default) | Errors and warnings only |
| INFO | `-v` | User-visible milestones |
| DEBUG | `-vv` | Troubleshooting, summaries |
| TRACE | `-vvv` | Per-item loops, wire protocol |

When to use each level in your own code, and the spdlog-only rule (never
`printf`/`cout`/`LV_LOG_*`): → [LOGGING.md](LOGGING.md).

### Log Destinations

```bash
./build/bin/helix-screen --log-dest=console  # Console (default on macOS)
./build/bin/helix-screen --log-dest=journal  # systemd journal (Linux)
./build/bin/helix-screen --log-dest=file --log-file=/tmp/helix.log
```

**Viewing logs on Linux:**
```bash
journalctl -t helix -f              # systemd
tail -f /var/log/helix-screen.log   # file
```

Logging on target devices (backend auto-detection, systemd service):
→ [BUILD_SYSTEM.md](BUILD_SYSTEM.md) § "Logging on Target".

## Configuration

### Config File Pattern

```bash
cp config/settings.json.template config/settings.json  # First-time setup
```

- `config/settings.json` - User settings (git-ignored)
- `config/settings.json.template` - Defaults (versioned)

**Never commit user config.** Legacy root location auto-migrates.

### Config Structure

```json
{
  "printer": {
    "heaters": { "bed": "heater_bed", "hotend": "extruder" },
    "temp_sensors": { "bed": "heater_bed", "hotend": "extruder" },
    "fans": { "part": "fan", "hotend": "heater_fan hotend_fan" }
  }
}
```

**Naming:** Container keys plural (`heaters`), role keys singular (`bed`).

## DPI & Hardware Profiles

`--dpi <n>` declares the **panel's physical DPI**. `DisplayMetrics` turns it into a UI
scale factor that multiplies every px token, the grid cell, and the font ladder; LVGL's
own DPI is then set to `LV_DPI_DEF * scale`, never to `n` directly. UI constants are
authored against a 160 DPI reference.

The scale is flat at exactly **1.0 up to 225 DPI**, which covers every shipping printer,
so passing a printer's real DPI is a no-op. It only does something on phone-class panels:

```bash
./build/bin/helix-screen --test -s 1080x2400 --dpi 405   # ~1.578x — a 405 DPI handset
```

Measured true DPI of the test fleet (over `FBIOGET_VSCREENINFO`, corrected against the
panels' real diagonals — **not** the values the kernel reports, which are wrong or absent
on 6 of the 8):

| Hardware | Resolution | True DPI | Scale |
|----------|------------|----------|-------|
| Reference (authoring) | — | 160 | 1.0 |
| CC1 | 480×272 | 131 | 1.0 |
| SonicPad | 1024×600 | 170 | 1.0 |
| CB1 + BTT HDMI5 | 800×480 | 187 | 1.0 |
| Raspberry Pi rig | 800×480 | 206 | 1.0 |
| K1C, K2 Plus | 480×800 | 218 | 1.0 |
| AD5M | 800×480 | 221 | 1.0 |
| Phone (Android) | 1080×2400 | ~405 | 1.578 |

Do not read a panel's DPI off the kernel to fill this in. See
[THEME_SYSTEM.md § The high-DPI UI scale factor](THEME_SYSTEM.md#the-high-dpi-ui-scale-factor).

## Multi-Display (macOS)

`-d <n>` picks the display (`./build/bin/helix-screen -d 1 -s small`), `--x-pos`/`--y-pos`
position the window exactly. Details: → [BUILD_SYSTEM.md](BUILD_SYSTEM.md) § "Multi-Display Support".

## Screenshots

```bash
# Interactive: Press 'S' in running UI

# Automated:
./scripts/screenshot.sh helix-screen output-name [token] [options]
./scripts/screenshot.sh helix-screen home-screen home
./scripts/screenshot.sh helix-screen motion motion -s small

# Environment overrides:
HELIX_SCREENSHOT_DISPLAY=0 ./scripts/screenshot.sh helix-screen test home
HELIX_SCREENSHOT_OPEN=1 ./scripts/screenshot.sh helix-screen review home
```

Output: `/tmp/ui-screenshot-[name].png`

To bring up an arbitrary panel/overlay for debugging (the old `-p`/`--panel`
flags are gone), drive a running instance with `helix-screen ctl` — see
`docs/devel/HELIXCTL.md`.

## Icon & Font Workflow

```bash
make regen-fonts   # Regenerate MDI fonts + icon constants together (canonical path)
```

Adding a new icon glyph, font generation internals, and `make icon`:
→ [BUILD_SYSTEM.md](BUILD_SYSTEM.md) § "Font Generation" / "Icon Generation".

## IDE Setup

```bash
make compile_commands  # Generates compile_commands.json (requires bear)
```

**VS Code:** C/C++ extension + clangd extension
**Vim/Neovim:** Configure clangd LSP client
**CLion:** Import as Makefile project

## Daily Workflow

1. **Edit code** in `src/` or `include/`
2. **Edit XML** in `ui_xml/` — **no rebuild, no restart needed** (hot reload is ON by default; see below)
3. **Build** with `make -j` (only when C++ changes)
4. **Test** with `./build/bin/helix-screen --test -vv`
5. **Screenshot** with S key or `./scripts/screenshot.sh`
6. **Commit** working incremental changes

### XML vs C++ Changes

| Change Type | Location | Rebuild? | Hot Reload? |
|-------------|----------|----------|-------------|
| Layout, styling, colors | `ui_xml/*.xml` | **No** | **Yes** — ON by default for native builds |
| Logic, bindings, handlers | `src/*.cpp`, `include/*.h` | Yes (`make -j`) | No |
| Theme colors | `config/themes/*.json` | No — just restart | No |
| Translations | `translations/*.yml` (e.g. `translations/en.yml`) | Yes (code generation step) | No |

### XML Hot Reload

Hot reload is **ON by default for native (non-release) builds** — you don't need to set any env var. Just run the app, edit XML, save, and the active panel/overlay/modal rebuilds in place within ~500ms.

```bash
./build/bin/helix-screen --test -vv
# Edit ui_xml/home_panel.xml in another terminal, save, watch the UI update live.
```

What you'll see in the log on each save:

```
[HotReload] Detected change: home_panel
[HotReload] Reloaded: home_panel (0.3ms)
[PanelBase::rebuild] Home Panel — tearing down and re-creating
```

Robustness: if you save mid-write (truncated file, atomic-rename window) or the XML has a syntax error, the reloader silently skips that poll cycle — the existing UI stays live and the next poll retries. No crash, no stale state.

Override the default with `HELIX_HOT_RELOAD=0` (force off, e.g. for benchmarking) or `HELIX_HOT_RELOAD=1` (force on, e.g. on a device running a release build for live debugging). See [HELIX_HOT_RELOAD](ENVIRONMENT_VARIABLES.md#helix_hot_reload) for full details.

---

## UI Development

For layout work, styling fixes, and alternate screen layouts, the **[UI Contributor Guide](UI_CONTRIBUTOR_GUIDE.md)** is the primary reference. It covers breakpoints, design tokens, pre-themed widgets, layout overrides, and what needs work.

Key points for UI contributors:

- **XML layouts load at runtime + hot reload by default** — edit `ui_xml/*.xml`, save, see changes immediately without rebuilding or restarting
- **Design tokens are mandatory** — use `#space_md`, `#card_bg`, `<text_body>` instead of hardcoded values
- **7 breakpoint tiers** based on screen height: micro (≤272px), tiny (≤390px), small (≤460px), medium (≤550px), large (≤700px), xlarge (701–1000px), xxlarge (>1000px)
- **Layout overrides** let you provide alternate XML for ultrawide, portrait, or tiny screens without touching the standard layouts
- **Test at multiple sizes** with `-s WIDTHxHEIGHT`:
  ```bash
  ./build/bin/helix-screen --test -vv -s 480x320   # Tiny
  ./build/bin/helix-screen --test -vv -s 800x480   # Standard
  ./build/bin/helix-screen --test -vv -s 1920x480 --layout ultrawide
  ```

### Where UI files live

| Path | Contents |
|------|----------|
| `ui_xml/` | All XML layouts (230+ files) |
| `ui_xml/components/` | Reusable XML components |
| `ui_xml/ultrawide/` | Ultrawide layout overrides |
| `ui_xml/globals.xml` | Design tokens and global variables (shared, never override) |
| `config/themes/` | Theme JSON files (color palettes) |

---

## Worktrees

Use a worktree for any multi-file or risky change — rule of thumb: 4+ files, or
anything touching shutdown, threading, or the XML engine. A worktree isolates the
change (and its build) from main:

```bash
scripts/setup-worktree.sh feature/my-branch   # Creates in .worktrees/
```

What the script shares/symlinks for fast builds, ccache configuration, and
worktree cleanup: → [BUILD_SYSTEM.md](BUILD_SYSTEM.md) § "Git Worktrees".

---

## macOS WiFi Permission

Real WiFi scanning requires Location Services (network SSIDs reveal location).

**Easiest:** System Settings → Privacy & Security → Location Services → Enable Terminal

Without permission, app falls back to mock WiFi. Check with:
```bash
./build/bin/helix-screen --wizard -vv 2>&1 | grep -i "location\|wifi"
```

## Troubleshooting

```bash
make check-deps              # Check missing dependencies
make install-deps            # Auto-install
make clean && make V=1       # Verbose rebuild
```

**SDL2 not found:** `brew install sdl2` (macOS) or `sudo apt install libsdl2-dev` (Linux)

For complete troubleshooting, see **[BUILD_SYSTEM.md](BUILD_SYSTEM.md)**.

---

## Contributing

### First-Time Setup

```bash
make setup  # Configures pre-commit hook + commit template
```

The pre-commit hook auto-formats code (clang-format) and runs quality checks.

### Code Standards

**Class-based architecture required** for all new code:

```cpp
// ✅ CORRECT: Class-based panel
class MotionPanel : public PanelBase {
public:
    explicit MotionPanel(lv_obj_t* parent);
    void show() override;
};

// ❌ AVOID: Function-based (legacy)
void ui_panel_motion_init(lv_obj_t* parent);
```

**Naming conventions:**
- Functions/variables: `snake_case` (`ui_panel_home_init`, `temp_target`)
- XML files: `snake_case` (`history_dashboard_panel.xml`)
- Constants: `UPPER_SNAKE_CASE` (`MAX_TEMP`) — including `constexpr`, file-scope
  `static const`, and enum enumerators. **Not** Google's `kCamelCase`: the tree
  drifted into it for a while and was swept back, so a `kFoo` you find in a diff
  is a mistake to fix, not a precedent to follow. The exceptions are names that
  mirror a third-party API verbatim (Apple's `kCWSecurityWPA2Personal`) — match
  the foreign spelling there rather than inventing a local one.

**Namespace organization:** all HelixScreen code lives under `helix::` (UI
helpers in `helix::ui::`, sensor managers in `helix::sensors`):
- No `using` declarations in headers — always fully-qualified names in `.h`
  files; `using namespace helix;` is acceptable in `.cpp` files only
- Enums are `enum class` within `helix::` (e.g., `helix::PanelId`, `helix::PrintState`)
- JSON forward declarations come from `#include "json_fwd.h"`

**Critical patterns:**
```cpp
// Widget lookup: use names, not indices
lv_obj_t* label = lv_obj_find_by_name(panel, "temp_display");  // ✅
lv_obj_t* label = lv_obj_get_child(panel, 3);                   // ❌

// LVGL API: public only
lv_obj_get_x();        // ✅ Public
_lv_obj_mark_dirty();  // ❌ Private (underscore prefix)
```

**Copyright headers** (all new files):
```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
```

### Commit Messages

```
type(scope): description

Optional detailed explanation.
```

**Types:** `feat`, `fix`, `docs`, `style`, `refactor`, `test`, `chore`

**Examples:**
```
feat(ui): add temperature control overlay panel
fix(build): resolve SDL2 linking on macOS Sequoia
docs(readme): update build instructions
```

### Pull Requests

**Before submitting:**
1. Rebase on latest `main`
2. Test build and runtime
3. Update docs if patterns changed
4. Add screenshots for UI changes

**PR description includes:**
- What changed (summary)
- Why (context/problem solved)
- How to test
- Screenshots (if visual)
- Breaking changes (if any)

### Code Review Focus

- Architecture compliance (XML/Subject patterns)
- Error handling (logging, null checks)
- Performance (no blocking in UI thread)
- Documentation updated

---

## Installer Scripts

The installation system is modular POSIX shell (`scripts/lib/installer/`, 17
modules) bundled into the one-line `scripts/install.sh` for BusyBox-compatible
`curl | sh` distribution. Module structure, BusyBox rules, bundle regeneration,
and installer testing: → **[INSTALLER.md](INSTALLER.md)**.

---

## Related Documentation

- **[CONTRIBUTING.md](../../CONTRIBUTING.md)** - The front door for contributors
- **[ONBOARDING.md](ONBOARDING.md)** - Fresh checkout → first change
- **[YOUR_FIRST_CONTRIBUTION.md](YOUR_FIRST_CONTRIBUTION.md)** - Annotated walkthrough of a real contribution
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - The 15-minute whole-app model + routing into the chapter series
- **[UI Contributor Guide](UI_CONTRIBUTOR_GUIDE.md)** - Start here for UI/layout work
- **[BUILD_SYSTEM.md](BUILD_SYSTEM.md)** - Complete build reference
- **[INSTALLER.md](INSTALLER.md)** - Installer system
- **[LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md)** - XML syntax reference
- **[DEVELOPER_QUICK_REFERENCE.md](DEVELOPER_QUICK_REFERENCE.md)** - Common patterns
- **[TESTING.md](TESTING.md)** - Test infrastructure and Catch2 usage
- **[LOGGING.md](LOGGING.md)** - Log levels and when to use each
