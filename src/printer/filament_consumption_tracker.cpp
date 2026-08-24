// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_consumption_tracker.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "system/crash_handler.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

FilamentConsumptionTracker& FilamentConsumptionTracker::instance() {
    static FilamentConsumptionTracker inst;
    return inst;
}

void FilamentConsumptionTracker::start() {
    // Idempotent: a second start() without stop() would leak observers.
    if (print_state_obs_) {
        return;
    }
    PrinterState& printer = get_printer_state();

    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - tracks material actually
    // extruded; nothing is consumed during a preparing window.
    print_state_obs_ = helix::ui::observe_print_state<FilamentConsumptionTracker>(
        printer.get_print_state_enum_subject(), this,
        [](FilamentConsumptionTracker* self, PrintJobState state) {
            self->on_print_state_changed(state);
        });

    filament_used_obs_ = helix::ui::observe_int_sync<FilamentConsumptionTracker>(
        printer.get_print_filament_used_subject(), this,
        [](FilamentConsumptionTracker* self, int mm) { self->on_filament_used_changed(mm); });

    // Per-extruder filament_used_mm observers. These are dynamic subjects
    // ([L077]) and share one lifetime token so a single reset() in stop()
    // invalidates every observer before the subjects are deinit'd.
    for (int idx = 0; idx < MAX_TRACKED_EXTRUDERS; ++idx) {
        auto* subj = printer.get_extruder_filament_used_subject(idx, extruder_lifetime_);
        if (!subj) {
            continue;
        }
        extruder_obs_[idx] = helix::ui::observe_int_sync<FilamentConsumptionTracker>(
            subj, this,
            [idx](FilamentConsumptionTracker* self, int mm) {
                self->on_extruder_filament_used_changed(idx, mm);
            },
            extruder_lifetime_);
    }

    // Install the one-and-only ExternalSpoolSink on the first start(). Any
    // later stop()/start() cycle keeps the same sink alive.
    if (external_sink_raw_ == nullptr) {
        auto ext = std::make_unique<ExternalSpoolSink>();
        external_sink_raw_ = ext.get();
        sinks_.push_back(std::move(ext));
    }

    // Diagnostic crumbs to correlate with #927-class shutdown crashes.
    // Address logged at start vs at stop should match — divergence means the
    // observer was reassigned/freed externally between init and teardown.
    crash_handler::breadcrumb::note("fct", "start_a",
                                    reinterpret_cast<long>(print_state_obs_.get()));
    crash_handler::breadcrumb::note("fct", "start_b",
                                    reinterpret_cast<long>(filament_used_obs_.get()));

    // Self-register with StaticSubjectRegistry so stop() runs as part of
    // deinit_all() in proper LIFO order. Because PrinterState registers
    // earlier (during SubjectInitializer::init_core_and_state() at app
    // startup), our entry deinits FIRST — which means lv_observer_remove()
    // sees live PrinterState subjects, the same ordering invariant that
    // Application::shutdown()'s manual call was trying to provide. Closes
    // #927: the manual shutdown call ran AFTER StaticPanelRegistry::
    // destroy_all(), and intervening overlay teardown was apparently freeing
    // our observer struct out from under us — under self-registration that
    // window goes away because no other deinit step runs between
    // FilamentConsumptionTracker::stop() and our observers' creation.
    StaticSubjectRegistry::instance().register_deinit(
        "FilamentConsumptionTracker", []() { FilamentConsumptionTracker::instance().stop(); });
}

void FilamentConsumptionTracker::stop() {
    crash_handler::breadcrumb::note("fct", "stop_a",
                                    reinterpret_cast<long>(print_state_obs_.get()));
    print_state_obs_.reset();

    crash_handler::breadcrumb::note("fct", "stop_b",
                                    reinterpret_cast<long>(filament_used_obs_.get()));
    filament_used_obs_.reset();

    // [L077] Reset the lifetime FIRST so the weak_ptr inside each extruder
    // ObserverGuard expires before the guard's destructor runs — otherwise
    // lv_observer_remove() may fire on a PrinterState subject that is about
    // to be deinit'd.
    extruder_lifetime_.reset();
    for (auto& o : extruder_obs_) {
        o.reset();
    }

    active_ = false;
    print_in_progress_ = false;
}

FilamentConsumptionTracker::SinkHandle
FilamentConsumptionTracker::register_sink(std::unique_ptr<IConsumptionSink> sink) {
    if (!sink) {
        return nullptr;
    }
    IConsumptionSink* raw = sink.get();
    sinks_.push_back(std::move(sink));

    // If a print is already in progress, snapshot the new sink immediately so
    // it can start tracking from this point forward. If the new sink becomes
    // trackable and the tracker was previously idle (no other trackable sink),
    // flip active_ so deltas start flowing.
    //
    // Note: we always snapshot from the aggregate `print_stats.filament_used`.
    // For AmsSlotSinks whose backend declares a per-extruder mapping, the
    // first per-extruder tick will fall below this aggregate baseline and
    // hit the reset-detection branch in apply_delta(), which rebases to the
    // per-extruder stream. No gram is lost in that rebase because the
    // remaining_weight_g snapshot reads the backend's current value each time.
    if (print_in_progress_) {
        auto* subj = get_printer_state().get_print_filament_used_subject();
        const float mm = static_cast<float>(lv_subject_get_int(subj));
        raw->snapshot(mm);
        if (raw->is_trackable()) {
            active_ = true;
        }
        spdlog::debug("[FilamentTracker] Mid-print registered sink '{}' "
                      "(trackable={})",
                      raw->name(), raw->is_trackable());
    }
    return raw;
}

void FilamentConsumptionTracker::unregister_sink(SinkHandle handle) {
    if (!handle) {
        return;
    }
    auto it = std::find_if(
        sinks_.begin(), sinks_.end(),
        [handle](const std::unique_ptr<IConsumptionSink>& s) { return s.get() == handle; });
    if (it == sinks_.end()) {
        return;
    }
    (*it)->flush();
    if (external_sink_raw_ == handle) {
        external_sink_raw_ = nullptr;
    }
    sinks_.erase(it);
}

void FilamentConsumptionTracker::on_print_state_changed(PrintJobState state) {
    auto* printer_mm = get_printer_state().get_print_filament_used_subject();
    const float mm = static_cast<float>(lv_subject_get_int(printer_mm));

    // RAW_PRINT_STATE_OK: this tracks material actually extruded, so it keys on
    // the printer's own report. print_filament_used is 0 for the whole of a
    // preparing window - there is nothing to snapshot or flush there.
    switch (state) {
    case PrintJobState::PRINTING:
        if (!print_in_progress_) {
            snapshot_all_sinks(mm);
            print_in_progress_ = true;
            // active_ is true iff at least one sink is tracking so that
            // is_active() retains its original "tracker has live state"
            // meaning for existing tests.
            active_ = any_sink_trackable();
        }
        break;
    case PrintJobState::COMPLETE:
    case PrintJobState::CANCELLED:
    case PrintJobState::ERROR:
        if (print_in_progress_) {
            flush_all_sinks();
            spdlog::info("[FilamentTracker] Print ended in state {}; "
                         "flushed sinks",
                         static_cast<int>(state));
        }
        active_ = false;
        print_in_progress_ = false;
        break;
    // RAW_PRINT_STATE_OK: see the note on the switch - material, not intent.
    case PrintJobState::PAUSED:
        if (print_in_progress_) {
            flush_all_sinks(); // crash-safety snapshot
        }
        break;
    default:
        break;
    }
}

void FilamentConsumptionTracker::on_filament_used_changed(int filament_mm) {
    if (!print_in_progress_) {
        return;
    }
    const float f_mm = static_cast<float>(filament_mm);

    // Aggregate path: drives the ExternalSpoolSink always, and drives
    // AmsSlotSinks whose backend does NOT declare any per-extruder mapping
    // (single-extruder multi-slot backends like HappyHare, CFS, ACE, IFS).
    // Backends WITH a slot_for_extruder() mapping (tool-changers) are routed
    // via the per-extruder path so they don't get double-counted here.
    for (auto& sink : sinks_) {
        if (sink->kind() == SinkKind::ExternalSpool) {
            sink->apply_delta(f_mm);
            continue;
        }
        // SinkKind::AmsSlot
        auto* ams = static_cast<AmsSlotSink*>(sink.get());
        AmsBackend* backend = AmsState::instance().get_backend(ams->backend_index());
        if (!backend) {
            continue;
        }

        // Skip if ANY extruder on this backend claims a slot mapping — the
        // per-extruder routing path owns this backend.
        bool has_mapping = false;
        for (int e = 0; e < MAX_TRACKED_EXTRUDERS; ++e) {
            if (backend->slot_for_extruder(e)) {
                has_mapping = true;
                break;
            }
        }
        if (has_mapping) {
            continue;
        }

        // Single-extruder multi-slot backend: only the currently-loaded slot
        // accrues the delta.
        if (ams->slot_index() == backend->get_current_slot()) {
            ams->apply_delta(f_mm);
        }
    }

    // Sinks may have toggled trackability on this tick (e.g. external write
    // made a previously untrackable sink trackable). Keep is_active() in sync.
    active_ = any_sink_trackable();
}

void FilamentConsumptionTracker::on_extruder_filament_used_changed(int extruder_idx, int mm) {
    if (!print_in_progress_) {
        return;
    }
    const float f_mm = static_cast<float>(mm);

    // Find any backend that declares a slot mapping for this extruder and
    // route the delta to the matching AmsSlotSink. A single extruder can map
    // at most one slot per backend; multiple backends are independent.
    //
    // TODO(filament-tracker-deadidx): If a backend declares a mapping for an
    // extruder that Klipper never reports (e.g. mapping claims slot 1 →
    // extruder1, but only `extruder` appears in status), that slot silently
    // never accrues. Consider emitting a single spdlog::warn on first
    // print-start if a mapped extruder has never fired a status update.
    for (int b = 0; b < AmsState::instance().backend_count(); ++b) {
        AmsBackend* backend = AmsState::instance().get_backend(b);
        if (!backend) {
            continue;
        }
        auto slot = backend->slot_for_extruder(extruder_idx);
        if (!slot) {
            continue;
        }
        for (auto& sink : sinks_) {
            if (sink->kind() != SinkKind::AmsSlot) {
                continue;
            }
            auto* ams = static_cast<AmsSlotSink*>(sink.get());
            if (ams->backend_index() == b && ams->slot_index() == *slot) {
                ams->apply_delta(f_mm);
            }
        }
    }

    active_ = any_sink_trackable();
}

void FilamentConsumptionTracker::snapshot_all_sinks(float filament_used_mm) {
    for (auto& s : sinks_) {
        s->snapshot(filament_used_mm);
    }
}

void FilamentConsumptionTracker::flush_all_sinks() {
    for (auto& s : sinks_) {
        s->flush();
    }
}

bool FilamentConsumptionTracker::any_sink_trackable() const {
    for (const auto& s : sinks_) {
        if (s->is_trackable()) {
            return true;
        }
    }
    return false;
}

} // namespace helix