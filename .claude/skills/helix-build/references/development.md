# Development Guide

## Environment Setup

### macOS
```bash
brew install cmake bear imagemagick python3 node shellcheck bats-core
npm install && make venv-setup
```
Minimum: macOS 10.15 (Catalina) for CoreWLAN/CoreLocation WiFi APIs.

### Debian/Ubuntu
```bash
sudo apt install cmake bear imagemagick python3 python3-venv clang make npm \
    shellcheck bats libnl-3-dev libnl-genl-3-dev libssl-dev
npm install && make venv-setup
```

### Dependencies

| Category | Components | Notes |
|----------|------------|-------|
| Required | clang, cmake 3.16+, make, python3, node/npm | Core build tools |
| Auto-built | SDL2, spdlog, libhv | From submodules if not system-installed |
| Always submodule | lvgl | Project-specific patches required |
| Optional | bear, imagemagick, shellcheck, bats-core | IDE, screenshots, linting, testing |

```bash
make check-deps      # Check what's missing
make install-deps    # Auto-install (interactive)
```

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

Test mode keyboard shortcuts: S=screenshot, P=test prompt, N=test notification, Q/Esc=quit

## Logging

### Verbosity Levels

| Level | Flag | Use For |
|-------|------|---------|
| WARN | (default) | Errors and warnings only |
| INFO | `-v` | User-visible milestones |
| DEBUG | `-vv` | Troubleshooting, summaries |
| TRACE | `-vvv` | Per-item loops, wire protocol |

### Log Destinations
```bash
./build/bin/helix-screen --log-dest=console  # Default on macOS
./build/bin/helix-screen --log-dest=journal  # systemd journal
./build/bin/helix-screen --log-dest=file --log-file=/tmp/helix.log
```

### Code Usage
Always use spdlog — never printf/cout/LV_LOG_*:
```cpp
spdlog::info("[ComponentName] Message: {}", value);
spdlog::debug("[Theme] Registered {} items", count);
```

## DPI & Hardware Profiles

```bash
./build/bin/helix-screen --dpi 170  # 7" @ 1024x600 (BTT Pad 7)
./build/bin/helix-screen --dpi 187  # 5" @ 800x480
./build/bin/helix-screen --dpi 201  # 4.3" @ 720x480 (AD5M)
```

| Hardware | Resolution | DPI |
|----------|------------|-----|
| Reference | — | 160 |
| 7" LCD | 1024×600 | 170 |
| 5" LCD | 800×480 | 187 |
| AD5M | 720×480 | 201 |

## Daily Workflow

1. **Edit code** in `src/` or `include/`
2. **Edit XML** in `ui_xml/` — no rebuild needed (hot reload)
3. **Build** with `make -j`
4. **Test** with `./build/bin/helix-screen --test -vv [panel]`
5. **Screenshot** with S key or `./scripts/screenshot.sh`
6. **Commit** working incremental changes

### XML vs C++ Changes

| Change Type | Location | Rebuild? | Hot Reload? |
|-------------|----------|----------|-------------|
| Layout, styling, colors | `ui_xml/*.xml` | No | Yes — auto-detected |
| Logic, bindings, handlers | `src/*.cpp`, `include/*.h` | Yes | No |
| Theme colors | `config/themes/*.json` | No (restart) | No |
| Translations | `config/strings/*.yaml` | Yes (codegen) | No |

### XML Hot Reload
```bash
HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv
```

## Code Standards

### Class-based Architecture
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

### Naming
- Functions/variables: `snake_case`
- XML files: `kebab-case`
- Constants: `UPPER_SNAKE_CASE`

### Copyright Headers (all new files)
```cpp
// Copyright 2025 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
```

### Commit Messages
```
type(scope): description

Optional detailed explanation.
```
Types: `feat`, `fix`, `docs`, `style`, `refactor`, `test`, `chore`

## Screenshots

```bash
# Interactive: Press 'S' in running UI
./scripts/screenshot.sh helix-screen output-name [panel] [options]
HELIX_SCREENSHOT_DISPLAY=0 ./scripts/screenshot.sh helix-screen test home
```
Output: `/tmp/ui-screenshot-[name].png`

## macOS WiFi Permission

Real WiFi scanning requires Location Services access (SSIDs reveal location).

System Settings → Privacy & Security → Location Services → Enable Terminal

Without permission, falls back to mock WiFi.

## Worktrees

```bash
scripts/setup-worktree.sh feature/my-branch   # Creates in .worktrees/
# Symlinks lib/, precompiled headers, tools for fast builds
```
