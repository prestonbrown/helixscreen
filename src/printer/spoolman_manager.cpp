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
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

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
    using helix::ui::observe_int_sync;
    using helix::ui::observe_print_state;
    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - weights change when
    // filament moves, which is the printer's own transition.
    print_state_observer_ = observe_print_state<SpoolmanManager>(
        get_printer_state().get_print_state_enum_subject(), this,
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
    spoolman_availability_observer_ = observe_int_sync<SpoolmanManager>(
        get_printer_state().get_printer_has_spoolman_subject(), this,
        [](SpoolmanManager* self, int value) {
            if (value == 0) {
                std::lock_guard<std::recursive_mutex> lock(self->mutex_);
                spdlog::info("[SpoolmanManager] Spoolman became unavailable, stopping polling");
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

void SpoolmanManager::refresh_spoolman_weights() {
    // Resolve everything we need from AmsState BEFORE taking our own mutex_.
    // AmsState::sync_from_backend() holds AmsState::mutex_ across its call to
    // SpoolmanManager::find_identity(), so the canonical order is
    // AmsState -> SpoolmanManager; reaching into AmsState from under mutex_ closes an
    // ABBA cycle that ThreadSanitizer reports as a lock-order inversion. Both
    // accessors are const reads that take and release AmsState::mutex_ themselves, so
    // hoisting costs a vector index and a settings read on the early-return paths and
    // buys a one-way lock order.
    auto* backend = AmsState::instance().get_backend(0);
    auto ext_spool = AmsState::instance().get_external_spool_info();

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Mock mode polls too. AmsBackendMock seeds spoolman_id = lane + 1 to mirror
    // MoonrakerSpoolmanAPIMock::init_mock_spools() (ids 1-7), so those ids resolve
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
        if (elapsed < CB_BACKOFF_MS) {
            spdlog::trace("[SpoolmanManager] Spoolman circuit breaker open, {}ms remaining",
                          CB_BACKOFF_MS - elapsed);
            return;
        }
        // Backoff elapsed — half-open: allow one probe request through
        spdlog::info("[SpoolmanManager] Spoolman circuit breaker half-open, probing...");
        cb_open_ = false;
    }

    // Debounce: skip if called too recently
    if (last_refresh_ms_ > 0) {
        uint32_t since_last = now - last_refresh_ms_;
        if (since_last < DEBOUNCE_MS) {
            spdlog::trace("[SpoolmanManager] Spoolman refresh debounced ({}ms since last)",
                          since_last);
            return;
        }
    }
    last_refresh_ms_ = now;

    int linked_count = 0;

    // Refresh AMS backend slots (if a backend is active); `backend` was resolved
    // above the lock.
    if (backend) {
        // When the backend tracks weight locally (e.g., AFC decrements weight
        // via extruder position), we still need total_weight_g (initial weight)
        // from Spoolman — the backend only provides remaining weight.
        bool local_weight = backend->tracks_weight_locally();
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

                api_->spoolman().get_spoolman_spool(
                    spoolman_id,
                    [slot_index, spoolman_id,
                     local_weight](const std::optional<SpoolInfo>& spool_opt) {
                        if (!spool_opt.has_value()) {
                            spdlog::warn("[SpoolmanManager] Spoolman spool {} not found",
                                         spoolman_id);
                            // "No such spool" is an answer, not a transport
                            // failure — the circuit breaker must not see it, but
                            // the id must stop being polled.
                            helix::ui::queue_update(
                                [spoolman_id]() { note_identity_unresolvable(spoolman_id); });
                            return;
                        }

                        const SpoolInfo& spool = spool_opt.value();

                        // Data to pass to UI thread
                        struct WeightUpdate {
                            int slot_index;
                            int expected_spoolman_id; // To verify slot wasn't reassigned
                            float remaining_weight_g;
                            float total_weight_g;
                            bool local_weight; // Backend tracks remaining weight locally
                            // Whole record, carried only so the identity side
                            // channel can be filled on the UI thread. NOTHING
                            // from here is written onto the slot — see the
                            // persist=false note below.
                            SpoolInfo spool;
                        };

                        auto update_data = std::make_unique<WeightUpdate>(WeightUpdate{
                            slot_index, spoolman_id, static_cast<float>(spool.remaining_weight_g),
                            static_cast<float>(spool.initial_weight_g), local_weight, spool});

                        helix::ui::queue_update<
                            WeightUpdate>(std::move(update_data), [](WeightUpdate* d) {
                            // Skip if shutdown is in progress
                            if (s_shutdown_flag.load(std::memory_order_acquire)) {
                                return;
                            }

                            SpoolmanManager& mgr = SpoolmanManager::instance();

                            // Our own state, under our own lock, released before the
                            // AmsState work below. AmsState::sync_from_backend() holds
                            // AmsState::mutex_ across SpoolmanManager::find_identity(),
                            // so carrying mutex_ into AmsState here closes an ABBA cycle
                            // that ThreadSanitizer reports as a lock-order inversion.
                            bool identity_is_new = false;
                            {
                                std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

                                // Success response — reset circuit breaker (on UI thread)
                                if (mgr.consecutive_failures_ > 0) {
                                    spdlog::info(
                                        "[SpoolmanManager] Spoolman recovered after {} failures",
                                        mgr.consecutive_failures_);
                                }
                                mgr.consecutive_failures_ = 0;
                                mgr.unavailable_notified_ = false;

                                // Identity side channel. Must happen BEFORE the
                                // slot-reassignment and weights-unchanged early
                                // returns below — a slot whose weight never moves
                                // would otherwise never get a name.
                                identity_is_new = cache_identity(d->spool);
                            }

                            AmsState& ams = AmsState::instance();
                            if (identity_is_new) {
                                // A name the label consumers could not resolve a
                                // moment ago just became resolvable. The weight
                                // early-returns below would otherwise swallow it
                                // whenever the weight happens not to move, which
                                // is every poll once a spool settles (#1264).
                                ams.bump_slots_version();
                            }
                            auto* primary = ams.get_backend(0);
                            if (!primary) {
                                return;
                            }

                            // Get current slot info and verify it wasn't reassigned
                            SlotInfo slot = primary->get_slot_info(d->slot_index);
                            if (slot.spoolman_id != d->expected_spoolman_id) {
                                spdlog::debug(
                                    "[SpoolmanManager] Slot {} spoolman_id changed ({} -> {}), "
                                    "skipping stale weight update",
                                    d->slot_index, d->expected_spoolman_id, slot.spoolman_id);
                                return;
                            }

                            // When backend tracks weight locally, only update total_weight
                            // (initial weight from Spoolman). Preserve the backend's
                            // remaining_weight which is more accurate than Spoolman's.
                            float new_remaining =
                                d->local_weight ? slot.remaining_weight_g : d->remaining_weight_g;

                            // Skip update if weights haven't changed (avoids UI refresh cascade)
                            if (slot.remaining_weight_g == new_remaining &&
                                slot.total_weight_g == d->total_weight_g) {
                                spdlog::trace("[SpoolmanManager] Slot {} weights unchanged "
                                              "({:.0f}g / {:.0f}g)",
                                              d->slot_index, new_remaining, d->total_weight_g);
                                return;
                            }

                            // Update weights and set back.
                            // CRITICAL: persist=false prevents an infinite feedback loop.
                            // With persist=true, set_slot_info sends G-code to firmware
                            // (e.g., SET_WEIGHT for AFC, MMU_GATE_MAP for Happy Hare).
                            // Firmware then emits a status_update WebSocket event, which
                            // triggers sync_from_backend -> refresh_spoolman_weights ->
                            // set_slot_info again, ad infinitum. With 4 AFC lanes this
                            // fires 16+ G-code commands per cycle and saturates the CPU.
                            // Since these weights come FROM Spoolman (an external source),
                            // there's no need to write them back to firmware.
                            slot.remaining_weight_g = new_remaining;
                            slot.total_weight_g = d->total_weight_g;
                            primary->set_slot_info(d->slot_index, slot, /*persist=*/false);
                            ams.bump_slots_version();

                            spdlog::debug(
                                "[SpoolmanManager] Updated slot {} weights: {:.0f}g / {:.0f}g{}",
                                d->slot_index, new_remaining, d->total_weight_g,
                                d->local_weight ? " (local remaining)" : "");
                        });
                    },
                    [spoolman_id](const MoonrakerError& err) {
                        spdlog::warn("[SpoolmanManager] Failed to fetch Spoolman spool {}: {}",
                                     spoolman_id, err.message);

                        // Track failure for circuit breaker (post to UI thread for
                        // thread-safe access to SpoolmanManager and ToastManager)
                        helix::ui::queue_update([]() {
                            if (s_shutdown_flag.load(std::memory_order_acquire)) {
                                return;
                            }

                            SpoolmanManager& mgr = SpoolmanManager::instance();
                            std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

                            mgr.consecutive_failures_++;

                            if (mgr.consecutive_failures_ >= CB_FAILURE_THRESHOLD) {
                                mgr.cb_open_ = true;
                                mgr.cb_tripped_at_ms_ = lv_tick_get();
                                spdlog::warn(
                                    "[SpoolmanManager] Spoolman circuit breaker OPEN after {} "
                                    "failures, backing off {}s",
                                    mgr.consecutive_failures_, CB_BACKOFF_MS / 1000);

                                // Notify user once per outage — only if Spoolman is
                                // actually configured (avoid confusing toast on printers
                                // that never set up Spoolman)
                                if (!mgr.unavailable_notified_) {
                                    mgr.unavailable_notified_ = true;
                                    auto* subj =
                                        lv_xml_get_subject(nullptr, "printer_has_spoolman");
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
        }
    } // if (backend)

    // Also refresh external spool if it has a Spoolman link (`ext_spool` was
    // resolved above the lock).
    if (ext_spool.has_value() && ext_spool->spoolman_id > 0 &&
        !is_identity_unresolvable(ext_spool->spoolman_id)) {
        ++linked_count;
        int ext_spoolman_id = ext_spool->spoolman_id;

        api_->spoolman().get_spoolman_spool(
            ext_spoolman_id,
            [ext_spoolman_id](const std::optional<SpoolInfo>& spool_opt) {
                if (!spool_opt.has_value()) {
                    spdlog::warn("[SpoolmanManager] External spool Spoolman #{} not found",
                                 ext_spoolman_id);
                    helix::ui::queue_update(
                        [ext_spoolman_id]() { note_identity_unresolvable(ext_spoolman_id); });
                    return;
                }

                const SpoolInfo& spool = spool_opt.value();
                float new_remaining = static_cast<float>(spool.remaining_weight_g);
                float new_total = static_cast<float>(spool.initial_weight_g);

                helix::ui::queue_update([ext_spoolman_id, new_remaining, new_total, spool]() {
                    if (s_shutdown_flag.load(std::memory_order_acquire)) {
                        return;
                    }

                    // Before the unchanged-weights early return below, same as
                    // the AMS slot path.
                    const bool identity_is_new = cache_identity(spool);

                    AmsState& state = AmsState::instance();
                    if (identity_is_new) {
                        state.bump_slots_version();
                    }
                    auto ext = state.get_external_spool_info();
                    if (!ext.has_value() || ext->spoolman_id != ext_spoolman_id) {
                        spdlog::debug(
                            "[SpoolmanManager] External spool changed, skipping stale update");
                        return;
                    }

                    // Skip if weights unchanged
                    if (ext->remaining_weight_g == new_remaining &&
                        ext->total_weight_g == new_total) {
                        return;
                    }

                    ext->remaining_weight_g = new_remaining;
                    ext->total_weight_g = new_total;
                    state.set_external_spool_info(*ext);

                    spdlog::debug(
                        "[SpoolmanManager] Updated external spool weights: {:.0f}g / {:.0f}g",
                        new_remaining, new_total);
                });
            },
            [ext_spoolman_id](const MoonrakerError& err) {
                spdlog::warn("[SpoolmanManager] Failed to fetch external spool Spoolman #{}: {}",
                             ext_spoolman_id, err.message);
            },
            /*silent=*/true);
    }

    if (linked_count > 0) {
        spdlog::trace("[SpoolmanManager] Refreshing Spoolman weights for {} linked slots",
                      linked_count);
    }
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
    // refresh_spoolman_weights(), which reads AmsState. mutex_ is recursive, so
    // holding it across this call nested silently and put AmsState::mutex_ under
    // it - the ABBA cycle TSan reports as a lock-order inversion.
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
            POLL_INTERVAL_MS, this);
    }

    // Also do an immediate refresh - outside the lock. refresh_spoolman_weights()
    // reads AmsState, and AmsState::sync_from_backend() holds AmsState::mutex_
    // across SpoolmanManager::find_identity(); calling it under mutex_ closes the
    // ABBA cycle. The timer callback above already runs lock-free.
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
