// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file spoolman_manager.cpp
 * @brief Centralized Spoolman weight polling, circuit breaker, and identity cache
 *
 * @pattern Singleton with static s_shutdown_flag atomic for callback safety
 * @threading Weight refresh callbacks arrive from HTTP thread; circuit breaker
 *            state and the identity cache are updated on the UI thread via
 *            queue_update. The identity entry points are additionally mutex-
 *            guarded and LVGL-free, so an invalidation from a save completion
 *            (HTTP thread) is safe without marshalling.
 *
 * @see ams_state.cpp (slot data remains in AmsState)
 * @see include/filament_display_name.h (the resolver this cache feeds)
 */

#include "spoolman_manager.h"

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "app_globals.h"
#include "filament_slot_override.h"
#include "i_moonraker_api.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <utility>
#include <vector>

using namespace helix;

// Shutdown flag to prevent async callbacks from accessing destroyed singleton
std::atomic<bool> SpoolmanManager::s_shutdown_flag{false};

namespace {

/**
 * @brief Project a Spoolman spool record onto the identity the label needs.
 *
 * `SpoolInfo::filament_name` is Spoolman's `filament.name`
 * ("PolyTerra Ambrosia Pink") — the human filament name, which is exactly
 * what the label wants.
 *
 * Pure — no LVGL, no singleton access — so it is safe to call from either thread.
 */
helix::SpoolIdentity identity_from_spool(const SpoolInfo& spool) {
    helix::SpoolIdentity identity;
    identity.vendor = spool.vendor;
    identity.filament_name = spool.filament_name;
    identity.material = spool.material;
    identity.color_hex = spool.color_hex;
    identity.filament_id = spool.filament_id;
    identity.vendor_id = spool.vendor_id;
    return identity;
}

/// The backend a fetch's answer still belongs to: the slot it asked about must
/// still name the spool the fetch asked for. An answer describes the binding
/// its request was made for, so a slot re-bound while the request was in
/// flight takes nothing from it.
AmsBackend* slot_still_bound_to(int backend_index, int slot_index, int spoolman_id) {
    // The backend this slot belongs to - NOT get_backend(0). A slot index is
    // meaningful only within its own backend, so writing a second AMS's answer
    // onto the primary's same-numbered bay corrupts both.
    AmsBackend* owner = AmsState::instance().get_backend(backend_index);
    if (!owner) {
        return nullptr;
    }
    const int bound_id = owner->get_slot_info(slot_index).spoolman_id;
    if (bound_id != spoolman_id) {
        spdlog::debug("[SpoolmanManager] Slot {} spoolman_id changed ({} -> {}), skipping stale "
                      "Spoolman answer",
                      slot_index, spoolman_id, bound_id);
        return nullptr;
    }
    return owner;
}

} // namespace

SpoolmanManager& SpoolmanManager::instance() {
    static SpoolmanManager inst;
    return inst;
}

SpoolmanManager::~SpoolmanManager() {
    // Signal shutdown to prevent async callbacks from accessing this instance
    s_shutdown_flag.store(true, std::memory_order_release);

    // Clean up poll timer if still active (check LVGL is initialized
    // to avoid crash during static destruction order issues)
    if (poll_timer_ && lv_is_initialized()) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }
}

void SpoolmanManager::init_subjects() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (initialized_) {
        return;
    }

    // Clear shutdown flag — supports soft restart (printer switching)
    s_shutdown_flag.store(false, std::memory_order_release);

    spdlog::trace("[SpoolmanManager] Initializing subjects");

    // Observe print state changes to auto-refresh Spoolman weights.
    // Refreshes when print starts, ends, or pauses to keep weight data current.
    using helix::ui::observe;
    using helix::ui::observe_print_state;
    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - weights change when
    // filament moves, which is the printer's own transition.
    print_state_observer_ = observe_print_state<SpoolmanManager>(
        get_printer_state().print_state().get_print_state_enum_subject(), this,
        [](SpoolmanManager* self, PrintJobState print_state) {
            // RAW_PRINT_STATE_OK: Spoolman weights only change when filament
            // actually moves, so this refreshes on the printer's own reported
            // transitions. Nothing has been consumed during Preparing.
            if (print_state == PrintJobState::PRINTING || print_state == PrintJobState::COMPLETE ||
                print_state == PrintJobState::PAUSED) {
                spdlog::debug(
                    "[SpoolmanManager] Print state changed to {}, refreshing Spoolman weights",
                    static_cast<int>(print_state));
                self->refresh_spoolman_weights();
            }
        },
        get_printer_state().get_subjects_lifetime());

    // Observe Spoolman availability — stop polling when Spoolman disappears, and
    // arm it when Spoolman shows up. Reached through the capabilities accessor
    // rather than lv_xml_get_subject("printer_has_spoolman"): that lookup misses
    // whenever subjects were initialised without XML registration, and the miss
    // is silent, which left the manager with no availability observer at all.
    spoolman_availability_observer_ = observe<int>(
        get_printer_state().capabilities_state().subject(Capability::HasSpoolman), this,
        [](SpoolmanManager* self, int value) {
            if (value == 0) {
                std::lock_guard<std::recursive_mutex> lock(self->mutex_);
                // LVGL fires an observer immediately on attach, and observe<int>
                // defers the handler, so the first callback routinely arrives
                // reading 0 for a Spoolman that was never available. The teardown
                // below is idempotent and stays unconditional - it is the LOG that
                // must not claim a loss that did not happen. Reporting one sent a
                // live K2 investigation after the wrong writer entirely.
                const bool had_something_to_stop = self->poll_timer_ != nullptr ||
                                                   !self->identity_cache_.empty() ||
                                                   !self->identity_unresolvable_.empty();
                if (had_something_to_stop) {
                    spdlog::info("[SpoolmanManager] Spoolman became unavailable, stopping polling");
                }
                // poll_refcount_ is deliberately kept: it counts panels that
                // still want polling, and they get no second chance to ask.
                // Zeroing it is what made a Spoolman that came back never
                // resume for an already-active panel.
                if (self->poll_timer_ && lv_is_initialized()) {
                    lv_timer_delete(self->poll_timer_);
                    self->poll_timer_ = nullptr;
                }
                self->reset_circuit_breaker();
                // Nothing will refresh these while Spoolman is gone, and the
                // next Spoolman may not be the same one (printer switch).
                // Drop them so the resolver falls back to firmware data.
                self->identity_cache_.clear();
                self->identity_unresolvable_.clear();
            } else {
                // Spoolman appeared. Honour any request that arrived while
                // it could not be served -- at boot, that is all of them.
                self->ensure_poll_timer();
            }
        },
        get_printer_state().get_subjects_lifetime());

    initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "SpoolmanManager", []() { SpoolmanManager::instance().deinit_subjects(); });
}

void SpoolmanManager::deinit_subjects() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!initialized_) {
        return;
    }

    spdlog::trace("[SpoolmanManager] Deinitializing subjects");

    s_shutdown_flag.store(true, std::memory_order_release);

    // Release cross-singleton observers — they observe subjects from PrinterState
    // which may already be destroyed during StaticSubjectRegistry::deinit_all()
    // reverse-order teardown. Using release() (not reset()) avoids dereferencing
    // a dangling subject pointer in lv_observer_remove().
    print_state_observer_.release();
    spoolman_availability_observer_.release();

    // Clear dangling API pointer — IMoonrakerAPI is destroyed during teardown
    api_ = nullptr;

    // Direct member access, not clear_identity_cache(): s_shutdown_flag is
    // already set above, so the static entry point would no-op and the cache
    // would survive a soft restart (printer switch) into a different Spoolman.
    identity_cache_.clear();
    identity_unresolvable_.clear();

    if (poll_timer_ && lv_is_initialized()) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }

    initialized_ = false;
}

void SpoolmanManager::set_api(IMoonrakerAPI* api) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    api_ = api;
    reset_circuit_breaker();
}

void SpoolmanManager::fetch_linked_external_spool(int ext_spoolman_id) {
    api_->spoolman().get_spoolman_spool(
        ext_spoolman_id,
        [ext_spoolman_id](const std::optional<SpoolInfo>& spool_opt) {
            if (!spool_opt.has_value()) {
                spdlog::warn("[SpoolmanManager] External spool Spoolman #{} not found",
                             ext_spoolman_id);
                helix::ui::queue_update("SpoolmanManager::ext_unresolvable", [ext_spoolman_id]() {
                    note_identity_unresolvable(ext_spoolman_id);
                    if (s_shutdown_flag.load(std::memory_order_acquire)) {
                        return;
                    }
                    AmsState& state = AmsState::instance();
                    const auto raw = state.raw_external_spool_info();
                    if (!raw.has_value() || raw->spoolman_id != ext_spoolman_id) {
                        // The binding moved while this denial was in
                        // flight; its records were already retired by
                        // the rebind, and a newer spool's records are
                        // not this denial's to touch.
                        return;
                    }
                    // Same contract as a denied slot: the record described
                    // a spool Spoolman no longer knows, so it stops being
                    // a source the resolved read can rank. The resolved
                    // view is captured first, while the record still
                    // stands, and what it carried goes back on the lane
                    // as remembered - a delete is bookkeeping, not a
                    // spool change, and the display keeps the freshest
                    // identity it had. The kept record names no spool
                    // (both binding ids zeroed) or the record
                    // translation would stand the Spoolman record just
                    // dropped right back up.
                    const bool had_record =
                        helix::ams::lane_sources(helix::ams::kBypassLaneId).spoolman.has_value();
                    const std::optional<SlotInfo> kept_view = state.get_external_spool_info();
                    helix::ams::drop_lane_source(helix::ams::kBypassLaneId,
                                                 helix::ams::ObservationSource::Spoolman);
                    if (had_record && kept_view.has_value()) {
                        SlotInfo kept = *kept_view;
                        kept.spoolman_id = 0;
                        kept.spoolman_filament_id = 0;
                        const helix::ams::Observation nothing_declared(
                            helix::ams::ObservationSource::Remembered);
                        helix::ams::file_kept_identity(
                            helix::ams::kBypassLaneId, helix::ams::kBypassSlotIndex,
                            helix::ams::user_override_from_slot_info(nothing_declared, kept,
                                                                     kept.material, nullptr));
                    }
                    state.bump_slots_version();
                });
                return;
            }

            const SpoolInfo& spool = spool_opt.value();
            float new_remaining = static_cast<float>(spool.remaining_weight_g);
            float new_total = static_cast<float>(spool.initial_weight_g);

            helix::ui::queue_update("SpoolmanManager::ext_weights", [ext_spoolman_id, new_remaining,
                                                                     new_total, spool]() {
                if (s_shutdown_flag.load(std::memory_order_acquire)) {
                    return;
                }

                // Same shape as the slot arm: cache the identity before
                // any early return, then file the spool on the bypass
                // lane and let resolve() rank it.
                const bool identity_is_new = cache_identity(spool);

                AmsState& state = AmsState::instance();
                if (identity_is_new) {
                    // One spool can be linked on a lane as well as here.
                    state.refresh_slot_names();
                    state.bump_slots_version();
                }
                auto ext = state.raw_external_spool_info();
                if (!ext.has_value() || ext->spoolman_id != ext_spoolman_id) {
                    spdlog::debug(
                        "[SpoolmanManager] External spool changed, skipping stale update");
                    return;
                }

                if (SpoolmanManager::file_spool_on_lane(helix::ams::kBypassLaneId, spool)) {
                    // The stored binding is what a restart reads before any
                    // poll answers, so the fetched record moves it too, the
                    // same reason a slot fetch persists its lane record.
                    SlotInfo stored = *ext;
                    apply_spool_to_slot(stored, spool);
                    if (spool.initial_weight_g <= 0) {
                        // No weight stated is not a zero weight: the filing
                        // above skipped the weights for the same reason.
                        stored.remaining_weight_g = ext->remaining_weight_g;
                        stored.total_weight_g = ext->total_weight_g;
                    }
                    state.set_external_spool_info(stored);
                    state.bump_slots_version();
                    spdlog::debug("[SpoolmanManager] Filed external spool weights on bypass lane: "
                                  "{:.0f}g / {:.0f}g",
                                  new_remaining, new_total);
                }
            });
        },
        [ext_spoolman_id](const MoonrakerError& err) {
            spdlog::warn("[SpoolmanManager] Failed to fetch external spool Spoolman #{}: {}",
                         ext_spoolman_id, err.message);
        },
        /*silent=*/true);
}

void SpoolmanManager::fetch_linked_slot(int backend_index, int slot_index, int spoolman_id) {
    api_->spoolman().get_spoolman_spool(
        spoolman_id,
        [slot_index, spoolman_id, backend_index](const std::optional<SpoolInfo>& spool_opt) {
            if (!spool_opt.has_value()) {
                spdlog::warn("[SpoolmanManager] Spoolman spool {} not found", spoolman_id);
                // "No such spool" is an answer, not a transport
                // failure: the circuit breaker must not see it, but
                // the id must stop being polled. It also proves the
                // lane's Spoolman record describes a spool that does
                // not exist, so that record goes. An unreachable
                // server proves nothing about the record and takes
                // the error path instead, leaving it standing.
                helix::ui::queue_update("SpoolmanManager::slot_unresolvable", [spoolman_id,
                                                                               slot_index,
                                                                               backend_index]() {
                    note_identity_unresolvable(spoolman_id);
                    if (s_shutdown_flag.load(std::memory_order_acquire)) {
                        return;
                    }
                    AmsBackend* bound = slot_still_bound_to(backend_index, slot_index, spoolman_id);
                    if (bound) {
                        const helix::ams::LaneId lane = bound->lane_id(slot_index);
                        const bool had_record = helix::ams::lane_sources(lane).spoolman.has_value();
                        helix::ams::drop_lane_source(lane, helix::ams::ObservationSource::Spoolman);
                        if (had_record) {
                            // A delete is bookkeeping, not a spool change: the slot
                            // goes on showing what is loaded. What the slot carried
                            // goes back on the lane as remembered, the way an unlink
                            // that kept the identity files it, so it stands until an
                            // edit or a spool change replaces it. The kept record
                            // names no spool - both binding ids zeroed, as an
                            // unlink's record does - or the record translation would
                            // stand the Spoolman record just dropped right back up.
                            // The slot struct keeps its own spoolman_id: the manager
                            // has no identity write path to a backend, and polling
                            // for this spool already stopped above.
                            SlotInfo kept = bound->get_slot_info(slot_index);
                            kept.spoolman_id = 0;
                            kept.spoolman_filament_id = 0;
                            const helix::ams::Observation nothing_declared(
                                helix::ams::ObservationSource::Remembered);
                            helix::ams::file_kept_identity(
                                lane, slot_index,
                                helix::ams::user_override_from_slot_info(nothing_declared, kept,
                                                                         kept.material, nullptr));
                            // A backend serving its slots from a cache
                            // painted this one while the record stood,
                            // and the drop raises no backend event to
                            // resync the slot's subjects.
                            bound->repaint_slot_from_lane(slot_index);
                            AmsState::instance().update_slot_for_backend(backend_index, slot_index);
                        }
                    }
                });
                return;
            }

            const SpoolInfo& spool = spool_opt.value();

            // Data to pass to UI thread
            struct WeightUpdate {
                int slot_index;
                int backend_index;        // Which AMS owns that slot index
                int expected_spoolman_id; // To verify slot wasn't reassigned
                // Whole record, carried so the identity cache and
                // the lane's Spoolman record are filled on the UI
                // thread. Nothing from it is written onto the slot:
                // the Spoolman record is the filing.
                SpoolInfo spool;
            };

            auto update_data = std::make_unique<WeightUpdate>(
                WeightUpdate{slot_index, backend_index, spoolman_id, spool});

            helix::ui::queue_update<WeightUpdate>(std::move(update_data), [](WeightUpdate* d) {
                // Skip if shutdown is in progress
                if (s_shutdown_flag.load(std::memory_order_acquire)) {
                    return;
                }

                SpoolmanManager& mgr = SpoolmanManager::instance();

                // Our own state, under our own lock, released before the
                // AmsState work below. AmsState::sync_from_backend() calls
                // SpoolmanManager::find_identity(), so AmsState is never
                // entered with mutex_ held.
                bool identity_is_new = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

                    // Success response: reset the circuit breaker (on the UI thread).
                    if (mgr.consecutive_failures_ > 0) {
                        spdlog::info("[SpoolmanManager] Spoolman recovered after {} failures",
                                     mgr.consecutive_failures_);
                    }
                    mgr.consecutive_failures_ = 0;
                    mgr.unavailable_notified_ = false;

                    // Identity side channel. Must happen BEFORE the
                    // slot-reassignment and weights-unchanged early
                    // returns below, because a slot whose weight never
                    // moves would otherwise never get a name.
                    identity_is_new = cache_identity(d->spool);
                }

                AmsState& ams = AmsState::instance();
                if (identity_is_new) {
                    // A name the label consumers could not resolve a
                    // moment ago just became resolvable. The weight
                    // early-returns below would otherwise swallow it
                    // whenever the weight happens not to move, which
                    // is every poll once a spool settles (#1264).
                    ams.refresh_slot_names();
                    ams.bump_slots_version();
                }
                AmsBackend* owner =
                    slot_still_bound_to(d->backend_index, d->slot_index, d->expected_spoolman_id);
                if (!owner) {
                    return;
                }

                // Every answer reaches the lane, not only one that
                // carries a new identity: a spool edited on the
                // server keeps its id, so cache_identity() has
                // already seen it. Refiling an unchanged record is
                // harmless, since it replaces the old one, and
                // apply_fetched_spool() stops at the lane when the
                // record did not move.
                //
                // The Spoolman record IS the filing: its weights ride
                // the lane's sources, where resolve() ranks them above
                // the meter's, and the slot follows through the
                // repaint below. Writing the weights onto the slot as
                // well would file them a second time under Metered and
                // put two producers on one source.
                apply_fetched_spool(*owner, d->backend_index, d->slot_index, d->spool);
            });
        },
        [spoolman_id](const MoonrakerError& err) {
            spdlog::warn("[SpoolmanManager] Failed to fetch Spoolman spool {}: {}", spoolman_id,
                         err.message);

            // Track failure for circuit breaker (post to UI thread for
            // thread-safe access to SpoolmanManager and ToastManager)
            helix::ui::queue_update("SpoolmanManager::cb_failure", []() {
                if (s_shutdown_flag.load(std::memory_order_acquire)) {
                    return;
                }

                SpoolmanManager& mgr = SpoolmanManager::instance();
                std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

                mgr.consecutive_failures_++;

                if (mgr.consecutive_failures_ >= kCbFailureThreshold) {
                    mgr.cb_open_ = true;
                    mgr.cb_tripped_at_ms_ = lv_tick_get();
                    spdlog::warn("[SpoolmanManager] Spoolman circuit breaker OPEN after {} "
                                 "failures, backing off {}s",
                                 mgr.consecutive_failures_, kCbBackoffMs / 1000);

                    // Notify the user once per outage, and only when
                    // Spoolman is actually configured: a printer that never
                    // set one up has nothing to be told about.
                    if (!mgr.unavailable_notified_) {
                        mgr.unavailable_notified_ = true;
                        auto* subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
                        if (subj && lv_subject_get_int(subj) == 1) {
                            // i18n: Spoolman is a product name, do not translate
                            ToastManager::instance().show(
                                ToastSeverity::WARNING,
                                lv_tr("Spoolman unavailable — filament weights "
                                      "may be stale"),
                                6000);
                        }
                    }
                }
            });
        },
        /*silent=*/true);
}

void SpoolmanManager::refresh_spool(int spool_id) {
    if (spool_id <= 0 || s_shutdown_flag.load(std::memory_order_acquire)) {
        return;
    }

    // AmsState first, then mutex_: the one-way lock order refresh_spoolman_weights()
    // documents, for the same reason.
    std::vector<std::pair<int, AmsBackend*>> backends;
    {
        const int count = AmsState::instance().backend_count();
        backends.reserve(static_cast<size_t>(count > 0 ? count : 0));
        for (int bi = 0; bi < count; ++bi) {
            if (auto* b = AmsState::instance().get_backend(bi)) {
                backends.emplace_back(bi, b);
            }
        }
    }
    const int ext_spoolman_id =
        AmsState::instance().raw_external_spool_info().value_or(SlotInfo{}).spoolman_id;

    SpoolmanManager& mgr = instance();
    std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
    if (!mgr.api_ || !get_printer_state().is_spoolman_available()) {
        return;
    }
    // A spool the server has already denied exists has nothing to re-read.
    if (is_identity_unresolvable(spool_id)) {
        return;
    }

    for (const auto& entry : backends) {
        AmsBackend* backend = entry.second;
        const int slot_count = backend->get_system_info().total_slots;
        for (int i = 0; i < slot_count; ++i) {
            if (backend->get_slot_info(i).spoolman_id == spool_id) {
                mgr.fetch_linked_slot(entry.first, i, spool_id);
            }
        }
    }
    if (ext_spoolman_id == spool_id) {
        mgr.fetch_linked_external_spool(spool_id);
    }
}

void SpoolmanManager::refresh_spoolman_weights() {
    // Resolve everything we need from AmsState BEFORE taking our own mutex_.
    // AmsState::sync_from_backend() calls SpoolmanManager::find_identity(), so the
    // order is AmsState -> SpoolmanManager and AmsState is never entered with mutex_
    // held. Hoisting costs a vector index and a settings read on the early-return
    // paths and buys a one-way order.
    // Every backend, not just the primary. A second AMS's lanes carry their own
    // spoolman_id links and were simply never polled, so their weights sat at
    // whatever the last manual edit left and the low-filament checks read them
    // as fact. Resolved here, above the lock, for the same lock-order reason.
    std::vector<std::pair<int, AmsBackend*>> backends;
    {
        const int count = AmsState::instance().backend_count();
        backends.reserve(static_cast<size_t>(count > 0 ? count : 0));
        for (int bi = 0; bi < count; ++bi) {
            if (auto* b = AmsState::instance().get_backend(bi)) {
                backends.emplace_back(bi, b);
            }
        }
    }
    auto ext_spool = AmsState::instance().get_external_spool_info();

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Mock mode polls too. AmsBackendMock seeds spoolman_id = lane + 1 to mirror
    // MockSpoolmanServer::init_mock_spools() (ids 1-7), so those ids resolve
    // against the mock API and the identity cache fills exactly as it does on real
    // hardware. The old guard here predated that alignment and skipped the poll on
    // the premise that mock ids were fabricated; keeping it would have made every
    // Spoolman-sourced label undemonstrable under --test.
    if (!api_) {
        return;
    }

    // Skip if Spoolman is not configured/connected in Moonraker
    if (!get_printer_state().is_spoolman_available()) {
        spdlog::trace("[SpoolmanManager] Spoolman not available, skipping weight refresh");
        return;
    }

    uint32_t now = lv_tick_get();

    // Circuit breaker: if open, check if backoff period has elapsed
    if (cb_open_) {
        uint32_t elapsed = now - cb_tripped_at_ms_;
        if (elapsed < kCbBackoffMs) {
            spdlog::trace("[SpoolmanManager] Spoolman circuit breaker open, {}ms remaining",
                          kCbBackoffMs - elapsed);
            return;
        }
        // Backoff elapsed — half-open: allow one probe request through
        spdlog::info("[SpoolmanManager] Spoolman circuit breaker half-open, probing...");
        cb_open_ = false;
    }

    // Debounce: skip if called too recently
    if (last_refresh_ms_ > 0) {
        uint32_t since_last = now - last_refresh_ms_;
        if (since_last < kDebounceMs) {
            spdlog::trace("[SpoolmanManager] Spoolman refresh debounced ({}ms since last)",
                          since_last);
            return;
        }
    }
    last_refresh_ms_ = now;

    int linked_count = 0;

    // Refresh AMS backend slots (if a backend is active); `backend` was resolved
    // above the lock.
    for (const auto& entry : backends) {
        const int backend_index = entry.first;
        AmsBackend* backend = entry.second;
        int slot_count = backend->get_system_info().total_slots;

        for (int i = 0; i < slot_count; ++i) {
            SlotInfo slot = backend->get_slot_info(i);
            if (slot.spoolman_id > 0) {
                // A spool Spoolman has already denied exists has no weight to
                // fetch either. Skipping it is what stops a deleted link from
                // becoming one request per slot per poll, forever.
                if (is_identity_unresolvable(slot.spoolman_id)) {
                    spdlog::trace("[SpoolmanManager] Slot {} spool {} is unresolvable, skipping", i,
                                  slot.spoolman_id);
                    continue;
                }

                ++linked_count;
                int slot_index = i;
                int spoolman_id = slot.spoolman_id;

                fetch_linked_slot(backend_index, slot_index, spoolman_id);
            }
        }
    } // if (backend)

    // Also refresh external spool if it has a Spoolman link (`ext_spool` was
    // resolved above the lock).
    if (ext_spool.has_value() && ext_spool->spoolman_id > 0 &&
        !is_identity_unresolvable(ext_spool->spoolman_id)) {
        ++linked_count;
        fetch_linked_external_spool(ext_spool->spoolman_id);
    }

    if (linked_count > 0) {
        spdlog::trace("[SpoolmanManager] Refreshing Spoolman weights for {} linked slots",
                      linked_count);
    }
}

bool SpoolmanManager::apply_fetched_spool(helix::AmsBackend& owner, int backend_index,
                                          int slot_index, const SpoolInfo& spool) {
    const bool lane_changed = file_spool_on_lane(owner.lane_id(slot_index), spool);
    // Every poll refiles every linked lane, so only a changed record is worth
    // the work below.
    if (!lane_changed) {
        return false;
    }
    // The stored record holds the identity the server stated when it was
    // written, so a restart with Spoolman unreachable shows what the filing
    // just changed only if the record moves with it.
    owner.persist_external_identity(slot_index);
    // A backend serving its slots from a cache painted this one from the lane as
    // it stood before the filing, and the slot's subjects follow backend events,
    // of which a filing raises none.
    owner.repaint_slot_from_lane(slot_index);
    AmsState::instance().update_slot_for_backend(backend_index, slot_index);
    // Version-keyed consumers (the pre-print filament check) need the weight
    // change the filing carried, which no backend event raises.
    AmsState::instance().bump_slots_version();
    return true;
}

bool SpoolmanManager::file_spool_on_lane(helix::ams::LaneId lane, const SpoolInfo& spool) {
    helix::ams::Observation stated = helix::ams::spool_identity_observation(spool);
    // Spoolman computes the remaining weight from the initial one and serves
    // both as null when it has none, which the parser reads as zero. Zero here
    // is therefore no weight at all, and filing it would outrank the meter.
    if (spool.initial_weight_g > 0) {
        stated.total_weight_g = static_cast<float>(spool.initial_weight_g);
        stated.remaining_weight_g = static_cast<float>(spool.remaining_weight_g);
    }
    // Read back rather than compared with `stated`: the store is what decides
    // whether the lane took the filing at all.
    const std::optional<helix::ams::Observation> before = helix::ams::lane_sources(lane).spoolman;
    helix::ams::ingest(lane, stated);
    const std::optional<helix::ams::Observation> after = helix::ams::lane_sources(lane).spoolman;
    if (before.has_value() != after.has_value()) {
        return true;
    }
    return before.has_value() && before->fields() != after->fields();
}

// ============================================================================
// Spoolman identity side channel
//
// Every entry point below is static and checks s_shutdown_flag BEFORE touching
// the singleton: the flag has its own storage and stays readable after the
// instance is gone, which is what makes these callable from an HTTP-thread
// completion during teardown. None of them calls into LVGL.
// ============================================================================

std::optional<helix::SpoolIdentity> SpoolmanManager::find_identity(int spool_id) {
    if (spool_id <= 0 || s_shutdown_flag.load(std::memory_order_acquire)) {
        return std::nullopt;
    }

    SpoolmanManager& mgr = instance();
    std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

    auto it = mgr.identity_cache_.find(spool_id);
    if (it == mgr.identity_cache_.end()) {
        return std::nullopt;
    }
    return it->second; // by value — the map can rehash under a later poll
}

bool SpoolmanManager::cache_identity(const SpoolInfo& spool) {
    if (spool.id <= 0 || s_shutdown_flag.load(std::memory_order_acquire)) {
        return false;
    }

    SpoolmanManager& mgr = instance();
    std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

    // A record came back, so whatever made this id unresolvable is over.
    mgr.identity_unresolvable_.erase(spool.id);

    // Identity is immutable in practice. Skipping the re-extraction here is the
    // whole point of splitting the cadences: the weight fetch that carried this
    // record still lands, the identity work happens once.
    if (mgr.identity_cache_.count(spool.id) > 0) {
        spdlog::trace("[SpoolmanManager] Identity for spool {} already cached, not re-extracting",
                      spool.id);
        return false;
    }

    helix::SpoolIdentity identity = identity_from_spool(spool);
    if (!identity.valid()) {
        // Ids and a colour hex cannot name anything — storing this would be
        // indistinguishable from a miss to the resolver, and would block a
        // later, better record from being taken.
        spdlog::trace("[SpoolmanManager] Spool {} carries no usable identity, not caching",
                      spool.id);
        return false;
    }

    spdlog::debug("[SpoolmanManager] Cached identity for spool {}: vendor='{}' name='{}' "
                  "material='{}'",
                  spool.id, identity.vendor, identity.filament_name, identity.material);
    mgr.identity_cache_.emplace(spool.id, std::move(identity));
    return true;
}

void SpoolmanManager::note_identity_unresolvable(int spool_id) {
    if (spool_id <= 0 || s_shutdown_flag.load(std::memory_order_acquire)) {
        return;
    }

    SpoolmanManager& mgr = instance();
    std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

    mgr.identity_cache_.erase(spool_id);
    if (mgr.identity_unresolvable_.insert(spool_id).second) {
        spdlog::info("[SpoolmanManager] Spool {} is not in Spoolman — no longer polling it",
                     spool_id);
    }
}

bool SpoolmanManager::is_identity_unresolvable(int spool_id) {
    if (spool_id <= 0 || s_shutdown_flag.load(std::memory_order_acquire)) {
        return false;
    }

    SpoolmanManager& mgr = instance();
    std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
    return mgr.identity_unresolvable_.count(spool_id) > 0;
}

void SpoolmanManager::invalidate_identity(int spool_id) {
    if (spool_id <= 0 || s_shutdown_flag.load(std::memory_order_acquire)) {
        return;
    }

    SpoolmanManager& mgr = instance();
    std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

    const bool had = mgr.identity_cache_.erase(spool_id) > 0;
    const bool was_dead = mgr.identity_unresolvable_.erase(spool_id) > 0;
    if (had || was_dead) {
        spdlog::debug("[SpoolmanManager] Invalidated cached identity for spool {}", spool_id);
    }
}

void SpoolmanManager::clear_identity_cache() {
    if (s_shutdown_flag.load(std::memory_order_acquire)) {
        return;
    }

    SpoolmanManager& mgr = instance();
    std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

    if (!mgr.identity_cache_.empty() || !mgr.identity_unresolvable_.empty()) {
        spdlog::debug("[SpoolmanManager] Clearing identity cache ({} entries, {} unresolvable)",
                      mgr.identity_cache_.size(), mgr.identity_unresolvable_.size());
    }
    mgr.identity_cache_.clear();
    mgr.identity_unresolvable_.clear();
}

void SpoolmanManager::reset_circuit_breaker() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    last_refresh_ms_ = 0;
    consecutive_failures_ = 0;
    cb_tripped_at_ms_ = 0;
    cb_open_ = false;
    unavailable_notified_ = false;
}

void SpoolmanManager::start_spoolman_polling() {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        // Record the wish unconditionally. Returning early here instead used to
        // discard it outright: at boot every caller arrives before Spoolman is
        // marked available, so the Home panel polled nothing for the whole session.
        ++poll_refcount_;
        spdlog::debug("[SpoolmanManager] Starting Spoolman polling (refcount: {})", poll_refcount_);
    }

    // Outside the lock: ensure_poll_timer() takes mutex_ itself and ends in
    // refresh_spoolman_weights(), which reads AmsState, and AmsState is never
    // entered with mutex_ held (see refresh_spoolman_weights).
    ensure_poll_timer();
}

void SpoolmanManager::ensure_poll_timer() {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (poll_refcount_ == 0 || poll_timer_ != nullptr) {
            return;
        }

        if (!get_printer_state().is_spoolman_available()) {
            spdlog::debug(
                "[SpoolmanManager] Spoolman not available yet, poll deferred (refcount: {})",
                poll_refcount_);
            return;
        }

        poll_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* self = static_cast<SpoolmanManager*>(lv_timer_get_user_data(timer));
                self->refresh_spoolman_weights();
            },
            kPollIntervalMs, this);
    }

    // Also do an immediate refresh - outside the lock. refresh_spoolman_weights()
    // reads AmsState, which is never entered with mutex_ held. The timer callback
    // above already runs lock-free.
    refresh_spoolman_weights();
}

void SpoolmanManager::stop_spoolman_polling() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (poll_refcount_ > 0) {
        --poll_refcount_;
    }

    spdlog::debug("[SpoolmanManager] Stopping Spoolman polling (refcount: {})", poll_refcount_);

    // Only delete timer when refcount reaches zero
    // Guard against LVGL already being deinitialized during shutdown
    if (poll_refcount_ == 0 && poll_timer_ && lv_is_initialized()) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }
}
