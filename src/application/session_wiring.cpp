// SPDX-License-Identifier: GPL-3.0-or-later

#include "session_wiring.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "discovery_steps.h"
#include "http_request_epoch.h"
#include "lap_log.h"
#include "led/led_controller.h"
#include "light_button_config.h"
#include "print_history_manager.h"
#include "printer_discovery.h"
#include "printer_print_state.h"
#include "printer_state.h"
#include "system/crash_handler.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix {

namespace {

/// Queued work belongs to the printer whose epoch it carries, and to a live session.
bool still_current(const DiscoveryHooks& hooks, uint64_t epoch) {
    return epoch == http_epoch::current() && (!hooks.alive || hooks.alive());
}

} // namespace

void wire_discovery(IMoonrakerAPI& api, IMoonrakerClient& client, DiscoveryHooks hooks) {
    auto shared = std::make_shared<DiscoveryHooks>(std::move(hooks));
    IMoonrakerAPI* a = &api;
    IMoonrakerClient* c = &client;

    // On a WLED-only printer discovery-complete finds nothing to light; WLED's answer is
    // LED on at Start's next chance, and the latch keeps it to one.
    led::LedController::instance().set_on_wled_settled(settle_light_buttons);

    client.set_on_hardware_discovered([a, c, shared](const PrinterDiscovery& hardware) {
        // Copied on the WebSocket thread so the queued callback owns a stable, unaliased
        // snapshot, and moved on the UI thread so no copy-assign iterates hash-table nodes
        // there (#761, #789).
        auto snapshot = std::make_shared<PrinterDiscovery>(hardware);
        // Read on the WebSocket thread. A switch stops the previous printer's socket before
        // the next connect moves the epoch, so the previous printer's work carries the old
        // value and is dropped on the UI thread.
        const uint64_t epoch = http_epoch::current();
        ui::queue_update("wire_discovery::hardware_discovered", [a, c, shared, snapshot, epoch]() {
            if (!still_current(*shared, epoch)) {
                spdlog::debug("[Discovery] dropping hardware queued for a previous printer or a "
                              "closed session");
                return;
            }
            LapLog laps("hardware discovered");
            if (shared->begin_cycle) {
                shared->begin_cycle();
            }
            a->hardware() = std::move(*snapshot);
            init_subsystems_from_hardware(a->hardware(), a, c);
            laps.lap("subsystems");
        });
    });

    client.set_on_discovery_complete([a, c, shared](const PrinterDiscovery& hardware,
                                                    const nlohmann::json& initial_status) {
        spdlog::debug("[Discovery] complete, WebSocket-thread entry (status keys: {})",
                      initial_status.is_object() ? initial_status.size() : 0);
        auto snapshot = std::make_shared<PrinterDiscovery>(hardware);
        auto status = std::make_shared<const nlohmann::json>(initial_status);
        const uint64_t epoch = http_epoch::current();
        ui::queue_update("wire_discovery::discovery_complete", [a, c, shared, snapshot, status,
                                                                epoch]() {
            if (!still_current(*shared, epoch)) {
                spdlog::info("[Discovery] dropping a pass queued for a previous printer or a "
                             "closed session");
                return;
            }
            // Crash bundles tell a first discovery from a reconnect's by this count.
            static long s_passes = 0;
            const long n = ++s_passes;
            crash_handler::breadcrumb::note("disc", "cb_begin", n);
            LapLog laps("discovery");

            // A copy, not a move: set_hardware moves the snapshot into PrinterState, and
            // nothing else aliases it (#789, #799).
            crash_handler::breadcrumb::note("disc", "pre_api_hw",
                                            static_cast<long>(snapshot->macros().size()));
            a->hardware() = *snapshot;
            crash_handler::breadcrumb::note("disc", "post_api_hw", n);

            // A reconnect with the same hardware shape skips the steps that are pure
            // functions of it (hardware_fingerprint.h). Computed from a->hardware(): the
            // snapshot is empty once set_hardware has run.
            const size_t fingerprint = compute_hardware_fingerprint(a->hardware());
            const bool hw_changed = shared->changes.note(fingerprint);
            crash_handler::breadcrumb::note("disc", "hw_changed", hw_changed ? 1L : 0L);
            spdlog::info("[Discovery] pass #{}: hardware shape {} (fingerprint=0x{:x})", n,
                         hw_changed ? "changed" : "unchanged", fingerprint);

            // The print_active subject is not yet updated from this discovery's status
            // (dispatch only queues it), so the status itself decides whether a print is
            // running: nothing may send gcode over a live print on a mid-print reconnect.
            DiscoveryContext ctx{
                *a,
                *c,
                a->hardware(),
                *snapshot,
                *status,
                /*prompter=*/nullptr,
                get_job_queue_state(),
                /*screen=*/nullptr,
                n,
                hw_changed,
                discovery_print_active(
                    lv_subject_get_int(
                        get_printer_state().print_state().get_print_active_subject()) != 0,
                    *status)};
            run_discovery_steps(discovery_core_steps(), ctx);
            laps.lap("core steps");

            if (shared->after_core) {
                shared->after_core(ctx);
                laps.lap("after core");
            }
            if (auto* history = get_print_history_manager()) {
                history->on_discovery_complete();
            }
            spdlog::info("[Discovery] applied: {} heaters, {} fans, {} sensors, {} initial-status "
                         "keys",
                         a->hardware().heaters().size(), a->hardware().fans().size(),
                         a->hardware().sensors().size(), status->is_object() ? status->size() : 0);
        });
    });
}

} // namespace helix
