# CLAUDE.md

## 📚 Lazy Documentation Loading

**Load ONLY when actively working on the topic:**
| Doc | When |
|-----|------|
| `docs/LVGL9_XML_GUIDE.md` | Modifying XML layouts, debugging XML |
| `docs/MOONRAKER_SECURITY_REVIEW.md` | Moonraker/security work |
| `docs/WIFI_WPA_SUPPLICANT_MIGRATION.md` | WiFi features |
| `docs/BUILD_SYSTEM.md` | Build troubleshooting, Makefile changes |
| `docs/DOXYGEN_GUIDE.md` | Documenting APIs |
| `docs/CI_CD_GUIDE.md` | GitHub Actions |

---

## 🤖 Agent Delegation

**Use agents for complex/multi-step work. Handle simple single-file ops directly.**

| Task Type | Agent | When |
|-----------|-------|------|
| UI/XML | `widget-maker` | XML/LVGL changes beyond trivial edits |
| UI Review | `ui-reviewer` | XML audits, LVGL pattern validation |
| Moonraker | `moonraker-agent` | WebSocket, API, printer commands, state |
| Testing | `test-harness-agent` | Unit tests, mocking, CI/CD |
| Build issues | `cross-platform-build-agent` | Makefile, compilation, linking |
| G-code/Files | `gcode-preview-agent` | G-code handling, thumbnails, file browser |
| Codebase exploration | `Explore` (quick/medium/thorough) | "How does X work?", "Where is Y?" |
| Multi-file refactor | `general-purpose` | Changes across 3+ files |
| Security review | `critical-reviewer` | Paranoid code review |

---

## ⚠️ CRITICAL RULES

**These are frequently violated. Check before coding.**

| # | Rule | ❌ Wrong | ✅ Correct |
|---|------|----------|-----------|
| 1 | No hardcoded colors | `lv_color_hex(0xE0E0E0)` | `ui_theme_parse_color(lv_xml_get_const("card_border"))` |
| 2 | Reference existing patterns | Inventing new approach | Study `motion_panel.xml` / `ui_panel_motion.cpp` first |
| 3 | spdlog only | `printf()`, `cout`, `LV_LOG_*` | `spdlog::info("temp: {}", t)` |
| 4 | No auto-mock fallbacks | `if(!start()) return Mock()` | Check `RuntimeConfig::should_mock_*()` |
| 5 | Read docs BEFORE coding | Start coding immediately | Read relevant guide for the area first |
| 6 | `make -j` (no number) | `make -j4`, `make -j8` | `make -j` auto-detects cores |
| 7 | RAII for widgets | `lv_malloc()` / `lv_free()` | `lvgl_make_unique<T>()` + `release()` |
| 8 | SPDX headers | 20-line GPL boilerplate | `// SPDX-License-Identifier: GPL-3.0-or-later` |
| 9 | Class-based architecture | `ui_panel_*_init()` functions | Classes: `MotionPanel`, `WiFiManager` |
| 10 | Clang-format | Inconsistent formatting | Let pre-commit hook fix it |

---

## Project Overview

**HelixScreen** - A best-in-class Klipper touchscreen UI designed for a variety of 3D printers. Built with LVGL 9.4 using declarative XML layouts and reactive Subject-Observer data binding. Runs on SDL2 for development, targets framebuffer displays on embedded hardware.

**Core pattern:** XML (layout) → Subjects (reactive data) → C++ (logic). No hardcoded colors - use `globals.xml` with `*_light`/`*_dark` variants.

**Pluggable backends:** Manager → Abstract Interface → Platform implementations (macOS/Linux/Mock). Factory pattern at runtime.

---

## Quick Start

```bash
make -j                              # Incremental build
./build/bin/helix-ui-proto           # Run (default: home panel, small screen)
./build/bin/helix-ui-proto -p motion -s large
./build/bin/helix-ui-proto --test    # Mock printer (REQUIRED without real printer!)
```

**⚠️ IMPORTANT:** Always use `--test` when testing without a real printer. Without it, panels expecting printer data show nothing.

**Panels:** home, controls, motion, nozzle-temp, bed-temp, extrusion, filament, settings, advanced, print-select

**Screenshots:** Press 'S' in UI, or `./scripts/screenshot.sh helix-ui-proto output-name [panel]`

---

## LVGL 9.4 API Changes (from 9.3)

```cpp
// Registration renamed:
lv_xml_register_component_from_file()  // was: lv_xml_component_register_from_file
lv_xml_register_widget()               // was: lv_xml_widget_register
```

```xml
<!-- Event syntax: -->
<event_cb trigger="clicked" callback="my_callback"/>  <!-- was: lv_event-call_function -->

<!-- Valid align values (NOT just "left"): -->
<!-- left_mid, right_mid, top_left, top_mid, top_right, bottom_left, bottom_mid, bottom_right, center -->
```

---

## Critical Patterns

| Pattern | Key Point |
|---------|-----------|
| Subject init order | Register components → init subjects → create XML |
| Component names | Always add explicit `name="..."` to component tags |
| Widget lookup | `lv_obj_find_by_name()` not `lv_obj_get_child(idx)` |
| Copyright headers | SPDX header required in all new source files |
| Image scaling | Call `lv_obj_update_layout()` before scaling |
| Nav history | `ui_nav_push_overlay()`/`ui_nav_go_back()` for overlays |
| Public API only | Never use `_lv_*()` private LVGL interfaces |
| API docs | Doxygen `@brief`/`@param`/`@return` required |

---

## Common Gotchas

1. **No `flag_` prefix** - Use `hidden="true"` not `flag_hidden="true"`
2. **Conditional bindings = child elements** - `<lv_obj-bind_flag_if_eq>` not attributes
3. **Three flex properties** - `style_flex_main_place` + `style_flex_cross_place` + `style_flex_track_place`
4. **Subject conflicts** - Don't declare subjects in `globals.xml`
5. **Component names = filename** - `nozzle_temp_panel.xml` → component name is `nozzle_temp_panel`

---

## Documentation

**Core docs:**
- `README.md` - Project overview
- `docs/DEVELOPMENT.md` - Build system, daily workflow
- `docs/ARCHITECTURE.md` - System design, patterns
- `docs/CONTRIBUTING.md` - Code standards, git workflow
- `docs/ROADMAP.md` - Future features

**Reference (load when needed):**
- `docs/LVGL9_XML_GUIDE.md` - Complete XML reference
- `docs/QUICK_REFERENCE.md` - Common code patterns
- `docs/BUILD_SYSTEM.md` - Makefile, patches
- `docs/TESTING.md` - Catch2, test infrastructure
- `docs/COPYRIGHT_HEADERS.md` - SPDX headers

---

## File Organization

```
helixscreen/
├── src/              # C++ business logic
├── include/          # Headers
├── lib/              # External libs (lvgl, libhv, spdlog, sdl2, tinygl, etc.)
├── ui_xml/           # XML component definitions
├── assets/           # Fonts, images, icons
├── config/           # Config templates (helixconfig.json.template, printer_database.json)
├── scripts/          # Build/screenshot automation
├── docs/             # Documentation
└── Makefile          # Build system
```

---

## Development Workflow

**Session startup:** Check `git log --oneline -10` and `git status` to understand recent work.

**Daily cycle:** Edit XML (no recompile) or C++ → `make -j` → test → screenshot
