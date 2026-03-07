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
| [**Plugin Development**](devel/PLUGIN_DEVELOPMENT.md) | Create custom plugins |

---

## For Developers

**Building from source and contributing to HelixScreen.**

| Document | Description |
|----------|-------------|
| [**Development Guide**](devel/DEVELOPMENT.md) | Build system, workflow, and contributing |
| [**UI Contributor Guide**](devel/UI_CONTRIBUTOR_GUIDE.md) | **Start here for UI/layout work** — breakpoints, tokens, widgets, overrides |
| [**Architecture**](devel/ARCHITECTURE.md) | System design and patterns |
| [**Build System**](devel/BUILD_SYSTEM.md) | Makefile, cross-compilation, patches |
| [**Testing**](devel/TESTING.md) | Test infrastructure and Catch2 usage |

---

## Technical Reference

**API documentation and implementation details.**

| Document | Description |
|----------|-------------|
| [**LVGL 9 XML Guide**](devel/LVGL9_XML_GUIDE.md) | Complete XML syntax reference (92K) |
| [**Quick Reference**](devel/DEVELOPER_QUICK_REFERENCE.md) | Common patterns and code snippets |
| [**Modal System**](devel/MODAL_SYSTEM.md) | ui_dialog, modal_button_row, Modal pattern |
| [**Environment Variables**](devel/ENVIRONMENT_VARIABLES.md) | All runtime and build env vars |
| [**Moonraker Architecture**](devel/MOONRAKER_ARCHITECTURE.md) | Moonraker integration details |
| [**Theme System**](devel/THEME_SYSTEM.md) | Reactive theming, color tokens, responsive sizing |
| [**Layout System**](devel/LAYOUT_SYSTEM.md) | Alternative layouts, auto-detection, CLI override |
| [**Translation System**](devel/TRANSLATION_SYSTEM.md) | i18n: YAML → code generation, runtime lookups |
| [**UI Testing**](devel/UI_TESTING.md) | Headless LVGL testing, UITest utilities |
| [**Logging Guidelines**](devel/LOGGING.md) | Log levels and message format |
| [**Copyright Headers**](devel/COPYRIGHT_HEADERS.md) | SPDX license requirements |
| [**Config Migration**](devel/CONFIG_MIGRATION.md) | Versioned schema migration system |

### Feature Systems

| Document | Description |
|----------|-------------|
| [**Filament Management**](devel/FILAMENT_MANAGEMENT.md) | AMS, AFC, Happy Hare, ValgACE, Tool Changer |
| [**Spoolman Spool Wizard**](devel/plans/2026-02-15-spool-wizard-status.md) | 3-step spool creation wizard, API methods, visual test plan |
| [**Input Shaper & PID**](devel/INPUT_SHAPER.md) | Calibration, frequency response charts, CSV parser |
| [**Preprint Prediction**](devel/PREPRINT_PREDICTION.md) | ETA prediction engine, phase timing, history |
| [**Exclude Objects**](devel/EXCLUDE_OBJECTS.md) | Object exclusion, thumbnails, slicer setup |
| [**Print Start Profiles**](devel/PRINT_START_PROFILES.md) | Print start phase detection, profiles |
| [**Print Start Integration**](devel/PRINT_START_INTEGRATION.md) | User-facing macro setup guide |
| [**Update System**](devel/UPDATE_SYSTEM.md) | Channels, R2 CDN, downloads, Moonraker updater |
| [**Sound System**](devel/SOUND_SYSTEM.md) | Audio architecture, JSON themes, backends |
| [**LED Control**](devel/LED_CONTROL.md) | LED system: 4 backends, auto-state lighting, overlays |
| [**Printer Manager**](devel/PRINTER_MANAGER.md) | Printer overlay, custom images, inline editing |
| [**Timelapse**](devel/TIMELAPSE.md) | Moonraker timelapse plugin integration |
| [**Crash Reporter**](devel/CRASH_REPORTER.md) | Crash detection, delivery pipeline, CF Worker |
| [**Telemetry**](user/TELEMETRY.md) | Anonymous telemetry, privacy controls |
| [**HelixPrint Plugin**](../moonraker-plugin/README.md) | Phase tracking Moonraker plugin |

### Platform Support

| Document | Description |
|----------|-------------|
| [**Installer**](devel/INSTALLER.md) | Installation system, KIAUH, platforms, shell tests |
| [**QIDI Support**](devel/printers/QIDI_SUPPORT.md) | QIDI Q1 Pro / Plus platform guide |
| [**Snapmaker U1 Support**](devel/printers/SNAPMAKER_U1_SUPPORT.md) | Snapmaker U1 toolchanger platform guide |
| [**Creality K2 Support**](devel/printers/CREALITY_K2_SUPPORT.md) | Creality K2 series platform guide |
| [**FlashForge AD5X Support**](devel/printers/FLASHFORGE_AD5X_SUPPORT.md) | FlashForge Adventurer 5X (MIPS, ZMOD) |

---

## Planning & Status

**Project roadmap and feature tracking.**

| Document | Description |
|----------|-------------|
| [**Roadmap**](devel/ROADMAP.md) | Feature timeline and milestones |
| [**Active Plans**](devel/plans/) | Current implementation plans and tech debt |
| [**Archive**](archive/) | Historical implementation plans |

---

## Audits & Reports

**Security reviews and quality assessments.**

| Document | Description |
|----------|-------------|
| [**Security Review**](audits/MOONRAKER_SECURITY_REVIEW.md) | Moonraker security assessment |
| [**Memory Analysis**](audits/MEMORY_ANALYSIS.md) | Memory profiling and optimization |
| [**Test Coverage**](audits/TEST_COVERAGE_REPORT.md) | Test coverage report |

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
│   └── (see devel/PLUGIN_DEVELOPMENT.md)
│
├── devel/                    # DEVELOPER DOCUMENTATION
│   ├── CLAUDE.md             # Full developer doc index
│   ├── DEVELOPMENT.md        # Developer setup, contributing
│   ├── ARCHITECTURE.md       # System design
│   ├── BUILD_SYSTEM.md       # Build internals
│   ├── TESTING.md            # Test infrastructure
│   ├── LVGL9_XML_GUIDE.md    # XML reference
│   ├── MODAL_SYSTEM.md       # Modal architecture
│   ├── FILAMENT_MANAGEMENT.md # AMS, AFC, Happy Hare, TC
│   ├── INPUT_SHAPER.md       # Calibration, freq charts
│   ├── LED_CONTROL.md        # LED system, 4 backends
│   ├── UPDATE_SYSTEM.md      # Update channels, downloads
│   ├── ROADMAP.md            # Feature timeline
│   ├── plans/                # Active implementation plans
│   └── ...                   # 40+ more dev docs
│
├── audits/                   # Security & quality
├── archive/                  # Historical implementation plans
└── images/                   # Screenshots

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
| Fix layouts / contribute UI | [UI Contributor Guide](devel/UI_CONTRIBUTOR_GUIDE.md) |
| Create XML layouts | [LVGL 9 XML Guide](devel/LVGL9_XML_GUIDE.md) |
| Understand the architecture | [Architecture Guide](devel/ARCHITECTURE.md) |
| Cross-compile for Pi | [Build System - Cross-Compilation](devel/BUILD_SYSTEM.md#cross-compilation-embedded-targets) |

---

## Community

**[Join the HelixScreen Discord](https://discord.gg/RZCT2StKhr)** — Get help, share your setup, and follow development.

**Bug Reports & Feature Requests:** [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues)

---

*Back to [Project README](../README.md)*
