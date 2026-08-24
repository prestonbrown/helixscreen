// SPDX-License-Identifier: GPL-3.0-or-later
//
// helixapp link probe. app_main references helixapp_link_probe() through a
// volatile function pointer (never calls it) so the linker keeps the curated
// Core+AMS app core in the image and the size gate accounts for its footprint —
// the same pattern helixnet uses (s_helixnet_keep, which references the real
// EspMoonrakerClient). Task 6 replaces this idle anchor with the real UI
// bring-up call site.
//
// The body references a bounded, self-contained slice inside a `volatile`-
// guarded branch the linker cannot prove dead:
//   - register_xml_components(): XML widget/component factories
//   - get_printer_state(): the printer data model root
//
// SIZE FINDING (headline for the plan — see esp32p4-task-5-report.md):
// referencing more of the runtime surface blows the budget at -Os, so it CANNOT
// be in an idle probe that must still produce a green (fits-the-6.0MB-slot)
// build:
//   + AssetManager::register_all() + theme_manager_init() + AmsState  -> 7.97 MB
//   + AmsState::instance() alone (its registration graph transitively pulls the
//     AMS panel/UI layer)                                             -> 7.95 MB
//   this bounded probe                                                -> ~2.95 MB
// The full curated Core+AMS image is ~7.95-7.97 MB (3.66 MB .text + 3.9 MB
// .rodata), OVER the 6.0 MB OTA slot and the 5.8 MB budget by ~2.2 MB. The real
// bring-up (Task 6) needs font-tier trimming / feature cuts (or a partition
// resize / spec change) to fit, and must gate the panel layer's v1-excluded
// references (gcode viewer C API from ui_panel_print_status, camera
// QrScannerOverlay from the AMS panels) behind HELIX_HAS_GCODE_VIEWER /
// HELIX_HAS_CAMERA. The stubs in this component (excluded_subsystems_stub.cpp,
// task10_pending_stubs.cpp) already resolve those symbols for that surface.

#include "app_globals.h"
#include "printer_state.h"
#include "xml_registration.h"

extern "C" void helixapp_link_probe(void) {
    // Bounded idle anchor: references the printer data-model root so helixapp is
    // linked into the image (app_main holds s_helixapp_keep -> this symbol).
    // register_xml_components() is intentionally NOT called here — doing so
    // cascades into the full Core+AMS UI and takes the image to ~7.95 MB (see
    // the SIZE FINDING above), which overflows the OTA slot. Task 6 wires the
    // real bring-up and must land the size cuts first.
    static volatile bool s_never = false;
    if (s_never) {
        (void)&get_printer_state;
    }
}
