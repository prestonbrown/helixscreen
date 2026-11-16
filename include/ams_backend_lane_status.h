// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "core/timer_guard.h"
#include "lane_status_parse.h"

#include <string>

namespace helix {

/// Any producer of the Lane Status Surface (lane-status-spec RFC 0001): the
/// `lane_status` Klipper object for live state, and the `lane_data` records
/// its producer writes for identity, joined by lane name.
///
/// The producer owns its records (SPEC.md §5 "one writer per machine"), so
/// nothing here writes lane_data and edits are refused. Load and Unload are
/// offered only by a `controller` that lists `load` / `unload`, and send the
/// spec's `CHANGE_TOOL LANE=` and `TOOL_UNLOAD LANE=`. A printer with a
/// backend of its own never reaches this one: discovery tries it last.
class AmsBackendLaneStatus : public AmsSubscriptionBackend {
  public:
    AmsBackendLaneStatus(IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~AmsBackendLaneStatus() override = default;

    /// `{lane_status: [every key the spec defines]}` for a printer discovery
    /// gave to this backend; empty for any other.
    [[nodiscard]] static nlohmann::json required_status_objects(const helix::PrinterDiscovery& hw);

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::LANE_STATUS;
    }
    [[nodiscard]] AmsSystemInfo get_system_info() const override;

    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::HUB;
    }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    /// The producer's own lane name.
    [[nodiscard]] std::string lane_name(int global_slot) const override;

    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;
    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;

    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    /// The producer owns every record; an edit here would be a second writer.
    AmsError apply_user_edit(int slot_index, const SlotInfo& info) override;
    void clear_slot_override(int slot_index) override;
    [[nodiscard]] std::string clear_spool_refusal(int slot_index) const override;

    [[nodiscard]] AmsError can_set_tool_mapping(int tool_number, int slot_index) const override;

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override {
        return false;
    }

    /// The last status read, for tests and the device view.
    [[nodiscard]] lane_status::Status status() const;

    /// Adopt a `lane_data` namespace document as the records last read. Main
    /// thread. The poll calls it; a test hands it a document directly.
    void adopt_records(const nlohmann::json& namespace_doc);

  protected:
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_change_tool(int tool_number) override;

    void on_started() override;
    void on_stopping() override;
    void handle_status(const nlohmann::json& status) override;
    const char* backend_log_tag() const override {
        return "[AMS LaneStatus]";
    }
    SlotInfo* cached_slot_locked(int slot_index) override {
        return system_info_.get_slot_global(slot_index);
    }
    void persist_slot_weight(int slot_index, float remaining_weight_g,
                             float total_weight_g) override;
    void persist_external_identity_impl(int slot_index,
                                        const helix::ams::Observation& spoolman) override;

  private:
    /// Read lane_data and adopt it. Main thread.
    void read_records();
    /// Rebuild system_info_ from snapshot_ and records_. Caller holds mutex_.
    void rebuild_locked();
    /// A refusal for a command the producer does not offer, else success.
    [[nodiscard]] AmsError offered_locked(const char* capability, const char* what) const;

    /// Every key of lane_status so far: an update carries only the changed ones.
    nlohmann::json snapshot_;
    nlohmann::json records_;
    lane_status::Status status_;
    /// Slots filed with lane sources on the last rebuild, so a shrink drops
    /// the sources the removed lanes left.
    int filed_slots_ = 0;
    helix::ui::LvglTimerGuard record_poll_;
};

} // namespace helix
