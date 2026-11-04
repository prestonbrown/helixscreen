// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ams_state_external_spool.cpp
 * @brief AmsState: the external (bypass) spool record and its edit funnel
 *
 * One of the files AmsState's definitions are split across by concern; the
 * class and its threading contract are in ams_state.h.
 */

#include "ams_state.h"
#include "ams_state_internal.h"
#include "core/update_queue.h"
#include "filament_accounting_controller.h"
#include "i_moonraker_api.h"
#include "lane_apply.h"
#include "lane_binding.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "settings_manager.h"
#include "spoolman_manager.h"

#include <spdlog/spdlog.h>

namespace helix {
using ams_state_detail::assert_main_thread;

std::optional<SlotInfo> AmsState::raw_external_spool_info() const {
    assert_main_thread();
    // In-memory override takes priority when set (e.g. live tracker updates).
    if (in_memory_external_spool_.has_value()) {
        return in_memory_external_spool_;
    }
    return helix::SettingsManager::instance().get_external_spool_info();
}

std::optional<SlotInfo> AmsState::get_external_spool_info() const {
    assert_main_thread();
    std::optional<SlotInfo> out = raw_external_spool_info();
    if (!out.has_value()) {
        return out;
    }
    // The bypass lane carries what the sources say about this spool: the
    // weight poll's Spoolman record, the consumption meter's count, the
    // user's own edits. A Spoolman record standing from an earlier binding
    // names a spool the raw record does not, so it is dropped from the copy
    // rather than resolved - the same ranking a lane's declared id gets.
    helix::ams::LaneSources sources = helix::ams::lane_sources(helix::ams::kBypassLaneId);
    if (sources.spoolman.has_value() &&
        sources.spoolman->spoolman_id.value_or(0) != out->spoolman_id) {
        sources.drop(helix::ams::ObservationSource::Spoolman);
    }
    helix::ams::apply_resolved(*out, helix::ams::resolve(sources));
    return out;
}

void AmsState::set_external_spool_info_in_memory(const SlotInfo& info) {
    assert_main_thread();
    in_memory_external_spool_ = info;
    notify_external_spool_changed(info);
}

void AmsState::set_external_spool_info(const SlotInfo& info) {
    assert_main_thread();
    store_external_spool_info(info);
    notify_external_spool_changed(info);
}

void AmsState::store_external_spool_info(const SlotInfo& info) {
    // Moving the binding to a different spool retires the previous spool's
    // records: its Spoolman record, the user's pick, the kept identity and the
    // meter's count all describe a spool that is no longer bound, and resolve()
    // would keep ranking them onto the new one. The meter goes too, unlike a
    // lane's binding change: a lane's firmware re-files its meter every frame,
    // but nothing re-files the bypass meter while a spool is linked (the
    // consumption sink pauses), so a stale count would stand. Every persistent writer passes
    // through here (the edit funnel, the active-spool sync), so the reconcile lives at the funnel
    // rather than at each caller.
    const int previous_id = raw_external_spool_info().value_or(SlotInfo{}).spoolman_id;
    if (previous_id != info.spoolman_id) {
        helix::ams::drop_previous_spool_declarations(helix::ams::kBypassLaneId);
        helix::ams::drop_lane_source(helix::ams::kBypassLaneId,
                                     helix::ams::ObservationSource::Metered);
    }
    in_memory_external_spool_.reset(); // Persistent write wins; let SettingsManager be the source.
    helix::SettingsManager::instance().set_external_spool_info(info);
}

void AmsState::notify_external_spool_changed(const SlotInfo& info) {
    // Always notify observers — spool data (weight, name, etc.) may change
    // even when color stays the same
    int new_color = static_cast<int>(info.color_rgb);
    int old_color = lv_subject_get_int(&external_spool_color_);
    if (old_color == new_color) {
        // Force notification by toggling value
        lv_subject_set_int(&external_spool_color_, new_color ^ 1);
    }
    lv_subject_set_int(&external_spool_color_, new_color);
    // Material string reflector — copy_string only notifies on change, and
    // color observers re-read full spool info anyway, so no force-fire needed.
    lv_subject_copy_string(&external_spool_material_, info.material.c_str());
}

void AmsState::clear_external_spool_info() {
    assert_main_thread();
    in_memory_external_spool_.reset();
    helix::SettingsManager::instance().clear_external_spool_info();
    // The raw record's absence has to reach the sources that described it, or
    // resolve() keeps returning the identity the clear just removed.
    helix::ams::reset_lane_to_machine_readings(helix::ams::kBypassLaneId);
    // Force notification even when color was already 0 (e.g. previous spool was
    // black, RGB=0x000000) — observers read full spool info, not just the color.
    if (lv_subject_get_int(&external_spool_color_) == 0) {
        lv_subject_set_int(&external_spool_color_, 1);
    }
    lv_subject_set_int(&external_spool_color_, 0);
    lv_subject_copy_string(&external_spool_material_, "");
}

void AmsState::apply_external_spool_store(const SlotInfo& info) {
    // S5 + S7 — same emptiness predicate as the FilamentPanel completion arm
    const SlotInfo original = raw_external_spool_info().value_or(SlotInfo{});
    if (info.spoolman_id > 0 || !info.material.empty()) {
        store_external_spool_info(info);
        // The edit files on the bypass lane like a slot edit files on its
        // lane: store_external_spool_info() has already taken the previous
        // spool's records with any binding move, and the user's own values
        // now stand as LocalUser.
        helix::ams::commit_slot_edit(helix::ams::kBypassLaneId,
                                     helix::ams::user_edit_observation(original, info));
        // Observers re-read the resolved record, where the lane's previous
        // LocalUser weight outranks the raw one, so they hear of the edit only
        // once the lane carries it.
        notify_external_spool_changed(info);
    } else {
        // Takes the lane's declarations with it, exactly as it takes the
        // stored record's identity.
        clear_external_spool_info();
    }

    // Keep the slicer-sync lane (OrcaSlicer lane_data mirror) fresh on every
    // identity change — same capability dispatch as the bypass-engage hook.
    for (auto& backend : registry_.backends()) {
        if (backend) {
            backend->publish_external_spool_lane(&info);
        }
    }
}

void AmsState::invalidate_stale_external_identity(const SlotInfo& info) {
    // S6 — stale identity otherwise survives until a server 404
    const int previous_id = get_external_spool_info().value_or(SlotInfo{}).spoolman_id;
    if (previous_id > 0 && previous_id != info.spoolman_id) {
        SpoolmanManager::invalidate_identity(previous_id);
    }
}

void AmsState::commit_external_spool_edit(const SlotInfo& info) {
    assert_main_thread();

    // S1 — match the server active spool to what we are committing, only
    // while the external spool is what feeds: set it unless the printer keeps
    // its own assignment; an unlink clears the spool it unlinked.
    if (api_ && external_spool_feeds()) {
        const int previous_id = get_external_spool_info().value_or(SlotInfo{}).spoolman_id;
        if (info.spoolman_id > 0 &&
            helix_sets_active_spool_on_edit(get_backend(), /*external_spool=*/true)) {
            api_->spoolman().set_active_spool(
                info.spoolman_id, []() {},
                [](const MoonrakerError& err) {
                    spdlog::warn("[AmsState] Failed to set active spool: {}", err.message);
                });
        } else if (info.spoolman_id == 0) {
            // A manual entry (id=0, material set) over a linked spool takes
            // the server link back too: the UI no longer shows it in use.
            clear_active_spool_if(previous_id);
        }
    }

    invalidate_stale_external_identity(info);

    // S5 + S7
    apply_external_spool_store(info);
}

void AmsState::commit_external_spool_edit(const SlotInfo& info, std::function<void()> on_committed,
                                          std::function<void(const MoonrakerError& err)> on_error) {
    assert_main_thread();

    invalidate_stale_external_identity(info);

    const bool feeds = api_ && external_spool_feeds();
    if (feeds && info.spoolman_id > 0 &&
        helix_sets_active_spool_on_edit(get_backend(), /*external_spool=*/true)) {
        // Server-first: the store subset waits for the server round-trip. The
        // API callbacks fire on a background thread, so both the store write
        // and the caller's completion are marshalled to the main thread.
        api_->spoolman().set_active_spool(
            info.spoolman_id,
            [info, on_committed = std::move(on_committed)]() {
                helix::ui::queue_update("AmsState::commit_external_spool_edit",
                                        [info, on_committed]() {
                                            if (ams_state_detail::shutting_down()) {
                                                return;
                                            }
                                            AmsState::instance().apply_external_spool_store(info);
                                            if (on_committed) {
                                                on_committed();
                                            }
                                        });
            },
            [on_error = std::move(on_error)](const MoonrakerError& err) {
                helix::ui::queue_update("AmsState::commit_external_spool_edit", [on_error, err]() {
                    if (on_error) {
                        on_error(err);
                    }
                });
            });
        return;
    }

    // Manual entry, clear, a spool something else makes active, or one that is
    // not feeding: no server identity gates the store write. The clear arm
    // (replacing a linked spool with an empty record) still tells the server,
    // fire-and-forget, exactly like the sync commit.
    if (feeds && info.spoolman_id == 0) {
        clear_active_spool_if(get_external_spool_info().value_or(SlotInfo{}).spoolman_id);
    }

    apply_external_spool_store(info);
    if (on_committed) {
        on_committed();
    }
}
} // namespace helix
