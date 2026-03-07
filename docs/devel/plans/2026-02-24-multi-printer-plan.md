# Multi-Printer Support

## Branch: `feature/multi-printer`

## Overview

Runtime printer switching via config v3 schema and soft restart machinery. The display
stays alive while all printer-specific state (subjects, observers, WebSocket, panels) is
torn down and reinitialized for the new printer.

---

## Phase 1: Config v3 Schema — COMPLETE

**Commit:** `6eca88f6`

Config restructured from flat `/printer/` to `/printers/{id}/` object map.
`df()` dynamically routes all config reads/writes to the active printer.

- v2→v3 automatic migration
- Printer CRUD: `add_printer()`, `remove_printer()`, `set_active_printer()`
- `slugify()` for ID generation from printer names
- StaticSubjectRegistry/StaticPanelRegistry: added `clear()` for soft restart
- 13 new test cases, all ~70 existing call sites updated

**Known issue:** ~~11 LED config test failures from path changes~~ Fixed in `490027e8`.

## Phase 2: Soft Restart Machinery — COMPLETE

**Commits:** `e2d21607`, `717b704d`

### Architecture

`switch_printer()` → `tear_down_printer_state()` → `init_printer_state()`

- **Teardown** mirrors `shutdown()` ordering exactly: subjects stay alive through panel
  destruction so `lv_observer_remove()` works correctly
- **Init** follows `run()` phases (skips display/theme/assets — they're global)
- Display stays alive (`lv_deinit()` is NOT called)
- Theme subjects are NOT torn down (global, not per-printer)

### Key Mechanisms

**ObserverGuard global flag** (`invalidate_all()` / `revalidate_all()`):
- Called AFTER `StaticSubjectRegistry::deinit_all()` — all observers freed
- Protects surviving singleton guards from use-after-free during reinit
- `revalidate_all()` called at END of `init_printer_state()`
- `s_subjects_valid` is `std::atomic<bool>` (WebSocket thread safety)
- Error paths in init call `revalidate_all()` to avoid zombie state

**XML subject registry fix** (`lv_xml_register_subject()`):
- Re-registration now updates the subject pointer instead of silently returning
- Prevents stale XML bindings after destroy/recreate cycles

**Update queue lifecycle**:
- `update_queue_shutdown()` right after WebSocket disconnect (discard stale callbacks)
- `update_queue_init()` at START of init (before moonraker, so background threads work)

### Teardown Order (matches `shutdown()`)

1. Clear app_globals
2. NavigationManager shutdown
3. UpdateChecker stop_auto_check
4. Unload plugins
5. Disconnect WebSocket
6. Shutdown update queue (discard stale callbacks)
7. Release history managers
8. Unregister timelapse callback
9. Unregister action prompt callback
10. Clear AMS backends
11. Deinit LedController
12. Release PanelFactory + SubjectInitializer
13. Kill LVGL animations
14. Destroy static panels (ObserverGuards removed while subjects alive)
15. Release notification + media manager guards
16. Deinit subjects (StaticSubjectRegistry LIFO)
17. **invalidate_all()** — protect surviving singleton guards
18. Release MoonrakerManager
19. Delete LVGL widget tree

### Files Modified

| File | Change |
|------|--------|
| `include/application.h` | `switch_printer()`, `tear_down_printer_state()`, `init_printer_state()` |
| `src/application/application.cpp` | Teardown/init implementation, 'P' key shortcut |
| `include/ui_observer_guard.h` | `invalidate_all()`/`revalidate_all()` with atomic flag |
| `lib/helix-xml/src/xml/lv_xml.c` | Fix stale subject pointer on re-registration |
| `src/printer/ams_state.cpp` | Clear `api_` and reset observer in `deinit_subjects()` |
| `include/ui_notification.h` | `ui_notification_deinit()` declaration |
| `src/ui/ui_notification.cpp` | `ui_notification_deinit()` implementation |
| `include/active_print_media_manager.h` | `deinit_active_print_media_manager()` declaration |
| `src/print/active_print_media_manager.cpp` | `deinit_active_print_media_manager()` implementation |
| `src/ui/ui_nav_manager.cpp` | Reset `shutting_down_` in `deinit_subjects()` |

### Crashes Fixed

1. `lv_observer_remove` on freed observer (AmsState `print_state_observer_`)
2. Dangling `api_` in `AmsState::sync_current_loaded_from_backend`
3. Double free in static `s_notification_observer` (ui_notification.cpp)
4. Dangling observer in EmergencyStopOverlay singleton
5. Subject corruption from stale XML subject registry pointers (lv_xml.c)
6. Dangling `api_` in `LedController::discover_wled_strips`

## Phase 3: UI Integration — COMPLETE

**Commits:** `0eb78cf7`..`6d1a07be` (7 commits)
**Design:** `docs/devel/plans/2026-03-01-multi-printer-phase3-ui-design.md`

Navbar printer badge with context menu for quick switching between configured printers.

### What Was Built

- **Navbar printer icon** — `printer_3d` icon at bottom of navbar, visible only when >1 printer configured. Connection status dot (green/yellow/red) overlaid at top-right corner. Bound to `multi_printer_enabled` subject.
- **PrinterSwitchMenu** — `ContextMenu` subclass with dynamic printer list. Active printer shown with MDI `check` icon. Tapping a different printer calls `switch_printer()` → soft restart. Backdrop click dismisses.
- **"+ Add Printer"** — Creates empty config entry, soft restarts into wizard. Wizard completion callback clears the temporary state.
- **Subjects** — `active_printer_name` (string) and `multi_printer_enabled` (int) registered globally, set during `init_printer_state()`.

### New Files

| File | Purpose |
|------|---------|
| `include/ui_printer_switch_menu.h` | `PrinterSwitchMenu` class (extends `ContextMenu`) |
| `src/ui/ui_printer_switch_menu.cpp` | Dynamic row creation, switch/add callbacks |
| `ui_xml/printer_switch_menu.xml` | Context menu XML layout |

### Modified Files

| File | Change |
|------|--------|
| `include/printer_state.h` | Added `active_printer_name_` and `multi_printer_enabled_` subjects |
| `src/printer/printer_state.cpp` | Subject init/deinit/setters |
| `ui_xml/navigation_bar.xml` | Printer badge `ui_button` with icon + status dot |
| `include/ui_nav_manager.h` | Menu member, dot observer, badge click handler |
| `src/ui/ui_nav_manager.cpp` | Badge click → menu, connection dot color observer |
| `src/application/application.cpp` | `add_printer_via_wizard()`, subject wiring, P key test helper |
| `include/application.h` | `add_printer_via_wizard()` declaration |
| `src/xml_registration.cpp` | Register component + callbacks |

## Phase 4: Discovery & Auto-Add — NOT STARTED

- mDNS/Zeroconf printer discovery
- Auto-add discovered printers
- Scan network for Moonraker instances

---

## Testing

```bash
make -j                                          # Build
./build/bin/helix-tests "[core][config]"          # Config tests (63 assertions)
./build/bin/helix-tests "[core][registry]"        # Registry tests (5 assertions)
./build/bin/helix-screen --test -vv               # Manual test, press 'P' to switch
```

## Remaining Items (Low Priority)

- Add collision detection to `Config::add_printer()` slugify
- Tests for soft restart cycle (observer cleanup, queue lifecycle)
- ~~Fix 11 LED config test failures from v3 path changes~~ Fixed
- Wizard cancel recovery (remove empty printer entry, restore previous)
- Printer removal UI in Settings
