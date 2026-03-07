# Multi-Printer Phase 4: Printer Management Settings & Wizard Testing

**Date:** 2026-03-02
**Status:** Approved
**Prerequisite:** Phase 1 (Config v3), Phase 2 (Soft Restart), Phase 3 (UI Integration) — all complete.

## Scope

Four deliverables:

1. **Settings "Printers" row** — new entry point in Settings → PRINTER section
2. **PrinterListOverlay** — full printer management (switch, delete, add)
3. **Wizard cancel recovery** — clean up empty printer entry when wizard is cancelled
4. **Manual wizard testing** — verify add-printer wizard launch and cancel flows

Out of scope: mDNS discovery, re-running individual wizard steps, per-printer hardware reconfiguration.

## 1. Settings Row + Overlay Wiring

New `setting_action_row` at the **top of the PRINTER section** in `settings_panel.xml`:
- Label: "Printers" / Icon: `printer_3d` / Description: "Manage configured printers"
- Callback opens the existing `PrinterManagerOverlay`

Two entry points to `PrinterManagerOverlay`:
- Navbar printer chip (existing)
- Settings "Printers" row (new)

## 2. PrinterManagerOverlay Extension

Add a **"Manage Printers"** button at the bottom of the existing `PrinterManagerOverlay`, below the name/image/type section.

- Always visible (even with 1 printer — user may want to add a second)
- Pushes a new `PrinterListOverlay` onto the nav stack

## 3. PrinterListOverlay

New overlay using standard `overlay_panel` base with back button.

**Title:** "Manage Printers"

**Printer list** — one row per configured printer, dynamically populated in C++:
- Printer name (`text_body`)
- Connection status dot (green/yellow/red, same pattern as navbar badge)
- MDI `check` icon on the active printer
- Tap non-active printer → `switch_printer()` (soft restart)
- Tap active printer → no-op
- Delete/trash icon button on the right of each row
  - Tap → `modal_show_confirmation()` with destructive styling
  - On confirm: `Config::remove_printer()`, switch to another if deleting active
  - **Hidden when only 1 printer remains** (cannot delete last printer)

**"+ Add Printer" button** at the bottom — calls `add_printer_via_wizard()`.

## 4. Wizard Cancel Recovery

`Application` already stores `m_wizard_previous_printer_id` from Phase 3.

**On wizard cancel/back-out:**
1. `Config::remove_printer(new_id)` — delete the empty entry
2. `Config::set_active_printer(previous_id)` — restore previous
3. `Config::save()`
4. Soft restart back to previous printer

**On wizard completion:** Clear `m_wizard_previous_printer_id` (no cleanup needed).

**Edge case:** App closed mid-wizard → half-configured printer entry remains. Wizard re-triggers on next boot. No special handling needed.

## 5. New Files

| File | Purpose |
|------|---------|
| `include/ui_printer_list_overlay.h` | `PrinterListOverlay` class |
| `src/ui/ui_printer_list_overlay.cpp` | Dynamic row creation, switch/delete/add callbacks |
| `ui_xml/printer_list_overlay.xml` | Overlay XML layout |

## 6. Modified Files

| File | Change |
|------|--------|
| `ui_xml/settings_panel.xml` | Add "Printers" row at top of PRINTER section |
| `ui_xml/printer_image_overlay.xml` | Add "Manage Printers" button |
| `src/ui/ui_printer_manager_overlay.cpp` | Wire "Manage Printers" button callback |
| `src/ui/ui_panel_settings.cpp` | Add settings row callback |
| `src/application/application.cpp` | Wizard cancel recovery logic |
