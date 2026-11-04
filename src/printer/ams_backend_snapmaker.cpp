// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_SNAPMAKER

#include "ams_backend_snapmaker.h"

#include "ui_insert_notice.h"
#include "ui_notification.h"
#include "ui_snapmaker_nozzle_offset_overlay.h"
#include "ui_toast_manager.h"

#include "ams_device_action_value.h"
#include "ams_device_sections.h"
#include "ams_error.h"
#include "ams_fault_event.h"
#include "ams_state.h"
#include "app_globals.h"
#include "batch_feed_reconcile.h"
#include "display_numbering.h"
#include "filament_database.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "json_utils.h"
#include "klipper_extruder_naming.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_patterns.h"
#include "moonraker_api.h"
#include "multiace_status.h"
#include "pause_cause.h"
#include "post_op_cooldown_manager.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "snapmaker_channel_state.h"
#include "snapmaker_nozzle_offset.h"
#include "snapmaker_resume.h"
#include "snapmaker_screws_tilt.h"
#include "snapmaker_status_parse.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <lvgl.h>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace helix {

namespace {

/// The section the firmware's print preferences sit in.
constexpr const char* kPrintPrefsSection = "print_prefs";

/// Parses a pending insert verdict may hold while waiting for the tag
/// reader's filament_detect.info entry. Status frames arrive about once a
/// second, so eight parses is roughly eight seconds: long enough for a read to
/// land, short enough that an unverified insert cannot sit silent forever.
constexpr int kSnapPendingInsertPasses = 8;

/// A digit run short enough to parse as an index without overflowing. Ten or
/// more digits exceeds the narrowest supported target's parse range, and both
/// stoi and stoul report that by throwing, so the length is checked before the
/// parse instead of the throw being caught after it. No real extruder or tool
/// index comes anywhere near that many digits.
[[nodiscard]] bool is_parsable_index(std::string_view digits) {
    return !digits.empty() && digits.size() < 10 &&
           std::all_of(digits.begin(), digits.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Snapmaker's recognized filament SUB_TYPE product lines. The RFID read path
// stores SUB_TYPE into SlotInfo::spool_name (see apply_rfid_entry_locked), but
// a user can edit spool_name to a free-form string ("My Custom Spool"), so a
// spool_name is only a product line when it matches one of these.
constexpr std::array<std::string_view, 8> kKnownSubTypes = {
    "Basic", "Matte", "SnapSpeed", "Silk", "Support", "HF", "95A", "95A HF"};

// The vendor whose spools the product lines above name.
constexpr std::string_view kProductLineVendor = "Snapmaker";

[[nodiscard]] bool is_known_subtype(const std::string& s) {
    for (const auto& st : kKnownSubTypes) {
        if (s == st) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_product_line_vendor(const std::string& brand) {
    return helix::text_io::equals_ignore_case(brand, kProductLineVendor);
}

// The filament families the firmware's filament table marks soft (TPU, PEBA,
// PVA), plus TPE, which is softer than any TPU and has no row there, and the
// generic "Flex" names. The firmware's end-of-print macro skips a soft head,
// and the stock screen refuses to enable unload-at-end on one.
constexpr std::array<std::string_view, 6> kSoftFamilies = {"TPU", "TPE",  "PEBA",
                                                           "PVA", "FLEX", "FLEXIBLE"};

/// The material database's group for flexible filament.
constexpr std::string_view kSoftCompatGroup = "TPU";

/// Whether @p material is soft filament. The firmware keys its table by the
/// exact type, so a grade it has no row for ("TPU-85A", "TPU95A") reads rigid
/// there. This asks the material database first, then judges the family: a
/// soft family name followed by the end or a non-letter ("PVA+", "TPU95A"),
/// so a longer word that merely starts with one ("TPUX") stays rigid.
[[nodiscard]] bool is_soft_material(std::string_view material) {
    if (const auto info = filament::find_material(material);
        info && info->compat_group && kSoftCompatGroup == info->compat_group) {
        return true;
    }
    return std::any_of(kSoftFamilies.begin(), kSoftFamilies.end(), [&](std::string_view f) {
        if (material.size() < f.size() ||
            !helix::text_io::equals_ignore_case(material.substr(0, f.size()), f)) {
            return false;
        }
        return material.size() == f.size() ||
               !std::isalpha(static_cast<unsigned char>(material[f.size()]));
    });
}

/// True when @p spool_name is a product line that @p brand cannot carry: the
/// label of a Snapmaker spool left on a slot now declared as another vendor's.
/// An empty brand names no vendor, so it contradicts nothing.
[[nodiscard]] bool names_foreign_product_line(const std::string& spool_name,
                                              const std::string& brand) {
    return is_known_subtype(spool_name) && !brand.empty() && !is_product_line_vendor(brand);
}

/// The firmware's spelling of "nothing set" for a print_task_config string
/// field (filament_type, filament_vendor).
[[nodiscard]] bool is_unset_task_field(const std::string& value) {
    return value.empty() || value == "NONE";
}

/// @p value as SlotInfo carries it: an unset task field is an empty string,
/// never the literal.
[[nodiscard]] std::string task_field_value(const std::optional<std::string>& value) {
    return value.has_value() && !is_unset_task_field(*value) ? *value : std::string{};
}

/// The spool name a tag record states: its product line, or empty when the
/// record's brand cannot carry one. Firmware stores a placeholder SUB_TYPE
/// beside any other vendor's spool, so "Generic" + "Basic" names no line.
[[nodiscard]] std::string tag_spool_name(const helix::ams::Observation& record) {
    std::string line = record.product_name.value_or(std::string{});
    if (names_foreign_product_line(line, record.brand.value_or(std::string{}))) {
        line.clear();
    }
    return line;
}

/// The spool name the lane's tag states, or empty. The vendor-cache record
/// holds the last tag reading with our own write-back already withheld, so this
/// is the machine's word on the spool and never an echo of an edit.
[[nodiscard]] std::string tag_product_line(helix::ams::LaneId lane) {
    const std::optional<helix::ams::Observation> cache =
        helix::ams::lane_sources(lane).vendor_cache;
    return cache.has_value() ? tag_spool_name(*cache) : std::string{};
}

// Map a raw firmware channel_error token to a user-facing message. The firmware
// emits machine tokens (e.g. "no_filament") that are meaningless to a user.
// Unknown tokens fall back to the raw string so we never hide a novel error
// behind a generic message. Single source of truth shared by the error-set path
// here and any modal that surfaces operation_detail.
[[nodiscard]] std::string friendly_channel_error(const std::string& token, helix::ui::LaneNoun noun,
                                                 int lane_index) {
    if (token == "no_filament") {
        // The lane is a bare prefix, not part of the sentence: the frame must
        // not agree with a noun that varies per locale and backend.
        return helix::ui::lane_label(noun, lane_index) + ": " +
               lv_tr("No filament. Load filament and retry.");
    }
    return token;
}

// User-facing message for a *_fail channel_state (Change 2). The firmware
// signals the failure in channel_state itself (e.g. "load_fail") independently
// of the channel_error token, so surface a direction-aware message when a fail
// state lands. lane_index is 0-based.
[[nodiscard]] std::string friendly_channel_state_fail(const std::string& state,
                                                      helix::ui::LaneNoun noun, int lane_index) {
    const std::string lane = helix::ui::lane_label(noun, lane_index) + ": ";
    if (state.rfind("unload_", 0) == 0) {
        return lane + lv_tr("Unload failed");
    }
    if (state.rfind("preload_", 0) == 0) {
        return lane + lv_tr("Preload failed");
    }
    // load_fail, manual_sta_*_fail, and any other feed failure.
    return lane + lv_tr("Load failed");
}

using snapmaker::ChannelStateInfo;
using snapmaker::classify_channel_state;

} // namespace

// ============================================================================
// Construction
// ============================================================================

nlohmann::json AmsBackendSnapmaker::required_status_objects(const helix::PrinterDiscovery& hw) {
    nlohmann::json objects = nlohmann::json::object();
    if (hw.mmu_type() != AmsType::SNAPMAKER) {
        return objects;
    }
    objects["filament_detect"] = nullptr;
    objects["filament_feed left"] = nullptr;
    objects["filament_feed right"] = nullptr;
    objects["print_task_config"] = nullptr;
    objects["machine_state_manager"] = nullptr;
    for (int i = 0; i < 4; ++i) {
        objects[fmt::format("filament_motion_sensor e{}_filament", i)] = nullptr;
    }
    // The batch macro's `doing` variable drives batch-feed progress, so it needs a
    // subscription to be readable while a feed runs. Klipper keeps the config's case
    // for the status object key, so subscribe under the name as written in
    // printer.cfg; a guessed name makes Moonraker reject the whole subscription.
    const std::string batch_macro = hw.macro_config_name(helix::macro_patterns::AUTO_FEEDING_BATCH);
    if (!batch_macro.empty()) {
        objects[fmt::format("gcode_macro {}", batch_macro)] = nullptr;
    }
    // ACE units hung off the heads (AmsBackendMultiAce reads it).
    if (multiace::present(hw)) {
        objects["ace"] = nullptr;
    }
    return objects;
}

AmsBackendSnapmaker::AmsBackendSnapmaker(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info
    system_info_.type = AmsType::SNAPMAKER;
    system_info_.type_name = "Snapmaker SnapSwap";
    system_info_.supports_bypass = false;
    system_info_.has_hardware_bypass_sensor = false;
    // The U1 has no filament cutter and forms no discrete tip — unload is just
    // heat + retract. Leaving tip_method at its CUT default mislabels the unload
    // stepper's middle phase "Cut & retract"; NONE drives the 2-step
    // "Heat nozzle -> Retract" stepper (see recreate_step_progress_for_operation).
    system_info_.tip_method = TipMethod::NONE;

    // Initialize 1 unit with 4 slots
    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "SnapSwap";
    unit.display_name = "SnapSwap";
    unit.slot_count = kNumTools;
    unit.first_slot_global_index = 0;
    unit.connected = true;
    unit.topology = PathTopology::PARALLEL;

    for (int i = 0; i < kNumTools; i++) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::UNKNOWN;
        slot.mapped_tool = i;
        // Klipper uses "extruder" for T0, "extruder1" for T1, etc.
        slot.extruder_name = (i == 0) ? "extruder" : fmt::format("extruder{}", i);
        unit.slots.push_back(slot);
    }

    system_info_.units.push_back(std::move(unit));
    system_info_.total_slots = kNumTools;

    // Snapmaker U1 has a fixed 1:1 tool↔slot mapping (4 extruders, 4 slots):
    // every head permanently holds its own spool, so the PHYSICAL routing really
    // is identity. Consumers that need that read it here — the Load/Unload
    // buttons, and the persisted tool-map ledger in ams_tool_map_sync.h.
    //
    // It is NOT the print routing, and nothing may read it as such. Which head
    // prints logical tool N lives in print_task_config.extruder_map_table, which
    // get_tool_mapping() below publishes. This map was originally populated to
    // stop helix::ui::gcode_viewer_apply_ams_tool_colors() short-circuiting on an empty
    // map (which left the model black when the first tool's filament was dark) —
    // i.e. a safety valve was fed data to keep it from tripping, and the "fix"
    // made every tool render in head-index order. That consumer now resolves
    // through the routing table instead, so this map has no display job.
    system_info_.tool_to_slot_map.reserve(kNumTools);
    for (int i = 0; i < kNumTools; i++) {
        system_info_.tool_to_slot_map.push_back(i);
    }

    spdlog::debug("[AMS Snapmaker] Backend created with {} tools", kNumTools);
}

// ============================================================================
// Lifecycle
// ============================================================================

void AmsBackendSnapmaker::set_discovery(const helix::PrinterDiscovery& discovery) {
    std::lock_guard<std::mutex> lock(mutex_);
    // One lookup answers both the script-shape question and the object
    // key: the macro's config-case name is non-empty exactly when the
    // firmware ships it.
    const std::string macro =
        discovery.macro_config_name(helix::macro_patterns::AUTO_FEEDING_BATCH);
    use_batch_macro_ = !macro.empty();
    batch_macro_object_ = macro.empty() ? std::string{} : "gcode_macro " + macro;
    nozzle_offset_supported_ = snapmaker::nozzle_offset::supported(discovery);
}

void AmsBackendSnapmaker::before_disconnect() {
    bool nozzle_offset = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nozzle_offset = nozzle_offset_supported_;
    }
    if (nozzle_offset) {
        snapmaker::nozzle_offset::leave_live_run(client_);
    }
}

void AmsBackendSnapmaker::on_started() {
    // A run whose driver died leaves the firmware in its offset calibration
    // state with heaters on. One this process owned is ended; one it did not
    // may be another screen's, so the user decides.
    bool nozzle_offset = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nozzle_offset = nozzle_offset_supported_;
    }
    if (nozzle_offset && client_) {
        snapmaker::nozzle_offset::reconcile_on_connect(*client_, [] {
            helix::ui::queue_update("AmsBackendSnapmaker::held_nozzle_offset_cal", [] {
                helix::ui::get_snapmaker_nozzle_offset_overlay().prompt_end_held_calibration();
            });
        });
    }

    // Load persisted per-slot overrides (brand, spool name, spoolman IDs, etc.)
    // from the Moonraker DB lane_data namespace BEFORE any status parse runs.
    // AmsSubscriptionBackend::start() registers the WebSocket subscription
    // before on_started(); a status notification could in principle fire on
    // the libhv thread while we're still inside load_blocking(). Holding
    // mutex_ only during the swap keeps the parse path's read of overrides_
    // coherent without blocking it during the 5s DB round-trip.
    {
        // A start forgets what each channel's firmware said: a frame arriving
        // while the load below runs must not be judged against a silence from
        // the previous session.
        std::lock_guard<std::mutex> lock(mutex_);
        tag_names_identity_.fill(std::nullopt);
        task_vendor_.fill(std::nullopt);
        task_official_.fill(std::nullopt);
        task_type_.fill(std::nullopt);
        task_color_.fill(std::nullopt);
        task_subtype_.fill(std::nullopt);
        tag_slot_values_.fill(std::nullopt);
        tag_readings_.fill(std::nullopt);
        tag_has_card_.fill(std::nullopt);
        card_seen_.fill(false);
        // The first print state the connection reads decides whether the
        // per-print marks survive (apply_print_state_locked).
        print_state_.reset();
        main_state_.reset();
        // The overrides reload below; a held save belongs to the session that
        // made it.
        pending_record_writes_.fill(std::nullopt);
        record_restore_sent_.fill(false);
        insert_since_declaration_.fill(false);
    }
    if (!api_)
        return;

    auto loaded =
        helix::ams::make_loaded_override_store(api_, "snapmaker", get_type(), backend_log_tag());
    if (loaded.store) {
        // A product line the record only remembers, stored beside another
        // vendor's brand, is a label the slot kept from a Snapmaker spool that
        // has since left it. Dropped before the records are classified, so the
        // lane never shows it, and saved back so lane_data stops stating it.
        // Whether the record declares the name is the record translation's
        // answer, legacy records included; a declared name is the user's.
        std::vector<int> repaired;
        loaded.store->amend_loaded_records([&repaired](int slot_index,
                                                       helix::ams::FilamentSlotOverride& record,
                                                       const nlohmann::json& wire) {
            if (!names_foreign_product_line(record.spool_name, record.brand)) {
                return;
            }
            const helix::ams::LaneSources filed =
                helix::ams::sources_from_record(record, wire, helix::ams::LegacyLockKeys::LaneData);
            if (!filed.remembered.has_value() ||
                filed.remembered->spool_name != record.spool_name) {
                return;
            }
            record.spool_name.clear();
            repaired.push_back(slot_index);
        });
        for (const int slot_index : repaired) {
            auto it = loaded.overrides.find(slot_index);
            if (it == loaded.overrides.end() ||
                !names_foreign_product_line(it->second.spool_name, it->second.brand)) {
                continue;
            }
            spdlog::info("{} Slot {} dropped product line '{}' stored beside brand '{}'",
                         backend_log_tag(), slot_index, it->second.spool_name, it->second.brand);
            it->second.spool_name.clear();
            helix::ams::save_override_async(loaded.store.get(), slot_index, it->second,
                                            backend_log_tag(), "Product-line repair");
        }
        helix::ams::ingest_legacy_records(*loaded.store, helix::ams::LegacyLockKeys::LaneData,
                                          backend_index());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        override_store_ = std::move(loaded.store);
        overrides_ = std::move(loaded.overrides);
        helix::ams::bind_fingerprint_persistence(rfid_tracker_, override_store_.get(), overrides_);
        // A reconnect re-baselines the feed ports: an insert pending from
        // before it is judged against nothing this session has seen.
        pending_insert_passes_.fill(0);
        insert_verdict_.fill(InsertIdentity::NotRead);
        feed_presence_seen_.fill(false);
    }
}

// ============================================================================
// State Queries
// ============================================================================

namespace {
/// What each U1 step projects to, by the phase index classify_channel_state()
/// emits for its direction. Home and Select only position the head, so they
/// read as the direction. The step model and the published action both read
/// this, so the two cannot disagree.
std::optional<AmsAction> u1_step_action(bool unload, int phase) {
    static constexpr std::array kLoad{AmsAction::LOADING, AmsAction::LOADING, AmsAction::LOADING,
                                      AmsAction::HEATING, AmsAction::LOADING, AmsAction::PURGING};
    static constexpr std::array kUnload{AmsAction::UNLOADING, AmsAction::UNLOADING,
                                        AmsAction::HEATING, AmsAction::UNLOADING};
    const int count = static_cast<int>(unload ? kUnload.size() : kLoad.size());
    if (phase < 0 || phase >= count) {
        return std::nullopt;
    }
    return unload ? kUnload[static_cast<size_t>(phase)] : kLoad[static_cast<size_t>(phase)];
}
} // namespace

AmsSystemInfo AmsBackendSnapmaker::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AmsSystemInfo info = system_info_;
    info.action = published_action_locked();
    return info;
}

std::optional<AmsAction> AmsBackendSnapmaker::step_action_locked() const {
    // system_info_.action stays the operation's direction, because the frame
    // parser closes an operation by matching LOADING / UNLOADING; the step
    // under way is what gets published.
    return u1_step_action(system_info_.action == AmsAction::UNLOADING,
                          system_info_.operation_phase);
}

SlotInfo* AmsBackendSnapmaker::cached_slot_locked(int slot_index) {
    return system_info_.get_slot_global(slot_index);
}

AmsBackend::OperationStepModel
AmsBackendSnapmaker::get_operation_step_model(StepOperationType op) const {
    // Per-direction firmware step sequence. Each step's phase_id is the index the
    // classifier (classify_channel_state) emits into system_info_.operation_phase,
    // which the sidebar consumes directly as the current step index via the
    // ams_operation_phase subject. Load and unload use different-length step lists;
    // that is safe because only one backend + one operation is live at a time, so
    // Snapmaker owns the whole index space (classify_channel_state maps load/manual/
    // preload states into the LOAD indices and unload states into the UNLOAD ones).
    //
    //   LOAD  (6 steps): Home 0 -> Select 1 -> Feed 2 -> Heat 3 (live) -> Extrude 4 -> Purge 5
    //     load_prepare/homing -> Home; load_picking -> Select; load_feeding -> Feed
    //     (the module pushes filament to the cold toolhead); load_heating -> Heat;
    //     load_extruding -> Extrude (into the hot nozzle); load_flushing -> Purge.
    //     This is filament_feed.py's own order. A load whose head is already
    //     picked skips Select. Preload (prepare, feeding) and the manual_sta_*
    //     family (no feeding) reuse this model and skip the steps they lack.
    //   UNLOAD (4 steps): Home 0 -> Select 1 -> Heat 2 (live) -> Retract 3
    //     unload_prepare/homing -> Home; unload_picking -> Select;
    //     unload_heating/heat_finish -> Heat; unload_doing -> Retract.
    //
    // The Heat step shows a live nozzle temperature. All labels are
    // wrapped in lv_tr() so they are translated and picked up by the string tooling.
    const bool unload = (op == StepOperationType::UNLOAD);
    OperationStepModel model;
    model.steps.push_back({lv_tr("Home"), 0});
    model.steps.push_back({lv_tr("Select"), 1});
    if (unload) {
        model.steps.push_back({lv_tr("Heat nozzle"), 2, false, /*live_temp=*/true});
        model.steps.push_back({lv_tr("Retract"), 3});
    } else {
        model.steps.push_back({lv_tr("Feed filament"), 2});
        model.steps.push_back({lv_tr("Heat nozzle"), 3, false, /*live_temp=*/true});
        model.steps.push_back({lv_tr("Extrude"), 4});
        model.steps.push_back({lv_tr("Purge"), 5});
    }
    for (int i = 0; i < static_cast<int>(model.steps.size()); ++i) {
        model.steps[static_cast<size_t>(i)].coarse = u1_step_action(unload, i);
    }
    return model;
}

lv_subject_t* AmsBackendSnapmaker::get_operation_step_index_subject(StepOperationType /*op*/) {
    // The U1 firmware drives the current step directly via the operation-phase
    // subject (the per-direction step index from classify_channel_state), not via
    // narration.
    return AmsState::instance().get_ams_operation_phase_subject();
}

// ============================================================================
// Path Visualization
// ============================================================================

PathSegment AmsBackendSnapmaker::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // The system path is the mounted tool's own lane: nothing without a tool.
    return slot_segment_locked(system_info_.current_tool);
}

PathSegment AmsBackendSnapmaker::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot_segment_locked(slot_index);
}

PathSegment AmsBackendSnapmaker::slot_segment_locked(int i) const {
    if (i < 0 || i >= kNumTools || !system_info_.get_slot_global(i))
        return PathSegment::NONE;

    // Filament in THIS tool's toolhead draws to its own nozzle (PARALLEL: each
    // tool feeds a dedicated nozzle): loaded per the channel_state latch, or
    // parked short of the nozzle per the toolhead switch. The same answer as
    // filament_loaded for the picked tool, so the aggregate and per-slot paths
    // agree. The switch alone cannot say "loaded" (it still reads filament
    // after an unload), which is why parked stays distinct from the latch.
    if (loaded_at_toolhead_[i] || parked_in_toolhead_locked(i))
        return PathSegment::NOZZLE;

    // The feeder's port sensor is the lane's entry sensor: filament it alone
    // sees is at the top of the tube, not threaded toward the toolhead.
    if (port_sensor_filament_present_[i])
        return PathSegment::PREP;

    // A toolhead switch that has not reported keeps its "no runout" default,
    // which draws the tube to the toolhead sensor but not past it.
    if (sensor_filament_present_[i])
        return PathSegment::OUTPUT;
    return PathSegment::NONE;
}

bool AmsBackendSnapmaker::slot_has_prep_sensor(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot_index >= 0 && slot_index < kNumTools && !head_is_upstream_fed_locked(slot_index);
}

std::optional<bool> AmsBackendSnapmaker::slot_insert_presence(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= kNumTools || !feed_presence_seen_[slot_index] ||
        head_is_upstream_fed_locked(slot_index))
        return std::nullopt;
    return port_sensor_filament_present_[slot_index];
}

InsertIdentity AmsBackendSnapmaker::slot_insert_identity(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= kNumTools)
        return InsertIdentity::NotRead;
    if (pending_insert_passes_[slot_index] > 0)
        return InsertIdentity::Pending;
    return insert_verdict_[slot_index];
}

PathSegment AmsBackendSnapmaker::infer_error_segment() const {
    return PathSegment::NONE;
}

// ============================================================================
// Filament Operations
// ============================================================================

AmsError AmsBackendSnapmaker::do_load_filament(int slot_index) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS)
        return err;

    // Snapmaker U1 firmware: AUTO_FEEDING is a thin macro wrapper that
    // forwards to the underlying FEED_AUTO command with module/channel
    // resolved from _FILAMENT_FEED_VARIABLE. FEED_AUTO has explicit LOAD /
    // UNLOAD / AUTO parameters — passing none of them is a silent no-op
    // (cmd_FEED_AUTO falls through every branch and returns).
    //
    // We must pass LOAD=1 to actually trigger the feed sequence. PRINTING
    // is left at the default 0 so we skip the firmware's port-input
    // filament-detected gate (which silent-returns when PRINTING=1 and the
    // port sensor reads no filament — the exact runout-recovery state).
    //
    // Trail of bad guesses, in order:
    //   1. T{n} — no-op when target tool already active (Klipper logged
    //      "Extruder extruderN already active"). That's always the case
    //      after a runout, so loads did nothing.
    //   2. AUTO_FEEDING EXTRUDER={n} — silent no-op because no LOAD
    //      parameter was passed; cmd_FEED_AUTO fell through.
    //   3. SM_PRINT_AUTO_FEED — gated on print_task_config.extruders_used,
    //      which can be all-false on a partially-extruded paused print.
    // The firmware's cmd_FEED_AUTO is the definitive reference; see
    // /home/lava/klipper/klippy/extras/filament_feed.py around line 1681.
    //
    // FEED_AUTO answers OK and does nothing for a head that is already loaded,
    // whose module is absent or out of automatic mode, or whose motion sensor
    // is disabled. Those are refused here, where the user can be told why.
    switch (const auto eligibility = slot_op_eligibility(slot_index, /*load=*/true)) {
    case FilamentOpEligibility::AlreadyLoaded:
    case FilamentOpEligibility::FeederUnavailable:
    case FilamentOpEligibility::SensorDisabled:
        return AmsErrorHelper::slot_ineligible(lane_noun(), slot_index,
                                               filament_op_eligibility_reason(eligibility));
    default:
        break;
    }
    return dispatch_feed(fmt::format("AUTO_FEEDING EXTRUDER={} LOAD=1", slot_index),
                         kEventLoadComplete, slot_index);
}

bool AmsBackendSnapmaker::unload_heats_hotend(int slot_index) const {
    if (slot_index < 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        slot_index = system_info_.current_slot;
    }
    // Mirrors do_unload_filament(): no slot means the bare leaf.
    return slot_index >= 0 && supports_auto_heat_on_unload();
}

AmsError AmsBackendSnapmaker::do_unload_filament(int slot_index) {
    // Unload must mirror load: route through AUTO_FEEDING (the firmware macro
    // that forwards to FEED_AUTO with module/channel resolved from
    // _FILAMENT_FEED_VARIABLE), passing UNLOAD=1. FEED_AUTO with no STAGE runs
    // the full unload state machine (prepare → home → pick → heat → unload →
    // finish), which drives the firmware's per-channel feed state all the way
    // to "unload_finish".
    //
    // The bare INNER_FILAMENT_UNLOAD macro is the *leaf* the state machine
    // invokes internally — calling it directly skips the state transitions.
    // On a plain U1 that's fine, but it breaks aftermarket feeders that hook
    // the unload-finish state: the DnG-Crafts U1-Ace mod retracts the Anycubic
    // ACE Pro spool only when a channel reaches "unload_finish", so the bare
    // macro left filament dangling at the toolhead (prestonbrown/helixscreen#974).
    //
    // Resolve which extruder to unload: callers usually pass nothing (-1,
    // "the currently loaded one"), so fall back to current_slot. If we still
    // don't know which slot is loaded, fall back to the firmware's bare unload
    // rather than guess an EXTRUDER index.
    int extruder = slot_index;
    if (extruder < 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        extruder = system_info_.current_slot;
    }
    if (extruder < 0) {
        return dispatch_feed("INNER_FILAMENT_UNLOAD", kEventUnloadComplete);
    }

    auto err = validate_slot_index(extruder);
    if (err.result != AmsResult::SUCCESS)
        return err;

    return dispatch_feed(fmt::format("AUTO_FEEDING EXTRUDER={} UNLOAD=1", extruder),
                         kEventUnloadComplete);
}

AmsError AmsBackendSnapmaker::dispatch_feed(std::string gcode, const char* complete_event,
                                            int load_head) {
    return dispatch_feed(
        std::move(gcode), [this, complete_event] { emit_event(complete_event); },
        [this, load_head](const MoonrakerError& err) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                mark_load_unconfirmed_locked(load_head, err.message);
            }
            if (err.type != MoonrakerErrorType::TIMEOUT) {
                emit_event(kEventError, err.message);
            }
        });
}

AmsError AmsBackendSnapmaker::dispatch_feed(std::string gcode, std::function<void()> on_ok,
                                            std::function<void(const MoonrakerError&)> on_failed) {
    // FEED_AUTO homes itself, and both callbacks run on the main thread.
    return ensure_homed_then(std::move(gcode), std::move(on_ok), std::move(on_failed),
                             IMoonrakerAPI::kAmsOperationTimeoutMs, /*skip_homing=*/true,
                             /*silent=*/true,
                             // The error callback only ends the operation, so
                             // Klipper's `!!` broadcast stays the thing that
                             // shows the user why.
                             /*caller_surfaces_errors=*/false);
}

AmsError AmsBackendSnapmaker::do_filament_batch(const std::vector<int>& slots, bool load) {
    if (slots.empty()) {
        return AmsErrorHelper::invalid_parameter("batch filament op with no slots");
    }
    for (int slot : slots) {
        if (auto err = validate_slot_index(slot); err.result != AmsResult::SUCCESS) {
            return err;
        }
    }
    if (!api_) {
        return AmsErrorHelper::not_connected("IMoonrakerAPI not available");
    }
    bool use_batch_macro;
    uint64_t dispatch_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The in-flight claim spans only this dispatch call, and `action`
        // returns to IDLE at each head's terminal — so the preheat gap
        // between heads would otherwise admit a second batch whose
        // ACTION=START the firmware's `doing` interlock refuses, and that
        // refusal fires the RPC-error recovery's ACTION=END into the batch
        // still running.
        if (batch_.active) {
            return AmsErrorHelper::busy(
                ams_action_to_string(batch_.load ? AmsAction::LOADING : AmsAction::UNLOADING));
        }
        use_batch_macro = use_batch_macro_;
        // Resolve the progress words here, at dispatch: the cursor-advance
        // parse only formats them and makes no LVGL calls.
        batch_ = BatchPlan{slots,           load,
                           /*cursor=*/0,
                           /*active=*/true, load ? lv_tr("Load") : lv_tr("Unload"),
                           lv_tr("of"),     next_batch_dispatch_id_};
        dispatch_id = next_batch_dispatch_id_++;
    }
    const std::string chain = batch_feed_gcode(slots, load, use_batch_macro);
    const char* tag = backend_log_tag();
    spdlog::info("{} Executing G-code: {}", tag, chain);
    // Sent through api_ rather than the shared execute_gcode() so the timeout
    // can scale per op — the shared overloads pin kAmsOperationTimeoutMs
    // (300s), which a 4-head cold batch can outlast. The operation's
    // completion is owned by the firmware phase the sidebar tracks, not by
    // this RPC's return; the error path reaches `this` only through the
    // lifetime token, which marshals to main and skips a dead owner.
    auto tok = lifetime_.token();
    api_->execute_gcode(
        chain, [tag]() { spdlog::debug("{} batch G-code executed successfully", tag); },
        [this, tok, tag, chain, dispatch_id](const MoonrakerError& err) mutable {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("{} G-code response timed out (may still be running): {}", tag, chain);
            } else {
                spdlog::error("{} G-code failed: {} - {}", tag, chain, err.message);
            }
            tok.defer("AmsBackendSnapmaker::do_filament_batch.recover",
                      [this, tag, msg = err.message, dispatch_id] {
                          // The failure carries authority only while the plan
                          // it failed is still the live one: a TIMEOUT can
                          // land long after every head verified, and END
                          // then restores the targets snapshotted at that
                          // batch's START over a preheat the user started
                          // since.
                          bool clear_interlock;
                          int failed_head;
                          {
                              std::lock_guard<std::mutex> lock(mutex_);
                              clear_interlock = batch_.active && batch_.dispatch_id == dispatch_id;
                              failed_head = clear_interlock ? batch_.heads[batch_.cursor] : -1;
                              if (clear_interlock) {
                                  // The recovery's own END finishes the
                                  // batch: leaving the plan active would let
                                  // later unrelated channel traffic advance a
                                  // zombie cursor.
                                  batch_.active = false;
                              }
                          }
                          if (!clear_interlock) {
                              spdlog::info("{} batch RPC failure for dispatch {} is stale — "
                                           "interlock left alone",
                                           tag, dispatch_id);
                              return;
                          }
                          spdlog::warn("{} batch RPC failed at head {} ({}): clearing the "
                                       "firmware batch interlock",
                                       tag, failed_head, msg);
                          end_firmware_batch();
                      });
        },
        static_cast<uint32_t>(slots.size()) * kBatchFeedOpTimeoutMs,
        /*silent=*/true, /*on_queued=*/nullptr,
        // The callbacks above log and schedule the interlock clear; neither
        // claims the error report, so Klipper's `!!` broadcast still surfaces
        // — the explanation a failed feed actually needs.
        /*caller_surfaces_errors=*/false);
    return AmsErrorHelper::success();
}

std::string AmsBackendSnapmaker::batch_feed_gcode(const std::vector<int>& slots, bool load,
                                                  bool use_batch_macro) {
    const char* dir = load ? "LOAD=1" : "UNLOAD=1";
    if (!use_batch_macro) {
        std::string chain;
        for (int slot : slots) {
            if (!chain.empty()) {
                chain += '\n';
            }
            chain += fmt::format("AUTO_FEEDING EXTRUDER={} {}", slot, dir);
        }
        return chain;
    }

    // START snapshots every hotend target and raises the `doing` interlock;
    // END restores those targets mid-print and zeroes them when idle. The
    // firmware refuses a print start while `doing` is set, so END must run.
    // NEXT_EXTRUDER names the next selected head so the firmware preheats it
    // while the current one runs; it is omitted on the last.
    std::string chain = "AUTO_FEEDING_BATCH ACTION=START";
    for (size_t i = 0; i < slots.size(); ++i) {
        chain += fmt::format("\nAUTO_FEEDING_BATCH ACTION=DOING EXTRUDER={} {}", slots[i], dir);
        if (i + 1 < slots.size()) {
            chain += fmt::format(" NEXT_EXTRUDER={}", slots[i + 1]);
        }
    }
    chain += '\n';
    chain += batch_feeding::kEndGcode;
    return chain;
}

AmsBackendSnapmaker::BatchPlan AmsBackendSnapmaker::batch_plan() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return batch_;
}

void AmsBackendSnapmaker::end_firmware_batch() {
    bool use_batch_macro;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        use_batch_macro = use_batch_macro_;
    }
    if (!use_batch_macro) {
        return; // no interlock exists on this firmware
    }
    execute_gcode(batch_feeding::kEndGcode);
}

bool AmsBackendSnapmaker::can_unload_from_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= kNumTools) {
        return false;
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (!slot || !slot->is_present()) {
        return false;
    }
    // Loaded to the nozzle: the channel_state latch. See the header note.
    return loaded_at_toolhead_[slot_index];
}

bool AmsBackendSnapmaker::slot_has_filament_at_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= kNumTools) {
        return false;
    }
    // channel_state latch, NOT the toolhead switch; see the header note.
    return loaded_at_toolhead_[slot_index];
}

std::optional<bool> AmsBackendSnapmaker::head_has_filament(int head) const {
    if (head < 0 || head >= kNumTools) {
        return std::nullopt;
    }
    return can_unload_from_toolhead(head) || slot_filament_parked_in_toolhead(head);
}

bool AmsBackendSnapmaker::slot_filament_parked_in_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= kNumTools) {
        return false;
    }
    return parked_in_toolhead_locked(slot_index);
}

bool AmsBackendSnapmaker::slot_is_actively_loaded(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= kNumTools) {
        return false;
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    return slot && slot->status == SlotStatus::LOADED;
}

AmsBackendSnapmaker::ChannelSnapshot AmsBackendSnapmaker::channel_snapshot(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= kNumTools) {
        return {};
    }
    return channel_snapshots_[slot_index];
}

AmsBackend::FilamentOpEligibility AmsBackendSnapmaker::slot_op_eligibility(int slot_index,
                                                                           bool load) const {
    using E = FilamentOpEligibility;
    if (slot_index < 0 || slot_index >= kNumTools) {
        return E::Busy;
    }
    const ChannelSnapshot snap = channel_snapshot(slot_index);

    // The firmware reports channel_error="no_filament" for ANY empty lane, and
    // ""/"none" when a channel has nothing to say — none of the three is a
    // hard fault, or every empty feeder would read as an error.
    if (channel_error_is_fault(snap.error)) {
        return E::Error;
    }
    // An empty lane answers Empty ahead of the settled-state test: an idle
    // empty lane reports an unsettled state ("none"/"inited"), and presence
    // is knowable even when the state vocabulary is not.
    if (!snap.filament_detected) {
        return E::Empty;
    }
    // Only these five are settled states. Anything else is mid-operation or
    // unrecognised, and a batch must not act on a head it cannot describe.
    // manual_sta_finish is a persistent terminal: a head that finished a
    // manual feed sits in it until the next operation, so reading it as
    // Busy would refuse every batch naming that head.
    const bool settled = snap.state == "wait_insert" || snap.state == "preload_finish" ||
                         snap.state == "load_finish" || snap.state == "unload_finish" ||
                         snap.state == "manual_sta_finish";
    if (!settled) {
        return E::Busy;
    }
    const bool loaded = snap.state == "load_finish";
    if (load && loaded) {
        return E::AlreadyLoaded;
    }
    if (!load && !loaded && !slot_filament_parked_in_toolhead(slot_index)) {
        return E::NotLoaded;
    }
    // Eligible on state; now the feeder has to be able to act.
    if (!snap.module_exist || snap.disable_auto) {
        return E::FeederUnavailable;
    }
    if (load && !snap.sensor_enabled) {
        return E::SensorDisabled;
    }
    return E::Eligible;
}

AmsError AmsBackendSnapmaker::do_select_slot(int slot_index) {
    return do_change_tool(slot_index);
}

AmsError AmsBackendSnapmaker::do_change_tool(int tool_number) {
    auto err = validate_slot_index(tool_number);
    if (err.result != AmsResult::SUCCESS)
        return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_tool_ = tool_number;
        pending_until_ = std::chrono::steady_clock::now() + kToolChangeWindow;
    }
    auto result = execute_gcode(fmt::format("T{}", tool_number));
    if (!result.success()) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_tool_ = -1;
    }
    return result;
}

AmsError AmsBackendSnapmaker::do_park_active_tool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_tool_ = -1;
        if (system_info_.current_tool < 0) {
            return AmsErrorHelper::not_loaded();
        }
    }
    return execute_gcode("PARK_EXTRUDER");
}

// ============================================================================
// Recovery (not supported)
// ============================================================================

AmsError AmsBackendSnapmaker::recover() {
    return AmsErrorHelper::not_supported("Recover not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::reset() {
    return AmsErrorHelper::not_supported("Reset not supported on Snapmaker");
}

void AmsBackendSnapmaker::on_klipper_restarted() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channel_snapshots_ = {};
        preload_in_flight_ = {};
        unload_in_flight_ = {};
    }
    AmsSubscriptionBackend::on_klipper_restarted();
}

AmsError AmsBackendSnapmaker::cancel() {
    return AmsErrorHelper::not_supported("Cancel not supported on Snapmaker");
}

// ============================================================================
// Resume Preparation
// ============================================================================

// NOTE: This method has no caller in tree right now. The auto-recover-on-pause
// path that consumed it was pulled after field testing showed the
// motion=false + port=true signal can't distinguish "stale encoder, filament
// at gear" from "filament preloaded 4-inches short of gear" (firmware assist
// motor stops at preload_finish). Kept as detection infrastructure for the
// deferred follow-up in task #19 — when we have a verifiable signal that
// filament is *at* the extruder gear (likely filament_feed.channel_state ==
// 'load_finish'), this gate logic gets revived and tightened.
bool AmsBackendSnapmaker::is_stuck_motion_sensor_runout(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int slot = slot_index;
    if (slot < 0) {
        // Snapmaker has 1:1 tool↔slot mapping — current_tool and current_slot
        // hold the same value. Either is correct; current_slot matches the
        // convention used by other AmsBackend methods that take slot_index.
        slot = system_info_.current_slot;
    }
    if (slot < 0 || slot >= kNumTools) {
        return false;
    }
    // Motion (encoder) sensor latched false, port (buffer) sensor still
    // sees filament. The encoder is stale — the slot has physical filament
    // ready to feed, the motion sensor just hasn't been re-armed by extrusion.
    return !sensor_filament_present_[slot] && port_sensor_filament_present_[slot];
}

AmsBackendSnapmaker::~AmsBackendSnapmaker() = default;

void AmsBackendSnapmaker::prepare_for_resume(int slot_index, ResumeReadyCallback on_ready) {
    // Resolve target slot. Caller passes -1 when they don't know which tool
    // is active — fall back to system_info_.current_tool.
    int slot = slot_index;
    bool sensor_present = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot < 0) {
            // Snapmaker has 1:1 tool↔slot mapping — current_tool and
            // current_slot hold the same value. current_slot matches the
            // slot_index convention used by other AmsBackend methods.
            slot = system_info_.current_slot;
        }
        if (slot >= 0 && slot < kNumTools) {
            sensor_present = sensor_filament_present_[slot];
        }
    }

    // Classify why the print paused. Terminal causes (confirmed under #991)
    // get the restart modal up front; everything else attempts RESUME
    // (default-recoverable). A RESUME that truly no-ops returns an ERROR through
    // the RESUME gcode callback, which the dispatch layer handles. Every path
    // classifies first: a Terminal pause writes nothing.
    helix::PauseSignals sig;
    sig.exception_id = get_printer_state().print_state().get_print_exception_id();
    sig.exception_code = get_printer_state().print_state().get_print_exception_code();
    // On these firmware pauses print_stats.message is empty — the reason text
    // lives in exception.message. Fall back to print_stats.message when the
    // exception carries no text (e.g. non-Snapmaker pause paths).
    sig.message = get_printer_state().print_state().get_print_exception_message();
    if (sig.message.empty()) {
        sig.message =
            lv_subject_get_string(get_printer_state().print_state().get_print_message_subject());
    }
    sig.sdcard_active = get_printer_state().print_state().is_sdcard_active();
    sig.runout_tripped = !sensor_present;
    if (helix::classify_pause(sig, helix::snapmaker_terminal_matchers()) ==
        helix::PauseCause::Terminal) {
        spdlog::warn("{} prepare_for_resume: classified Terminal — surfacing restart UX",
                     backend_log_tag());
        if (on_ready) {
            // snapmaker_terminal_matchers() currently recognizes one terminal
            // cause (dirty bed), so a single fixed reason covers every match.
            on_ready(AmsErrorHelper::resume_requires_restart(
                "classify_pause: Terminal",
                lv_tr("The bed was reported dirty, so this print cannot resume.")));
        }
        return;
    }

    // A load or hand feed still moving filament on the head (started from the
    // printer's own screen, say) is answered before anything is sent: RESUME
    // would extrude into it, and a refeed would race it.
    if (slot >= 0 && slot < kNumTools) {
        ChannelSnapshot snap;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap = channel_snapshots_[slot];
        }
        if (feeder_serves_head(snap) && helix::snapmaker::channel_state_in_progress(snap.state)) {
            spdlog::warn("{} prepare_for_resume: tool {} feeder is mid-operation ('{}') — "
                         "refusing RESUME",
                         backend_log_tag(), slot, snap.state);
            if (on_ready) {
                on_ready(AmsErrorHelper::busy(snap.state));
            }
            return;
        }
    }

    // The presence switch cannot vouch for a head whose last load failed:
    // filament can reach it and stop short of the nozzle. Resume is the retry
    // for the head it resumes on; any other head has to be loaded first, since
    // loading it would leave it picked up when RESUME runs.
    int unconfirmed = -1;
    bool may_refeed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        unconfirmed = unconfirmed_load_head_locked(slot);
        may_refeed = slot >= 0 && slot < kNumTools && resume_may_refeed_locked(slot);
    }
    if (unconfirmed >= 0 && unconfirmed == slot && may_refeed) {
        spdlog::warn("{} prepare_for_resume: tool {} last load unconfirmed — retrying it "
                     "before RESUME",
                     backend_log_tag(), slot);
        run_runout_refeed(slot, std::move(on_ready));
        return;
    }
    if (unconfirmed >= 0) {
        spdlog::warn("{} prepare_for_resume: tool {} last load unconfirmed — refusing RESUME "
                     "on tool {}",
                     backend_log_tag(), unconfirmed, slot);
        if (on_ready) {
            on_ready(unconfirmed_load_error(unconfirmed, "load unconfirmed, not refed here",
                                            /*retry_on_resume=*/false));
        }
        return;
    }

    if (slot >= 0 && slot < kNumTools && !sensor_present) {
        // A runout leaves no record repair: the user may have loaded a
        // different spool, and the material remembered for the old one would
        // be recorded against it. The firmware's refusal and classify_error's
        // explanation are the answer there.
        if (!may_refeed) {
            spdlog::warn("{} prepare_for_resume: tool {} ran out and this backend does not "
                         "refeed it — refusing RESUME",
                         backend_log_tag(), slot);
            if (on_ready) {
                on_ready(
                    AmsError(AmsResult::COMMAND_FAILED, "runout: refeed left to the user",
                             helix::ui::tool_label_in_form(tool_noun(), helix::ui::tool_label(slot),
                                                           slot, helix::ui::ToolLabelForm::Full) +
                                 ": " + lv_tr("Filament runout"),
                             lv_tr("Load it in the filament panel, then tap Resume.")));
            }
            return;
        }
        run_runout_refeed(slot, std::move(on_ready));
        return;
    }

    if (slot >= 0 && slot < kNumTools) {
        ChannelSnapshot snap;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap = channel_snapshots_[slot];
        }
        // The motion sensor reads present partway through a load that then
        // fails, so on a head an automatic feeder serves, a feeder reporting a
        // failure outranks it. RESUME extrudes into the head before it prints,
        // which is the wrong thing to do to a jam.
        if (feeder_serves_head(snap) && feed_failure_reported(snap)) {
            spdlog::warn("{} prepare_for_resume: tool {} sensor reads present but the feeder "
                         "reads '{}' (error '{}') — {} before RESUME",
                         backend_log_tag(), slot, snap.state, snap.error,
                         may_refeed ? "loading" : "refusing");
            if (may_refeed) {
                run_runout_refeed(slot, std::move(on_ready));
            } else if (on_ready) {
                on_ready(unconfirmed_load_error(slot, "feeder has not confirmed a load",
                                                /*retry_on_resume=*/false));
            }
            return;
        }
    }

    if (slot < 0 || slot >= kNumTools) {
        spdlog::debug("{} prepare_for_resume: no active tool", backend_log_tag());
    } else {
        // Klipper's motion sensor reads filament — RESUME can clear its own
        // exception once extrusion resumes.
        spdlog::info("{} prepare_for_resume: tool {} sensor reports filament present, "
                     "skipping recovery",
                     backend_log_tag(), slot);
    }
    // The firmware refuses RESUME while a used head's record names no filament
    // type, which a tagless spool whose material was set only here leaves it
    // in. Writing the record is allowed while paused.
    std::vector<std::pair<int, std::string>> repairs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        repairs = resume_record_repairs_locked(slot);
    }
    send_resume_repairs_then(std::move(repairs), [on_ready]() {
        if (on_ready) {
            on_ready(AmsErrorHelper::success());
        }
    });
}

void AmsBackendSnapmaker::run_runout_refeed(int slot, ResumeReadyCallback on_ready) {
    // Sensor reads runout. The firmware's own INNER_RESUME auto-feed
    // (SM_PRINT_AUTO_FEED) is gated on print_task_config.extruders_used[slot],
    // which the firmware freezes False for the duration of a print — so a plain
    // RESUME no-ops the feed and immediately re-pauses at CHECK_FILAMENT_RUNOUT.
    // AUTO_FEEDING calls FEED_AUTO directly (no extruders_used gate): it homes,
    // switches to the tool, feeds filament from the AMS port across the gap to
    // the toolhead sensor, heats to the slot's filament temp, then extrudes and
    // flushes — leaving the channel at load_finish with the runout sensor reading
    // present, so the subsequent RESUME's CHECK_FILAMENT_RUNOUT passes. The gcode
    // blocks until load_finish (success) or raises (error), and is idempotent
    // (FEED_AUTO returns early if already loaded). Verified live on a physical U1
    // (#991). Disabling the runout sensor around it would neuter FEED_AUTO, and no
    // manual heat or extrude is needed: AUTO_FEEDING does both.
    if (!api_) {
        spdlog::warn("{} prepare_for_resume: IMoonrakerAPI unavailable", backend_log_tag());
        if (on_ready) {
            on_ready(AmsErrorHelper::not_connected("IMoonrakerAPI unavailable"));
        }
        return;
    }
    {
        // One refeed at a time: a second Resume tap while one runs would send a
        // second AUTO_FEEDING and leave the first caller unanswered.
        std::lock_guard<std::mutex> lock(mutex_);
        if (refeed_in_flight_) {
            spdlog::warn("{} prepare_for_resume: tool {} refeed already running", backend_log_tag(),
                         slot);
            if (on_ready) {
                on_ready(AmsErrorHelper::busy("refeed"));
            }
            return;
        }
        refeed_in_flight_ = true;
    }

    std::string chain = fmt::format("AUTO_FEEDING EXTRUDER={0} LOAD=1 PRINTING=1", slot);
    spdlog::info("{} prepare_for_resume: tool {} runout latched — driving AMS load "
                 "(AUTO_FEEDING) before RESUME",
                 backend_log_tag(), slot);

    // AUTO_FEEDING heats + feeds + flushes (~86s) before the resume lands. The
    // pending-action UI only shows an optimistic spinner with no text, so tell
    // the user what the wait is — otherwise the long pause reads as a hang.
    // prepare_for_resume runs on the main thread (resume-button path), so this
    // toast is safe to raise directly here.
    ToastManager::instance().show(ToastSeverity::INFO,
                                  lv_tr("Refeeding filament — this may take a minute"),
                                  /*duration_ms=*/8000);

    auto tok = lifetime_.token();
    const char* tag = backend_log_tag();
    IMoonrakerAPI* api_ptr = api_;
    api_ptr->execute_gcode(
        chain,
        [this, tok, on_ready, tag, slot]() mutable {
            // IMoonrakerAPI callbacks fire on the libhv WebSocket thread; defer to
            // main so on_ready (and the backstop arm) run on the UI thread.
            tok.defer("AmsBackendSnapmaker::prepare_for_resume.ok",
                      [this, cb = std::move(on_ready), slot]() mutable {
                          await_refeed_outcome(slot, std::move(cb));
                      });
        },
        [this, tok, on_ready, tag, slot](const MoonrakerError& err) mutable {
            std::string msg = err.message;
            tok.defer("AmsBackendSnapmaker::prepare_for_resume.err", [this,
                                                                      cb = std::move(on_ready), tag,
                                                                      slot, msg]() {
                spdlog::error("{} prepare_for_resume: tool {} AMS load failed: {}", tag, slot, msg);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    mark_load_unconfirmed_locked(slot, msg);
                    refeed_in_flight_ = false;
                }
                // Load failed → RESUME is never dispatched; report failure.
                // The copy names the head, not the firmware's own error
                // text, which spells it with a 0-based extruder name.
                if (cb) {
                    cb(unconfirmed_load_error(slot, "prepare_for_resume AMS load failed: " + msg));
                }
            });
        },
        // AUTO_FEEDING heats from cold + feeds + flushes; measured ~86s live, so
        // give generous headroom.
        /*timeout_ms=*/150000,
        /*silent=*/true);
}

void AmsBackendSnapmaker::mark_load_unconfirmed_locked(int head, std::string_view why) {
    if (head < 0 || head >= kNumTools) {
        return;
    }
    // A state no frame has named yet could be a print; only a known idle
    // printer has no Resume to hold.
    if (print_state_ && !print_in_progress_locked()) {
        return;
    }
    if (!load_unconfirmed_[head]) {
        spdlog::warn("{} tool {} load unconfirmed ({}); Resume waits for load_finish",
                     backend_log_tag(), head, why);
    }
    load_unconfirmed_[head] = true;
}

int AmsBackendSnapmaker::unconfirmed_load_head_locked(int preferred) const {
    if (preferred >= 0 && preferred < kNumTools && load_unconfirmed_[preferred]) {
        return preferred;
    }
    // Another head holds Resume only when the print is known to use it.
    for (int head = 0; head < kNumTools; ++head) {
        if (load_unconfirmed_[head] && static_cast<size_t>(head) < extruders_used_.size() &&
            extruders_used_[static_cast<size_t>(head)]) {
            return head;
        }
    }
    return -1;
}

void AmsBackendSnapmaker::await_refeed_outcome(int slot, ResumeReadyCallback on_ready) {
    bool wait = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wait = !feeder_reports_loaded(channel_snapshots_[slot]);
        if (wait) {
            // The status frame naming load_finish can trail the command's
            // answer; the head's next settled frame, or the timeout, decides.
            refeed_wait_ = RefeedWait{slot, std::move(on_ready)};
        }
    }
    if (!wait) {
        settle_refeed(slot, std::move(on_ready));
        return;
    }
    // The guard cancels the timer when the backend is destroyed.
    refeed_wait_timer_.reset(lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<AmsBackendSnapmaker*>(lv_timer_get_user_data(timer));
            self->finish_refeed_wait();
        },
        static_cast<uint32_t>(kRefeedFrameWait.count()), this));
    lv_timer_set_repeat_count(refeed_wait_timer_.get(), 1);
}

void AmsBackendSnapmaker::finish_refeed_wait() {
    std::optional<RefeedWait> wait;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wait.swap(refeed_wait_);
    }
    // Cancelling is safe from inside the timer's own callback.
    refeed_wait_timer_.reset();
    if (wait) {
        settle_refeed(wait->slot, std::move(wait->on_ready));
    }
}

void AmsBackendSnapmaker::settle_refeed(int slot, ResumeReadyCallback on_ready) {
    bool confirmed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        confirmed = feeder_reports_loaded(channel_snapshots_[slot]);
        refeed_in_flight_ = false;
        if (confirmed) {
            load_unconfirmed_[slot] = false;
        } else {
            mark_load_unconfirmed_locked(slot, "no load_finish after OK");
        }
    }
    if (!confirmed) {
        spdlog::error("{} prepare_for_resume: tool {} AUTO_FEEDING answered OK but the feeder "
                      "never reported load_finish",
                      backend_log_tag(), slot);
        if (on_ready) {
            on_ready(unconfirmed_load_error(slot, "prepare_for_resume AMS load unconfirmed"));
        }
        return;
    }
    spdlog::info("{} prepare_for_resume: tool {} AMS load complete (load_finish)",
                 backend_log_tag(), slot);
    if (on_ready) {
        on_ready(AmsErrorHelper::success());
    }
}

AmsError AmsBackendSnapmaker::unconfirmed_load_error(int head, std::string technical,
                                                     bool retry_on_resume) const {
    const std::string tool = helix::ui::tool_label_in_form(tool_noun(), helix::ui::tool_label(head),
                                                           head, helix::ui::ToolLabelForm::Full);
    return AmsError(
        AmsResult::COMMAND_FAILED, std::move(technical),
        fmt::format(fmt::runtime(lv_tr("{} didn't load. Check that its hotend is seated and do a "
                                       "test extrude before resuming.")),
                    tool),
        retry_on_resume
            ? fmt::format(fmt::runtime(lv_tr("Tap Resume to try the load again from {}.")),
                          AmsErrorHelper::position_label(lane_noun(), head))
            : std::string(lv_tr("Load it in the filament panel, then tap Resume.")));
}

bool AmsBackendSnapmaker::print_in_progress_locked() const {
    return print_state_ && (*print_state_ == "printing" || *print_state_ == "paused");
}

void AmsBackendSnapmaker::apply_print_state_locked(const std::string& state) {
    const bool had_state = print_state_.has_value();
    const bool was_in_progress = print_in_progress_locked();
    print_state_ = state;
    const bool in_progress = print_in_progress_locked();
    // A load's outcome and the used-heads check each belong to one print. The
    // first state a connection reads is no change of print: a reconnect
    // mid-print keeps them, and one that finds no print forgets them.
    if ((had_state && was_in_progress != in_progress) || (!had_state && !in_progress)) {
        load_unconfirmed_.fill(false);
        used_heads_checked_ = false;
        awaiting_restart_file_ = false;
        ++print_generation_;
    }
}

void AmsBackendSnapmaker::note_print_start_from_here() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (print_in_progress_locked()) {
        load_unconfirmed_.fill(false);
        used_heads_checked_ = false;
        awaiting_restart_file_ = true;
        ++print_generation_;
    }
}

bool AmsBackendSnapmaker::used_heads_check_due_locked(bool frame_names_state,
                                                      bool frame_names_file) {
    // A frame naming the print's state or file arms the check. After a
    // restart that never left the print, frames of the cancelled print can
    // still read printing, so only the new file's name arms it.
    const bool names_this_print =
        awaiting_restart_file_ ? frame_names_file : (frame_names_state || frame_names_file);
    if (used_heads_checked_ || !names_this_print || !print_state_ || *print_state_ != "printing") {
        return false;
    }
    awaiting_restart_file_ = false;
    used_heads_checked_ = true;
    return true;
}

std::set<int> AmsBackendSnapmaker::heads_for_tools_locked(const std::set<int>& tools) const {
    const std::vector<int> task_map = task_routing(extruders_used_, extruder_map_table_);
    const helix::FirmwareRouting routing = default_routing();
    std::set<int> heads;
    for (int tool : tools) {
        int head = routing.head(tool);
        if (tool >= 0 && static_cast<size_t>(tool) < task_map.size()) {
            head = task_map[static_cast<size_t>(tool)];
        }
        if (head >= 0 && head < kNumTools) {
            heads.insert(head);
        }
    }
    return heads;
}

void AmsBackendSnapmaker::warn_unloaded_used_heads(const std::string& filename, uint32_t print) {
    auto on_tools = [this, filename, print](std::set<int> tools) {
        if (tools.empty()) {
            spdlog::debug("{} '{}': tools not readable; no used-head check", backend_log_tag(),
                          filename);
            return;
        }
        std::vector<int> unloaded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (print != print_generation_) {
                return;
            }
            for (int head : heads_for_tools_locked(tools)) {
                const ChannelSnapshot& snap = channel_snapshots_[static_cast<size_t>(head)];
                // The firmware feeds a head the task names before it is used.
                const bool firmware_loads = static_cast<size_t>(head) < extruders_used_.size() &&
                                            extruders_used_[static_cast<size_t>(head)];
                if (!feeder_serves_head(snap) || firmware_loads || feeder_reports_loaded(snap) ||
                    helix::snapmaker::channel_state_in_progress(snap.state)) {
                    continue;
                }
                unloaded.push_back(head);
            }
        }
        for (int head : unloaded) {
            const std::string tool = helix::ui::tool_label_in_form(
                tool_noun(), helix::ui::tool_label(head), head, helix::ui::ToolLabelForm::Full);
            spdlog::warn("{} '{}' uses tool {}, whose feeder reports it not loaded",
                         backend_log_tag(), filename, head);
            ui_notification_warning(
                fmt::format(fmt::runtime(lv_tr("{} isn't loaded. The print will pause when it "
                                               "needs it.")),
                            tool)
                    .c_str());
        }
    };
    if (file_tools_probe_) {
        file_tools_probe_(filename, std::move(on_tools));
        return;
    }
    if (!api_ || filename.empty()) {
        return;
    }
    if (!file_facts_) {
        file_facts_ = std::make_unique<FileStartFactsLoader>(api_);
    }
    // The footer at most: the check must not cost a download of the file.
    // The loader is owned here and drops its answer when destroyed.
    file_facts_->load(
        filename,
        [on_tools = std::move(on_tools)](FileStartFacts facts) {
            on_tools(file_tools_used(facts));
        },
        nullptr, nullptr, FileRead::Footer);
}

// ============================================================================
// Configuration
// ============================================================================

AmsError AmsBackendSnapmaker::apply_user_edit(int slot_index, const SlotInfo& info,
                                              const helix::ams::Observation& declared) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS)
        return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = system_info_.units[0].get_slot(slot_index);
        if (!slot)
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, kNumTools - 1);

        slot->assign_filament_fields(info);
        insert_since_declaration_[static_cast<size_t>(slot_index)] = false;

        // handle_status writes RFID and print_task_config fields
        // unconditionally, so an edit kept only in memory is wiped by the next
        // Klipper status update. Stage the override into overrides_ so the edit
        // survives a restart; the lane's own declaration, filed when the edit is
        // committed, is what apply_resolved_lane lays back over firmware data on
        // every subsequent parse.
        //
        // NOTE on self-wipe: the AD5X IFS implementation pre-updates
        // last_firmware_color_ here to prevent the color-based hardware-event
        // check from misreading a user color edit as a physical spool swap.
        // Snapmaker's hardware-event check is RFID-UID-based, and the user
        // cannot set a CARD_UID through the edit UI — SlotInfo has no UID
        // field. So rfid_tracker_ keeps whatever the firmware last
        // reported, and the next parse compares firmware UID against that
        // baseline exactly as intended. No expected-echo value needed here.
        // (CFS shares the tracker and DOES register one — it writes
        // color_value back to the box, which is half of its fingerprint.)
        helix::ams::stage_user_override(overrides_, slot_index, info, declared);
    }

    if (override_store_) {
        helix::ams::persist_staged_override(override_store_.get(), mutex_, overrides_, slot_index,
                                            backend_log_tag(), "Override");
    }

    // print_task_config is the head's firmware record: RESUME refuses a used
    // head whose record names no type, and the stock screen and slicers read
    // the spool from it. SET_PRINT_FILAMENT_CONFIG writes it and leaves the
    // head unofficial. The Extended Firmware's filament_detect/set fills the
    // same record but flags the head official, as an RFID spool, and the
    // firmware then refuses plain writes to it. The filament_detect entry that
    // endpoint also writes is read elsewhere only for a slicer's bed
    // temperature.
    write_task_filament_record(slot_index, info, declared.material.has_value());

    // Pass slot_index as event data so AmsState can do a targeted slot sync.
    // Without it, AmsState::on_event silently skips the refresh and the AMS
    // panel never re-reads the edited slot — the UI shows stale data until
    // the next firmware status notification triggers a full refresh.
    emit_event(kEventSlotChanged, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendSnapmaker::sync_external_identity(int slot_index, const SlotInfo& info) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS)
        return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = system_info_.units[0].get_slot(slot_index);
        if (!slot)
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, kNumTools - 1);

        // overrides_ is left alone and nothing reaches firmware: the next
        // Klipper status update overwrites a synced value.
        slot->assign_filament_fields(info);
    }

    // Pass slot_index as event data so AmsState can do a targeted slot sync.
    emit_event(kEventSlotChanged, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendSnapmaker::persist_external_identity_impl(int slot_index,
                                                         const helix::ams::Observation& spoolman) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_external_identity(override_store_.get(), overrides_, slot_index,
                                                   spoolman, tag);
}

void AmsBackendSnapmaker::persist_slot_weight(int slot_index, float remaining_weight_g,
                                              float total_weight_g) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_weight(override_store_.get(), overrides_, slot_index,
                                        remaining_weight_g, total_weight_g, tag);
}

AmsError AmsBackendSnapmaker::can_set_tool_mapping(int /*tool_number*/, int /*slot_index*/) const {
    return AmsErrorHelper::not_supported("Tool mapping not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::set_tool_mapping_impl(int tool_number, int slot_index) {
    return can_set_tool_mapping(tool_number, slot_index);
}

// ============================================================================
// Bypass (not applicable)
// ============================================================================

AmsError AmsBackendSnapmaker::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass not supported on Snapmaker");
}

// ============================================================================
namespace {

/// The tracker fingerprint for one RFID reading, built from exactly the fields
/// the insert rule judges (prestonbrown/helixscreen#1710): the UID when the
/// read produced one, else the tag's material and colour when the tag decoded
/// but its UID did not, else "-" for a finished read that found no tag. A bare
/// UID is the spelling records already persist as helix_fingerprint, so it
/// cannot grow a prefix without every restart reading as a swap.
std::string fingerprint_from_evidence(const helix::ams::SpoolEvidence& evidence) {
    if (!evidence.tag_uid.empty()) {
        return evidence.tag_uid;
    }
    if (!evidence.material.empty() || evidence.color_rgb.has_value()) {
        return fmt::format("M|{}|{:06X}", evidence.material, evidence.color_rgb.value_or(0u));
    }
    if (evidence.tag_read_complete) {
        return "-";
    }
    return "";
}

/// The reading a fingerprint names, for the comparison side of the insert
/// rule. Inverse of fingerprint_from_evidence(); "" names no reading. A stored
/// fingerprint is foreign input, so a colour that does not parse reads as none
/// rather than throwing.
std::optional<helix::ams::SpoolEvidence> evidence_from_fingerprint(const std::string& fingerprint) {
    if (fingerprint.empty()) {
        return std::nullopt;
    }
    helix::ams::SpoolEvidence evidence;
    if (fingerprint == "-") {
        evidence.tag_read_complete = true;
        return evidence;
    }
    if (fingerprint.rfind("M|", 0) == 0) {
        // A tag whose UID never decoded: material and colour stand, and the
        // UID is the one part of the read still outstanding.
        const size_t split = fingerprint.find('|', 2);
        evidence.material = fingerprint.substr(2, split - 2);
        if (split != std::string::npos) {
            uint32_t color = 0;
            const std::string_view hex(fingerprint.data() + split + 1,
                                       fingerprint.size() - split - 1);
            const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), color, 16);
            if (ec == std::errc{} && ptr == hex.data() + hex.size()) {
                evidence.color_rgb = color;
            }
        }
        return evidence;
    }
    evidence.tag_uid = fingerprint;
    evidence.tag_read_complete = true;
    return evidence;
}

} // namespace

// Status Update Handling
// ============================================================================

void AmsBackendSnapmaker::apply_extruders_locked(const snapmaker::StatusDelta& delta,
                                                 FrameEffects& fx) {
    for (int i = 0; i < kNumTools; i++) {
        const auto& extruder = delta.extruders[i];
        if (!extruder) {
            continue;
        }

        // A parked tool is not the loaded one. LOADED itself is written only by
        // the per-frame recompute after the sensor parse: an active pin says the
        // tool is on the carriage, not that it has filament at the nozzle.
        auto* slot = system_info_.units[0].get_slot(i);
        if (slot && extruder->park_pin.value_or(false) && slot->status != SlotStatus::AVAILABLE) {
            slot->status = SlotStatus::AVAILABLE;
            fx.changed = true;
        }

        extruder_states_[i].apply(*extruder);
    }
}

bool AmsBackendSnapmaker::tool_change_in_flight_locked() const {
    const bool pending = pending_tool_ >= 0 && std::chrono::steady_clock::now() < pending_until_;
    return pending || system_info_.action == AmsAction::SELECTING ||
           std::any_of(extruder_states_.begin(), extruder_states_.end(),
                       [](const auto& e) { return e.activating_move; });
}

void AmsBackendSnapmaker::apply_active_tool_locked(const snapmaker::StatusDelta& delta,
                                                   FrameEffects& fx) {
    // Detect active tool from extruder pin state and toolhead.extruder. Only
    // update when we have actual evidence: incremental status updates may omit
    // extruder/toolhead keys, so preserve the current value when no relevant
    // data is present (prevents oscillation between valid and -1).
    bool has_extruder_data = false;
    int active = -1;
    for (int i = 0; i < kNumTools; i++) {
        if (extruder_states_[i].active_pin ||
            (!extruder_states_[i].state.empty() && extruder_states_[i].state != "PARKED")) {
            active = i;
            has_extruder_data = true;
            break;
        }
    }
    if (delta.toolhead_extruder) {
        // "extruder" = 0, "extruder1" = 1, etc. An unparseable name leaves
        // whatever the per-extruder state decided above. toolhead.extruder names
        // the last head used, not a mounted one: a head reporting PARKED is on
        // its dock, so the per-extruder answer stands, -1 when every head is
        // parked (prestonbrown/helixscreen#1784).
        if (const auto tool_number = helix::tool_number_for_extruder(*delta.toolhead_extruder)) {
            const bool named_head_parked = *tool_number >= 0 && *tool_number < kNumTools &&
                                           extruder_states_[*tool_number].state == "PARKED";
            if (!named_head_parked) {
                active = *tool_number;
            }
        }
        has_extruder_data = true;
    } else {
        // Parking leaves toolhead.extruder as it was, so a park arrives as
        // deltas carrying only the head's own fields: its state, and its active
        // pin, which can drop a frame later with no state beside it. The mounted
        // head reporting PARKED with no active pin is evidence on its own.
        const bool frame_has_head_data = std::any_of(delta.extruders.begin(), delta.extruders.end(),
                                                     [](const auto& e) { return e.has_value(); });
        const int current = system_info_.current_tool;
        if (frame_has_head_data && current >= 0 && current < kNumTools &&
            extruder_states_[current].state == "PARKED" && !extruder_states_[current].active_pin) {
            has_extruder_data = true;
        }
    }
    if (active >= 0 && active == pending_tool_) {
        pending_tool_ = -1; // the requested head arrived
    }
    if (active < 0 && has_extruder_data && tool_change_in_flight_locked()) {
        // The old head docks before the new one reports, so every head reads
        // PARKED for a frame mid-change. That is not an empty carriage.
        has_extruder_data = false;
    }
    if (has_extruder_data && active != system_info_.current_tool) {
        // Demote previous active tool from LOADED to AVAILABLE
        if (system_info_.current_tool >= 0 && system_info_.current_tool < kNumTools) {
            auto* prev_slot = system_info_.units[0].get_slot(system_info_.current_tool);
            if (prev_slot && prev_slot->status == SlotStatus::LOADED) {
                prev_slot->status = SlotStatus::AVAILABLE;
            }
        }
        // A pick alone says nothing about filament; the active tool's LOADED
        // status and filament_loaded are derived after the sensor parse, from the
        // latch and the toolhead switch.
        system_info_.current_tool = active;
        system_info_.current_slot = active; // 1:1 tool-to-slot on Snapmaker
        fx.changed = true;
    }
}

void AmsBackendSnapmaker::apply_filament_detect_locked(const snapmaker::StatusDelta& delta,
                                                       FrameEffects& fx) {
    if (!delta.filament_detect) {
        return;
    }
    const auto& detect = *delta.filament_detect;

    // RFID info per channel. Only apply it when it carries real values (not
    // "NONE"): print_task_config is the authoritative source, and RFID
    // supplements it when tags are present.
    for (int i = 0; i < kNumTools; i++) {
        if (detect.info[i]) {
            apply_rfid_entry_locked(i, *detect.info[i], fx);
        }
    }

    // Filament state per channel: [int, int, int, int], the entrance/tag reader
    // per channel. It reads 0 once filament has been fed THROUGH it to the
    // toolhead, so a 0 is not "no filament": lane presence is declared at the
    // parse convergence point from the port sensor and the loaded-at-toolhead
    // latch, and this array only seeds a status for slots nothing better has
    // spoken for.
    for (int i = 0; i < kNumTools; i++) {
        if (!detect.state[i]) {
            continue;
        }
        auto* slot = system_info_.units[0].get_slot(i);
        if (slot) {
            // Only set from filament_detect if extruder state hasn't already
            // provided a more authoritative status (LOADED/AVAILABLE via
            // park_pin/active_pin)
            if (slot->status == SlotStatus::UNKNOWN) {
                slot->status = (*detect.state[i] != 0) ? SlotStatus::AVAILABLE : SlotStatus::EMPTY;
            }
            fx.changed = true;
        }
    }
}

void AmsBackendSnapmaker::apply_rfid_entry_locked(int i, const SnapmakerRfidInfo& rfid,
                                                  FrameEffects& fx) {
    // Capture what this reading physically got off the spool,
    // before any early exit, so the insert rule sees it
    // regardless of whether the rest of the RFID fields apply.
    // A read is finished only when it named a tag: a UID names
    // it, and a decoded MAIN_TYPE with no UID leaves the UID
    // the one part still outstanding (material and colour
    // stand as evidence without it). A NONE entry with no UID
    // is three indistinguishable states - reader disabled,
    // untagged spool, empty channel - so it files no evidence
    // at all and the fingerprint comes out empty (no signal).
    tag_names_identity_[static_cast<size_t>(i)] =
        rfid.main_type != "NONE" || (!rfid.sub_type.empty() && rfid.sub_type != "NONE");
    tag_has_card_[static_cast<size_t>(i)] = rfid.has_card;
    if (rfid.has_card.value_or(false)) {
        card_seen_[static_cast<size_t>(i)] = true;
    }
    helix::ams::SpoolEvidence& evidence = fx.observed_evidence[i];
    evidence.tag_uid = rfid.uid;
    if (!rfid.uid.empty()) {
        evidence.tag_read_complete = true;
    }
    // A pending insert is judged HERE: this entry is the
    // reader's answer for the spool that just went in, which
    // the port edge could not know. A UID or a decoded
    // MAIN_TYPE verifies it - the tail's
    // check_hardware_event_clear judges any swap from this
    // very reading - and an entry that files nothing is the
    // reader saying no tag is behind the insert, so the stored
    // record could describe a spool that left (#1710).
    if (pending_insert_passes_[i] > 0) {
        pending_insert_passes_[i] = 0;
        if (rfid.uid.empty() && rfid.main_type == "NONE") {
            insert_verdict_[i] = InsertIdentity::NoTag;
            fx.unverified_insert_lanes.push_back(i);
        } else {
            insert_verdict_[i] = InsertIdentity::FromTag;
        }
    }
    if (rfid.main_type != "NONE") {
        evidence.material = rfid.main_type;
        if (helix::ams::is_declarable_color(rfid.color_rgb)) {
            evidence.color_rgb = rfid.color_rgb;
        }
    }

    // The vendor cache mirrors the firmware's info, so an entry naming no tag
    // leaves nothing cached: the record of an earlier tag would otherwise keep
    // describing a spool the reader no longer reports.
    if (rfid.main_type == "NONE") {
        auto* slot = system_info_.units[0].get_slot(i);
        if (slot && !*tag_names_identity_[static_cast<size_t>(i)] &&
            retract_tag_reading_locked(i, *slot)) {
            fx.changed = true;
        }
        return;
    }
    // A filled entry whose CARD_UID names no card was written over the API (the
    // Extended Firmware's filament_detect/set), not read off a spool, so it is
    // no vendor reading: what an earlier tag left cached goes with it. An entry
    // with no readable CARD_UID says nothing about a card either way.
    if (rfid.has_card == std::optional<bool>(false)) {
        auto* slot = system_info_.units[0].get_slot(i);
        if (slot && retract_tag_reading_locked(i, *slot)) {
            fx.changed = true;
        }
        return;
    }

    auto* slot = system_info_.units[0].get_slot(i);
    if (slot) {
        slot->material = rfid.main_type;
        auto brand = !rfid.manufacturer.empty() ? rfid.manufacturer : rfid.vendor;
        if (brand != "NONE")
            slot->brand = brand;
        slot->color_rgb = rfid.color_rgb;
        slot->nozzle_temp_min = rfid.hotend_min_temp;
        slot->nozzle_temp_max = rfid.hotend_max_temp;
        slot->bed_temp = rfid.bed_temp;
        slot->total_weight_g = static_cast<float>(rfid.weight_g);
        // A tag read is a cache of what a vendor printed, not a
        // sensor of identity: it survives the spool leaving the
        // channel, so it never carries presence.
        //
        // Every value here is this parse's own, never slot->*.
        // SlotInfo persists across frames and
        // apply_resolved_lane rewrites it in place at the tail
        // of every one, so
        // reading the struct back would file a user's edit as
        // something the tag says.
        //
        helix::ams::Observation cache(helix::ams::ObservationSource::VendorCache);
        if (!rfid.main_type.empty())
            cache.material = rfid.main_type;
        // Both spellings of "the tag named no vendor" retract
        // the brand here: whole-record replacement means this
        // record states what THIS read said, and a field the
        // read is silent about is not one it still stands
        // behind. SlotInfo above tests only the literal "NONE",
        // so it blanks on an absent key and KEEPS its last
        // value on the literal. That one input is the only
        // place the two layers disagree.
        if (!brand.empty() && brand != "NONE")
            cache.brand = brand;
        // SnapmakerRfidInfo::color_rgb rests on
        // kAmsDefaultSlotColor when the tag carried no
        // ARGB_COLOR, which is that struct's "no reading" and
        // not a grey anybody chose.
        if (helix::ams::is_declarable_color(rfid.color_rgb))
            cache.color_rgb = rfid.color_rgb;
        // SUB_TYPE names the product line inside MAIN_TYPE
        // ("Silk" inside "PLA"), so it is the branded product
        // and routing it to material would destroy the
        // material.
        if (!rfid.sub_type.empty() && rfid.sub_type != "NONE")
            cache.product_name = rfid.sub_type;
        if (rfid.weight_g > 0)
            cache.total_weight_g = static_cast<float>(rfid.weight_g);
        tag_slot_values_[static_cast<size_t>(i)] = TagSlotValues{
            slot->nozzle_temp_min, slot->nozzle_temp_max, slot->bed_temp, slot->total_weight_g};
        record_tag_reading_locked(i, rfid, cache, *slot);
        // SUB_TYPE is Snapmaker's product-line name ("SnapSpeed" for
        // their PLA line, akin to Polymaker's "PolyLite"), shown as
        // the spool name; the tag carries no colour name. Blank when
        // the tag names no line or names it beside a brand that cannot
        // carry it: a kept name would outlive the spool it described.
        slot->spool_name = tag_spool_name(cache);
        helix::ams::ingest(lane_id(i), cache);
    }
    fx.changed = true;
}

void AmsBackendSnapmaker::apply_feed_channels_locked(const snapmaker::StatusDelta& delta,
                                                     FrameEffects& fx) {
    for (const auto& channel : delta.feed_channels) {
        apply_feed_channel_locked(channel, fx);
    }
}

void AmsBackendSnapmaker::apply_feed_channel_locked(const snapmaker::FeedChannelDelta& channel,
                                                    FrameEffects& fx) {
    const int i = channel.lane;
    if (channel.filament_detected) {
        apply_port_presence_locked(i, *channel.filament_detected, fx);
    }

    // channel_state is the single authoritative signal for load state and
    // operation progress. classify_channel_state maps every firmware state (39
    // total) to {action, phase, terminal, fail, latch set/clear}; the parse
    // reads off that one table rather than scattered string compares. The
    // defaults are the intended "nothing to report" sentinels: "" is checked by
    // the !state.empty() gate, "ok" by classify_channel_state.
    const std::string state = channel.channel_state.value_or("");
    const std::string error = channel.channel_error.value_or("ok");

    // Keep the raw fields for eligibility queries; the latch below collapses
    // them to a single bit. Status frames are deltas: start from the previous
    // snapshot and overwrite only the keys this frame carries, so a
    // channel_state-only frame leaves filament_detected/module_exist standing
    // and a filament_detected-only frame does not blank state (the feeder frame
    // never carries the motion sensor's enabled flag, so that field rides the
    // same rule).
    ChannelSnapshot snap = channel_snapshots_[static_cast<size_t>(i)];
    const bool was_load_confirmed = feeder_reports_loaded(snap);
    const std::string prev_state = snap.state;
    const std::string prev_action_state = snap.action_state;
    if (channel.channel_state) {
        snap.state = *channel.channel_state;
    }
    if (channel.channel_action_state) {
        snap.action_state = *channel.channel_action_state;
    }
    if (channel.channel_error) {
        snap.error = *channel.channel_error;
    }
    if (channel.filament_detected) {
        snap.filament_detected = *channel.filament_detected;
    }
    if (channel.module_exist) {
        snap.module_exist = *channel.module_exist;
    }
    if (channel.disable_auto) {
        snap.disable_auto = *channel.disable_auto;
    }
    // The op lifecycle, error surfacing and batch
    // verification read the op's outcome; the latch and the
    // step bar read where the filament is. They differ only
    // when the outcome survives in channel_action_state alone.
    helix::snapmaker::OpOutcome resolved = helix::snapmaker::resolve_op_outcome(
        snap.state, prev_action_state, snap.action_state, snap.pending_outcome);
    snap.pending_outcome = std::move(resolved.held);
    const std::optional<std::string> settled_outcome = std::move(resolved.outcome);
    if (feeder_reports_loaded(snap) && !was_load_confirmed && load_unconfirmed_[i]) {
        spdlog::info("{} tool {} reports '{}' with no error; Resume may proceed", backend_log_tag(),
                     i, snap.state);
        load_unconfirmed_[i] = false;
    }
    channel_snapshots_[static_cast<size_t>(i)] = std::move(snap);
    // A terminal ends the op or marks the lane only when this
    // frame moved the channel to it; see observed_change.
    const bool outcome_is_new =
        settled_outcome.has_value() || helix::snapmaker::observed_change(prev_state, state);

    const ChannelStateInfo info = classify_channel_state(state);
    std::string op_state = settled_outcome ? *settled_outcome : state;
    ChannelStateInfo op_info = settled_outcome ? classify_channel_state(op_state) : info;
    if (!settled_outcome && state == "preload_finish") {
        if (preload_in_flight_[i]) {
            op_info.is_terminal = true;
        } else if (unload_in_flight_[i] && preload_finish_ends_unload_locked(i)) {
            // The unload's outcome, for the op lifecycle, the unloaded-lane
            // grace and the batch cursor alike.
            op_state = "unload_finish";
            op_info = classify_channel_state(op_state);
        }
    }
    if (helix::snapmaker::channel_state_in_progress(state)) {
        preload_in_flight_[i] = state.rfind("preload_", 0) == 0;
        unload_in_flight_[i] = state.rfind("unload_", 0) == 0;
    } else if (!state.empty()) {
        preload_in_flight_[i] = false;
        unload_in_flight_[i] = false;
    }

    // A frame carries a channel's state or error only when it changed, or
    // whole when a subscription starts, which rebuilds the marks after a
    // reconnect.
    if ((channel.channel_state || channel.channel_error) &&
        feed_failure_reported(channel_snapshots_[static_cast<size_t>(i)])) {
        mark_load_unconfirmed_locked(i, op_state);
    }
    // A frame settles the refeed only when it is decisive: loaded with no
    // error, or a failure. load_finish can land a frame before channel_error
    // turns ok; that frame waits for the next one, within the timeout.
    if (refeed_wait_ && refeed_wait_->slot == i &&
        (channel.channel_state || channel.channel_error)) {
        const ChannelSnapshot& now = channel_snapshots_[static_cast<size_t>(i)];
        if (feeder_reports_loaded(now) || feed_failure_reported(now)) {
            fx.refeed_settled = true;
        }
    }

    apply_channel_progress_locked(i, state, info, fx);
    apply_channel_outcome_locked(i, op_state, op_info, error, outcome_is_new, fx);
    advance_batch_locked(i, op_state, op_info, fx);

    // Trace the firmware channel_state sequence during a load/unload: the
    // true physical completion is a different event from an intermediate
    // one (preload_finish is staged-in-buffer), and the order is
    // firmware-specific.
    if (!state.empty() || settled_outcome) {
        spdlog::debug("[AmsBackendSnapmaker] tool {} channel_state='{}' "
                      "op_state='{}' error='{}' -> action={} current_slot={}",
                      i, state, op_state, error, ams_action_to_string(system_info_.action),
                      system_info_.current_slot);
    }
}

void AmsBackendSnapmaker::apply_port_presence_locked(int i, bool detected, FrameEffects& fx) {
    // The port flag mirrors into port_sensor_filament_present_ so
    // is_stuck_motion_sensor_runout can distinguish a real runout (both sensors
    // false) from a stale motion-sensor false positive (motion=false,
    // port=true).
    // Mirror into port_sensor_filament_present_ so
    // is_stuck_motion_sensor_runout can distinguish a real
    // runout (both sensors false) from a stale motion-sensor
    // false positive (motion=false, port=true). Tracked
    // independent of slot->status because slot status flips
    // to AVAILABLE/LOADED based on extruder pin state which
    // is orthogonal to the port sensor reading.
    if (i >= 0 && i < kNumTools) {
        // The port flag's false -> true edge is an
        // insert into the channel; the first sighting
        // is the baseline, not an edge. The tag
        // reader's answer lands a frame or two later,
        // so the edge holds a pending verdict for the
        // info loop to judge (#1710). A drop cancels
        // one: the spool left before any read.
        //
        // A feed the firmware itself drives - its own
        // tool-change unload/load, or one of our
        // batch ops - drops and raises this flag too,
        // and no spool changed hands, so it arms
        // nothing. The edge runs before this frame's
        // channel_state parse, so the gate reads the
        // channel's last reported state; the state
        // parse below cancels anything the ordering
        // missed.
        const bool firmware_driven = batch_.active || helix::snapmaker::channel_state_in_progress(
                                                          channel_snapshots_[i].state);
        if (detected && feed_presence_seen_[i] && !port_sensor_filament_present_[i] &&
            !firmware_driven && !head_is_upstream_fed_locked(i)) {
            pending_insert_passes_[i] = 1;
            insert_verdict_[i] = InsertIdentity::NotRead;
            insert_since_declaration_[i] = true;
        } else if (!detected) {
            pending_insert_passes_[i] = 0;
        }
        port_sensor_filament_present_[i] = detected;
        feed_presence_seen_[i] = true;
    }
    auto* slot = system_info_.units[0].get_slot(i);
    if (slot) {
        if (detected &&
            (slot->status == SlotStatus::EMPTY || slot->status == SlotStatus::UNKNOWN)) {
            slot->status = SlotStatus::AVAILABLE;
            fx.changed = true;
        } else if (!detected && slot->status != SlotStatus::LOADED) {
            slot->status = SlotStatus::EMPTY;
            fx.changed = true;
        }
    }
}

void AmsBackendSnapmaker::apply_channel_progress_locked(int i, const std::string& state,
                                                        const ChannelStateInfo& info,
                                                        FrameEffects& fx) {
    // A feed under way on this channel is the firmware
    // moving filament itself, so a presence edge that
    // armed a pending verdict this parse was not a user
    // insert; the spool never left.
    if (pending_insert_passes_[i] > 0 &&
        (info.action == AmsAction::LOADING || info.action == AmsAction::UNLOADING)) {
        pending_insert_passes_[i] = 0;
    }

    // A channel reporting any state at all makes the
    // lane's presence inputs live for the
    // convergence-point ingest below.
    if (!state.empty()) {
        feed_presence_seen_[i] = true;
    }

    // Mirror the granular firmware sub-phase into the system
    // info so the sidebar step bar can show the real
    // Home/Select/Heat/Move sequence. -1 for any non-active
    // state (idle, *_finish, *_fail, preload_finish). Updated
    // only when the firmware actually reports a channel_state,
    // so an incremental status omitting it doesn't clear the
    // phase spuriously.
    if (!state.empty()) {
        if (system_info_.operation_phase != info.phase) {
            system_info_.operation_phase = info.phase;
            fx.changed = true;
        }
    }

    // Capture the head whose channel is mid-op; the single
    // derivation of operation_working_slot below decides
    // what the header names from it (batch cursor wins).
    if (fx.in_progress_head < 0 &&
        (info.action == AmsAction::LOADING || info.action == AmsAction::UNLOADING)) {
        fx.in_progress_head = i;
    }

    // "Loaded at toolhead" latch (the core fix). Driven purely
    // from channel_state transitions, NOT the motion sensor
    // (which fails to clear after an unload on current firmware).
    // SET on load_finish; CLEAR on unload_finish / wait_insert /
    // preload_finish; KEEP on every transient / in-progress /
    // fail state. Mirrors the firmware's persisted
    // config['load_finish'].
    if (!state.empty() && !info.ignore) {
        if (info.sets_loaded && !loaded_at_toolhead_[i]) {
            loaded_at_toolhead_[i] = true;
            fx.changed = true;
        } else if (info.clears_loaded && loaded_at_toolhead_[i]) {
            loaded_at_toolhead_[i] = false;
            fx.changed = true;
        }
    }
}

void AmsBackendSnapmaker::apply_channel_outcome_locked(int i, const std::string& op_state,
                                                       const ChannelStateInfo& op_info,
                                                       const std::string& error,
                                                       bool outcome_is_new, FrameEffects& fx) {
    // Error surfacing: either a firmware channel_error token OR
    // a *_fail channel_state (Change 2). Preserve the multi-color
    // false-alarm guard — the firmware reports
    // channel_error="no_filament" for ANY empty lane, and briefly
    // a *_fail channel_state when it auto-feeds a lane deliberately
    // left unloaded for a multi-color print (heads 0+2 used, head 1
    // empty). Neither must latch the whole backend into
    // action=Error and pop a spurious modal on such an idle empty
    // non-active lane. An error is real when the lane holds
    // filament (lane not empty), is the active lane, or an
    // operation is genuinely underway on it (an in-progress
    // LOADING/UNLOADING state — a *_fail is terminal, so the same
    // empty-lane guard applies to it as to the no_filament token).
    const bool has_error_token = error != "ok" && !error.empty() && error != "none";
    if (has_error_token || op_info.is_fail) {
        const auto* slot = system_info_.units[0].get_slot(i);
        const bool lane_empty = slot == nullptr || !slot->is_present();
        const bool active_lane = system_info_.current_slot == i || system_info_.current_tool == i;
        const bool op_in_progress =
            op_info.action == AmsAction::LOADING || op_info.action == AmsAction::UNLOADING;
        if (lane_empty && !op_in_progress && !active_lane) {
            spdlog::debug("[AmsBackendSnapmaker] ignoring error (token='{}' state='{}') "
                          "on idle empty lane {} (not operating, not active)",
                          error, op_state, i);
        } else {
            system_info_.action = AmsAction::ERROR;
            system_info_.operation_detail =
                has_error_token ? friendly_channel_error(error, lane_noun(), i)
                                : friendly_channel_state_fail(op_state, lane_noun(), i);
            fx.changed = true;
        }
    } else if (!op_state.empty() && !op_info.ignore) {
        // No error — drive the action / operation lifecycle from
        // the classifier.
        if (op_info.action == AmsAction::LOADING) {
            if (system_info_.action != AmsAction::LOADING) {
                system_info_.action = AmsAction::LOADING;
                fx.changed = true;
            }
        } else if (op_info.action == AmsAction::UNLOADING) {
            if (system_info_.action != AmsAction::UNLOADING) {
                system_info_.action = AmsAction::UNLOADING;
                fx.changed = true;
            }
        } else if (op_info.is_terminal) {
            // A *_finish state resolves the operation. The
            // latch above carries its load meaning; the active
            // tool's LOADED status and filament_loaded are
            // derived from it after the sensor parse.
            //
            // current_slot / current_tool are NOT reset here:
            // they track which toolhead is picked up on the
            // carriage (toolhead.extruder is the authority, set
            // in the extruder-pin parse above), which is
            // independent of whether feeder filament is at the
            // nozzle. A tool fed without the feeders (TPU loaded
            // straight into the toolhead) stays picked up while
            // its channel reports unload_finish permanently, and
            // unload_active_filament() needs current_slot to
            // name that tool: with no slot it dispatches the
            // bare INNER_FILAMENT_UNLOAD, which the firmware
            // runs on T0.
            if (op_state == "unload_finish" && outcome_is_new) {
                // Deferred to after the lock, as emit_event is:
                // calls into AmsState stay outside our mutex,
                // because the backend registry takes its own
                // lock before ours (AmsBackendRegistry).
                fx.unloaded_lanes.push_back(i);
            }
            if (outcome_is_new && (system_info_.action == AmsAction::LOADING ||
                                   system_info_.action == AmsAction::UNLOADING)) {
                system_info_.action = AmsAction::IDLE;
                system_info_.operation_detail.clear();
                PostOpCooldownManager::instance().schedule();
                fx.changed = true;
            }
        }
        // A resting state (none / inited / wait_insert, and
        // preload_finish after anything but a preload) leaves
        // the action untouched: one also appears while the
        // nozzle heats for an unload, and the op's own
        // outcome, when a resting state hides it, arrives as
        // settled_outcome above. The latch already handled
        // the resting states' clears.
    }
}

void AmsBackendSnapmaker::advance_batch_locked(int i, const std::string& op_state,
                                               const ChannelStateInfo& op_info, FrameEffects& fx) {
    // Batch verification. Only the plan's cursor head can
    // advance the cursor, so a sibling channel repeating its
    // settled state in this frame is inert. The direction's
    // own terminal is matched exactly: manual_sta_finish ends
    // a single-op lifecycle but a load
    // batch counts a head only at load_finish (unload at
    // unload_finish). A *_fail on the cursor head stops the
    // batch where it stands; the error branch above has
    // already set operation_detail to the failure message,
    // which must win over a progress line. Runs after the
    // terminal resolution so its operation_detail.clear()
    // cannot wipe the progress string this writes.
    if (batch_.active && i == batch_.heads[batch_.cursor]) {
        if (op_info.is_fail) {
            batch_.active = false;
            fx.batch_failed_head = i;
            fx.batch_failed_state = op_state;
        } else if (op_state == (batch_.load ? "load_finish" : "unload_finish")) {
            ++batch_.cursor;
            batch_.active = batch_.cursor < batch_.heads.size();
            if (batch_.active) {
                // "Load 2 of 4" — the head now in progress.
                // The words arrive pretranslated from dispatch, so
                // this parse makes no LVGL calls.
                system_info_.operation_detail =
                    fmt::format("{} {} {} {}", batch_.direction_label, batch_.cursor + 1,
                                batch_.of_label, batch_.heads.size());
            } else {
                // Every head verified. Nothing is in
                // progress, and no later frame clears the
                // line once the action is IDLE.
                system_info_.operation_detail.clear();
            }
            fx.changed = true;
        }
    }
}

void AmsBackendSnapmaker::apply_batch_state_locked(const snapmaker::StatusDelta& delta,
                                                   FrameEffects& fx) {
    // The batch macro's `doing` save-variable is the firmware's own word on
    // whether a batch script is running. A false reading retires any plan this
    // process still holds active: the script ended without the cursor head
    // reaching a terminal or a *_fail (lost response, script abort, a feeder
    // wedging mid-feed), and no channel_state detector covers that end.
    if (delta.batch_doing && !*delta.batch_doing && batch_.active) {
        batch_.active = false;
        fx.batch_retired = true;
        fx.changed = true;
        spdlog::info("{} batch macro reports doing=false — retiring the active plan",
                     backend_log_tag());
    }
}

void AmsBackendSnapmaker::apply_working_slot_locked(FrameEffects& fx) {
    // ONE derivation of "the head an operation is working on": the batch cursor
    // while a plan is active, else the head whose channel reported an
    // in-progress state, else none. A toolhead-only delta carries no channel
    // evidence, so mid-op it keeps the previous answer instead of flapping the
    // header back to the carriage tool; a batch the firmware just reported
    // ended carries nothing forward. current_slot is NOT touched here: it stays
    // the carriage answer its other consumers (bypass unload, filament panel
    // gating, the loaded card) read.
    int working_slot = -1;
    if (batch_.active) {
        working_slot = batch_.heads[batch_.cursor];
    } else if (system_info_.action == AmsAction::LOADING ||
               system_info_.action == AmsAction::UNLOADING) {
        if (fx.in_progress_head >= 0) {
            working_slot = fx.in_progress_head;
        } else if (!fx.batch_retired) {
            working_slot = system_info_.operation_working_slot;
        }
    }
    if (system_info_.operation_working_slot != working_slot) {
        system_info_.operation_working_slot = working_slot;
        fx.changed = true;
    }
}

void AmsBackendSnapmaker::apply_print_task_config_locked(const snapmaker::StatusDelta& delta,
                                                         FrameEffects& fx) {
    // print_task_config is the authoritative filament record from Snapmaker's
    // task manager: per-extruder type, vendor, color and presence, plus the
    // routing of the running print.
    if (!delta.print_task_config) {
        return;
    }
    const auto& ptc = *delta.print_task_config;

    // Firmware-stored preferences are held as told, never filed as lane
    // observations: they are a write surface like the filament_type/vendor/color
    // fields below.
    const auto replenish_before = print_preferences_.auto_replenish;
    print_preferences_.merge(ptc.preferences);
    // Auto-replenish is the Endless Spool switch, which AmsState republishes
    // only on a change event.
    if (print_preferences_.auto_replenish != replenish_before) {
        fx.changed = true;
    }

    apply_task_routing_locked(ptc, fx);
    apply_task_slot_identity_locked(ptc, fx);
}

void AmsBackendSnapmaker::apply_task_routing_locked(const snapmaker::PrintTaskConfigDelta& ptc,
                                                    FrameEffects& fx) {
    // extruder_map_table: [int x32], logical tool -> physical head. The
    // firmware's own routing authority for the running print (see the member's
    // doc comment). Mirrored verbatim; interpretation belongs to
    // get_tool_mapping()'s callers, not here.
    if (ptc.extruder_map_table && *ptc.extruder_map_table != extruder_map_table_) {
        spdlog::debug("[AMS Snapmaker] extruder_map_table changed ({} entries)",
                      ptc.extruder_map_table->size());
        extruder_map_table_ = *ptc.extruder_map_table;
        fx.changed = true;
    }

    // extruders_used: [bool x4], the heads this task uses. Gates whether the map
    // above may be read at all (see the member's doc comment).
    if (ptc.extruders_used && *ptc.extruders_used != extruders_used_) {
        extruders_used_ = *ptc.extruders_used;
        fx.changed = true;
    }

    // Snapshot the routing while the task is still configured. Both fields are
    // members, so this is evaluated against the accumulated state rather than
    // only what THIS frame carried: an incremental update that names one of
    // them still lands on the right answer.
    //
    // This is the only moment the routing is knowable. Once the print ends the
    // firmware clears extruders_used and resets the table, and a reprint has
    // nothing left to read: no detail view, no picker, no colour match to
    // recompute. An empty table is never snapshotted: "known: nothing" is
    // indistinguishable from a real answer to the caller, and the honest value
    // is "not known".
    const bool task_configured_now =
        std::any_of(extruders_used_.begin(), extruders_used_.end(), [](bool b) { return b; });
    if (task_configured_now && !extruder_map_table_.empty() &&
        last_task_extruder_map_ != extruder_map_table_) {
        last_task_extruder_map_ = extruder_map_table_;
        spdlog::debug("[AMS Snapmaker] recorded task routing ({} entries) for reprint",
                      last_task_extruder_map_.size());
    }
}

void AmsBackendSnapmaker::apply_task_slot_identity_locked(
    const snapmaker::PrintTaskConfigDelta& ptc, FrameEffects& fx) {
    // filament_exist: whether filament is loaded per slot
    for (int i = 0; i < kNumTools; i++) {
        if (!ptc.filament_exist[i]) {
            continue;
        }
        const bool exists = *ptc.filament_exist[i];
        auto* slot = system_info_.units[0].get_slot(i);
        if (slot) {
            if (exists && slot->status != SlotStatus::LOADED) {
                slot->status = SlotStatus::AVAILABLE;
            } else if (!exists) {
                slot->status = SlotStatus::EMPTY;
            }
            fx.changed = true;
        }
    }

    // These three fields write SlotInfo and deliberately file NO lane
    // observation, unlike the RFID parse.
    //
    // print_task_config is a write surface, not a sensor.
    // SET_PRINT_FILAMENT_CONFIG takes VENDOR / FILAMENT_TYPE / FILAMENT_SUBTYPE /
    // FILAMENT_COLOR_RGBA as gcode parameters and persists them, so whoever sent
    // that command set these values: the machine's own screen, a slicer, a
    // console, or this backend's own write-back. Filing any of it as
    // VendorCache would return a user's own edit as firmware truth.
    //
    // The firmware carries the provenance bit itself, and it shows the channel is
    // redundant rather than merely unsafe: filament_official marks a head whose
    // entry came from a Snapmaker RFID spool, and SET_PRINT_FILAMENT_CONFIG is
    // refused on such a head without FORCE. An official entry is the tag
    // filament_detect.info already reports, which the RFID parse files; an
    // unofficial one is somebody's declaration. Neither is a reading this key can
    // contribute.
    //
    // A user's declaration reaches the lane model through commit_slot_edit, which
    // is the funnel that records authorship.
    for (int i = 0; i < kNumTools; i++) {
        auto* slot = system_info_.units[0].get_slot(i);
        if (!slot) {
            continue;
        }
        if (ptc.filament_type[i]) {
            task_type_[static_cast<size_t>(i)] = *ptc.filament_type[i];
            // Base type only (e.g., "PLA") for compact display; firmware's
            // spelling of no type is no material.
            slot->material = task_field_value(ptc.filament_type[i]);
            // Flow calibration and RESUME both stop on a head whose record
            // names no type (`not edit filament info`).
            slot->material_required = is_unset_task_field(*ptc.filament_type[i]);
            fx.changed = true;
        }
        if (ptc.filament_vendor[i]) {
            task_vendor_[static_cast<size_t>(i)] = *ptc.filament_vendor[i];
            slot->brand = task_field_value(ptc.filament_vendor[i]);
            fx.changed = true;
        }
        if (ptc.filament_sub_type[i]) {
            task_subtype_[static_cast<size_t>(i)] = *ptc.filament_sub_type[i];
        }
        if (ptc.filament_official[i]) {
            task_official_[static_cast<size_t>(i)] = *ptc.filament_official[i];
        }
        if (ptc.filament_color_rgba[i]) {
            if (const auto rgb = snapmaker::rgb_from_rgba_hex(*ptc.filament_color_rgba[i])) {
                task_color_[static_cast<size_t>(i)] = *rgb;
                slot->color_rgb = *rgb;
            }
            fx.changed = true;
        }
    }
}

void AmsBackendSnapmaker::apply_toolhead_sensors_locked(const snapmaker::StatusDelta& delta,
                                                        FrameEffects& fx) {
    // The U1's [filament_motion_sensor e{N}_filament] is a motion runout during
    // a print (filament_detected:false when extrusion outruns the encoder, then
    // PAUSE via pause_on_runout) and a presence switch otherwise (it copies the
    // toolhead pin). Mirror the flag: the path canvas breaks the spool->toolhead
    // line on runout, and outside a print it answers whether filament sits in
    // the toolhead.
    for (const auto& sensor : delta.toolhead_sensors) {
        const int tool_idx = sensor.tool;

        // `enabled` rides the same status objects and gates loading: a sensor
        // the firmware has disabled cannot confirm feed.
        if (sensor.enabled) {
            channel_snapshots_[static_cast<size_t>(tool_idx)].sensor_enabled = *sensor.enabled;
        }
        if (!sensor.filament_detected) {
            continue;
        }
        const bool present = *sensor.filament_detected;
        if (!toolhead_switch_reported_[tool_idx]) {
            toolhead_switch_reported_[tool_idx] = true;
            fx.changed = true;
            // The unit has a toolhead sensor once any head reports its switch.
            system_info_.units[0].has_toolhead_sensor = true;
        }
        if (sensor_filament_present_[tool_idx] != present) {
            sensor_filament_present_[tool_idx] = present;
            fx.changed = true;
            spdlog::info("{} Tool {} filament sensor: {} ({})", backend_log_tag(), tool_idx,
                         present ? "PRESENT" : "RUNOUT", sensor.object);
        }
    }
}

void AmsBackendSnapmaker::derive_active_tool_loaded_locked(FrameEffects& fx) {
    // The active tool's loaded answers, derived from held state on every
    // frame so they cannot depend on which fields this frame carried: a
    // pick and the channel_state it pairs with often arrive in different
    // frames. filament_loaded ("filament in the toolhead") follows a
    // reported toolhead switch, which also breaks the canvas line on a
    // mid-print runout, and falls back to the latch. LOADED status (loaded
    // to the nozzle, the Load gate) needs the latch and no runout.
    if (system_info_.current_tool >= 0 && system_info_.current_tool < kNumTools) {
        const int t = system_info_.current_tool;
        const bool switch_known = toolhead_switch_reported_[t];
        const bool in_toolhead =
            switch_known ? sensor_filament_present_[t] : loaded_at_toolhead_[t];
        if (system_info_.filament_loaded != in_toolhead) {
            system_info_.filament_loaded = in_toolhead;
            fx.changed = true;
        }
        const bool at_nozzle =
            loaded_at_toolhead_[t] && (!switch_known || sensor_filament_present_[t]);
        auto* slot = system_info_.units[0].get_slot(t);
        if (slot && at_nozzle && slot->status == SlotStatus::AVAILABLE) {
            slot->status = SlotStatus::LOADED;
            fx.changed = true;
        } else if (slot && !at_nozzle && slot->status == SlotStatus::LOADED) {
            slot->status = SlotStatus::AVAILABLE;
            fx.changed = true;
        }
    } else if (system_info_.filament_loaded) {
        system_info_.filament_loaded = false;
        fx.changed = true;
    }
}

void AmsBackendSnapmaker::demote_runout_slots_locked(FrameEffects& fx) {
    // Per-slot runout demotion: any slot whose motion sensor reports
    // no filament should be AVAILABLE (spool present, ready to feed), not
    // LOADED. Without this, the AMS context menu's Load button is gated off
    // (pending_is_loaded_ from slot.status==LOADED disables it) and the user
    // has no way to re-feed filament from the UI after a runout — they get
    // Unload/Reset on a slot that has no filament between feeder and nozzle.
    // EMPTY is wrong here because the slot's RFID/print_task_config still
    // reports a spool present; AVAILABLE accurately captures "spool yes,
    // filament-at-toolhead no".
    for (int i = 0; i < kNumTools; ++i) {
        if (sensor_filament_present_[i])
            continue;
        auto* slot = system_info_.units[0].get_slot(i);
        if (slot && slot->status == SlotStatus::LOADED) {
            slot->status = SlotStatus::AVAILABLE;
            fx.changed = true;
        }
    }
}

void AmsBackendSnapmaker::converge_lanes_locked(FrameEffects& fx) {
    // Parse convergence point. After every firmware-sourced field on the
    // SlotInfo has been populated above, loop through slots and lay each
    // lane's resolved values on top. check_hardware_event_clear must run
    // FIRST so it sees firmware-truth fields (not the resolved view) and
    // can clear a stale override when a physical spool swap is detected.
    // apply_resolved_lane runs after, so the final SlotInfo the UI reads
    // through get_slot_info / the emitted event reflects what the lane
    // resolves to.
    //
    // Snapmaker has multiple parse paths feeding the same slot (RFID info,
    // print_task_config, filament_feed). Rather than hook the override logic
    // into each one, we run it once here at the tail — the tradeoff is that
    // get_slot_info during a partial parse would observe uncleared overrides,
    // but since everything runs under mutex_ and handle_status is the
    // only writer, there's no observable window.
    for (int i = 0; i < kNumTools; ++i) {
        auto* slot = system_info_.units[0].get_slot(i);
        if (!slot)
            continue;

        // A pending insert ages one pass per parse. A read that never
        // lands (reader disabled, the channel's entry never came) must
        // not hold its verdict forever: past the bound, ask (#1710).
        if (pending_insert_passes_[i] > 0 &&
            ++pending_insert_passes_[i] > kSnapPendingInsertPasses) {
            pending_insert_passes_[i] = 0;
            insert_verdict_[i] = InsertIdentity::NoTag;
            fx.unverified_insert_lanes.push_back(i);
        }

        // A channel this parse carried no filament_detect.info for keeps
        // its default evidence, which the insert rule reads as no signal -
        // so the call is unconditional rather than gated on which keys the
        // notification happened to carry.
        check_hardware_event_clear(*slot, i, fx.observed_evidence[i]);
        // Mirror firmware-truth color/material into lane_data so OrcaSlicer's
        // MoonrakerPrinterAgent sees the spool. OverwriteAlways policy: user
        // edits via apply_user_edit round-trip through firmware via
        // SET_PRINT_FILAMENT_CONFIG, so firmware-truth and user-truth
        // converge, and overwriting lane_data is safe and also catches
        // external edits (CHANGE_ZCOLOR from a print, manual gcode, the stock
        // screen, OrcaSlicer). A head holding an RFID tag's record is not
        // written, and the user's override is persisted to lane_data
        // separately. See mirror_firmware_to_lane_data docs and AD5X IFS for
        // the same pattern.
        //
        // The stored override defers to what a declaring lane source holds,
        // so the store reads the lane here. A Spoolman record or a user's
        // value never reaches firmware, and firmware's reading must not
        // overwrite it in the override or in the lane_data record it
        // persists.
        helix::ams::mirror_firmware_to_lane_data(
            override_store_.get(), overrides_, i, slot->color_rgb, slot->material,
            slot->status == SlotStatus::AVAILABLE, helix::ams::MirrorPolicy::OverwriteAlways,
            backend_log_tag(), helix::ams::declared_on_lane(lane_id(i)));

        // Lane presence: the port/buffer sensor OR the loaded-at-toolhead
        // latch. The port sensor is the spool-side reading; the latch
        // carries filament fed through to the nozzle — the point at which
        // the entrance/tag reader (filament_detect.state) drops to 0, so
        // that array is not a presence source. Declared here, at the
        // tail of the parse, so both member arrays already hold this
        // frame's values when the lane resolves below; the arrays persist
        // across delta frames, so a frame silent on both signals leaves
        // the last reading standing. A lane filament_feed has never
        // reported stays silent too: the array defaults are "no reading
        // yet", not "no filament".
        if (feed_presence_seen_[i]) {
            helix::ams::Observation sensed(helix::ams::ObservationSource::Sensed);
            sensed.present = port_sensor_filament_present_[i] || loaded_at_toolhead_[i];
            helix::ams::ingest(lane_id(i), sensed);
        }

        retract_unbacked_identity_locked(i, *slot);
        apply_resolved_lane(*slot, i);
    }
}

void AmsBackendSnapmaker::record_tag_reading_locked(int slot_index, const SnapmakerRfidInfo& rfid,
                                                    const helix::ams::Observation& cache,
                                                    SlotInfo& slot) {
    const auto i = static_cast<size_t>(slot_index);
    std::optional<helix::ams::TagReading>& kept = tag_readings_[i];
    const std::string previous_multi = kept ? kept->multi_color_hexes : std::string{};
    {
        helix::ams::TagReading reading = helix::ams::tag_reading_from(cache);
        // Zero is the firmware's spelling of a temperature the tag left out.
        if (rfid.hotend_min_temp > 0) {
            reading.nozzle_temp_min = rfid.hotend_min_temp;
        }
        if (rfid.hotend_max_temp > 0) {
            reading.nozzle_temp_max = rfid.hotend_max_temp;
        }
        if (rfid.bed_temp > 0) {
            reading.bed_temp = rfid.bed_temp;
        }
        reading.multi_color_hexes = rfid.multi_color_hexes;
        kept = std::move(reading);
    }

    // No lane record carries a multi-colour, so the tag's is laid onto the slot
    // here, and only while nobody declares the lane's colour: a declared colour
    // replaced the tag's and must survive this frame.
    if (helix::ams::declared_on_lane(lane_id(slot_index)).color) {
        return;
    }
    const std::string& now = kept ? kept->multi_color_hexes : std::string{};
    if (!now.empty()) {
        slot.multi_color_hexes = now;
    } else if (!previous_multi.empty() && slot.multi_color_hexes == previous_multi) {
        slot.multi_color_hexes.clear();
    }
}

std::optional<helix::ams::TagReading> AmsBackendSnapmaker::tag_reading(int slot_index) const {
    if (slot_index < 0 || slot_index >= kNumTools) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return tag_readings_[static_cast<size_t>(slot_index)];
}

bool AmsBackendSnapmaker::retract_tag_reading_locked(int slot_index, SlotInfo& slot) {
    const helix::ams::LaneId lane = lane_id(slot_index);
    const std::optional<helix::ams::Observation> cache =
        helix::ams::lane_sources(lane).vendor_cache;
    if (!cache.has_value()) {
        return false;
    }
    helix::ams::drop_lane_source(lane, helix::ams::ObservationSource::VendorCache);

    // A SlotInfo field still holding the dropped record's value got it from
    // the tag; print_task_config is the firmware's other word on the head, so
    // it takes the field back, and a head it never spoke for goes blank. A
    // field that moved since was written by something else and stays.
    // A declaring source lays its values back over these at the tail.
    const auto i = static_cast<size_t>(slot_index);
    if (cache->material.has_value() && slot.material == *cache->material) {
        slot.material = task_field_value(task_type_[i]);
    }
    if (cache->brand.has_value() && slot.brand == *cache->brand) {
        slot.brand = task_field_value(task_vendor_[i]);
    }
    if (cache->color_rgb.has_value() && slot.color_rgb == *cache->color_rgb) {
        slot.color_rgb = task_color_[i].value_or(kAmsDefaultSlotColor);
    }
    // No lane record carries temperatures or weight, so a user's own values
    // are told apart only by the lane holding a declaration or a bound spool.
    const helix::ams::LaneSources sources = helix::ams::lane_sources(lane);
    const bool user_declared = sources.local_user.has_value() || sources.spoolman.has_value();
    if (const std::optional<TagSlotValues> tag = tag_slot_values_[i]; tag && !user_declared) {
        const SlotInfo blank;
        if (slot.nozzle_temp_min == tag->nozzle_temp_min) {
            slot.nozzle_temp_min = blank.nozzle_temp_min;
        }
        if (slot.nozzle_temp_max == tag->nozzle_temp_max) {
            slot.nozzle_temp_max = blank.nozzle_temp_max;
        }
        if (slot.bed_temp == tag->bed_temp) {
            slot.bed_temp = blank.bed_temp;
        }
        if (slot.total_weight_g == tag->total_weight_g) {
            slot.total_weight_g = blank.total_weight_g;
        }
    }
    tag_slot_values_[i].reset();
    if (const auto& kept = tag_readings_[i]; kept && !kept->multi_color_hexes.empty() &&
                                             slot.multi_color_hexes == kept->multi_color_hexes) {
        slot.multi_color_hexes.clear();
    }
    tag_readings_[i].reset();
    if (cache->product_name.has_value()) {
        if (slot.spool_name == *cache->product_name) {
            slot.spool_name.clear();
        }
        if (slot.product_name == *cache->product_name) {
            slot.product_name.clear();
        }
    }
    spdlog::debug("{} Slot {} tag reader names no tag, dropped the cached tag reading",
                  backend_log_tag(), slot_index);
    return true;
}

void AmsBackendSnapmaker::retract_unbacked_identity_locked(int slot_index, SlotInfo& slot) {
    // The channel has to be silent in both places firmware keeps identity:
    // the tag names nothing, and print_task_config names no vendor and no
    // official spool. Either one speaking is the firmware standing behind a
    // spool, and a source not heard from yet is no verdict.
    const auto i = static_cast<size_t>(slot_index);
    const bool tag_silent = tag_names_identity_[i].has_value() && !*tag_names_identity_[i];
    const bool task_silent = task_vendor_[i].has_value() && is_unset_task_field(*task_vendor_[i]) &&
                             !task_official_[i].value_or(false);
    if (!tag_silent || !task_silent) {
        return;
    }

    // Only identity in the firmware's own vocabulary that nobody declared
    // retracts: a remembered Snapmaker product line, and the Snapmaker brand
    // beside it, which a stored record copied from what firmware once said.
    // The same rule the vendor-cache brand follows when a tag stops naming a
    // vendor. Anything else remembered (a name kept across a Spoolman
    // unlink, one typed by hand) never came from firmware and stays, as do
    // declared fields and a brand beside the user's catalog pick, which names
    // it. A slot whose record still names a Spoolman spool is the spool's.
    auto ovr = overrides_.find(slot_index);
    if (ovr != overrides_.end() && ovr->second.spoolman_id > 0) {
        return;
    }
    const helix::ams::LaneId lane = lane_id(slot_index);
    const helix::ams::LaneSources sources = helix::ams::lane_sources(lane);
    if (!sources.remembered.has_value()) {
        return;
    }
    helix::ams::Observation kept = *sources.remembered;
    // A product-line literal is Snapmaker's word only beside Snapmaker or no
    // brand at all; beside another vendor ("eSUN Silk") it names that
    // vendor's product.
    const bool line_vendor =
        !kept.brand.has_value() || kept.brand->empty() || is_product_line_vendor(*kept.brand);
    const auto is_line = [line_vendor](const std::optional<std::string>& v) {
        return line_vendor && v.has_value() && is_known_subtype(*v);
    };
    const bool drop_name = is_line(kept.spool_name);
    const bool drop_product = is_line(kept.product_name);
    const bool picked =
        sources.local_user.has_value() && sources.local_user->catalog_id.has_value();
    const bool drop_brand = (drop_name || drop_product) && kept.brand.has_value() &&
                            is_product_line_vendor(*kept.brand) && !picked;
    if (!drop_name && !drop_product && !drop_brand) {
        return;
    }
    const helix::ams::Observation dropped = kept;
    if (drop_name) {
        kept.spool_name.reset();
        if (slot.spool_name == *dropped.spool_name) {
            slot.spool_name.clear();
        }
    }
    if (drop_product) {
        kept.product_name.reset();
    }
    if (drop_brand) {
        kept.brand.reset();
        if (slot.brand == *dropped.brand) {
            slot.brand.clear();
        }
    }
    helix::ams::drop_lane_source(lane, helix::ams::ObservationSource::Remembered);
    const bool kept_any =
        std::apply([](const auto&... f) { return (f.has_value() || ...); }, kept.fields());
    if (kept_any) {
        helix::ams::ingest(lane, kept);
    }

    if (ovr == overrides_.end()) {
        return;
    }
    // The stored record loses a field only where it still holds the value
    // being retracted; anything it says beyond that is not this rule's to drop.
    helix::ams::FilamentSlotOverride& record = ovr->second;
    bool changed = false;
    if (drop_name && record.spool_name == *dropped.spool_name) {
        record.spool_name.clear();
        changed = true;
    }
    if (drop_product && record.product_name == *dropped.product_name) {
        record.product_name.clear();
        changed = true;
    }
    if (drop_brand && record.brand == *dropped.brand) {
        record.brand.clear();
        changed = true;
    }
    if (!changed) {
        return;
    }
    spdlog::info("{} Slot {} firmware states no spool; retracted remembered '{}' '{}'",
                 backend_log_tag(), slot_index, dropped.brand.value_or(""),
                 dropped.spool_name.value_or(dropped.product_name.value_or("")));
    if (override_store_) {
        helix::ams::save_override_async(override_store_.get(), slot_index, record,
                                        backend_log_tag(), "Retraction");
    }
}

void AmsBackendSnapmaker::track_active_port_present_locked(FrameEffects& fx) {
    // First-gate (port) filament presence for the ACTIVE tool (#991). The
    // runout dialog gates Resume on THIS signal — the port/buffer sensor that
    // flips true the moment a user re-feeds a spool — NOT the toolhead motion
    // sensor (sensor_filament_present_), which stays "runout" until extrusion.
    // No active tool → treat as present (1) so Resume is never gated. Computed
    // under mutex_ (reads current_tool + the port array); published after the
    // mutex is released. Only publish on an actual change to avoid spamming
    // the UpdateQueue on every incremental notify.
    int active_tool = system_info_.current_tool;
    bool active_port_present = !(active_tool >= 0 && active_tool < kNumTools) ||
                               port_sensor_filament_present_[active_tool];
    int port_val = active_port_present ? 1 : 0;
    if (port_val != last_published_port_present_) {
        last_published_port_present_ = port_val;
        fx.port_present_changed = true;
    }
}

void AmsBackendSnapmaker::converge_locked(FrameEffects& fx) {
    derive_active_tool_loaded_locked(fx);
    demote_runout_slots_locked(fx);
    converge_lanes_locked(fx);
    track_active_port_present_locked(fx);
}

void AmsBackendSnapmaker::handle_status(const nlohmann::json& status) {
    FrameEffects fx;
    std::string batch_macro_object;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch_macro_object = batch_macro_object_;
    }
    const snapmaker::StatusDelta delta = snapmaker::parse_status(status, batch_macro_object);

    { // Scope lock — emit_event MUST be called outside mutex_ to avoid deadlock
      // with sync_from_backend() which acquires mutex_ via get_system_info()
        std::lock_guard<std::mutex> lock(mutex_);

        bool frame_names_state = false;
        bool frame_names_file = false;
        if (const nlohmann::json* ps = helix::json_util::find_member(status, "print_stats")) {
            if (const nlohmann::json* file = helix::json_util::find_member(*ps, "filename");
                file && file->is_string()) {
                print_filename_ = file->get<std::string>();
                frame_names_file = true;
            }
            if (const nlohmann::json* state = helix::json_util::find_member(*ps, "state");
                state && state->is_string()) {
                apply_print_state_locked(state->get<std::string>());
                frame_names_state = true;
            }
        }
        if (const auto main_state = snapmaker::screws_tilt::main_state_from_status(status)) {
            main_state_ = *main_state;
            if (*main_state != snapmaker::nozzle_offset::kMainStateXyzOffsetCalibrate) {
                snapmaker::nozzle_offset::clear_exit_pending();
            }
        }

        apply_extension_locked(status, fx.changed);

        apply_extruders_locked(delta, fx);
        apply_active_tool_locked(delta, fx);

        apply_filament_detect_locked(delta, fx);

        apply_feed_channels_locked(delta, fx);

        apply_batch_state_locked(delta, fx);
        apply_working_slot_locked(fx);

        apply_print_task_config_locked(delta, fx);
        fx.check_used_heads = used_heads_check_due_locked(frame_names_state, frame_names_file);
        fx.check_used_heads_file = print_filename_;
        fx.check_used_heads_print = print_generation_;

        apply_toolhead_sensors_locked(delta, fx);

        converge_locked(fx);

        fx.official_repairs = official_flag_repairs_locked();
        fx.record_restores = record_restores_locked();
        // A save held across a print waits for the heads to be free: a pause
        // the print raised (a runout, an M600) is no moment to reset a head's
        // flow calibration behind the user's back.
        if (heads_free_locked()) {
            for (int head = 0; head < kNumTools; ++head) {
                auto& pending = pending_record_writes_[static_cast<size_t>(head)];
                if (pending) {
                    fx.record_flushes.emplace_back(head, std::move(*pending));
                    pending.reset();
                }
            }
        }
    } // Release mutex_ before emitting event

    dispatch_effects(fx);
}

// Everything a frame decided that reaches outside mutex_: AmsState takes its own
// mutex and AmsState::add_backend() takes that one first and then ours via
// set_event_callback(), the UI queue and gcode dispatch must not run under
// either, and emit_event() re-enters get_system_info().
void AmsBackendSnapmaker::dispatch_effects(const FrameEffects& fx) {
    if (fx.port_present_changed) {
        AmsState::instance().set_active_tool_port_present(last_published_port_present_ != 0);
    }

    // Record the just-unloaded lanes so FilamentSensorManager suppresses the
    // runout modal during the grace window when the user is EXPECTED to pull
    // filament out of the lane.
    for (int lane : fx.unloaded_lanes) {
        AmsState::instance().mark_slot_unloaded(lane);
    }

    // An insert the RFID side vouches nothing for asks whether the stored
    // record still describes the spool that went in (#1710). The notice
    // re-checks its own guards (print-feeding lane, lane with nothing to
    // clear) on the UI thread.
    for (int lane : fx.unverified_insert_lanes) {
        helix::ui::queue_update("AmsBackendSnapmaker::handle_status",
                                [lane] { helix::ui::offer_clear_after_unverified_insert(lane); });
    }

    if (fx.batch_failed_head >= 0) {
        spdlog::warn("{} head {} reached '{}' — clearing the firmware batch interlock",
                     backend_log_tag(), fx.batch_failed_head, fx.batch_failed_state);
        end_firmware_batch();
    }

    dispatch_record_scripts(fx.official_repairs,
                            "is flagged official with no card behind it; rewriting the record "
                            "HelixScreen wrote to clear the flag");
    dispatch_record_scripts(fx.record_restores,
                            "record names no filament while the spool declared for it is still "
                            "in the feeder; writing the declaration back");
    for (const auto& [head, pending] : fx.record_flushes) {
        write_task_filament_record(head, pending.info, pending.material_declared);
    }

    if (fx.refeed_settled) {
        finish_refeed_wait();
    }

    if (fx.check_used_heads) {
        std::string file = fx.check_used_heads_file;
        if (file.empty()) {
            file = lv_subject_get_string(
                get_printer_state().print_state().get_print_filename_subject());
        }
        warn_unloaded_used_heads(file, fx.check_used_heads_print);
    }

    if (fx.changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

// ============================================================================
// Override layering
// ============================================================================

void AmsBackendSnapmaker::check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                                     const helix::ams::SpoolEvidence& observed) {
    // The bookkeeping lives in the shared tracker (CFS runs the same one);
    // the verdict on a change is the insert rule's. Snapmaker registers no
    // expect() value, so OwnWriteEcho cannot occur here - nothing on this
    // backend writes a CARD_UID back to firmware.
    //
    //   NoSignal  = the frame answered no RFID info for the channel. Baseline
    //               untouched, no clear.
    //   Baseline  = first reading. Even when the override was saved against a
    //               different spool, the first observation is NEVER a swap
    //               signal; apply_resolved_lane runs after us and a declared
    //               value outranks this reading.
    //   Unchanged = the same reading repeated.
    std::string old_fingerprint;
    const auto event =
        rfid_tracker_.observe(slot_index, fingerprint_from_evidence(observed), &old_fingerprint);
    if (event != helix::ams::FingerprintEvent::Changed) {
        if (event == helix::ams::FingerprintEvent::Baseline) {
            spdlog::debug("{} Slot {} baseline RFID fingerprint: {}", backend_log_tag(), slot_index,
                          fingerprint_from_evidence(observed));
        }
        return;
    }

    const auto verdict =
        helix::ams::classify_insert(evidence_from_fingerprint(old_fingerprint), observed);
    if (verdict != helix::ams::InsertVerdict::DifferentSpool) {
        spdlog::debug(
            "{} Slot {} RFID fingerprint changed {} -> {} ({}); record stands", backend_log_tag(),
            slot_index, old_fingerprint, fingerprint_from_evidence(observed),
            verdict == helix::ams::InsertVerdict::SameSpool ? "same spool" : "no evidence");
        return;
    }

    auto ovr_it = overrides_.find(slot_index);
    if (ovr_it == overrides_.end()) {
        spdlog::debug("{} Slot {} RFID fingerprint changed {} -> {} (no override to clear)",
                      backend_log_tag(), slot_index, old_fingerprint,
                      fingerprint_from_evidence(observed));
        return;
    }

    spdlog::info("{} Slot {} RFID fingerprint changed {} -> {}, clearing override "
                 "(different spool detected)",
                 backend_log_tag(), slot_index, old_fingerprint,
                 fingerprint_from_evidence(observed));

    // Delegate the erase + field reset + clear_async to the shared helper so
    // hardware-event clears and user-initiated clears share one field-reset
    // policy. Caller already holds mutex_.
    (void)ovr_it; // erased inside clear_override_locked
    clear_override_locked(slot_index, &slot);
    // A held save described the spool that left.
    drop_pending_record_write_locked(slot_index);
    slot.spool_name = tag_product_line(lane_id(slot_index));
}

void AmsBackendSnapmaker::clear_override_fields(SlotInfo& slot) const {
    // All three Spoolman handles go. The spool name goes with the record that
    // named it; the callers restate the tag's own product line, which no clear
    // touches.
    slot.spool_name.clear();
    slot.spoolman_id = 0;
    slot.spoolman_vendor_id = 0;
    slot.spoolman_filament_id = 0;
    slot.spoolman_orca_filament_id.clear();
    slot.remaining_weight_g = -1.0f;
    slot.color_name.clear();
    slot.catalog_id.clear();
    slot.product_name.clear();
}

void AmsBackendSnapmaker::clear_slot_override(int slot_index) {
    if (auto err = validate_slot_index(slot_index); !err.success()) {
        spdlog::warn("{} clear_slot_override: invalid slot {}", backend_log_tag(), slot_index);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot =
            system_info_.units.empty() ? nullptr : system_info_.units[0].get_slot(slot_index);
        if (!slot) {
            spdlog::warn("{} clear_slot_override: no slot entry for index {}", backend_log_tag(),
                         slot_index);
            return;
        }
        spdlog::info("{} Slot {} override cleared by user request", backend_log_tag(), slot_index);
        clear_override_locked(slot_index, slot);
        drop_pending_record_write_locked(slot_index);
        slot->spool_name = tag_product_line(lane_id(slot_index));
    }

    emit_event(kEventSlotChanged, std::to_string(slot_index));
}

// ============================================================================
// Internal Helpers
// ============================================================================

std::vector<int> AmsBackendSnapmaker::task_routing(const std::vector<bool>& extruders_used,
                                                   const std::vector<int>& extruder_map) {
    // Gate on a task actually being configured. With no task the firmware holds
    // a default identity map, and answering [0,1,2,3] there would hand callers a
    // confident wrong routing for any file whose tools do not line up with the
    // lanes — the identity-as-truth mistake this accessor exists to end, just
    // moved one layer down. All-false extruders_used is the firmware's own "no
    // task" signal: it sets the flags when a task is set up and clears them when
    // the print ends (observed idle [F,F,F,F], mid-print [F,F,T,F]).
    const bool task_configured =
        std::any_of(extruders_used.begin(), extruders_used.end(), [](bool b) { return b; });
    if (!task_configured) {
        return {};
    }
    return extruder_map;
}

std::vector<int> AmsBackendSnapmaker::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return task_routing(extruders_used_, extruder_map_table_);
}

std::vector<int> AmsBackendSnapmaker::last_print_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Deliberately NOT gated on a task being configured: the whole point is to
    // answer after the task has ended, which is when a reprint asks. Empty means
    // no configured task has ever been observed.
    return last_task_extruder_map_;
}

std::string AmsBackendSnapmaker::build_preprint_gcode(const std::set<int>& tools_used,
                                                      const std::map<int, int>& remap) const {
    // The unload choice travels with every start, including one whose routing
    // is unknown (empty tools_used): it needs no routing and the firmware
    // forgets it when each print ends.
    std::string gcode = preprint_gcode(tools_used, remap);
    const std::string unload = end_unload_gcode();
    if (!unload.empty())
        gcode += (gcode.empty() ? "" : "\n") + unload;
    return gcode;
}

std::string AmsBackendSnapmaker::used_extruders_line(const std::set<int>& heads) {
    std::string csv;
    for (int head : heads) {
        if (!csv.empty()) {
            csv += ',';
        }
        csv += std::to_string(head);
    }
    return fmt::format("SET_PRINT_USED_EXTRUDERS EXTRUDERS={}", csv);
}

std::string
AmsBackendSnapmaker::build_unrouted_preprint_gcode(const std::set<int>& tools_used) const {
    const helix::FirmwareRouting routing = default_routing();
    std::set<int> heads;
    bool map_at_default = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int t : tools_used) {
            const int head = routing.head(t);
            if (head < 0) {
                continue;
            }
            heads.insert(head);
            if (static_cast<size_t>(t) < extruder_map_table_.size() &&
                extruder_map_table_[static_cast<size_t>(t)] != head) {
                map_at_default = false;
            }
        }
    }
    const std::string unload = end_unload_gcode();
    if (heads.empty() || !map_at_default) {
        if (!map_at_default) {
            spdlog::warn("[Snapmaker] preprint: the firmware holds a non-default extruder map; "
                         "sending no used heads for an unrouted reprint");
        }
        return unload;
    }
    std::string gcode = used_extruders_line(heads);
    if (!unload.empty()) {
        gcode += "\n" + unload;
    }
    return gcode;
}

std::string AmsBackendSnapmaker::preprint_gcode(const std::set<int>& tools_used,
                                                const std::map<int, int>& remap) {
    if (tools_used.empty()) {
        return "";
    }

    // Ask the backend's own routing table rather than restating it here.
    // FilamentMapper::identity_filtered_remap() decides which mappings count as
    // genuine remaps from this same table; a second copy of it would let the two
    // halves disagree about which head a tool defaults to, filtering a mapping
    // out as identity while resolving it somewhere else.
    const helix::FirmwareRouting routing = default_routing();

    // Resolve every used logical tool to the head it must print from: the user's
    // remap when there is one, the firmware default otherwise. A tool the routing
    // gives no head (-1) is dropped: MAP_EXTRUDER=-1 clears the firmware's
    // `>= PHYSICAL_EXTRUDER_NUM` bounds check and then indexes
    // extruder_map_table[-1], which in Python is the LAST entry.
    std::map<int, int> resolved;
    for (int t : tools_used) {
        auto it = remap.find(t);
        const int head = (it != remap.end()) ? it->second : routing.head(t);
        if (head < 0) {
            spdlog::warn("[Snapmaker] preprint: tool {} has no head in the firmware routing - "
                         "leaving it out of the extruder map",
                         t);
            continue;
        }
        resolved[t] = head;
    }
    if (resolved.empty()) {
        return "";
    }

    std::vector<std::string> lines;

    // Emit SET_PRINT_EXTRUDER_MAP for EVERY used tool, including the ones landing
    // on their firmware-default head.
    //
    // Emitting only the genuine remaps left the rest at whatever the PREVIOUS
    // print wrote: the command sets one entry and resets nothing, and the
    // firmware's reset_print_info() does not run between our send and the print.
    // So a job that remapped T0->head2 left extruder_map_table[0]=2 behind, and
    // the next job — needing plain T0->head0 and therefore emitting nothing for
    // it — printed from head 2. That also corrupted the SET_PRINT_USED_EXTRUDERS
    // line below, which assumed the default applied. Writing each used entry
    // explicitly makes the table say exactly what this print means, which is
    // also what lets get_tool_mapping() be read back as truth.
    //
    // std::map iterates in ascending key order, so the sequence is deterministic.
    for (const auto& [logical, physical] : resolved) {
        lines.push_back(fmt::format("SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER={} MAP_EXTRUDER={}",
                                    logical, physical));
    }

    // Derived from the SAME resolution, so the two commands cannot disagree.
    std::set<int> used_heads;
    for (const auto& [logical, physical] : resolved) {
        (void)logical;
        used_heads.insert(physical);
    }

    lines.push_back(used_extruders_line(used_heads));

    // Join with newlines, no trailing newline.
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

namespace {

/// The VENDOR and FILAMENT_SUBTYPE a record write sends for @p slot. The stock
/// screen offers only these two vendors; anything else the firmware would
/// store verbatim, and other Snapmaker tooling compares it.
std::pair<std::string, std::string> record_vendor_subtype(const SlotInfo& slot) {
    std::string vendor =
        is_product_line_vendor(slot.brand) ? std::string(kProductLineVendor) : "Generic";
    std::string subtype = is_known_subtype(slot.spool_name) ? slot.spool_name : "Basic";
    return {std::move(vendor), std::move(subtype)};
}

} // namespace

std::string AmsBackendSnapmaker::build_filament_config_gcode(int head, const SlotInfo& slot,
                                                             bool force, RecordLines lines) {
    if (is_unset_task_field(slot.material) ||
        !IMoonrakerAPI::is_safe_material_param(slot.material)) {
        return {};
    }
    const auto [vendor, subtype] = record_vendor_subtype(slot);
    const char* force_arg = force ? " FORCE=1" : "";
    // Colour rides its own line, as the stock screen sends it: the firmware's
    // colour forms are parsed apart from the identity fields.
    // SOFT=1 makes the firmware store the head as soft when its own table has
    // no row for the type. Rigid is left to the table: SOFT=0 would override a
    // row that knows better.
    const char* soft_arg = is_soft_material(slot.material) ? " SOFT=1" : "";
    const std::string identity =
        fmt::format("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER={} VENDOR={} FILAMENT_TYPE={} "
                    "FILAMENT_SUBTYPE={}{}{}",
                    head, vendor, IMoonrakerAPI::gcode_param_value(slot.material),
                    IMoonrakerAPI::gcode_param_value(subtype), soft_arg, force_arg);
    const std::string colour =
        fmt::format("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER={} FILAMENT_COLOR_RGBA={:06X}FF{}",
                    head, slot.color_rgb & 0xFFFFFFu, force_arg);
    switch (lines) {
    case RecordLines::Identity:
        return identity;
    case RecordLines::Colour:
        return colour;
    case RecordLines::Both:
        break;
    }
    return identity + "\n" + colour;
}

std::optional<bool> AmsBackendSnapmaker::record_write_needs_force_locked(int head) const {
    const auto idx = static_cast<size_t>(head);
    if (!task_official_[idx].value_or(false)) {
        return false;
    }
    // filament_official alone does not mean a tag: the Extended Firmware's
    // filament_detect/set marks every write carrying fields official, and a
    // later clear leaves the flag set. Only a card the reader saw is the
    // tag's record to keep. A reader reporting no card for a head the user is
    // editing yields to the edit: asking for it is the user's intent.
    if (tag_has_card_[idx].value_or(true)) {
        return std::nullopt;
    }
    return true;
}

bool AmsBackendSnapmaker::record_writes_held_locked() const {
    // Every record write runs FLOW_RESET_K on its head, which must not land
    // under a running print or between the offset calibration's steps.
    if (calibration_holds_heads_locked()) {
        return true;
    }
    if (print_state_) {
        return *print_state_ == "printing";
    }
    // Before this backend's own frames have named a state, the app's print
    // state answers for a user's save, and a state nobody has read holds it.
    // RAW_PRINT_STATE_OK: the write is gated on what the job is doing to the
    // heads, not on the lifecycle the UI shows.
    if (!get_printer_state().are_subjects_initialized()) {
        return true;
    }
    return get_printer_state().print_state().get_print_job_state() == PrintJobState::PRINTING;
}

void AmsBackendSnapmaker::write_task_filament_record(int head, const SlotInfo& info,
                                                     bool material_declared) {
    if (head < 0 || head >= kNumTools || !api_) {
        return;
    }
    const auto idx = static_cast<size_t>(head);
    bool force = false;
    RecordLines lines = RecordLines::Both;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record_writes_held_locked()) {
            spdlog::info("{} head {} filament record held until the print stops running",
                         backend_log_tag(), head);
            pending_record_writes_[idx] = PendingRecordWrite{info, material_declared};
            return;
        }
        const std::optional<bool> needs_force = record_write_needs_force_locked(head);
        if (!needs_force) {
            spdlog::debug("{} head {} holds an RFID tag's record; leaving it to the tag",
                          backend_log_tag(), head);
            return;
        }
        if (!IMoonrakerAPI::is_safe_material_param(info.material)) {
            spdlog::warn("{} head {} material '{}' cannot be sent as a gcode parameter",
                         backend_log_tag(), head, info.material);
            return;
        }
        // Every line runs FLOW_RESET_K on the head, so a save that leaves the
        // firmware's record standing sends nothing, forced head or not.
        if (!task_type_[idx].has_value()) {
            if (!material_declared) {
                return;
            }
        } else {
            const auto [vendor, subtype] = record_vendor_subtype(info);
            const bool identity_differs =
                is_unset_task_field(*task_type_[idx]) ||
                !helix::text_io::equals_ignore_case(*task_type_[idx], info.material) ||
                (task_vendor_[idx] &&
                 !helix::text_io::equals_ignore_case(*task_vendor_[idx], vendor)) ||
                (task_subtype_[idx] &&
                 !helix::text_io::equals_ignore_case(*task_subtype_[idx], subtype));
            // A colour not seen yet goes out beside an identity change, and
            // never alone.
            const bool colour_known = task_color_[idx].has_value();
            const bool colour_differs =
                colour_known && *task_color_[idx] != (info.color_rgb & 0xFFFFFFu);
            if (identity_differs && (colour_differs || !colour_known)) {
                lines = RecordLines::Both;
            } else if (identity_differs) {
                lines = RecordLines::Identity;
            } else if (colour_differs) {
                lines = RecordLines::Colour;
            } else {
                return;
            }
        }
        // An official head is rewritten whole, so no line leaves the flag.
        force = *needs_force;
        if (force) {
            lines = RecordLines::Both;
        }
        // This write is the record's restore until the firmware echoes it.
        record_restore_sent_[idx] = true;
    }
    const std::string gcode = build_filament_config_gcode(head, info, force, lines);
    if (gcode.empty()) {
        return;
    }
    const char* tag = backend_log_tag();
    spdlog::info("{} writing head {} filament record: {}", tag, head, info.material);
    api_->execute_gcode(
        gcode, [tag, head]() { spdlog::debug("{} head {} filament record written", tag, head); },
        [tag, head](const MoonrakerError& err) {
            spdlog::warn("{} head {} filament record write failed: {}", tag, head, err.message);
        },
        /*timeout_ms=*/0, /*silent=*/false, /*on_queued=*/nullptr,
        /*caller_surfaces_errors=*/false);
}

void AmsBackendSnapmaker::drop_pending_record_write_locked(int head) {
    if (head >= 0 && head < kNumTools) {
        pending_record_writes_[static_cast<size_t>(head)].reset();
    }
}

bool AmsBackendSnapmaker::calibration_holds_heads_locked() const {
    return main_state_ && *main_state_ == snapmaker::nozzle_offset::kMainStateXyzOffsetCalibrate;
}

bool AmsBackendSnapmaker::heads_free_locked() const {
    if (calibration_holds_heads_locked()) {
        return false;
    }
    return print_state_ && *print_state_ != "printing" && *print_state_ != "paused";
}

std::vector<std::pair<int, std::string>> AmsBackendSnapmaker::official_flag_repairs_locked() {
    std::vector<std::pair<int, std::string>> out;
    // A forced rewrite runs FLOW_RESET_K, so it waits for a print state this
    // backend has read itself and that holds no head.
    if (system_info_.units.empty() || !heads_free_locked()) {
        return out;
    }
    for (int head = 0; head < kNumTools; ++head) {
        const auto idx = static_cast<size_t>(head);
        if (official_repair_sent_[idx]) {
            continue;
        }
        // Only a filled entry with no card behind it, on a channel that has
        // shown no card this session, is an API write's record: an empty entry
        // is also what a reader that is off reports for a real spool.
        if (!task_official_[idx].value_or(false) || !tag_names_identity_[idx].value_or(false) ||
            !tag_has_card_[idx].has_value() || *tag_has_card_[idx] || card_seen_[idx]) {
            continue;
        }
        // The record has to be the one this backend's stored edit wrote. A
        // record naming anything else came from some other writer, whose
        // choice a forced rewrite would discard.
        const auto ovr = overrides_.find(head);
        if (ovr == overrides_.end() || is_unset_task_field(ovr->second.material) ||
            !task_type_[idx] ||
            !helix::text_io::equals_ignore_case(*task_type_[idx], ovr->second.material)) {
            continue;
        }
        if (ovr->second.color_set && task_color_[idx] &&
            *task_color_[idx] != (ovr->second.color_rgb & 0xFFFFFFu)) {
            continue;
        }
        const SlotInfo* slot = system_info_.units[0].get_slot(head);
        if (!slot) {
            continue;
        }
        std::string gcode = build_filament_config_gcode(head, *slot, /*force=*/true);
        if (gcode.empty()) {
            continue;
        }
        official_repair_sent_[idx] = true;
        out.emplace_back(head, std::move(gcode));
    }
    return out;
}

std::vector<std::pair<int, std::string>> AmsBackendSnapmaker::record_restores_locked() {
    std::vector<std::pair<int, std::string>> out;
    // Every write runs FLOW_RESET_K on its head.
    if (system_info_.units.empty() || !heads_free_locked() || batch_.active) {
        return out;
    }
    for (int head = 0; head < kNumTools; ++head) {
        const auto idx = static_cast<size_t>(head);
        if (!task_type_[idx]) {
            continue;
        }
        if (!is_unset_task_field(*task_type_[idx])) {
            record_restore_sent_[idx] = false;
            continue;
        }
        // A read still pending may yet name a different spool than the
        // declaration.
        if (record_restore_sent_[idx] || insert_since_declaration_[idx] ||
            pending_insert_passes_[idx] > 0) {
            continue;
        }
        // The firmware's clear does not say the spool left: unload-at-end
        // leaves it in the feeder. An empty port does, and a spool that went
        // in since is one nobody has named.
        const bool spool_in_lane =
            (feed_presence_seen_[idx] && port_sensor_filament_present_[idx]) ||
            loaded_at_toolhead_[idx];
        if (!spool_in_lane ||
            helix::snapmaker::channel_state_in_progress(channel_snapshots_[idx].state)) {
            continue;
        }
        const auto ovr = overrides_.find(head);
        if (ovr == overrides_.end() ||
            !helix::ams::declared_on_lane(lane_id(head), &ovr->second).material) {
            continue;
        }
        std::string gcode = unset_record_restore_locked(head);
        if (gcode.empty()) {
            continue;
        }
        record_restore_sent_[idx] = true;
        out.emplace_back(head, std::move(gcode));
    }
    return out;
}

std::string AmsBackendSnapmaker::unset_record_restore_locked(int head) const {
    const auto idx = static_cast<size_t>(head);
    if (!task_type_[idx].has_value() || !is_unset_task_field(*task_type_[idx])) {
        return {};
    }
    const std::optional<bool> needs_force = record_write_needs_force_locked(head);
    if (!needs_force) {
        return {};
    }
    const SlotInfo* slot =
        system_info_.units.empty() ? nullptr : system_info_.units[0].get_slot(head);
    if (!slot) {
        return {};
    }
    return build_filament_config_gcode(head, *slot, *needs_force);
}

void AmsBackendSnapmaker::dispatch_record_scripts(
    const std::vector<std::pair<int, std::string>>& writes, const char* why) {
    if (!api_) {
        return;
    }
    const char* tag = backend_log_tag();
    for (const auto& [head, gcode] : writes) {
        spdlog::info("{} head {} {}", tag, head, why);
        api_->execute_gcode(
            gcode,
            [tag, h = head]() { spdlog::debug("{} head {} filament record written", tag, h); },
            [tag, h = head](const MoonrakerError& err) {
                spdlog::warn("{} head {} filament record write failed: {}", tag, h, err.message);
            },
            /*timeout_ms=*/0, /*silent=*/false, /*on_queued=*/nullptr,
            /*caller_surfaces_errors=*/false);
    }
}

std::vector<std::pair<int, std::string>>
AmsBackendSnapmaker::resume_record_repairs_locked(int active_slot) const {
    // extruders_used can read all false mid-print, so the heads the task
    // routes to are taken from its map as well, alongside the head paused on.
    std::array<bool, kNumTools> used{};
    for (size_t i = 0; i < extruders_used_.size() && i < used.size(); ++i) {
        used[i] = extruders_used_[i];
    }
    for (const int head : last_task_extruder_map_) {
        if (head >= 0 && head < kNumTools) {
            used[static_cast<size_t>(head)] = true;
        }
    }
    if (active_slot >= 0 && active_slot < kNumTools) {
        used[static_cast<size_t>(active_slot)] = true;
    }
    std::vector<std::pair<int, std::string>> out;
    for (int head = 0; head < kNumTools; ++head) {
        const auto idx = static_cast<size_t>(head);
        if (!used[idx]) {
            continue;
        }
        std::string gcode = unset_record_restore_locked(head);
        if (gcode.empty()) {
            continue;
        }
        spdlog::info("{} prepare_for_resume: head {} has no firmware filament type; "
                     "restoring {} before RESUME",
                     backend_log_tag(), head, system_info_.units[0].get_slot(head)->material);
        out.emplace_back(head, std::move(gcode));
    }
    return out;
}

void AmsBackendSnapmaker::send_resume_repairs_then(std::vector<std::pair<int, std::string>> repairs,
                                                   std::function<void()> next) {
    if (repairs.empty() || !api_) {
        next();
        return;
    }
    // One head per request, in order: a head the firmware refuses must not
    // drop the heads after it.
    const auto [head, gcode] = repairs.front();
    repairs.erase(repairs.begin());
    auto tok = lifetime_.token();
    const char* tag = backend_log_tag();
    auto rest = std::make_shared<std::vector<std::pair<int, std::string>>>(std::move(repairs));
    auto after = [this, tok, rest, next]() mutable {
        tok.defer("AmsBackendSnapmaker::send_resume_repairs_then",
                  [this, rest, next = std::move(next)]() mutable {
                      send_resume_repairs_then(std::move(*rest), std::move(next));
                  });
    };
    api_->execute_gcode(
        gcode, [after]() mutable { after(); },
        [after, tag, head = head](const MoonrakerError& err) mutable {
            spdlog::warn("{} prepare_for_resume: head {} filament record repair failed: {}", tag,
                         head, err.message);
            after();
        },
        // RESUME reports the refusal this repair failed to prevent.
        /*timeout_ms=*/0, /*silent=*/true, /*on_queued=*/nullptr,
        /*caller_surfaces_errors=*/false);
}

// The U1 blocks RESUME whenever a used extruder still reads filament_type
// "" or "NONE" in print_task_config: INNER_CHECK_AND_RELOAD_FILAMENT_INFO
// raises `e<N> not edit filament`. Tagless third-party spools land here,
// because only a Snapmaker RFID spool fills that field on its own.
//
// The refusal is raised oneshot, so it never reaches print_stats.exception --
// that still holds whatever paused the print, typically a runout. The generic
// classifier would see an uncoded `!!` on a paused printer and offer Resume,
// which this fault refuses again, redisplaying the same modal indefinitely.
std::optional<helix::ErrorEvent>
AmsBackendSnapmaker::classify_error(const std::string& raw_line,
                                    const helix::ClassifyContext& ctx) const {
    if (!helix::is_bang_line(raw_line)) {
        return std::nullopt;
    }

    // Only meaningful against a job the user is trying to continue. Echoing the
    // words into the console on an idle machine earns no modal.
    if (!ctx.is_paused && !ctx.is_printing) {
        return std::nullopt;
    }

    // Klipper's wording, verbatim: "e<N> not edit filament".
    const std::string detail = helix::strip_bang_prefix(raw_line);
    static constexpr std::string_view kSuffix = " not edit filament";
    if (detail.empty() || detail.front() != 'e') {
        return std::nullopt;
    }
    const auto suffix_pos = detail.find(kSuffix);
    if (suffix_pos == std::string::npos || suffix_pos < 2) {
        return std::nullopt;
    }
    const std::string digits = detail.substr(1, suffix_pos - 1);
    if (!is_parsable_index(digits)) {
        return std::nullopt;
    }

    // Firmware counts extruders from 0; every slot number the user reads is
    // 1-based, matching the machine's own labels and the slicer's filament list.
    const auto parsed_slot_no = helix::text_io::parse_leading<int>(digits);
    if (!parsed_slot_no) {
        return std::nullopt;
    }
    const int slot = *parsed_slot_no + 1;

    // No log here: the router asks this ahead of the `!!` copy too, and logs
    // the classified event when it presents it.

    // No gcode fixes this in one tap: SET_PRINT_FILAMENT_CONFIG needs a vendor,
    // type and subtype the user has to choose. So the action set is a plain
    // dismiss and the sentence carries the fix. Offering Resume here would
    // rebuild the loop this classifier exists to break.
    std::vector<helix::RecoveryAction> actions;
    actions.push_back(
        {lv_tr("OK"), "", "ams_backend_snapmaker::not_edit_filament_dismiss", "", false});

    helix::ErrorEvent e = helix::make_ams_fault_event(
        helix::ErrorSource::SNAPMAKER, lv_tr("Filament not set"),
        fmt::format(lv_tr("Slot {} has no filament type set, so the printer will not resume. "
                          "Set its material in the filament panel, then resume the print."),
                    slot),
        std::move(actions));
    // make_ams_fault_event leaves raw_detail empty; the router's cross-source
    // dedup matches on the firmware's untranslated wording, not our sentence.
    e.raw_detail = detail;
    // The resume caller's own report would be the raw firmware words.
    e.command_refusal = true;
    return e;
}

namespace {
bool head_is_soft(const std::vector<bool>& soft, size_t head) {
    return head < soft.size() && soft[head];
}

/// The unload-at-end list for the toolheads the firmware reports, in its list
/// order. The firmware clears its own list when every print ends, so the
/// setting is the record of what the user picked. A soft head goes out as 0:
/// the firmware's end macro trusts its own soft flag, which can call loaded
/// soft filament rigid. The saved bit itself is kept for a later rigid spool.
std::vector<bool> end_unload_list(const snapmaker::PrintPreferences& p,
                                  const std::vector<bool>& soft) {
    std::vector<bool> list(p.end_unload_filament.size());
    for (size_t t = 0; t < list.size(); ++t) {
        list[t] = SettingsManager::instance().get_end_of_print_unload_tool(static_cast<int>(t)) &&
                  !head_is_soft(soft, t);
    }
    return list;
}

/// The end-unload action id's tool, or nullopt for an id that is not one.
std::optional<size_t> end_unload_tool(const std::string& action_id) {
    static constexpr std::string_view prefix = "snapmaker_end_unload_t";
    if (action_id.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    const std::string suffix = action_id.substr(prefix.size());
    // Malformed ids stop here: a suffix that is not a parsable index names
    // no tool.
    if (!is_parsable_index(suffix)) {
        return std::nullopt;
    }
    const auto parsed = helix::text_io::parse_leading<unsigned long>(suffix);
    if (!parsed) {
        return std::nullopt;
    }
    return static_cast<size_t>(*parsed);
}
} // namespace

std::vector<bool> AmsBackendSnapmaker::soft_heads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<bool> soft(kNumTools, false);
    for (int i = 0; i < kNumTools; ++i) {
        const auto idx = static_cast<size_t>(i);
        const SlotInfo* slot =
            system_info_.units.empty() ? nullptr : system_info_.units[0].get_slot(i);
        soft[idx] = (idx < print_preferences_.filament_soft.size() &&
                     print_preferences_.filament_soft[idx]) ||
                    (slot != nullptr && is_soft_material(slot->material));
    }
    return soft;
}

std::vector<helix::printer::DeviceSection> AmsBackendSnapmaker::get_device_sections() const {
    using helix::printer::DeviceSection;
    std::vector<DeviceSection> out;
    // Nothing reported yet - an empty section is worse than none.
    if (!print_preferences().empty()) {
        out.push_back(helix::printer::device_section(kPrintPrefsSection, 50, ""));
    }
    return out;
}

std::vector<helix::printer::DeviceAction> AmsBackendSnapmaker::get_device_actions() const {
    using helix::printer::ActionType;
    using helix::printer::DeviceAction;

    // A copy taken under mutex_: every status frame rewrites the member.
    const auto p = print_preferences();
    std::vector<DeviceAction> out;

    auto add_toggle = [&](const char* id, const char* label, bool value) {
        out.push_back(DeviceAction::toggle(id, label, kPrintPrefsSection, value));
    };

    // Only settings the firmware has reported become actions: a toggle whose
    // state is unknown renders off and invites "changing" it to its own value.
    if (p.replenish_ignore_color) {
        add_toggle("snapmaker_replenish_ignore_color", TR_NOOP("Replenish ignores color"),
                   *p.replenish_ignore_color);
    }
    if (p.filament_entangle_detect) {
        add_toggle("snapmaker_entangle_detect", TR_NOOP("Detect filament tangles"),
                   *p.filament_entangle_detect);
    }
    if (p.filament_entangle_sen) {
        out.push_back(DeviceAction::dropdown("snapmaker_entangle_sen", "Tangle sensitivity",
                                             kPrintPrefsSection, {"low", "medium", "high"},
                                             std::string(*p.filament_entangle_sen)));
    }
    if (p.end_led_turn_off) {
        add_toggle("snapmaker_end_led_off", TR_NOOP("Turn LED off when the print ends"),
                   *p.end_led_turn_off);
    }
    // The firmware's list says how many heads there are; the values are the
    // saved choice, because the firmware clears its own when a print ends.
    const auto soft = soft_heads();
    const auto saved = end_unload_list(p, soft);
    for (size_t t = 0; t < saved.size(); ++t) {
        // Copy out as bool: indexing a const vector<bool> yields a standard-library
        // proxy type on some platforms, not bool, and the UI casts the value back to bool.
        const bool unload = saved[t];
        const int tool = static_cast<int>(t);
        DeviceAction a = DeviceAction::toggle("snapmaker_end_unload_t" + std::to_string(t),
                                              "Unload at end", kPrintPrefsSection, unload);
        a.slot_index = static_cast<int>(t);
        a.tool_index = tool;
        if (head_is_soft(soft, t)) {
            a.enabled = false;
            a.disable_reason = TR_NOOP("Soft filament is never unloaded");
        }
        out.push_back(std::move(a));
    }
    return out;
}

std::string AmsBackendSnapmaker::build_preference_gcode(const std::string& action_id,
                                                        const std::any& value) const {
    snapmaker::PrintPreferences changes;

    // Values arrive from the UI thread's action callback, so every cast is
    // pointer-form: a value whose type does not match the action reads as
    // absent and refuses like a malformed id, never as a bad_any_cast out
    // of the callback.
    if (action_id == "snapmaker_replenish_ignore_color") {
        const auto [v, err] = printer::device_action_value<bool>(value, "setting");
        if (!err) {
            return {};
        }
        changes.replenish_ignore_color = v;
    } else if (action_id == "snapmaker_entangle_detect") {
        const auto [v, err] = printer::device_action_value<bool>(value, "setting");
        if (!err) {
            return {};
        }
        changes.filament_entangle_detect = v;
    } else if (action_id == "snapmaker_end_led_off") {
        const auto [v, err] = printer::device_action_value<bool>(value, "setting");
        if (!err) {
            return {};
        }
        changes.end_led_turn_off = v;
    } else if (action_id == "snapmaker_entangle_sen") {
        const auto [v, err] = printer::device_action_value<std::string>(value, "sensitivity");
        if (!err) {
            return {};
        }
        changes.filament_entangle_sen = v;
    } else if (const auto tool = end_unload_tool(action_id)) {
        // END_UNLOAD_FILAMENT takes the whole list, so the untouched tools
        // travel at their saved values or they are cleared. The firmware's
        // own list is not used: it lags a tap behind, and it is cleared when
        // each print ends.
        const auto prefs = print_preferences();
        // A tool past the reported list is a stale id from before the firmware
        // reported fewer toolheads.
        if (*tool >= prefs.end_unload_filament.size()) {
            return {};
        }
        const auto [v, err] = printer::device_action_value<bool>(value, "setting");
        // The end macro skips a soft-filament head, so turning it on would
        // promise an unload that never happens.
        const auto soft = soft_heads();
        if (!err || (v && head_is_soft(soft, *tool))) {
            return {};
        }
        auto list = end_unload_list(prefs, soft);
        list[*tool] = v;
        changes.end_unload_filament = std::move(list);
    } else {
        return {};
    }
    return snapmaker::write_print_preferences_gcode(changes);
}

AmsError AmsBackendSnapmaker::do_execute_device_action(const std::string& action_id,
                                                       const std::any& value) {
    const std::string gcode = build_preference_gcode(action_id, value);
    if (gcode.empty()) {
        return AmsErrorHelper::not_supported(action_id);
    }
    if (const auto tool = end_unload_tool(action_id)) {
        // The saved choice is what every print start sends, so it is written
        // whether or not the firmware can take it now.
        SettingsManager::instance().set_end_of_print_unload_tool(
            static_cast<int>(*tool), printer::device_action_value<bool>(value, "setting").first);
        // The firmware refuses END_UNLOAD_FILAMENT while printing or paused
        // unless FORCE=1, which we do not pass; the next print start carries it.
        bool heads_free = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            heads_free = heads_free_locked();
        }
        if (!heads_free) {
            spdlog::info("[AMS Snapmaker] Unload at end for T{} saved; sent at the next print "
                         "start",
                         *tool);
            return AmsErrorHelper::success();
        }
    }
    return execute_gcode(gcode);
}

helix::printer::EndlessSpoolCapabilities
AmsBackendSnapmaker::get_endless_spool_capabilities() const {
    using namespace helix::printer;
    // The firmware's auto-replenish picks the backup head itself; until it has
    // reported the flag, the feature's state is unknown and nothing is offered.
    const auto replenish = print_preferences().auto_replenish;
    if (!replenish) {
        return {};
    }
    return {.availability = EndlessSpoolAvailability::Available,
            .enabled = *replenish ? EndlessSpoolEnabled::On : EndlessSpoolEnabled::Off,
            .editability = EndlessSpoolEditability::ReadOnly,
            .restriction = EndlessSpoolRestriction::FirmwareManaged,
            .switchable = true};
}

AmsError AmsBackendSnapmaker::apply_endless_spool_enabled(bool enabled) {
    snapmaker::PrintPreferences changes;
    changes.auto_replenish = enabled;
    return execute_gcode(snapmaker::write_print_preferences_gcode(changes));
}

std::string AmsBackendSnapmaker::end_unload_gcode(bool print_running) const {
    // The firmware resets end_unload_filament when every print ends, so the
    // saved choice is re-sent ahead of each one. Before it has reported its
    // list, the head count and support for the command are both unknown.
    const auto prefs = print_preferences();
    if (prefs.end_unload_filament.empty()) {
        return {};
    }
    snapmaker::PrintPreferences changes;
    changes.end_unload_filament = end_unload_list(prefs, soft_heads());
    changes.force = print_running;
    return snapmaker::write_print_preferences_gcode(changes);
}

std::string AmsBackendSnapmaker::standing_print_gcode() const {
    // A running print refuses SET_PRINT_EXTRUDER_MAP and SET_PRINT_USED_EXTRUDERS
    // outright, so only the unload choice can still be applied.
    return end_unload_gcode(/*print_running=*/true);
}

} // namespace helix

#endif // HELIX_HAS_SNAPMAKER
