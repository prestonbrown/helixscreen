// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ams_state_external_spool.cpp
 * @brief AmsState: the external (bypass) spool's lane record and its edit funnel
 *
 * One of the files AmsState's definitions are split across by concern; the
 * class and its threading contract are in ams_state.h.
 */

#include "ams_state.h"
#include "ams_state_internal.h"
#include "core/update_queue.h"
#include "filament_accounting_controller.h"
#include "filament_slot_override_store.h"
#include "i_moonraker_api.h"
#include "lane_apply.h"
#include "lane_binding.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "spoolman_manager.h"

#include <spdlog/spdlog.h>

namespace helix {
using ams_state_detail::assert_main_thread;

namespace {
/// How often AmsState re-reads the bypass record it keeps itself, the same
/// period a backend polls its lane records at.
constexpr uint32_t kBypassPollPeriodMs = 10'000;

/// AmsState keeps the bypass record at lane 0 of lane_data: with no filament
/// system the bypass spool is the printer's only lane.
constexpr int kOwnBypassIndex = 0;

constexpr const char* kBypassTag = "[AmsState bypass]";
} // namespace

AmsBackend* AmsState::bypass_record_holder() const {
    AmsBackend* primary = registry_.get(0);
    return primary != nullptr && primary->holds_bypass_record() ? primary : nullptr;
}

std::optional<helix::ams::FilamentSlotOverride> AmsState::bypass_record() const {
    if (AmsBackend* holder = bypass_record_holder()) {
        return holder->bypass_record();
    }
    return bypass_record_read_;
}

std::optional<SlotInfo> AmsState::raw_external_spool_info() const {
    assert_main_thread();
    const helix::ams::LaneSources all = helix::ams::lane_sources(helix::ams::kBypassLaneId);
    if (!all.record.has_value() && !all.pending.has_value()) {
        return std::nullopt;
    }
    // The record's temps have no lane source to ride, so they come from the
    // record itself; everything else from what the bypass lane holds.
    SlotInfo out;
    if (const auto record = all.record.has_value() ? bypass_record() : std::nullopt) {
        out = helix::ams::external_spool_from_record(*record);
    } else {
        out = helix::ams::external_spool_from_record(helix::ams::FilamentSlotOverride{});
    }
    helix::ams::LaneSources held;
    held.record = all.record;
    held.pending = all.pending;
    helix::ams::apply_resolved(out, helix::ams::resolve(held));
    return out;
}

std::optional<SlotInfo> AmsState::get_external_spool_info() const {
    assert_main_thread();
    helix::ams::LaneSources sources = helix::ams::lane_sources(helix::ams::kBypassLaneId);
    std::optional<SlotInfo> out = raw_external_spool_info();
    if (!out.has_value()) {
        // A spool weight load cell meters a spool nobody has recorded.
        if (!sources.metered.has_value()) {
            return out;
        }
        out = helix::ams::external_spool_from_record(helix::ams::FilamentSlotOverride{});
    }
    // The record's weights are the base the meter counts down from, so the
    // meter and the Spoolman record rank above them here.
    if (sources.record.has_value()) {
        sources.record->remaining_weight_g.reset();
        sources.record->total_weight_g.reset();
    }
    helix::ams::apply_resolved(*out, helix::ams::resolve(sources));
    return out;
}

void AmsState::set_external_spool_info(const SlotInfo& info) {
    assert_main_thread();
    store_external_spool_info(info);
    notify_external_spool_changed(get_external_spool_info().value_or(info));
}

void AmsState::store_external_spool_info(const SlotInfo& info) {
    // Moving the binding to a different spool retires the previous spool's
    // records: its Spoolman record and the meter's count describe a spool
    // that is no longer bound. Every writer passes through here (the edit
    // funnel, the active-spool sync, the meter), so the reconcile lives here.
    const SlotInfo original = raw_external_spool_info().value_or(SlotInfo{});
    if (original.spoolman_id != info.spoolman_id) {
        helix::ams::drop_previous_spool_declarations(helix::ams::kBypassLaneId);
        helix::ams::drop_lane_source(helix::ams::kBypassLaneId,
                                     helix::ams::ObservationSource::Metered);
    }
    helix::ams::file_pending(helix::ams::kBypassLaneId,
                             helix::ams::user_edit_observation(original, info));
    const helix::ams::FilamentSlotOverride record = helix::ams::external_lane_record(info);
    write_bypass_record(&record);
}

void AmsState::write_bypass_record(const helix::ams::FilamentSlotOverride* record) {
    ++bypass_write_generation_;
    if (AmsBackend* holder = bypass_record_holder()) {
        holder->publish_external_spool_lane(record);
        return;
    }
    if (!bypass_store_) {
        spdlog::warn("{} not written: no Moonraker connection", kBypassTag);
        helix::ams::withdraw_pending(helix::ams::kBypassLaneId, "no Moonraker connection");
        return;
    }
    if (record != nullptr) {
        helix::ams::save_override_async(bypass_store_.get(), kOwnBypassIndex, *record, kBypassTag,
                                        "Bypass spool", helix::ams::kBypassLaneId);
        return;
    }
    bypass_record_read_.reset();
    bypass_store_->clear_async(kOwnBypassIndex, [](bool ok, const std::string& err) {
        if (!ok) {
            spdlog::warn("{} deleting the record failed: {}", kBypassTag, err);
        }
    });
}

void AmsState::restart_bypass_reads() {
    bypass_poll_.reset();
    bypass_store_.reset();
    bypass_record_read_.reset();
    bypass_store_read_ = false;
    bypass_read_in_flight_ = false;
    bypass_read_again_ = false;
    if (api_ == nullptr) {
        sync_lane_records_ready();
        return;
    }
    bypass_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        api_, "bypass", helix::ams::LaneKeyStyle::Lane);
    sync_lane_records_ready();
    bypass_store_->set_on_written([token = async_lifetime_.token()]() {
        token.defer("AmsState::refresh_bypass_record",
                    []() { AmsState::instance().refresh_bypass_record(); });
    });
    if (lv_is_initialized()) {
        bypass_poll_.reset(
            lv_timer_create([](lv_timer_t*) { AmsState::instance().refresh_bypass_record(); },
                            kBypassPollPeriodMs, nullptr));
    }
}

void AmsState::refresh_bypass_record() {
    assert_main_thread();
    if (!bypass_store_ || bypass_record_holder() != nullptr) {
        return;
    }
    if (bypass_read_in_flight_) {
        bypass_read_again_ = true;
        return;
    }
    bypass_read_in_flight_ = true;
    auto token = async_lifetime_.token();
    helix::ams::FilamentSlotOverrideStore* store = bypass_store_.get();
    const uint64_t generation = bypass_write_generation_;
    store->reload_async(
        [token, store, generation](std::unordered_map<int, helix::ams::LaneDataRecord> records) {
            token.defer("AmsState::adopt_bypass_records",
                        [store, generation, records = std::move(records)]() {
                            AmsState& self = AmsState::instance();
                            // A reconnect replaced the store while this read was out.
                            if (self.bypass_store_.get() != store) {
                                return;
                            }
                            self.bypass_read_in_flight_ = false;
                            // Read before a write of ours landed: it says nothing about the
                            // record now, so it is not filed; the write's own read follows.
                            if (generation == self.bypass_write_generation_) {
                                self.adopt_bypass_records(records);
                            } else {
                                self.bypass_read_again_ = true;
                            }
                            if (self.bypass_read_again_) {
                                self.bypass_read_again_ = false;
                                self.refresh_bypass_record();
                            }
                        });
        },
        [token]() {
            token.defer("AmsState::bypass_record_unread", []() {
                AmsState& self = AmsState::instance();
                self.bypass_read_in_flight_ = false;
                self.bypass_read_again_ = false;
            });
        });
}

void AmsState::adopt_bypass_records(
    const std::unordered_map<int, helix::ams::LaneDataRecord>& records) {
    bypass_store_->adopt_read(records);
    const auto found = helix::ams::find_bypass_record(records);
    std::optional<helix::ams::FilamentSlotOverride> record;
    if (found.has_value()) {
        record = found->second;
    }
    helix::ams::file_bypass_record(record ? &*record : nullptr);
    bypass_record_read_ = std::move(record);
    bypass_store_read_ = true;
    sync_lane_records_ready();
    refresh_external_spool();
    run_legacy_spool_import();
}

void AmsState::refresh_external_spool() {
    assert_main_thread();
    if (!initialized_) {
        return;
    }
    if (const auto info = get_external_spool_info()) {
        notify_external_spool_changed(*info);
        return;
    }
    // Force notification even when color was already 0 (e.g. previous spool was
    // black, RGB=0x000000) — observers read full spool info, not just the color.
    if (lv_subject_get_int(&external_spool_color_) == 0) {
        lv_subject_set_int(&external_spool_color_, 1);
    }
    lv_subject_set_int(&external_spool_color_, 0);
    lv_subject_copy_string(&external_spool_material_, "");
    if (registry_.count() == 0) {
        sync_current_loaded_from_backend();
    }
}

void AmsState::persist_external_spool_identity() {
    assert_main_thread();
    const helix::ams::LaneSources sources = helix::ams::lane_sources(helix::ams::kBypassLaneId);
    std::optional<helix::ams::FilamentSlotOverride> record = bypass_record();
    if (!sources.spoolman.has_value() || !sources.record.has_value() || !record.has_value()) {
        return;
    }
    std::unordered_map<int, helix::ams::FilamentSlotOverride> amended{{kOwnBypassIndex, *record}};
    if (!helix::ams::persist_override_external_identity(nullptr, amended, kOwnBypassIndex,
                                                        *sources.spoolman, kBypassTag)) {
        return;
    }
    write_bypass_record(&amended[kOwnBypassIndex]);
}

void AmsState::notify_external_spool_changed(const SlotInfo& info) {
    if (!initialized_) {
        return;
    }
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
    if (registry_.count() == 0) {
        sync_current_loaded_from_backend();
    }
}

void AmsState::clear_external_spool_info() {
    assert_main_thread();
    // The record's absence has to reach the sources that described it, or
    // resolve() keeps returning the identity the clear just removed.
    helix::ams::reset_lane_to_machine_readings(helix::ams::kBypassLaneId);
    write_bypass_record(nullptr);
    refresh_external_spool();
}

void AmsState::apply_external_spool_store(const SlotInfo& info) {
    // S5 + S7 — same emptiness predicate as the FilamentPanel completion arm
    const SlotInfo original = raw_external_spool_info().value_or(SlotInfo{});
    if (info.spoolman_id > 0 || !info.material.empty()) {
        store_external_spool_info(info);
        // The record is the edit; a meter count from before it no longer
        // describes the spool.
        if (info.remaining_weight_g != original.remaining_weight_g) {
            helix::ams::drop_lane_source(helix::ams::kBypassLaneId,
                                         helix::ams::ObservationSource::Metered);
        }
        notify_external_spool_changed(get_external_spool_info().value_or(info));
    } else {
        clear_external_spool_info();
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
