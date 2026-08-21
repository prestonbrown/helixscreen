# HelixScreen Documentation

Welcome to the HelixScreen documentation. Choose your path:

---

## For Users

**Installing and using HelixScreen with pre-built packages.**

| Document | Description |
|----------|-------------|
| [**Installation Guide**](user/INSTALL.md) | Get HelixScreen running on your display |
| [**User Guide**](user/USER_GUIDE.md) | Learn how to use the interface (includes multi-printer management) |
| [**Configuration**](user/CONFIGURATION.md) | All settings explained |
| [**Troubleshooting**](user/TROUBLESHOOTING.md) | Solutions to common problems |
| [**FAQ**](user/FAQ.md) | Quick answers to common questions |
| [**Telemetry**](user/TELEMETRY.md) | What telemetry collects, privacy controls, opt-in/out |
| [**Feature Guides**](user/guide/) | 24 per-feature guides: printing, calibration, filament, sensors, camera, security, and more |
| [**Plugin Development**](devel/PLUGIN_DEVELOPMENT.md) | Create custom plugins |

---

## For Developers

**Building from source and contributing to HelixScreen.**

| Document | Description |
|----------|-------------|
| [**Development Guide**](devel/DEVELOPMENT.md) | Build system, workflow, and contributing |
| [**UI Contributor Guide**](devel/UI_CONTRIBUTOR_GUIDE.md) | **Start here for UI/layout work** — breakpoints, tokens, widgets, overrides |
| [**Your First Contribution**](devel/YOUR_FIRST_CONTRIBUTION.md) | Annotated walkthrough of a real settings overlay, plus pattern tour of a full subsystem |
| [**Contributor Gotchas**](devel/CONTRIBUTOR_GOTCHAS.md) | "If you see X, you forgot Y" — symptom-indexed troubleshooting for common traps |
| [**Architecture**](devel/ARCHITECTURE.md) | Whole-app model + routing to the 15-chapter guide (`devel/architecture/`) |
| [**Build System**](devel/BUILD_SYSTEM.md) | Makefile, cross-compilation, patches |
| [**Testing**](devel/TESTING.md) | Test infrastructure and Catch2 usage |

---

## Technical Reference

**API documentation and implementation details.**

| Document | Description |
|----------|-------------|
| [**LVGL 9 XML Guide**](devel/LVGL9_XML_GUIDE.md) | Complete XML syntax reference (92K) |
| [**LVGL XML Fork Situation**](devel/LVGL_XML_SITUATION.md) | Where `lib/helix-xml/` came from, its MIT position, and the clean-room rule |
| [**Quick Reference**](devel/DEVELOPER_QUICK_REFERENCE.md) | Common patterns and code snippets |
| [**Modal System**](devel/MODAL_SYSTEM.md) | ui_dialog, modal_button_row, Modal pattern |
| [**Environment Variables**](devel/ENVIRONMENT_VARIABLES.md) | All runtime and build env vars |
| [**Mock & Testing Env Vars**](devel/MOCK_ENVIRONMENT_VARIABLES.md) | The `HELIX_MOCK_*` matrix and test-harness variables (`--test` runs, replay, forced modals) |
| [**Moonraker Architecture**](devel/MOONRAKER_ARCHITECTURE.md) | Moonraker integration details |
| [**RPC Error Ownership**](devel/RPC_ERROR_OWNERSHIP.md) | Which surface reports a failed JSON-RPC call — caller UI, generic fallback, or the `!!` router |
| [**Theme System**](devel/THEME_SYSTEM.md) | Reactive theming, color tokens, responsive sizing |
| [**Theme Contributor Guide**](devel/THEME_CONTRIBUTOR_GUIDE.md) | For people creating themes — JSON schema, palette design, no C++ needed |
| [**Layout System**](devel/LAYOUT_SYSTEM.md) | Alternative layouts, auto-detection, CLI override, home widget grid |
| [**Page Scroll Buttons**](devel/PAGE_SCROLL_BUTTONS.md) | Chevron gutter: auto-attach policy, why it stops at a home widget tile, ESP32-only default |
| [**Translation System**](devel/TRANSLATION_SYSTEM.md) | i18n: YAML → code generation, runtime lookups |
| [**Translation Contributor Guide**](devel/TRANSLATION_CONTRIBUTOR_GUIDE.md) | For translators — improve existing languages or add a new one, no code needed |
| [**UI Testing**](devel/UI_TESTING.md) | Headless LVGL testing, UITest utilities |
| [**Logging Guidelines**](devel/LOGGING.md) | Log levels and message format |
| [**Copyright Headers**](devel/COPYRIGHT_HEADERS.md) | SPDX license requirements |
| [**Config Migration**](devel/CONFIG_MIGRATION.md) | Versioned schema migration system |

### Feature Systems

| Document | Description |
|----------|-------------|
| [**Label Printer System**](devel/LABEL_PRINTER_SYSTEM.md) | Brother QL, Phomemo, Niimbot, MakeID; USB/TCP/Bluetooth |
| [**Filament Management**](devel/FILAMENT_MANAGEMENT.md) | Shared filament-system architecture: multi-backend, dispatch ladder, slot metadata, endless spool, dryer, device ops |
| **Filament Backend Leaves** | Per-backend docs: [AFC](devel/FILAMENT_BACKEND_AFC.md), [Happy Hare](devel/FILAMENT_BACKEND_HAPPY_HARE.md), [ACE](devel/FILAMENT_BACKEND_ACE.md), [Tool Changer](devel/FILAMENT_BACKEND_TOOLCHANGER.md), [AD5X IFS](devel/FILAMENT_BACKEND_AD5X_IFS.md), [CFS](devel/FILAMENT_BACKEND_CFS.md), [QIDI Box](devel/FILAMENT_BACKEND_QIDI_BOX.md) |
| [**Filament Slot Metadata (internal)**](devel/FILAMENT_SLOT_METADATA.md) | `FilamentSlotOverrideStore` implementation: per-backend hooks, hardware-event clearing, cache, migration |
| [**Creality CFS Internals**](devel/CREALITY_CFS_INTERNALS.md) | K1-family CFS box-wrapper RE reference: `BOX_*` semantics, the printer-side tn_data.json userdata, deferred-failure/resume traps |
| [**Filament Slots Spec (public)**](specs/filament_slots.md) | Wire-format convention for the `lane_data` Moonraker DB namespace — readable by any third party |
| [**Input Shaper & PID**](devel/INPUT_SHAPER.md) | Calibration, frequency response charts, CSV parser |
| [**Preprint Prediction**](devel/PREPRINT_PREDICTION.md) | ETA prediction engine, phase timing, history |
| [**Exclude Objects**](devel/EXCLUDE_OBJECTS.md) | Object exclusion, thumbnails, slicer setup |
| [**Print Start Profiles**](devel/PRINT_START_PROFILES.md) | Print start phase detection, profiles |
| [**Print Start Observers**](devel/PRINT_START_OBSERVERS.md) | Pre-print observer system: signal sources, threading, tests |
| [**Print Start Integration**](devel/PRINT_START_INTEGRATION.md) | User-facing macro setup guide |
| [**Update System**](devel/UPDATE_SYSTEM.md) | Channels, R2 CDN, downloads, Moonraker updater |
| [**Sound System**](devel/SOUND_SYSTEM.md) | Audio architecture, JSON themes, backends |
| [**LED Control**](devel/LED_CONTROL.md) | LED system: 5 backends, auto-state lighting, overlays |
| [**Printer Manager**](devel/PRINTER_MANAGER.md) | Printer overlay, custom images, inline editing |
| [**Timelapse**](devel/TIMELAPSE.md) | Moonraker timelapse plugin integration |
| [**Crash Reporter**](devel/CRASH_REPORTER.md) | Crash detection, delivery pipeline, CF Worker |
| [**HelixPrint Plugin**](../moonraker-plugin/README.md) | Phase tracking Moonraker plugin |

### Platform Support

| Document | Description |
|----------|-------------|
| [**Installer**](devel/INSTALLER.md) | Installation system, KIAUH, platforms, shell tests |
| [**QIDI Support**](devel/printers/QIDI_SUPPORT.md) | QIDI 3-series / 4-series platform guide (Q2, Max 4) |
| [**Snapmaker U1 Support**](devel/printers/SNAPMAKER_U1_SUPPORT.md) | Snapmaker U1 toolchanger platform guide |
| [**Creality K2 Support**](devel/printers/CREALITY_K2_SUPPORT.md) | Creality K2 series platform guide |
| [**FlashForge AD5X Support**](devel/printers/FLASHFORGE_AD5X_SUPPORT.md) | FlashForge Adventurer 5X (MIPS, ZMOD) |

---

## Planning & Status

**Project roadmap and feature tracking.**

| Document | Description |
|----------|-------------|
| [**Roadmap**](devel/ROADMAP.md) | Feature timeline and milestones |
| [**In-Flight Plans**](devel/plans/) | In-flight plans and specs — point-in-time, not current truth |

---

## Audits & Reports

**Security reviews and quality assessments.**

| Document | Description |
|----------|-------------|
| [**Safety Audit**](audits/SAFETY_AUDIT.md) | User-facing safety: printer damage, self-harm, misleading state |
| [**Security Review**](audits/MOONRAKER_SECURITY_REVIEW.md) | Moonraker security assessment |
| [**Memory Analysis**](audits/MEMORY_ANALYSIS.md) | Memory profiling and optimization |
| [**Test Coverage**](audits/TEST_COVERAGE_REPORT.md) | Test coverage report |
| [**Moonraker Audit Summary**](audits/MOONRAKER_AUDIT_SUMMARY.md) | Moonraker client code-quality audit |
| [**Moonraker Client Test Results**](audits/MOONRAKER_CLIENT_TEST_RESULTS.md) | Moonraker client robustness testing |

---

## Documentation Map

```
docs/
├── README.md                 # This file - documentation index
├── CLAUDE.md                 # Documentation routing guide
│
├── user/                     # END-USER DOCUMENTATION
│   ├── CLAUDE.md             # Style guide for user docs
│   ├── INSTALL.md            # Installation guide
│   ├── USER_GUIDE.md         # How to use HelixScreen
│   ├── CONFIGURATION.md      # Settings reference
│   ├── TROUBLESHOOTING.md    # Common problems
│   ├── FAQ.md                # Frequently asked questions
│   └── guide/                # 24 per-feature guides (+ guide/settings/)
│
├── devel/                    # DEVELOPER DOCUMENTATION
│   ├── CLAUDE.md             # Full developer doc index
│   ├── DEVELOPMENT.md        # Developer setup, contributing
│   ├── ARCHITECTURE.md       # Architecture router (whole-app model)
│   ├── architecture/         # 15-chapter architecture guide
│   ├── BUILD_SYSTEM.md       # Build internals
│   ├── TESTING.md            # Test infrastructure
│   ├── ROADMAP.md            # Feature timeline + docs-debt backlog
│   ├── plans/                # In-flight plans and specs (single home)
│   ├── printers/             # Platform guides (K1, K2, QIDI, U1, AD5X)
│   ├── printer-research/     # Reverse-engineering notes
│   └── ...                   # 70+ more dev docs (index: devel/CLAUDE.md)
│
├── specs/                    # PUBLIC, VENDOR-NEUTRAL CONVENTION SPECS
│   ├── CLAUDE.md             # Specs subtree routing
│   └── filament_slots.md     # lane_data Moonraker DB convention
│
├── audits/                   # 6 audit and review reports
├── store/android/            # Play Store listing assets
└── images/                   # Screenshots (+ images/user/)

moonraker-plugin/
└── README.md                 # HelixPrint plugin docs
```

---

## Quick Start

**I want to...**

| Goal | Start Here |
|------|------------|
| Install HelixScreen | [Installation Guide](user/INSTALL.md) |
| Use HelixScreen | [User Guide](user/USER_GUIDE.md) |
| Build from source | [Development Guide](devel/DEVELOPMENT.md) |
| Contribute code | [Development Guide - Contributing](devel/DEVELOPMENT.md#contributing) |
| Set up a fresh checkout and first build | [Onboarding](devel/ONBOARDING.md) |
| Fix layouts / contribute UI | [UI Contributor Guide](devel/UI_CONTRIBUTOR_GUIDE.md) |
| Write my first contribution | [Your First Contribution](devel/YOUR_FIRST_CONTRIBUTION.md) |
| Debug "my change did nothing" | [Contributor Gotchas](devel/CONTRIBUTOR_GOTCHAS.md) |
| Create XML layouts | [LVGL 9 XML Guide](devel/LVGL9_XML_GUIDE.md) |
| Understand the architecture | [Architecture router](devel/ARCHITECTURE.md) → chapter series |
| Cross-compile for Pi | [Build System - Cross-Compilation](devel/BUILD_SYSTEM.md#cross-compilation-embedded-targets) |

---

## Community

**[Join the HelixScreen Discord](https://discord.gg/RZCT2StKhr)** — Get help, share your setup, and follow development.

**Bug Reports & Feature Requests:** [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues)

---

*Back to [Project README](../README.md)*
