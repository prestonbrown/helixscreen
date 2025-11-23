# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 📚 Lazy Documentation Loading

**DO NOT read these at startup. Load ONLY when actively working on the topic:**
- `docs/LVGL9_XML_GUIDE.md` (2,488 lines) - When modifying XML layouts or debugging XML issues
- `docs/MOONRAKER_SECURITY_REVIEW.md` (2,088 lines) - When working on Moonraker/security
- `docs/WIFI_WPA_SUPPLICANT_MIGRATION.md` (1,260 lines) - When working on WiFi features
- `docs/BUILD_SYSTEM.md` (653 lines) - When troubleshooting builds or modifying Makefile
- `docs/DOXYGEN_GUIDE.md` - When documenting APIs
- `docs/CI_CD_GUIDE.md` - When modifying GitHub Actions

**Philosophy:** Start lean, read docs on-demand. Git history > development journals.

---

## 🤖 Agent Delegation Policy

**CRITICAL: Check this BEFORE starting ANY task. Delegate to agents rather than doing work yourself.**

### Task → Agent Mapping (USE THESE)

| Task Type | Agent | When |
|-----------|-------|------|
| **UI/XML** | `widget-maker` | ANY XML/LVGL work - NO EXCEPTIONS |
| **UI Review** | `ui-reviewer` | XML audits, LVGL pattern validation |
| **Moonraker** | `moonraker-agent` | WebSocket, API, printer commands, state management |
| **Testing** | `test-harness-agent` | Unit tests, mocking, CI/CD |
| **Build issues** | `cross-platform-build-agent` | Dependencies, Makefile, compilation |
| **G-code/Files** | `gcode-preview-agent` | G-code handling, thumbnails, file browser |
| **Codebase exploration** | `Explore` agent (quick/medium/thorough) | "How does X work?", "Where is Y?" |
| **Multi-file refactor** | `general-purpose` agent | Changes across 3+ files |
| **Security review** | `critical-reviewer` | Paranoid code review |

**See ~/.claude/CLAUDE.md for threshold rules. When in doubt, delegate.**

---

## ⚠️ CRITICAL RULES - CHECK BEFORE CODING ⚠️

**These rules are MANDATORY and frequently violated. Check this list before proposing any code changes.**

| # | Rule | ❌ Wrong | ✅ Correct | Why | Reference |
|---|------|----------|-----------|-----|-----------|
| 1 | **NO hardcoded colors/dimensions** | `lv_color_hex(0xE0E0E0)` | `ui_theme_parse_color(lv_xml_get_const("card_border"))` | Theme changes w/o recompile, dark/light mode | `ui_card.cpp:39-59` |
| 2 | **Reference existing patterns** | Inventing new approach | Study `motion_panel.xml` / `ui_panel_motion.cpp` first | Consistency, avoid bugs | ARCHITECTURE.md |
| 3 | **Use spdlog only** | `printf()`, `cout`, `LV_LOG_*` | `spdlog::info("temp: {}", t)` | Configurable verbosity: `-v`=info, `-vv`=debug, `-vvv`=trace | CONTRIBUTING.md |
| 4 | **NO auto-mock fallbacks** | `if(!start()) return Mock()` | Check `RuntimeConfig::should_mock_*()`, fail gracefully | Production security, no fake data | - |
| 5 | **Read docs BEFORE coding** | Start coding immediately | Read LVGL9_XML_GUIDE.md, BUILD_SYSTEM.md for area | Avoid known gotchas | docs/ |
| 6 | **Use `make -j` (auto-detect)** | `make -j4`, `make -j8` | `make -j` (no number) | Auto-detects cores via NPROC, works on any system | Makefile:202,215 |
| 7 | **MANDATORY RAII for widgets** | `lv_malloc()` / `lv_free()` | `lvgl_make_unique<T>()` + `release()` | Exception safety, prevent memory leaks | ARCHITECTURE.md, `ui_widget_memory.h` |

---

## Project Overview

This is the **LVGL 9 UI Prototype** for HelixScreen - a declarative XML-based touch UI system using LVGL 9.4 with reactive Subject-Observer data binding. The prototype runs on SDL2 for rapid development and will eventually target framebuffer displays on embedded hardware.

**Key Innovation:** Complete separation of UI layout (XML) from business logic (C++), similar to modern web frameworks. No manual widget management - all updates happen through reactive subjects.

## Quick Start

**See DEVELOPMENT.md for complete setup instructions.**

**Essential dependencies**: cmake, clang/gcc, make, python3, npm
**Install**: `brew install cmake` (macOS) or `sudo apt install cmake` (Linux), then `npm install`
**Note**: SDL2, spdlog, libhv are optional - will be built from submodules if not system-installed

**Build & Run**:

```bash
make -j                          # Parallel incremental build (daily development)
make build                       # Clean parallel build with progress/timing
./build/bin/helix-ui-proto       # Run (default: home panel, small screen)
./build/bin/helix-ui-proto -p motion -s large  # Specific panel/size
```

**Common commands**:
- `make -j` - Parallel incremental build (auto-detects cores)
- `make build` - Clean build from scratch
- `make help` - Show all targets
- **NEVER invoke compilers directly** - always use `make`

**Binary**: `build/bin/helix-ui-proto`
**Panels**: home, controls, motion, nozzle-temp, bed-temp, extrusion, filament, settings, advanced, print-select

**Screenshots**:
```bash
# Interactive: Press 'S' in running UI
./build/bin/helix-ui-proto

# Automated: Script takes screenshot after 2s, quits after 3s
./scripts/screenshot.sh helix-ui-proto output-name [panel_name]
./scripts/screenshot.sh helix-ui-proto home home
./scripts/screenshot.sh helix-ui-proto motion motion -s small
```

See **DEVELOPMENT.md** section "Screenshot Workflow" for complete details.

## Architecture

**See ARCHITECTURE.md for complete system design.**

**Core Pattern:** XML (layout) → Subjects (reactive data) → C++ (logic). No hardcoded colors - all in `globals.xml` with `*_light`/`*_dark` variants.

**Pluggable Backends:** Manager → Abstract Interface → Platform-specific implementations (macOS/Linux/Mock). Factory pattern selects at runtime. Used for WiFi, Ethernet, future hardware integrations.

## LVGL 9.4 API Changes

**Upgraded from v9.3.0 to v9.4.0** (2025-10-28)

### C++ API Renames

```cpp
// OLD (v9.3):
lv_xml_component_register_from_file("A:/ui_xml/globals.xml");
lv_xml_widget_register("widget_name", create_cb, apply_cb);

// NEW (v9.4):
lv_xml_register_component_from_file("A:/ui_xml/globals.xml");
lv_xml_register_widget("widget_name", create_cb, apply_cb);
```

**Pattern:** All XML registration functions now use `lv_xml_register_*` prefix for consistency.

### XML Event Syntax Change

```xml
<!-- OLD (v9.3): -->
<lv_button>
    <lv_event-call_function trigger="clicked" callback="my_callback"/>
</lv_button>

<!-- NEW (v9.4): -->
<lv_button>
    <event_cb trigger="clicked" callback="my_callback"/>
</lv_button>
```

**Why:** The event callback is now a proper child element (`access="add"` in schema), not a standalone widget tag. This aligns with LVGL's pattern where child elements use simple names.

### Object Alignment Values

```xml
<!-- CORRECT: -->
<lv_obj align="left_mid"/>    <!-- Object positioning -->
<lv_label style_text_align="left"/>  <!-- Text alignment within object -->

<!-- WRONG: -->
<lv_obj align="left"/>  <!-- "left" is not a valid lv_align_t value -->
```

**Valid align values:** `left_mid`, `right_mid`, `top_left`, `top_mid`, `top_right`, `bottom_left`, `bottom_mid`, `bottom_right`, `center`

## Critical Patterns (Project-Specific)

**⚠️ ALWAYS reference existing implementations:** Study `motion_panel.xml`/`ui_panel_motion.cpp` or `nozzle_temp_panel.xml`/`ui_panel_controls_temp.cpp` before implementing new features.

| # | Pattern | Key Point | Reference |
|---|---------|-----------|-----------|
| 1 | **Subject init order** | Register components → init subjects → create XML | ARCHITECTURE.md |
| 2 | **Component names** | Always add explicit `name="..."` to component tags in XML | LVGL9_XML_GUIDE.md |
| 3 | **Widget lookup** | Use `lv_obj_find_by_name()` not `lv_obj_get_child(idx)` | QUICK_REFERENCE.md |
| 4 | **Copyright headers** | GPL v3 header required in all new source files | COPYRIGHT_HEADERS.md |
| 5 | **Image scaling** | Call `lv_obj_update_layout()` before scaling (deferred layout) | ui_utils.h |
| 6 | **Nav history** | Use `ui_nav_push_overlay()`/`ui_nav_go_back()` for overlays | ui_nav.h:54-62 |
| 7 | **Public API only** | Never use `_lv_*()` or private LVGL interfaces | - |
| 8 | **API docs** | Doxygen `@brief`/`@param`/`@return` required before commit | DOXYGEN_GUIDE.md |

## Common Gotchas

**⚠️ READ DOCS FIRST:** See **docs/LVGL9_XML_GUIDE.md** (XML/layouts/bindings), **docs/QUICK_REFERENCE.md** (patterns/icons), **docs/BUILD_SYSTEM.md** (patches), **ARCHITECTURE.md** (patterns)

**Top 5 Gotchas:**
1. **No `flag_` prefix** - Use `hidden="true"` not `flag_hidden="true"` (see LVGL9_XML_GUIDE.md "Troubleshooting")
2. **Conditional bindings = child elements** - Use `<lv_obj-bind_flag_if_eq>` not attributes (see LVGL9_XML_GUIDE.md "Data Binding")
3. **Three flex properties** - `style_flex_main_place` + `style_flex_cross_place` + `style_flex_track_place`, never `flex_align` (see LVGL9_XML_GUIDE.md "Layouts")
4. **Subject conflicts** - Don't declare subjects in `globals.xml` (see ARCHITECTURE.md)
5. **Component names = filename** - `nozzle_temp_panel.xml` → component name is `nozzle_temp_panel` (see QUICK_REFERENCE.md)

## Documentation Structure

📋 **docs/HANDOFF.md** - Active work + next 3 priorities (≤150 lines, prune aggressively)
🗺️ **docs/ROADMAP.md** - Future features and milestones
📖 **README.md** - Project overview and quick start (root level)
🔧 **docs/DEVELOPMENT.md** - Build system and daily workflow
🏗️ **docs/ARCHITECTURE.md** - System design and patterns
✅ **docs/CONTRIBUTING.md** - Code standards and git workflow

**Technical Reference (lazy-load only when needed):**
📘 **docs/LVGL9_XML_GUIDE.md** - Complete XML reference
⚡ **docs/QUICK_REFERENCE.md** - Common code patterns
🔨 **docs/BUILD_SYSTEM.md** - Makefile and patches
🧪 **docs/TESTING.md** - Test infrastructure and Catch2 usage
©️ **docs/COPYRIGHT_HEADERS.md** - GPL v3 headers
🚀 **docs/CI_CD_GUIDE.md** - GitHub Actions

## File Organization

```
helixscreen/
├── src/              # C++ business logic
├── include/          # Headers
├── lib/              # External libraries and dependencies
│   ├── lvgl/         # LVGL 9.4 UI library (submodule)
│   ├── libhv/        # HTTP/WebSocket library (submodule)
│   ├── spdlog/       # Logging library (submodule)
│   ├── sdl2/         # SDL2 for development (submodule)
│   ├── glm/          # OpenGL Mathematics (submodule)
│   ├── openvdb/      # VDB library (submodule)
│   ├── wpa_supplicant/ # WiFi management (submodule)
│   └── tinygl/       # Software 3D rasterizer (local)
├── ui_xml/           # XML component definitions
├── assets/           # Fonts, images, icons
├── config/           # Configuration templates and data
│   ├── helixconfig.json.template
│   ├── printer_database.json
│   └── printing_tips.json
├── scripts/          # Build/screenshot automation
├── docs/             # All documentation files
└── Makefile          # Build system
```

## Development Workflow

**Session startup:** Always check `git log --oneline -10` and `git status` to understand recent work and avoid repeating failed approaches.

**Daily cycle:** Edit XML (no recompile) or C++ → `make` → test → screenshot (press 'S' or use `scripts/screenshot.sh`)

**For context:** HANDOFF.md (active work), ROADMAP.md (future features), `git log` (history)