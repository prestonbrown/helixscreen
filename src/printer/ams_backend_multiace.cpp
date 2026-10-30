// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_SNAPMAKER

#include "ams_backend_multiace.h"

#include "ace_slot_parse.h"
#include "ams_fault_event.h"
#include "klipper_extruder_naming.h"
#include "lvgl.h"
#include "snapmaker_exceptions.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace helix {

namespace {

/// multiACE's console faults: the U1's coded-exception shape with module id
/// 525 (FEEDING) and a code in the 200 band (its ace.py#_ace_error registry).
constexpr int kMultiAceFaultId = 525;
constexpr int kMultiAceCodeFirst = 200;
constexpr int kMultiAceCodeLast = 299;

/// The head a multiACE fault names. The engine sends index 0 both for head 0
/// and for a fault tied to no head, so index 0 counts only when the message
/// names the head, which it does in its own 1-based words ("head 1").
std::optional<int> fault_head(int index, const std::string& message) {
    if (index < 0 || index >= multiace::kHeadCount) {
        return std::nullopt;
    }
    if (index > 0) {
        return index;
    }
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (size_t at = lower.find("head 1"); at != std::string::npos;
         at = lower.find("head 1", at + 1)) {
        const size_t end = at + 6;
        const bool word_start = at == 0 || !std::isalpha(static_cast<unsigned char>(lower[at - 1]));
        const bool number_ends =
            end >= lower.size() || !std::isdigit(static_cast<unsigned char>(lower[end]));
        if (word_start && number_ends) {
            return 0;
        }
    }
    return std::nullopt;
}

} // namespace

AmsBackendMultiAce::AmsBackendMultiAce(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : AmsBackendSnapmaker(api, client) {}

// ============================================================================
// Engine state
// ============================================================================

bool AmsBackendMultiAce::active_locked() const {
    return api_version_ == multiace::kApiVersion &&
           (mode_ == multiace::Mode::MULTI || mode_ == multiace::Mode::HEAD);
}

bool AmsBackendMultiAce::ace_fed_locked(int head) const {
    if (head < 0 || head >= kHeads || !active_locked()) {
        return false;
    }
    const auto h = static_cast<size_t>(head);
    return multiace::head_feed(mode_, head_manual_[h], head_feeder_[h]) == multiace::HeadFeed::ACE;
}

std::optional<std::pair<int, int>> AmsBackendMultiAce::routed_bay_locked(int head) const {
    if (head < 0 || head >= kHeads) {
        return std::nullopt;
    }
    const auto& src = head_source_[static_cast<size_t>(head)];
    if (!src || src->load_failed || src->ace < 0 || src->slot < 0) {
        return std::nullopt;
    }
    // A route naming a unit the engine is not reporting describes a bay this
    // backend cannot show; reading it would badge and caption a lane that has
    // no spool behind it.
    if (unit_position_locked(src->ace) < 0) {
        return std::nullopt;
    }
    return std::make_pair(src->ace, src->slot);
}

int AmsBackendMultiAce::unit_position_locked(int ace) const {
    for (size_t i = 0; i < units_.size(); ++i) {
        if (units_[i].idx == ace) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<int> AmsBackendMultiAce::head_for_global_locked(int global) const {
    const auto bay = multiace::bay_of(global);
    if (!bay || !active_locked() || unit_position_locked(bay->first) < 0) {
        return std::nullopt;
    }
    std::array<multiace::HeadFeed, kHeads> feed{};
    for (int h = 0; h < kHeads; ++h) {
        const auto i = static_cast<size_t>(h);
        feed[i] = multiace::head_feed(mode_, head_manual_[i], head_feeder_[i]);
    }
    return multiace::head_for_bay(mode_, bay->first, bay->second, head_ace_, feed);
}

const multiace::Unit* AmsBackendMultiAce::unit_for_index_locked(int unit_index) const {
    if (!active_locked() || unit_index < 1 || unit_index > static_cast<int>(units_.size())) {
        return nullptr;
    }
    return &units_[static_cast<size_t>(unit_index - 1)];
}

void AmsBackendMultiAce::apply_extension_locked(const nlohmann::json& status, bool& changed) {
    const auto it = status.find("ace");
    if (it == status.end()) {
        return;
    }
    const multiace::StatusDelta d = multiace::parse_ace_status(*it);

    if (d.api_version) {
        api_version_ = d.api_version;
        if (*d.api_version != multiace::kApiVersion && !api_mismatch_logged_) {
            spdlog::warn("{} multiACE reports engine API {}, this screen speaks {}: showing the "
                         "U1's own heads only",
                         backend_log_tag(), *d.api_version, multiace::kApiVersion);
            api_mismatch_logged_ = true;
        }
    }
    if (d.mode) {
        mode_ = *d.mode;
    }
    if (d.head_ace) {
        head_ace_ = *d.head_ace;
    }
    if (d.head_feeder) {
        head_feeder_ = *d.head_feeder;
    }
    if (d.head_manual) {
        head_manual_ = *d.head_manual;
    }
    std::vector<int> load_failed_heads;
    if (d.head_source) {
        for (int h = 0; h < kHeads; ++h) {
            const auto& src = (*d.head_source)[static_cast<size_t>(h)];
            const auto& prev = head_source_[static_cast<size_t>(h)];
            if (src && src->load_failed && !(prev && prev->load_failed)) {
                load_failed_heads.push_back(h);
            }
        }
        head_source_ = *d.head_source;
    }
    if (d.aces) {
        units_ = *d.aces;
    }
    if (d.swap_in_progress) {
        swap_in_progress_ = *d.swap_in_progress;
    }
    if (d.unload_all_active) {
        unload_all_active_ = *d.unload_all_active;
    }
    if (d.swap_phase) {
        swap_phase_ = *d.swap_phase;
    }
    if (d.last_swap_result) {
        last_swap_ = *d.last_swap_result;
    }
    if (d.event_seq) {
        event_seq_ = *d.event_seq;
    }

    if (d.ace_heads && active_locked()) {
        // The engine's own list of ACE-driven heads, against ours from the
        // per-head flags. A disagreement means the two readings of its mode
        // drifted; the flags are what the engine's commands act on.
        std::vector<int> ours;
        for (int h = 0; h < kHeads; ++h) {
            if (ace_fed_locked(h)) {
                ours.push_back(h);
            }
        }
        if (ours != *d.ace_heads) {
            spdlog::debug("{} ace_heads disagrees with the per-head flags", backend_log_tag());
        }
    }

    if (d.api_version || d.mode || d.head_ace || d.head_feeder || d.head_manual || d.head_source ||
        d.aces) {
        rebuild_units_locked();
        changed = true;
    }

    // The engine's own account of a load that failed: the frame hides the
    // route (load_failed) and this says why, on the rising edge only so a
    // stream of frames carrying the standing failure reports it once.
    // Collected here under the lock; handle_status emits outside it.
    for (int h : load_failed_heads) {
        mark_load_unconfirmed_locked(h, "engine reported load_failed");
        pending_load_failed_errors_.push_back(AmsErrorHelper::position_label(tool_noun(), h) +
                                              ": " + lv_tr("Load failed"));
    }
}

void AmsBackendMultiAce::rebuild_units_locked() {
    system_info_.units.resize(1);
    int total = kHeads;
    if (active_locked()) {
        std::array<multiace::HeadFeed, kHeads> feed{};
        for (int h = 0; h < kHeads; ++h) {
            const auto i = static_cast<size_t>(h);
            feed[i] = multiace::head_feed(mode_, head_manual_[i], head_feeder_[i]);
        }
        for (size_t pos = 0; pos < units_.size(); ++pos) {
            const multiace::Unit& u = units_[pos];
            AmsUnit unit;
            unit.unit_index = static_cast<int>(pos) + 1;
            unit.name = fmt::format("ACE {}", u.idx + 1);
            unit.display_name = unit.name;
            unit.slot_count = multiace::kBaysPerUnit;
            unit.first_slot_global_index = multiace::bay_global(u.idx, 0);
            unit.connected = u.connected;
            unit.firmware_version = u.firmware;
            if (u.temp || u.humidity) {
                EnvironmentData env;
                env.temperature_c = u.temp.value_or(0.0f);
                env.humidity_pct = u.humidity.value_or(0.0f);
                env.has_humidity = u.humidity.has_value();
                unit.environment = env;
            }
            if (mode_ == multiace::Mode::HEAD) {
                unit.topology = PathTopology::HUB;
                unit.hub_tool_label =
                    multiace::head_for_bay(mode_, u.idx, 0, head_ace_, feed).value_or(-1);
            } else {
                unit.topology = PathTopology::PARALLEL;
            }

            for (int s = 0; s < multiace::kBaysPerUnit; ++s) {
                SlotInfo slot;
                slot.slot_index = s;
                slot.global_index = multiace::bay_global(u.idx, s);
                const auto bay = std::find_if(u.bays.begin(), u.bays.end(),
                                              [s](const multiace::Bay& b) { return b.index == s; });
                if (bay != u.bays.end()) {
                    slot.status = bay->status;
                    slot.material = bay->material;
                    slot.brand = bay->brand;
                    if (bay->color) {
                        slot.color_rgb = *bay->color;
                    }
                }
                const auto head = multiace::head_for_bay(mode_, u.idx, s, head_ace_, feed);
                if (head) {
                    slot.mapped_tool = *head;
                    slot.extruder_name = helix::extruder_name_for_tool(*head);
                    if (routed_bay_locked(*head) == std::make_pair(u.idx, s)) {
                        slot.status = SlotStatus::LOADED;
                    }
                }

                // The engine's bay table is also the bay's identity signal,
                // the role a head's RFID reading plays: a change runs the
                // shared insert rule and a different-spool verdict clears the
                // bay's user edit, the same hardware-event clear a head with
                // a tag has. A bay entry stating no identity (an empty bay)
                // and an absent entry are both no signal: they state nothing
                // about a DIFFERENT spool, so the baseline waits for one.
                // Must run before the ingest below so the clear sees the
                // engine's own reading, not the lane's resolved view.
                helix::ams::SpoolEvidence bay_evidence;
                if (bay != u.bays.end()) {
                    bay_evidence.tag_uid = bay->uid;
                    bay_evidence.material = bay->material;
                    bay_evidence.color_rgb = bay->color;
                    bay_evidence.tag_read_complete =
                        !bay->uid.empty() || !bay->material.empty() || bay->color.has_value();
                }
                check_hardware_event_clear(slot, slot.global_index, bay_evidence);

                // The engine's own table is the bay's machine reading, the role
                // a head's RFID tag plays: filed whole (an absent bay entry
                // states nothing, retracting what an earlier frame said), then
                // outranked by the lane's resolution, where a user's edit - and,
                // after a restart, its stored record - declares what the bay
                // holds. Temps no engine frame carries come from the stored
                // record alone.
                helix::ams::Observation engine(helix::ams::ObservationSource::VendorCache);
                if (bay != u.bays.end()) {
                    if (!bay->material.empty()) {
                        engine.material = bay->material;
                    }
                    if (!bay->brand.empty()) {
                        engine.brand = bay->brand;
                    }
                    engine.color_rgb = bay->color;
                }
                helix::ams::ingest(lane_id(slot.global_index), engine);
                apply_resolved_lane(slot, slot.global_index);
                if (const auto ovr = overrides_.find(slot.global_index); ovr != overrides_.end()) {
                    const ams::ResolvedTemps temps = ams::resolved_temps(ovr->second);
                    if (temps.nozzle_temp > 0) {
                        slot.nozzle_temp_min = temps.nozzle_temp;
                        slot.nozzle_temp_max = temps.nozzle_temp;
                    }
                    if (temps.bed_temp > 0) {
                        slot.bed_temp = temps.bed_temp;
                    }
                }
                unit.slots.push_back(std::move(slot));
            }
            total = std::max(total, unit.first_slot_global_index + unit.slot_count);
            system_info_.units.push_back(std::move(unit));
        }
    }
    system_info_.total_slots = total;
}

// ============================================================================
// Base hooks
// ============================================================================

bool AmsBackendMultiAce::head_is_upstream_fed_locked(int head) const {
    return ace_fed_locked(head);
}

bool AmsBackendMultiAce::resume_may_refeed_locked(int head) const {
    // The engine owns the feed state machine for every head while its contract
    // is in force, and loads through ACE_LOAD_HEAD; AUTO_FEEDING is the stock
    // firmware's load, which the engine is not known to honour mid-print.
    (void)head;
    return !active_locked();
}

bool AmsBackendMultiAce::preload_finish_ends_unload_locked(int head) const {
    // multiACE's FEED_AUTO unload leaves an ACE head's channel at
    // preload_finish: the filament is back in the ACE, not at the head's port.
    return ace_fed_locked(head);
}

// ============================================================================
// Slot identity
// ============================================================================

AmsError AmsBackendMultiAce::apply_user_edit(int slot_index, const SlotInfo& info,
                                             const helix::ams::Observation& declared) {
    if (!multiace::bay_of(slot_index)) {
        return AmsBackendSnapmaker::apply_user_edit(slot_index, info, declared);
    }
    if (auto err = validate_slot_index(slot_index); err.result != AmsResult::SUCCESS) {
        return err;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotInfo* slot = system_info_.get_slot_global(slot_index);
        if (!slot) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index,
                                                system_info_.total_slots - 1);
        }
        slot->assign_filament_fields(info);
        // The staged record survives the restart; the lane's declaration, filed
        // by commit_user_edit() once this returns, is what rebuild_units_locked
        // lays back over every engine frame.
        helix::ams::stage_user_override(overrides_, slot_index, info, declared);
    }

    if (override_store_) {
        helix::ams::persist_staged_override(override_store_.get(), mutex_, overrides_, slot_index,
                                            backend_log_tag(), "Override");
    }

    emit_event(kEventSlotChanged, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendMultiAce::sync_external_identity(int slot_index, const SlotInfo& info) {
    if (!multiace::bay_of(slot_index)) {
        return AmsBackendSnapmaker::sync_external_identity(slot_index, info);
    }
    if (auto err = validate_slot_index(slot_index); err.result != AmsResult::SUCCESS) {
        return err;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotInfo* slot = system_info_.get_slot_global(slot_index);
        if (!slot) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index,
                                                system_info_.total_slots - 1);
        }
        // overrides_ is left alone and nothing reaches the engine: the next
        // frame restates its own table, which outranks a synced value.
        slot->assign_filament_fields(info);
    }

    emit_event(kEventSlotChanged, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendMultiAce::clear_slot_override(int slot_index) {
    if (!multiace::bay_of(slot_index)) {
        AmsBackendSnapmaker::clear_slot_override(slot_index);
        return;
    }
    if (auto err = validate_slot_index(slot_index); !err.success()) {
        spdlog::warn("{} clear_slot_override: invalid slot {}", backend_log_tag(), slot_index);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotInfo* slot = system_info_.get_slot_global(slot_index);
        if (!slot) {
            spdlog::warn("{} clear_slot_override: no slot entry for index {}", backend_log_tag(),
                         slot_index);
            return;
        }
        spdlog::info("{} Slot {} override cleared by user request", backend_log_tag(), slot_index);
        clear_override_locked(slot_index, slot);
    }

    emit_event(kEventSlotChanged, std::to_string(slot_index));
}

// ============================================================================
// Queries
// ============================================================================

PathTopology AmsBackendMultiAce::get_unit_topology(int unit_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_index >= 0 && unit_index < static_cast<int>(system_info_.units.size())) {
        return system_info_.units[static_cast<size_t>(unit_index)].topology;
    }
    return get_topology();
}

PathSegment AmsBackendMultiAce::get_slot_filament_segment(int slot_index) const {
    int routed_head = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index < kHeads) {
            routed_head = slot_index;
        } else {
            const SlotInfo* slot = system_info_.get_slot_global(slot_index);
            const auto head = head_for_global_locked(slot_index);
            if (!slot) {
                return PathSegment::NONE;
            }
            if (head && routed_bay_locked(*head) == multiace::bay_of(slot_index)) {
                routed_head = *head;
            } else {
                // Filament an ACE holds back sits at its gate.
                return slot->is_present() ? PathSegment::PREP : PathSegment::NONE;
            }
        }
    }
    return AmsBackendSnapmaker::get_slot_filament_segment(routed_head);
}

std::optional<int> AmsBackendMultiAce::head_for_slot(int slot) const {
    if (slot >= 0 && slot < kHeads) {
        return slot;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return head_for_global_locked(slot);
}

std::optional<int> AmsBackendMultiAce::head_source_slot(int head, const AmsSystemInfo&) const {
    if (head < 0 || head >= kHeads) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ace_fed_locked(head)) {
        return head;
    }
    std::vector<int> bays;
    for (int global = kHeads; global < system_info_.total_slots; ++global) {
        if (head_for_global_locked(global) == head) {
            bays.push_back(global);
        }
    }
    if (bays.size() == 1) {
        return bays.front();
    }
    if (const auto routed = routed_bay_locked(head)) {
        const int global = multiace::bay_global(routed->first, routed->second);
        if (std::find(bays.begin(), bays.end(), global) != bays.end()) {
            return global;
        }
    }
    return std::nullopt;
}

bool AmsBackendMultiAce::needs_unload_before_load(const AmsSystemInfo& info,
                                                  int target_slot) const {
    if (multiace::bay_of(target_slot)) {
        return false;
    }
    return AmsBackendSnapmaker::needs_unload_before_load(info, target_slot);
}

AmsBackend::FilamentOpEligibility AmsBackendMultiAce::slot_op_eligibility(int slot_index,
                                                                          bool load) const {
    if (load) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index >= 0 && slot_index < kHeads && active_locked() &&
            ace_fed_locked(slot_index)) {
            return FilamentOpEligibility::NotSupported;
        }
    }
    return AmsBackendSnapmaker::slot_op_eligibility(slot_index, load);
}

std::optional<int> AmsBackendMultiAce::upstream_slot(int slot_index) const {
    if (slot_index < 0 || slot_index >= kHeads) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ace_fed_locked(slot_index)) {
        return std::nullopt;
    }
    const auto bay = routed_bay_locked(slot_index);
    if (!bay) {
        return std::nullopt;
    }
    return multiace::bay_global(bay->first, bay->second);
}

int AmsBackendMultiAce::spool_display_number(int slot_index) const {
    if (slot_index < 0) {
        return slot_index + 1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // A routed head aliases its bay; every other lane (bays, unrouted heads)
    // numbers in global order, so the numbers on spool-owning lanes run 1..N.
    // Only indices a unit actually covers count: a gap the engine left in the
    // ace numbering is not a spool, so the numbers stay consecutive.
    const auto aliased = [this](int head) {
        return head < kHeads && ace_fed_locked(head) && routed_bay_locked(head).has_value();
    };
    int target = slot_index;
    if (aliased(slot_index)) {
        const auto bay = *routed_bay_locked(slot_index);
        target = multiace::bay_global(bay.first, bay.second);
    }
    int n = 0;
    for (int gi = 0; gi < system_info_.total_slots; ++gi) {
        if (!aliased(gi) && system_info_.get_slot_global(gi)) {
            ++n;
        }
        if (gi == target) {
            break;
        }
    }
    return n;
}

bool AmsBackendMultiAce::can_unload_from_toolhead(int slot_index) const {
    int head = slot_index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index >= kHeads) {
            const auto h = head_for_global_locked(slot_index);
            if (!h || routed_bay_locked(*h) != multiace::bay_of(slot_index)) {
                return false;
            }
            head = *h;
        }
    }
    return AmsBackendSnapmaker::can_unload_from_toolhead(head);
}

bool AmsBackendMultiAce::slot_has_filament_at_toolhead(int slot_index) const {
    int head = slot_index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index >= kHeads) {
            const auto h = head_for_global_locked(slot_index);
            if (!h || routed_bay_locked(*h) != multiace::bay_of(slot_index)) {
                return false;
            }
            head = *h;
        }
    }
    return AmsBackendSnapmaker::slot_has_filament_at_toolhead(head);
}

bool AmsBackendMultiAce::slot_is_actively_loaded(int slot_index) const {
    if (slot_index < kHeads) {
        return AmsBackendSnapmaker::slot_is_actively_loaded(slot_index);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const SlotInfo* slot = system_info_.get_slot_global(slot_index);
    return slot && slot->status == SlotStatus::LOADED;
}

// ============================================================================
// Filament operations
// ============================================================================

std::optional<AmsError> AmsBackendMultiAce::head_load_refusal(int head, bool ace_head) const {
    if (AmsBackendSnapmaker::slot_has_filament_at_toolhead(head)) {
        return AmsErrorHelper::slot_ineligible(
            lane_noun(), head,
            filament_op_eligibility_reason(FilamentOpEligibility::AlreadyLoaded));
    }
    if (ace_head) {
        return std::nullopt;
    }
    // A feeder or manual head: the engine loads it through the side feeder,
    // under the same feeder preconditions the U1 checks.
    switch (const auto e = slot_op_eligibility(head, /*load=*/true)) {
    case FilamentOpEligibility::AlreadyLoaded:
    case FilamentOpEligibility::FeederUnavailable:
    case FilamentOpEligibility::SensorDisabled:
        return AmsErrorHelper::slot_ineligible(lane_noun(), head,
                                               filament_op_eligibility_reason(e));
    default:
        return std::nullopt;
    }
}

AmsError AmsBackendMultiAce::do_load_filament(int slot_index) {
    int head = -1;
    bool engine = false;
    bool ace_head = false;
    std::optional<std::pair<int, int>> bay;
    std::optional<std::pair<int, int>> recorded;
    bool head_has_source = false;
    bool bay_empty = false;
    multiace::Mode mode = multiace::Mode::UNKNOWN;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine = active_locked();
        mode = mode_;
        if (slot_index >= 0 && slot_index < kHeads) {
            head = engine ? slot_index : -1;
        } else if (const auto h = head_for_global_locked(slot_index)) {
            head = *h;
            bay = multiace::bay_of(slot_index);
            const SlotInfo* slot = system_info_.get_slot_global(slot_index);
            bay_empty = !slot || slot->status == SlotStatus::EMPTY;
        } else if (multiace::bay_of(slot_index)) {
            return AmsErrorHelper::not_supported("Loading an ACE bay no toolhead is wired to");
        }
        if (head >= 0) {
            ace_head = ace_fed_locked(head);
            const auto& src = head_source_[static_cast<size_t>(head)];
            head_has_source = src.has_value();
            if (src && src->ace >= 0 && src->slot >= 0) {
                recorded = std::make_pair(src->ace, src->slot);
            }
        }
    }

    if (head < 0) {
        // No engine contract, or no slot at all: the U1's own path, which
        // validates the index.
        return AmsBackendSnapmaker::do_load_filament(slot_index);
    }

    const bool head_loaded = AmsBackendSnapmaker::slot_has_filament_at_toolhead(head);
    const auto already_loaded = [&] {
        return AmsErrorHelper::slot_ineligible(
            lane_noun(), slot_index,
            filament_op_eligibility_reason(FilamentOpEligibility::AlreadyLoaded));
    };

    if (!bay) {
        if (auto refusal = head_load_refusal(head, ace_head)) {
            return *refusal;
        }
        if (!ace_head) {
            AmsError err =
                dispatch_feed(multiace::load_gcode(head, std::nullopt), kEventLoadComplete, head);
            if (err.success()) {
                cancel_pending_load("a new load");
            }
            return err;
        }
        if (mode == multiace::Mode::MULTI) {
            // Multi mode defaults a bare load to the globally active unit, not
            // to anything this head was fed from, so the bay is always named.
            if (!recorded) {
                return AmsError(AmsResult::WRONG_STATE,
                                "multi mode: head " + std::to_string(head) +
                                    " has no recorded bay to load from",
                                lv_tr("Select a filament slot to load"), "", slot_index);
            }
            AmsError err =
                dispatch_feed(multiace::load_gcode(head, recorded), kEventLoadComplete, head);
            if (err.success()) {
                cancel_pending_load("a new load");
            }
            return err;
        }
        // Head mode defaults to the wired unit and the head's last bay.
        AmsError err =
            dispatch_feed(multiace::load_gcode(head, std::nullopt), kEventLoadComplete, head);
        if (err.success()) {
            cancel_pending_load("a new load");
        }
        return err;
    }

    // The engine swaps bays mid-print itself, as the sliced file asks; a bay
    // change from the screen while a print holds the heads is refused.
    if (auto err = refuse_if_printing(/*paused_blocks=*/true); !err.success()) {
        return err;
    }
    if (bay_empty) {
        return AmsErrorHelper::slot_not_available(lane_noun(), slot_index);
    }
    if (head_loaded && recorded == bay) {
        return already_loaded();
    }
    if (head_loaded && !head_has_source) {
        // Filament at the head that the engine has no record of: no bay of
        // its own to return it to, so a swap would feed a second strand.
        return AmsError(AmsResult::WRONG_STATE,
                        "head " + std::to_string(head) +
                            " is loaded with no recorded source; refusing a bay swap",
                        AmsErrorHelper::position_label(tool_noun(), head) + ": " +
                            lv_tr("Unload from toolhead first"),
                        "", slot_index);
    }
    if (!head_loaded && !head_has_source) {
        AmsError err = dispatch_feed(multiace::load_gcode(head, bay), kEventLoadComplete, head);
        if (err.success()) {
            cancel_pending_load("a new load");
        }
        return err;
    }

    // ACE_LOAD_HEAD answers OK and feeds nothing into a head that still holds
    // filament, so the unload goes alone and the load waits for a frame that
    // shows the head empty (advance_pending_load()).
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_pending_id_++;
        PendingBayLoad pending;
        pending.head = head;
        pending.bay = *bay;
        pending.id = id;
        pending_load_ = pending;
    }
    const auto answered = [this, id] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_load_ && pending_load_->id == id) {
            pending_load_->unload_answered = true;
            pending_load_->unload_answered_at = std::chrono::steady_clock::now();
        }
    };
    auto err = dispatch_feed(multiace::unload_gcode(head), answered,
                             [this, id, answered](const MoonrakerError& e) {
                                 if (e.type == MoonrakerErrorType::TIMEOUT) {
                                     // The unload may still be running; the frames decide.
                                     answered();
                                     return;
                                 }
                                 {
                                     std::lock_guard<std::mutex> lock(mutex_);
                                     if (pending_load_ && pending_load_->id == id) {
                                         pending_load_.reset();
                                     }
                                 }
                                 emit_event(kEventError, e.message);
                             });
    if (!err.success()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_load_ && pending_load_->id == id) {
            pending_load_.reset();
        }
    }
    return err;
}

void AmsBackendMultiAce::handle_status(const nlohmann::json& status) {
    AmsBackendSnapmaker::handle_status(status);
    std::vector<std::string> load_failed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        load_failed.swap(pending_load_failed_errors_);
    }
    for (const std::string& message : load_failed) {
        emit_event(kEventError, message);
    }
    advance_pending_load();
}

void AmsBackendMultiAce::on_klipper_restarted() {
    cancel_pending_load("Klipper restarted");
    AmsBackendSnapmaker::on_klipper_restarted();
}

void AmsBackendMultiAce::cancel_pending_load(const char* why) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_load_) {
        spdlog::info("{} Dropping the load of ACE {} bay {} into head {}: {}", backend_log_tag(),
                     pending_load_->bay.first + 1, pending_load_->bay.second + 1,
                     pending_load_->head + 1, why);
        pending_load_.reset();
    }
}

void AmsBackendMultiAce::advance_pending_load() {
    PendingBayLoad pending;
    std::chrono::milliseconds grace{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_load_) {
            return;
        }
        pending = *pending_load_;
        grace = pending_load_grace_;
    }
    // Takes mutex_ itself, so read before the decision below re-takes it.
    const bool head_loaded = AmsBackendSnapmaker::slot_has_filament_at_toolhead(pending.head);
    std::string dropped;
    bool send = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_load_ || pending_load_->id != pending.id) {
            return;
        }
        const bool empty =
            !head_loaded && !head_source_[static_cast<size_t>(pending.head)].has_value();
        // Expiry is judged before emptiness: a head that empties only after
        // its window closed was emptied by something other than this unload,
        // and the user has already been told the load is off.
        const bool expired = pending.unload_answered &&
                             std::chrono::steady_clock::now() > pending.unload_answered_at + grace;
        if (!active_locked()) {
            dropped = "multiACE stopped driving the ACE units";
        } else if (expired) {
            dropped =
                fmt::format("toolhead {} did not empty in time after its unload", pending.head + 1);
        } else if (empty) {
            send = true;
        } else {
            return;
        }
        pending_load_.reset();
    }
    if (send) {
        // A print that took the heads while the unload ran owns them now.
        if (auto err = refuse_if_printing(/*paused_blocks=*/true); !err.success()) {
            dropped = "a print holds the toolheads";
        }
    }
    if (!dropped.empty()) {
        spdlog::warn("{} Not loading ACE {} bay {} into head {}: {}", backend_log_tag(),
                     pending.bay.first + 1, pending.bay.second + 1, pending.head + 1, dropped);
        emit_event(kEventError,
                   fmt::format("ACE {} bay {} was not loaded: {}", pending.bay.first + 1,
                               pending.bay.second + 1, dropped));
        return;
    }
    spdlog::info("{} Head {} is empty; loading ACE {} bay {}", backend_log_tag(), pending.head + 1,
                 pending.bay.first + 1, pending.bay.second + 1);
    dispatch_feed(multiace::load_gcode(pending.head, pending.bay), kEventLoadComplete,
                  pending.head);
}

AmsError AmsBackendMultiAce::do_unload_filament(int slot_index) {
    int head = slot_index;
    bool engine = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine = active_locked();
        if (head < 0) {
            head = system_info_.current_slot;
        }
        if (head >= kHeads) {
            const auto h = head_for_global_locked(head);
            if (!h || routed_bay_locked(*h) != multiace::bay_of(head)) {
                return AmsErrorHelper::not_loaded();
            }
            head = *h;
        }
    }
    if (!engine) {
        return AmsBackendSnapmaker::do_unload_filament(head);
    }
    if (head < 0) {
        // The engine owns the feed state machine while it is active: no head
        // resolved means nothing to unload, never the stock leaf macro.
        return AmsErrorHelper::not_loaded();
    }
    // Every head, ACE-fed or not: the engine owns the feed state machine.
    AmsError err = dispatch_feed(multiace::unload_gcode(head), kEventUnloadComplete);
    if (err.success()) {
        cancel_pending_load("an unload");
    }
    return err;
}

AmsError AmsBackendMultiAce::do_select_slot(int slot_index) {
    if (slot_index < kHeads) {
        return AmsBackendSnapmaker::do_select_slot(slot_index);
    }
    int head = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto h = head_for_global_locked(slot_index);
        if (h && routed_bay_locked(*h) == multiace::bay_of(slot_index)) {
            head = *h;
        }
    }
    if (head < 0) {
        // Selecting a bay is a tool change to its head, which prints that bay
        // only once it is the head's routed bay.
        return AmsErrorHelper::slot_ineligible(
            lane_noun(), slot_index,
            filament_op_eligibility_reason(FilamentOpEligibility::NotLoaded));
    }
    return do_change_tool(head);
}

AmsError AmsBackendMultiAce::do_change_tool(int tool_number) {
    if (tool_number >= kHeads) {
        return AmsErrorHelper::tool_out_of_range(tool_number);
    }
    AmsError err = AmsBackendSnapmaker::do_change_tool(tool_number);
    if (err.success()) {
        cancel_pending_load("a tool change");
    }
    return err;
}

AmsError AmsBackendMultiAce::do_park_active_tool() {
    AmsError err = AmsBackendSnapmaker::do_park_active_tool();
    if (err.success()) {
        cancel_pending_load("a tool park");
    }
    return err;
}

AmsError AmsBackendMultiAce::do_filament_batch(const std::vector<int>& slots, bool load) {
    std::vector<int> heads;
    bool engine = false;
    bool any_ace = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine = active_locked();
        if (engine) {
            for (int s : slots) {
                int head = s;
                if (s >= kHeads) {
                    const auto h = head_for_global_locked(s);
                    if (load || !h || routed_bay_locked(*h) != multiace::bay_of(s)) {
                        return AmsErrorHelper::not_supported(
                            load ? "Batch load from ACE bays"
                                 : "Batch unload of unrouted ACE bays");
                    }
                    head = *h;
                }
                if (head < 0 || head >= kHeads) {
                    return validate_slot_index_locked(head);
                }
                if (std::find(heads.begin(), heads.end(), head) == heads.end()) {
                    heads.push_back(head);
                    any_ace = any_ace || ace_fed_locked(head);
                }
            }
        }
    }
    if (!engine) {
        return AmsBackendSnapmaker::do_filament_batch(slots, load);
    }
    if (heads.empty()) {
        return AmsErrorHelper::invalid_parameter("empty batch");
    }
    if (load && any_ace) {
        return AmsErrorHelper::not_supported("Batch load into ACE-fed toolheads");
    }
    if (load) {
        // Every head is checked before anything is sent: Klipper aborts the
        // rest of a script at the first line that raises.
        for (int h : heads) {
            if (auto refusal = head_load_refusal(h, /*ace_head=*/false)) {
                return *refusal;
            }
        }
    }

    if (!load) {
        std::vector<int> loaded_heads;
        for (int h = 0; h < kHeads; ++h) {
            if (AmsBackendSnapmaker::slot_has_filament_at_toolhead(h)) {
                loaded_heads.push_back(h);
            }
        }
        std::vector<int> sorted = heads;
        std::sort(sorted.begin(), sorted.end());
        if (!loaded_heads.empty() && sorted == loaded_heads) {
            // The engine's own unload-all: its loop walks all four heads and
            // unloads every one whose motion sensor sees filament, ACE-fed,
            // feeder and manual alike (its _unload_all_heads).
            AmsError err = dispatch_feed(multiace::unload_all_gcode(), kEventUnloadComplete);
            if (err.success()) {
                cancel_pending_load("a batch operation");
            }
            return err;
        }
    }

    // One engine command per head; Klipper stops the script at the first one
    // that raises.
    std::string script;
    for (int h : heads) {
        if (!script.empty()) {
            script += "\n";
        }
        script += load ? multiace::load_gcode(h, std::nullopt) : multiace::unload_gcode(h);
    }
    AmsError err =
        dispatch_feed(std::move(script), load ? kEventLoadComplete : kEventUnloadComplete);
    if (err.success()) {
        cancel_pending_load("a batch operation");
    }
    return err;
}

// ============================================================================
// Dryer
// ============================================================================

DryerInfo AmsBackendMultiAce::get_dryer_info(int unit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const multiace::Unit* u = unit_for_index_locked(unit)) {
        return u->dryer;
    }
    return DryerInfo{.supported = false};
}

AmsError AmsBackendMultiAce::start_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    (void)fan_pct;
    auto err = check_preconditions();
    if (!err.success()) {
        return err;
    }
    int ace = -1;
    DryerInfo dryer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const multiace::Unit* u = unit_for_index_locked(unit)) {
            ace = u->idx;
            dryer = u->dryer;
        }
    }
    if (ace < 0) {
        return AmsErrorHelper::not_supported("Drying on a unit with no ACE");
    }
    if (auto refusal = helix::ace::check_dryer_request(dryer, temp_c, duration_min)) {
        return *refusal;
    }
    spdlog::info("{} Drying ACE {}: {}°C for {} minutes", backend_log_tag(), ace + 1, temp_c,
                 duration_min);
    return execute_gcode(multiace::dry_gcode(ace, static_cast<int>(temp_c), duration_min));
}

AmsError AmsBackendMultiAce::stop_drying(int unit) {
    int ace = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const multiace::Unit* u = unit_for_index_locked(unit)) {
            ace = u->idx;
        }
    }
    if (ace < 0) {
        return AmsErrorHelper::not_supported("Drying on a unit with no ACE");
    }
    spdlog::info("{} Stopping drying on ACE {}", backend_log_tag(), ace + 1);
    return execute_gcode(multiace::stop_dry_gcode(ace));
}

AmsError AmsBackendMultiAce::update_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    auto err = stop_drying(unit);
    if (!err.success()) {
        return err;
    }
    const DryerInfo dryer = get_dryer_info(unit);
    const auto [target_temp, target_duration] =
        helix::ace::restart_request(dryer, temp_c, duration_min);
    return start_drying(target_temp, target_duration, fan_pct, unit);
}

std::vector<DryingPreset> AmsBackendMultiAce::get_drying_presets() const {
    return get_default_drying_presets();
}

// ============================================================================
// Errors
// ============================================================================

std::optional<helix::ErrorEvent>
AmsBackendMultiAce::classify_error(const std::string& raw_line,
                                   const helix::ClassifyContext& ctx) const {
    if (!helix::is_bang_line(raw_line)) {
        return AmsBackendSnapmaker::classify_error(raw_line, ctx);
    }
    const std::string detail = helix::strip_bang_prefix(raw_line);
    const auto code = snapmaker::decode_exception_code(detail);
    if (!code || code->id != kMultiAceFaultId || code->code < kMultiAceCodeFirst ||
        code->code > kMultiAceCodeLast) {
        return AmsBackendSnapmaker::classify_error(raw_line, ctx);
    }

    const std::string token = snapmaker::format_exception_code(*code);
    std::string message = detail;
    if (const auto at = message.find(token); at != std::string::npos) {
        message.erase(at, token.size());
    }
    const auto first = message.find_first_not_of(" :");
    message = first == std::string::npos ? std::string{} : message.substr(first);
    while (!message.empty() && message.back() == ' ') {
        message.pop_back();
    }

    std::vector<helix::RecoveryAction> actions;
    actions.push_back({lv_tr("OK"), "", "ams_backend_multiace::fault_dismiss", "", false});
    const std::optional<int> head = fault_head(code->index, message);
    helix::ErrorEvent e = helix::make_ams_fault_event(
        helix::ErrorSource::SNAPMAKER, lv_tr("Filament operation failed"),
        head ? fmt::format("{}: {}", AmsErrorHelper::position_label(tool_noun(), *head), message)
             : message,
        std::move(actions));
    e.code = token;
    e.raw_detail = detail;
    // Idle, the fault is the engine refusing a command the user just sent.
    e.command_refusal = !ctx.is_printing && !ctx.is_paused;
    return e;
}

} // namespace helix

#endif // HELIX_HAS_SNAPMAKER
