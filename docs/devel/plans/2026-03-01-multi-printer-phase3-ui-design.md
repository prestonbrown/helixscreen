# Multi-Printer Phase 3: UI Integration Design

**Date:** 2026-03-01
**Status:** Complete (2026-03-02)
**Prerequisite:** Phase 1 (Config v3) and Phase 2 (Soft Restart) — both complete.

## Scope

Three deliverables:

1. **Navbar printer badge** — shows active printer name + connection status dot
2. **Printer switch context menu** — quick-switch list + "Add Printer"
3. **Wizard "add printer" mode** — reuse wizard for new printer entries

Out of scope: printer removal (deferred — rare operation, future Settings section), per-printer status in dropdown, mDNS discovery (Phase 4).

## 1. Navbar Printer Badge

New clickable element at the bottom of the navbar showing:
- Truncated printer name (bound to `active_printer_name` subject)
- Small connection status dot (green=connected, red=disconnected, yellow=connecting)

**Hidden when only 1 printer is configured** — visibility bound to `multi_printer_enabled` subject.

Tapping opens the printer switch context menu.

## 2. Printer Switch Context Menu

Subclass of existing `ContextMenu` base class (same pattern as `AmsContextMenu`, `SpoolmanContextMenu`).

**Contents:**
- "Switch Printer" header
- Dynamically populated printer list (one row per configured printer)
  - Active printer highlighted with checkmark or accent styling
  - Tap non-active printer → `Application::switch_printer(id)` → soft restart
  - Tap active printer → dismiss (no-op)
- Divider
- "+ Add Printer" row → launches wizard in add-printer mode

**Positioning:** Anchored near the navbar badge using `ContextMenu::show_near_widget()`.

**List built dynamically in C++** (like macro picker) since printer count varies at runtime.

## 3. Wizard "Add Printer" Mode

Reuse the existing setup wizard for adding new printers:

- **Before wizard launch:** `Config::add_printer(new_id, {})` + `set_active_printer(new_id)`. Wizard writes to `df()` paths naturally.
- **Wizard completion:** Soft restart connects to the new printer. Standard flow.
- **Wizard cancellation:** `remove_printer(new_id)` + `set_active_printer(previous_id)` + soft restart back.

Goal: share/DRY the wizard setup code between initial setup and add-printer flows.

## 4. Subjects & Data Flow

| Subject | Type | Source | Purpose |
|---------|------|--------|---------|
| `active_printer_name` | string | Config printer name | Badge text binding |
| `multi_printer_enabled` | int (bool) | Config printer count > 1 | Badge visibility |
| `printer_connection_status` | int (enum) | WebSocket state | Badge dot color |

All are static singleton subjects (no `SubjectLifetime` tokens needed). Initialized during `init_printer_state()`.

## 5. New Files

| File | Purpose |
|------|---------|
| `include/ui_printer_switch_menu.h` | `PrinterSwitchMenu` (subclass of `ContextMenu`) |
| `src/ui/ui_printer_switch_menu.cpp` | Dynamic row creation, switch/add callbacks |
| `ui_xml/printer_switch_menu.xml` | Backdrop + card + header + list container + add button |

## 6. Modified Files

| File | Change |
|------|--------|
| `ui_xml/navigation_bar.xml` | Add printer badge element at bottom |
| Navbar C++ code | Badge click handler, subject wiring |
| `src/application/application.cpp` | Wire up add-printer wizard mode |
| `src/system/config.cpp` or `PrinterState` | Subject init for printer name/count |
| `src/xml_registration.cpp` | Register `printer_switch_menu` XML component |
