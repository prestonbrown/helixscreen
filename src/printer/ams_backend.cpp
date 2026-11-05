// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend.h"

#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_indx.h"
#include "ams_backend_openams.h"
#include "ams_state.h"
#include "ams_step_model.h"
#include "settings_manager.h"
#include "system/afc_message_dedup.h"
#ifdef HELIX_ENABLE_MOCKS
#include "ams_backend_mock.h"
#include "app_globals.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#endif
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_row.h"
#include "ams_backend_toolchanger.h"
#include "core/text_io.h"
#include "filament_database.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "filament_variants.h"
#include "i_moonraker_api.h"
#include "lane_apply.h"
#include "lane_legacy_migration.h"
#include "lane_translation.h"
#include "lvgl.h"
#include "printer_discovery.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>
#include <tuple>

namespace helix {
namespace {

/// What a row takes from a backend's class. Empty for a backend this build
/// leaves out, which is all that a HELIX_HAS_* gate decides in this file.
struct BackendClassOps {
    const BackendTraits* traits = nullptr;
    AmsBackendRow::Construct construct = nullptr;
    AmsBackendRow::OwnsFilamentSensor owns_filament_sensor = nullptr;
    AmsBackendRow::RequiredStatusObjects required_status_objects = nullptr;
};

template <class Backend>
std::unique_ptr<AmsBackend> construct_backend(IMoonrakerAPI* api, IMoonrakerClient* client,
                                              const PrinterDiscovery* /*hw*/) {
    return std::make_unique<Backend>(api, client);
}

} // namespace
} // namespace helix

#if HELIX_HAS_IFS
namespace helix {
namespace {
nlohmann::json ifs_required_status_objects(const PrinterDiscovery& hw) {
    nlohmann::json objects = nlohmann::json::object();
    if (hw.mmu_type() == AmsType::AD5X_IFS) {
        for (const auto& name : AmsBackendAd5xIfs::required_status_objects(hw)) {
            objects[name] = nullptr;
        }
    }
    return objects;
}
constexpr BackendClassOps kIfsOps{&AmsBackendAd5xIfs::kTraits, construct_backend<AmsBackendAd5xIfs>,
                                  AmsBackendAd5xIfs::owns_filament_sensor,
                                  ifs_required_status_objects};
} // namespace
} // namespace helix
#else
namespace helix {
namespace {
constexpr BackendClassOps kIfsOps{};
} // namespace
} // namespace helix
#endif

#if HELIX_HAS_CFS
#include "ams_backend_cfs.h"
namespace helix {
namespace {
constexpr BackendClassOps kCfsOps{
    &printer::AmsBackendCfs::kTraits, construct_backend<printer::AmsBackendCfs>,
    printer::AmsBackendCfs::owns_filament_sensor, printer::AmsBackendCfs::required_status_objects};
} // namespace
} // namespace helix
#else
namespace helix {
namespace {
constexpr BackendClassOps kCfsOps{};
} // namespace
} // namespace helix
#endif

#if HELIX_HAS_ACE
#include "ams_backend_ace.h"
namespace helix {
namespace {
constexpr BackendClassOps kAceOps{&AmsBackendAce::kTraits, construct_backend<AmsBackendAce>,
                                  nullptr, AmsBackendAce::required_status_objects};
} // namespace
} // namespace helix
#else
namespace helix {
namespace {
constexpr BackendClassOps kAceOps{};
} // namespace
} // namespace helix
#endif

#if HELIX_HAS_QIDI
#include "ams_backend_qidi.h"
namespace helix {
namespace {
constexpr BackendClassOps kQidiOps{&AmsBackendQidi::kTraits, construct_backend<AmsBackendQidi>,
                                   nullptr, AmsBackendQidi::required_status_objects};
} // namespace
} // namespace helix
#else
namespace helix {
namespace {
constexpr BackendClassOps kQidiOps{};
} // namespace
} // namespace helix
#endif

#if HELIX_HAS_SNAPMAKER
#include "ams_backend_multiace.h"
#include "ams_backend_snapmaker.h"
namespace helix {
namespace {
std::unique_ptr<AmsBackend> construct_snapmaker(IMoonrakerAPI* api, IMoonrakerClient* client,
                                                const PrinterDiscovery* hw) {
    if (hw && multiace::present(*hw)) {
        spdlog::debug("[AMS Backend] Snapmaker SnapSwap has multiACE");
        return std::make_unique<AmsBackendMultiAce>(api, client);
    }
    return std::make_unique<AmsBackendSnapmaker>(api, client);
}
constexpr BackendClassOps kSnapmakerOps{&AmsBackendSnapmaker::kTraits, construct_snapmaker, nullptr,
                                        AmsBackendSnapmaker::required_status_objects};
} // namespace
} // namespace helix
#else
namespace helix {
namespace {
constexpr BackendClassOps kSnapmakerOps{};
} // namespace
} // namespace helix
#endif

namespace helix {

namespace {
// Task 15 R2: ACE (500ms REST poll) and AD5X IFS (5s whole-file HTTP poll)
// are the only AMS backends that need a raw HTTP transport instead of the
// WebSocket JSON-RPC channel every other backend rides. On ESP32 that
// transport is the Task 10 HTTP lane (EspHttpLane) — a single dedicated
// worker thread with a bounded, capped fetch model — still an evaluation arm
// pending a real-hardware budget/latency measurement, so it's gated by
// CONFIG_HELIX_AMS_HTTP_POLL_BACKENDS (default n). Flipping the flag is the
// only delta between "unsupported on this screen" and a real backend.
// Desktop always supports both (unconditional real HTTP stack).
#if defined(ESP_PLATFORM)
bool http_poll_ams_backends_supported() {
    // A Kconfig bool set to 'n' (the default) is OMITTED from sdkconfig.h
    // entirely, not defined as 0 — so this must be a preprocessor #if, not a
    // runtime return of the macro's value (the latter fails to compile when
    // off: the token is simply undeclared).
#if CONFIG_HELIX_AMS_HTTP_POLL_BACKENDS
    return true;
#else
    return false;
#endif
}
#else
constexpr bool http_poll_ams_backends_supported() {
    return true;
}
#endif
} // namespace

// ============================================================================
// Endless Spool - shared validation, reset loop and eligibility rule
// ============================================================================

namespace {

/// The one self-backup rejection. Three backends used to phrase this three
/// different ways for the same condition.
AmsError endless_spool_self_backup_error(int slot_index) {
    return AmsError(AmsResult::INVALID_SLOT,
                    "Slot " + std::to_string(slot_index) +
                        " cannot be its own endless spool backup",
                    "A slot cannot back itself up", "Select a different backup slot", slot_index);
}

/// The one read-only rejection, carrying the backend's translated reason.
AmsError endless_spool_read_only_error(helix::printer::EndlessSpoolRestriction restriction) {
    std::string reason = helix::printer::endless_spool_restriction_text(restriction);
    return AmsError(AmsResult::NOT_SUPPORTED,
                    "Endless spool is read-only on this backend" +
                        (reason.empty() ? std::string() : " (" + reason + ")"),
                    reason.empty() ? std::string("Endless Spool cannot be changed here") : reason,
                    "");
}

} // namespace

int AmsBackend::endless_spool_slot_count() const {
    return get_system_info().total_slots;
}

AmsError AmsBackend::set_endless_spool_backup(int slot_index, int backup_slot) {
    const auto caps = get_endless_spool_capabilities();
    if (!caps.available()) {
        return AmsErrorHelper::not_supported("Endless spool");
    }
    if (!caps.editable()) {
        return endless_spool_read_only_error(caps.restriction);
    }

    const int slot_count = endless_spool_slot_count();
    if (slot_count <= 0) {
        return endless_spool_read_only_error(helix::printer::EndlessSpoolRestriction::NotReady);
    }
    const int max_slot = slot_count - 1;

    if (slot_index < 0 || slot_index > max_slot) {
        return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, max_slot);
    }
    if (backup_slot != -1) {
        if (backup_slot < 0 || backup_slot > max_slot) {
            return AmsErrorHelper::invalid_slot(lane_noun(), backup_slot, max_slot);
        }
        if (backup_slot == slot_index) {
            return endless_spool_self_backup_error(slot_index);
        }
    }

    return apply_endless_spool_backup(slot_index, backup_slot);
}

AmsError AmsBackend::set_endless_spool_enabled(bool enabled) {
    const auto caps = get_endless_spool_capabilities();
    if (!caps.available()) {
        return AmsErrorHelper::not_supported("Endless spool");
    }
    if (!caps.switchable) {
        return endless_spool_read_only_error(caps.restriction);
    }
    spdlog::info("[AMS Backend] Switching Endless Spool {}", enabled ? "on" : "off");
    return apply_endless_spool_enabled(enabled);
}

AmsError AmsBackend::reset_endless_spool() {
    const auto caps = get_endless_spool_capabilities();
    if (!caps.available()) {
        return AmsErrorHelper::not_supported("Reset endless spool");
    }
    if (!caps.editable()) {
        return endless_spool_read_only_error(caps.restriction);
    }

    // Same guard as set_endless_spool_backup(): a backend that is editable() but
    // has not yet reported total_slots would otherwise skip the loop entirely and
    // hand back success() — the user confirms a destructive warning, nothing is
    // cleared, and nothing says so.
    const int slot_count = endless_spool_slot_count();
    if (slot_count <= 0) {
        return endless_spool_read_only_error(helix::printer::EndlessSpoolRestriction::NotReady);
    }
    spdlog::info("[AMS Backend] Clearing endless spool backups for {} slots", slot_count);

    // Continue past failures so as many slots as possible end up cleared, and
    // report the first error.
    AmsError first_error = AmsErrorHelper::success();
    for (int slot = 0; slot < slot_count; ++slot) {
        AmsError result = set_endless_spool_backup(slot, -1);
        if (!result.success()) {
            spdlog::error("[AMS Backend] Failed to clear endless spool backup for slot {}: {}",
                          slot, result.technical_msg);
            if (first_error.success()) {
                first_error = result;
            }
        }
    }
    return first_error;
}

helix::printer::BackupEligibility
AmsBackend::endless_spool_backup_eligibility(int slot_index, int backup_slot) const {
    using helix::printer::BackupEligibility;
    if (slot_index < 0 || backup_slot < 0 || slot_index == backup_slot) {
        return BackupEligibility::Incompatible;
    }
    // An unknown material on either side is eligible rather than blocking a
    // slot the user has simply not labelled yet.
    const std::string material = get_slot_info(slot_index).material;
    const std::string backup_material = get_slot_info(backup_slot).material;
    if (material.empty() || backup_material.empty()) {
        return BackupEligibility::Eligible;
    }
    if (!filament::materials_compatible(material, backup_material)) {
        return BackupEligibility::Incompatible;
    }
    // Same polymer. A filled grade still prints, so this is a heads-up rather
    // than a refusal - see BackupEligibility's own note.
    return filament::grades_match(material, backup_material) ? BackupEligibility::Eligible
                                                             : BackupEligibility::GradeDiffers;
}

OperationStepModel operation_step_model(const AmsBackend& backend, StepOperationType op) {
    if (const IStepOperation* const steps = backend.as<IStepOperation>()) {
        return steps->get_operation_step_model(op);
    }
    OperationStepModel model;
    for (const auto& p : backend.toolchange_phase_template(op)) {
        model.steps.push_back({p.label, -1, p.optional, false, std::nullopt});
    }
    return model;
}

lv_subject_t* operation_step_index_subject(AmsBackend& backend, StepOperationType op) {
    if (IStepOperation* const steps = backend.as<IStepOperation>()) {
        return steps->get_operation_step_index_subject(op);
    }
    if (!backend.toolchange_phase_template(op).empty()) {
        return AmsState::instance().get_toolchange_step_subject();
    }
    return nullptr;
}

std::vector<helix::printer::EnvironmentZone> AmsBackend::get_environment_zones(int unit) const {
    using helix::printer::EnvironmentZone;
    using helix::printer::ZoneDryingState;

    std::vector<EnvironmentZone> zones;
    const AmsSystemInfo info = get_system_info();

    for (size_t u = 0; u < info.units.size(); ++u) {
        const int unit_idx = static_cast<int>(u);
        if (unit >= 0 && unit_idx != unit) {
            continue;
        }
        const AmsUnit& au = info.units[u];
        const DryerInfo dryer = get_dryer_info(unit_idx);
        // Only a running cycle is visible here. Queued needs a firmware report that
        // this layer does not receive, so a backend that caps concurrent heaters
        // overrides to say so.
        const ZoneDryingState state =
            dryer.active ? ZoneDryingState::Active : ZoneDryingState::Idle;

        const bool per_slot = std::any_of(au.slots.begin(), au.slots.end(), [](const SlotInfo& s) {
            return s.environment.has_value();
        });

        if (per_slot) {
            for (const SlotInfo& s : au.slots) {
                if (!s.environment.has_value()) {
                    continue;
                }
                // slot_index is unit-relative; a zone's gates are global.
                const int gate = au.first_slot_global_index + s.slot_index;
                EnvironmentZone z;
                z.id = au.name + "#" + std::to_string(gate);
                z.gates = {gate};
                z.unit_index = unit_idx;
                z.env = *s.environment;
                z.dryer = dryer;
                // A dryer that heats gates one by one reports each lane's own part in
                // the cycle; one running gate does not make the whole unit Active.
                z.state = dryer.supports_gate_selection ? s.drying_state : state;
                if (dryer.supports_gate_selection) {
                    z.dryer.active = z.state == ZoneDryingState::Active;
                }
                zones.push_back(std::move(z));
            }
        } else if (au.environment.has_value() || dryer.supported) {
            EnvironmentZone z;
            // Falls back to an ordinal rather than the empty string: AmsUnit::name
            // carries no non-empty invariant, and two empty names would otherwise
            // collide on id == "" and let update_from_backend()'s id-match bind the
            // wrong zone's values.
            z.id = au.name.empty() ? "unit-" + std::to_string(unit_idx) : au.name;
            z.label = au.display_name.empty() ? au.name : au.display_name;
            z.unit_index = unit_idx;
            z.gates.reserve(static_cast<size_t>(au.slot_count));
            for (int g = 0; g < au.slot_count; ++g) {
                z.gates.push_back(au.first_slot_global_index + g);
            }
            if (au.environment.has_value()) {
                z.env = *au.environment;
            }
            z.dryer = dryer;
            z.state = state;
            zones.push_back(std::move(z));
        }
    }
    return zones;
}

AmsError AmsBackend::start_drying_request(const DryingRequest& request) {
    if (!request.gates.empty() || request.rotate) {
        return AmsErrorHelper::not_supported("Drying selected gates");
    }
    if (request.humidity_pct) {
        return start_drying_to_humidity(request.temp_c, request.duration_min, *request.humidity_pct,
                                        request.fan_pct, request.unit);
    }
    return start_drying(request.temp_c, request.duration_min, request.fan_pct, request.unit);
}

AmsError AmsBackend::stop_drying_gates(int unit, const std::vector<int>& gates) {
    if (!gates.empty()) {
        return AmsErrorHelper::not_supported("Drying selected gates");
    }
    return stop_drying(unit);
}

HeadFilamentOps AmsBackend::head_filament_ops(int /*head*/) const {
    HeadFilamentOps ops;
    if (unload_is_manual()) {
        ops.unload = FilamentOpSupport::Manual;
        ops.unload_reason = lv_tr("This printer has no automatic unload. Cut the filament at the "
                                  "head and pull it out by hand.");
    }
    return ops;
}

std::optional<int> AmsBackend::head_source_slot(int head, const AmsSystemInfo& info) const {
    for (int slot = 0; slot < info.total_slots; ++slot) {
        const SlotInfo* lane = info.get_slot_global(slot);
        if (lane && lane->mapped_tool == head && head_for_slot(slot) == head) {
            return slot;
        }
    }
    return std::nullopt;
}

AmsError AmsBackend::unload_active_filament() {
    // Single source of truth for "unload active slot". Reads current_slot ONCE
    // from the same get_system_info() snapshot the caller would use, then
    // forwards. Backends' unload_filament overrides no longer re-resolve -1 →
    // current_slot themselves, so the UI's "is anything loaded?" check and the
    // unload call can't diverge on different snapshots.
    //
    // If current_slot is -1 (no active tool), -1 is forwarded — each backend
    // documents its own behavior for that case (Snapmaker: bare leaf macro,
    // Toolchanger: not_loaded, AFC: bare TOOL_UNLOAD).
    return unload_filament(get_system_info().current_slot);
}

// Own-write spool-id expectations. Both methods run under the subclass's
// mutex_ (see the @warning in ams_backend.h) — the same discipline as every
// other protected hook on this base.
void AmsBackend::record_own_spool_write(int slot_index, int new_id, int previous_firmware_id) {
    // An unlink: nothing will echo but an id Rule 1 already ignores, so a
    // pending expectation must not outlive the write it belonged to.
    if (new_id <= 0) {
        own_write_expectations_.erase(slot_index);
        return;
    }
    auto it = own_write_expectations_.find(slot_index);
    if (it != own_write_expectations_.end() && previous_firmware_id == it->second.second) {
        // Chained re-link before the first echo landed (42->169 then
        // 169->180): the caller's "previous" is our own prior write mirrored
        // back, not firmware truth. Keep the ORIGINAL previous id so stale
        // 42 frames stay suppressed too.
        it->second.second = new_id;
        return;
    }
    own_write_expectations_[slot_index] = {previous_firmware_id, new_id};
}

std::pair<int, int> AmsBackend::own_write_expectation(int slot_index, int firmware_id) {
    auto it = own_write_expectations_.find(slot_index);
    if (it == own_write_expectations_.end())
        return {0, 0};

    const int old_id = it->second.first;
    const int new_id = it->second.second;
    if (firmware_id == new_id || (firmware_id > 0 && firmware_id != old_id)) {
        // Either the echo landed (firmware now agrees with the override) or
        // firmware moved to a third id — a genuine external change. Both end
        // the expectation; neither frame needs Rule-1 suppression.
        own_write_expectations_.erase(it);
        return {0, 0};
    }
    // firmware_id == old_id: a stale pre-echo frame — suppress Rule 1 for
    // this poll, keep the entry for the next one. firmware_id <= 0: no
    // signal; the echo may still be in flight, so the entry survives.
    return {old_id, new_id};
}

helix::ams::BindingVerdict AmsBackend::reconcile_lane_binding(int slot_index,
                                                              int firmware_spool_id) {
    helix::ams::BindingReading reading;
    reading.firmware_spool_id = firmware_spool_id;
    reading.printer_reports_spool_ids = printer_reports_spool_ids();
    reading.keep_spool_info_on_eject =
        helix::SettingsManager::instance().get_ams_keep_spool_info_on_eject();
    std::tie(reading.own_write_old_id, reading.own_write_new_id) =
        own_write_expectation(slot_index, firmware_spool_id);
    return helix::ams::reconcile_binding(lane_id(slot_index), reading);
}

void AmsBackend::update_slot_weight(int slot_index, float remaining_weight_g, float total_weight_g,
                                    bool persist) {
    // File the reading before the backend stores it, so a backend that refuses
    // the write still leaves the meter's own number on the lane rather than a
    // record that disagrees with every store.
    //
    // Only what this call states is filed. A total below zero is the caller
    // saying it has no total to report, and the record says so rather than
    // carrying an old one forward: an unobserved total leaves whatever the
    // backend holds standing, which is the same "leave it unchanged" the
    // parameter asks for.
    helix::ams::Observation metered(helix::ams::ObservationSource::Metered);
    if (remaining_weight_g >= 0.0F) {
        metered.remaining_weight_g = remaining_weight_g;
    }
    if (total_weight_g >= 0.0F) {
        metered.total_weight_g = total_weight_g;
    }
    if (metered.remaining_weight_g.has_value() || metered.total_weight_g.has_value()) {
        helix::ams::ingest(lane_id(slot_index), metered);
    }

    update_slot_weight_impl(slot_index, remaining_weight_g, total_weight_g, persist);
}

void AmsBackend::persist_external_identity(int slot_index) {
    const std::optional<helix::ams::Observation> spoolman =
        helix::ams::lane_sources(lane_id(slot_index)).spoolman;
    if (!spoolman.has_value()) {
        return;
    }
    persist_external_identity_impl(slot_index, *spoolman);
}

void AmsBackend::apply_resolved_lane(SlotInfo& slot, int slot_index) {
    helix::ams::apply_resolved(slot, helix::ams::resolved_lane(lane_id(slot_index)));
}

AmsError AmsBackend::commit_user_edit(int slot_index, const SlotInfo& original,
                                      const SlotInfo& info) {
    // The user's statement, answered once from the editor's own before and
    // after. apply_user_edit() records the stored authorship from this same
    // answer rather than diffing its own read of the slot, which a frame
    // landing while the editor was open has already moved.
    helix::ams::Observation declaration = helix::ams::user_edit_observation(original, info);
    if (firmware_stores_color_and_material(slot_index)) {
        declaration.color_rgb.reset();
        declaration.color_name.reset();
        declaration.material.reset();
    }
    const bool binding_changed = original.spoolman_id != info.spoolman_id;

    // An edit that keeps a spool cannot move what the spool states about
    // itself: firmware, the stored record and the lane all get the spool's
    // material, brand, spool name and vendor id instead of the edit's.
    //
    // A catalog pick names the product of the spool that was bound, so binding
    // a different spool takes it off the slot and out of the stored record,
    // as the drop below takes the colour picked for that spool. An unlink
    // keeps it: the slot still describes what is loaded.
    SlotInfo applied = helix::ams::keep_spool_owned_identity(
        original, info, helix::ams::lane_sources(lane_id(slot_index)).spoolman);
    if (binding_changed && info.spoolman_id > 0) {
        applied.catalog_id.clear();
        applied.product_name.clear();
    }

    // The slot's staging stamp before the dispatch: a backend that stages its
    // echo guard inside apply_user_edit() moves it, and a refusal must cancel
    // that staging without erasing the armed guard of an earlier edit whose
    // write did go out.
    const std::uint64_t echo_sequence = own_write_echo_sequence(slot_index);
    AmsError err = apply_user_edit(slot_index, applied, declaration);
    // A partly applied edit still changed what it applied: a binding that
    // reached firmware is bound whatever the call says about the rest.
    if (!err.success() && !err.partially_applied) {
        // The dispatch refused the edit, so firmware holds nothing it could
        // echo back. A backend that had staged its guard by then would
        // withhold the next genuine reading as its own failed write, so the
        // refusal cancels it here rather than in each backend's failure path.
        // Matched to the staging this refusal created: an unmoved stamp means
        // this edit staged nothing, and the cancel restores rather than erases
        // any armed predecessor the staging suspended.
        const std::uint64_t refused_sequence = own_write_echo_sequence(slot_index);
        if (refused_sequence != 0 && refused_sequence != echo_sequence) {
            abandon_own_write_echoes(slot_index, refused_sequence);
        }
        return err;
    }

    const helix::ams::LaneId lane = lane_id(slot_index);

    // A changed binding leaves every record that described the old spool
    // describing one that is no longer on the lane, the user's own included.
    // The Spoolman record outranks the user's, so standing it would keep naming
    // the old spool over an unlink and make a relink's own echo read as someone
    // else's rebind, which erases the stored record the user just saved; a
    // colour the user picked for the old spool would paint over the new one.
    // The id alone decides, so a return to an id the lane held before drops
    // too: telling the same spool back from another carrying that id would take
    // asking Spoolman.
    //
    // Only once the backend applied it: a refused edit leaves the old binding
    // in place, which the records still describe truly. And before the filing
    // below, which amends: dropping first files the new binding on a fresh
    // record, where dropping after would erase it.
    if (binding_changed) {
        helix::ams::drop_previous_spool_declarations(lane);
    }

    // Record the user's statement in the lane model, once the backend has
    // applied it. The lane is the one this edit was written through, so the
    // declaration cannot land on a backend the edit never reached, and a slot
    // the backend refused gets no declaration at all. Every backend paints its
    // slots from this record through apply_resolved_lane(), so it is what the
    // lane shows for each field the user declared.
    helix::ams::commit_slot_edit(lane, declaration);

    // An unlink that kept the slot's identity stops tracking the spool, not
    // what is loaded, so what the slot kept goes back on the lane at the rungs
    // its stored record reloads it on. That record names no spool and, the
    // binding having changed, declares nothing, so the record translation
    // itself files the identity as Remembered and the catalog pick as the
    // user's. An unlink that cleared the slot has nothing to file. Weights are
    // not identity, and the drop leaves the meter's own record standing.
    if (binding_changed && info.spoolman_id <= 0) {
        const helix::ams::FilamentSlotOverride kept = helix::ams::user_override_from_slot_info(
            declaration, applied, applied.material, nullptr);
        helix::ams::file_kept_identity(lane, slot_index, kept);
    }

    // A backend that paints from the lane while it applies an edit painted the
    // lane before the drop and the filings above.
    repaint_slot_from_lane(slot_index);

    // The rest of the edit is saved, but a field the user moved and the spool
    // kept is not, and reporting success would tell them it was.
    const std::vector<std::string_view> kept =
        helix::ams::spool_owned_fields_dropped(original, info, applied);
    if (kept.empty()) {
        return err;
    }
    std::string kept_names;
    for (const std::string_view name : kept) {
        if (!kept_names.empty()) {
            kept_names += ", ";
        }
        kept_names += name;
    }
    if (!err.success()) {
        spdlog::warn("[AMS Backend] slot {} kept spool {}'s {} beside its own error: {}",
                     slot_index, info.spoolman_id, kept_names, err.technical_msg);
        return err;
    }
    AmsError kept_by_spool(AmsResult::WRONG_STATE,
                           fmt::format("slot {} is linked to spool {}; kept Spoolman's {}",
                                       slot_index, info.spoolman_id, kept_names),
                           lv_tr("Brand and material come from Spoolman"),
                           lv_tr("Everything else was saved. Change them in Spoolman."),
                           slot_index);
    kept_by_spool.partially_applied = true;
    return kept_by_spool;
}

std::optional<std::string>
match_material(const std::string& material, const std::vector<std::string>& list,
               const std::vector<std::pair<std::string, std::string>>& aliases) {
    const std::string input_lc = helix::text_io::to_lower(material);

    // Case-insensitive exact match against the whitelist.
    for (const auto& s : list) {
        if (helix::text_io::to_lower(s) == input_lc) {
            return s;
        }
    }

    // Firmware-specific aliases (e.g. "Silk PLA" -> "SILK" on AD5X, which the
    // shared filament DB groups differently).
    for (const auto& [alias, target] : aliases) {
        if (helix::text_io::to_lower(alias) == input_lc) {
            return target;
        }
    }

    // compat_group match via the filament database.
    auto info = filament::find_material(material);
    if (info.has_value() && info->compat_group != nullptr) {
        std::string_view group(info->compat_group);
        for (const auto& s : list) {
            auto s_info = filament::find_material(s);
            if (s_info.has_value() && s_info->compat_group != nullptr &&
                std::string_view(s_info->compat_group) == group) {
                return s;
            }
        }
    }
    return std::nullopt;
}

std::string AmsBackend::normalize_material(const std::string& material) const {
    auto supported = get_supported_materials();
    if (!supported || supported->empty()) {
        return material;
    }
    // Fallback: first whitelist entry (typically the safest / most common).
    return match_material(material, *supported, get_material_aliases())
        .value_or(supported->front());
}

#ifdef HELIX_ENABLE_MOCKS

// Helper to create mock backend with optional features.
//
// `mock_client` (when non-null) is the client the caller wants the AMS mock
// bound to. The AMS mock subscribes to its active-gcode-tool notifications so
// the active-tool indicator follows the gcode (mock-side proxy for production's
// printer.mmu.tool / toolchanger.tool_number).
static std::unique_ptr<AmsBackendMock>
create_mock_with_features(int gate_count, IMoonrakerClient* mock_client = nullptr) {
    auto mock = std::make_unique<AmsBackendMock>(gate_count);

    // Find the moonraker mock to subscribe to. get_moonraker_client_mock() is
    // the registered client narrowed to the concrete mock type — non-null only
    // when the client really is a MoonrakerClientMock, which is what the
    // dynamic_cast here used to establish (the firmware builds -fno-rtti).
    // When the caller named a client explicitly, it only counts if it is that
    // same object. The AmsState init path calls AmsBackend::create(NONE, null,
    // null) before the factory hooks up specific backends, so the registered
    // mock is the only handle we have at that point.
    ::MoonrakerClientMock* mc = get_moonraker_client_mock();
    if (mc && mock_client && static_cast<IMoonrakerClient*>(mc) != mock_client) {
        mc = nullptr;
    }
    if (mc) {
        AmsBackendMock* mock_ptr = mock.get();
        mc->add_active_gcode_tool_observer([mock_ptr](int tool, uint32_t color) {
            mock_ptr->on_simulated_gcode_tool_changed(tool, color);
        });
        spdlog::info("[AMS Backend] Mock backend subscribed to MoonrakerClientMock "
                     "active-gcode-tool notifications");
    } else {
        spdlog::debug("[AMS Backend] No MoonrakerClientMock available; mock backend "
                      "current_tool will stay at default (not simulator-driven)");
    }

    // ========================================================================
    // HELIX_MOCK_AMS — topology/type selection
    // ========================================================================
    const std::string ams_type = helix::mock::effective_mock_ams(std::getenv("HELIX_MOCK_AMS"),
                                                                 std::getenv("HELIX_MOCK_PRINTER"));

    if (!ams_type.empty()) {
        if (ams_type == "afc" || ams_type == "box_turtle" || ams_type == "boxturtle") {
            mock->set_afc_mode(true);
            spdlog::info("[AMS Backend] Mock AFC mode enabled");
        } else if (ams_type == "toolchanger" || ams_type == "tool_changer" || ams_type == "tc") {
            mock->set_tool_changer_mode(true);
            spdlog::info("[AMS Backend] Mock tool changer mode enabled");
        } else if (ams_type == "mixed") {
            mock->set_mixed_topology_mode(true);
            spdlog::info("[AMS Backend] Mock mixed topology mode enabled");
        } else if (ams_type == "multi") {
            mock->set_multi_unit_mode(true);
            spdlog::info("[AMS Backend] Mock multi-unit mode enabled");
        } else if (ams_type == "torture") {
            mock->set_torture_mode(true);
            spdlog::info("[AMS Backend] Mock torture profile enabled (5 units / 16 lanes)");
        } else if (ams_type == "stealth") {
            mock->set_stealth_mode(true);
            spdlog::info("[AMS Backend] Mock stealth profile enabled (5 units / 7 toolheads)");
        } else if (ams_type == "vivid") {
            mock->set_vivid_mixed_mode(true);
            spdlog::info("[AMS Backend] Mock ViViD mixed mode enabled");
        } else if (ams_type == "ifs" || ams_type == "ad5x" || ams_type == "ad5x_ifs") {
            mock->set_ifs_mode(true);
            spdlog::info("[AMS Backend] Mock AD5X IFS mode enabled");
        } else if (ams_type == "htlf_toolchanger" || ams_type == "htlf_tc" || ams_type == "htlf") {
            mock->set_htlf_toolchanger_mode(true);
            spdlog::info("[AMS Backend] Mock HTLF+Toolchanger mode enabled");
        } else if (ams_type == "snapmaker" || ams_type == "snapswap" || ams_type == "u1") {
            mock->set_snapmaker_mode(true);
            spdlog::info("[AMS Backend] Mock Snapmaker U1 mode enabled");
        }
    }

    // ========================================================================
    // HELIX_MOCK_REMAP_READY — declared remap route, not usable yet
    // ========================================================================
    // The AD5X-IFS-before-`_IFS_VARS` shape: RemapStrategy::Native declared,
    // remap_ready() false, so every "can this printer remap" gate answers no.
    // It is the state all three readiness-gated UI surfaces act on (the
    // pre-flight Remap button, the detail-view chip tap, the AMS edit overlay's
    // tool dropdown), and without this knob it is unreachable in a --test run,
    // which is how a backend could declare a route it cannot use and no mock run
    // would show it.
    if (const char* ready_env = std::getenv("HELIX_MOCK_REMAP_READY")) {
        if (std::string(ready_env) == "0") {
            mock->set_remap_ready(false);
            spdlog::info("[AMS Backend] Mock remap route declared but NOT ready "
                         "(AD5X-before-_IFS_VARS shape)");
        }
    }

    // ========================================================================
    // HELIX_MOCK_REMAP — seed per-tool→slot mapping (test-only)
    // CSV of "tool:slot" pairs, e.g. "0:3,2:1". Sets each named slot's
    // firmware tool mapping so FilamentMapper::compute_defaults() resolves the
    // tool to that slot — lets the two-tone swatch show a non-identity remap.
    // ========================================================================
    if (const char* remap_env = std::getenv("HELIX_MOCK_REMAP")) {
        mock->apply_remap_overrides(remap_env);
        spdlog::info("[AMS Backend] Mock remap overrides applied: '{}'", remap_env);
    }

    // ========================================================================
    // HELIX_MOCK_AMS_STATE — visual scenario
    // ========================================================================
    const char* mock_state_env = std::getenv("HELIX_MOCK_AMS_STATE");
    std::string state_scenario;

    if (mock_state_env) {
        state_scenario = helix::text_io::to_lower(mock_state_env);
    }

    if (!state_scenario.empty() && state_scenario != "idle") {
        // All non-idle scenarios are applied after start() for consistency.
        // loading/bypass: require running_=true (use interruptible sleep + thread)
        // error: applied directly in start() (no thread needed, but deferred for uniformity)
        mock->set_initial_state_scenario(state_scenario);
        spdlog::info("[AMS Backend] Mock state scenario: {}", state_scenario);
    }

    // ========================================================================
    // Orthogonal features (kept separate)
    // ========================================================================

    // Enable mock dryer by default (disable with HELIX_MOCK_DRYER=0)
    const char* dryer_env = std::getenv("HELIX_MOCK_DRYER");
    bool dryer_enabled =
        !dryer_env || (std::string(dryer_env) != "0" && std::string(dryer_env) != "false");
    if (dryer_enabled) {
        mock->set_dryer_enabled(true);
        spdlog::info("[AMS Backend] Mock dryer enabled");

        // HELIX_MOCK_DRYER_HUMIDITY_TARGET=1: the dryer also takes a humidity target.
        if (const char* hum = std::getenv("HELIX_MOCK_DRYER_HUMIDITY_TARGET");
            hum && std::string(hum) != "0" && std::string(hum) != "false") {
            mock->set_dryer_humidity_target(true);
        }
        // HELIX_MOCK_DRYER_DEFAULT_HUMIDITY=<pct>: the target a start with none applies.
        if (const char* def = std::getenv("HELIX_MOCK_DRYER_DEFAULT_HUMIDITY")) {
            if (const auto pct = helix::text_io::parse_leading<float>(def); pct && *pct > 0.0f) {
                mock->set_dryer_default_humidity(*pct);
            }
        }

        // Optionally run an active drying session so the countdown ticks live
        // (HELIX_MOCK_DRYING). Uses the real countdown thread, so it respects
        // HELIX_MOCK_DRYER_SPEED. A 6h / 55 C session already ~1/3 through (4:00
        // left) so the countdown and progress bar read consistently with a typical
        // 6h preset.
        if (std::getenv("HELIX_MOCK_DRYING")) {
            mock->set_dryer_initial_elapsed_min(120);
            mock->start_drying(55.0f, 360, 50, 0);
            spdlog::info("[AMS Backend] Mock drying session started (HELIX_MOCK_DRYING)");
        }
    }

    // Environment sensor mode (auto-detects from dryer state if not specified)
    const char* env_mode_env = std::getenv("HELIX_MOCK_AMS_ENV");
    if (env_mode_env) {
        std::string env_mode = helix::text_io::to_lower(env_mode_env);
        mock->set_environment_mode(env_mode);
        spdlog::info("[AMS Backend] Mock environment mode: {}", env_mode);
    }

    // Simulate mid-print tool change progress (3rd of 5 swaps) for visual testing
    mock->set_toolchange_progress(2, 5);

    return mock;
}

// Check if mock mode is requested and not explicitly disabled via HELIX_MOCK_AMS=none
static std::unique_ptr<AmsBackend> try_create_mock(IMoonrakerClient* mock_client = nullptr) {
    const auto* config = get_runtime_config();
    if (!config->should_mock_ams()) {
        return nullptr;
    }

    const std::string mode = helix::mock::effective_mock_ams(std::getenv("HELIX_MOCK_AMS"),
                                                             std::getenv("HELIX_MOCK_PRINTER"));
    if (mode == "none") {
        spdlog::info("[AMS Backend] Mock AMS disabled via HELIX_MOCK_AMS=none");
        return nullptr;
    }
    // The MedusaHC modes are mock HARDWARE, not a mock backend: MoonrakerClientMock
    // seeds the discovery objects and status a real hotend changer publishes, and
    // the point is to exercise the production AmsBackendToolChanger +
    // toolchanger_addon path against them. Declining here is what lets real
    // discovery run, the same escape hatch HELIX_MOCK_AMS=none uses.
    if (helix::mock::medusa_variant(mode) != helix::mock::MedusaVariant::NONE) {
        spdlog::info("[AMS Backend] HELIX_MOCK_AMS={} selects mock hardware, not a mock "
                     "backend - deferring to real discovery",
                     mode);
        return nullptr;
    }
    // multiACE on a U1 and INDX: mock hardware the same way. MoonrakerClientMock
    // seeds their objects and frames, and the production AmsBackendMultiAce /
    // AmsBackendIndx run against them (multiACE pairs with
    // HELIX_MOCK_PRINTER=snapmaker_u1).
    if (mode == "multiace" || mode == "indx") {
        spdlog::info("[AMS Backend] HELIX_MOCK_AMS={} selects mock hardware, not a mock "
                     "backend - deferring to real discovery",
                     mode);
        return nullptr;
    }
    // Personas that model firmware HelixScreen talks to through a production
    // backend are mock hardware too: decline, and real discovery builds that
    // backend against the mock's objects. Decided from the header-only persona table: the
    // firmware build's link check reads this object and must find no MoonrakerClientMock symbol.
    const char* printer_env = std::getenv("HELIX_MOCK_PRINTER");
    if (mode.empty() && printer_env && helix::mock::is_hardware_persona(printer_env)) {
        spdlog::info("[AMS Backend] Mock printer persona is mock hardware - deferring to real "
                     "discovery");
        return nullptr;
    }

    spdlog::debug("[AMS Backend] Creating mock backend with {} gates (mock mode enabled)",
                  config->mock_ams_gate_count);
    return create_mock_with_features(config->mock_ams_gate_count, mock_client);
}
#endif

// ============================================================================
// One row per backend
// ============================================================================

namespace {

constexpr BackendClassOps kHappyHareOps{
    &AmsBackendHappyHare::kTraits, construct_backend<AmsBackendHappyHare>,
    AmsBackendHappyHare::owns_filament_sensor, AmsBackendHappyHare::required_status_objects};
constexpr BackendClassOps kAfcOps{&AmsBackendAfc::kTraits, construct_backend<AmsBackendAfc>,
                                  AmsBackendAfc::owns_filament_sensor,
                                  AmsBackendAfc::required_status_objects};
constexpr BackendClassOps kOpenAmsOps{&AmsBackendOpenAms::kTraits,
                                      construct_backend<AmsBackendOpenAms>, nullptr,
                                      AmsBackendOpenAms::required_status_objects};
// Sized and wired by set_discovery() before start().
constexpr BackendClassOps kIndxOps{&AmsBackendIndx::kIndxTraits, construct_backend<AmsBackendIndx>,
                                   nullptr, AmsBackendIndx::required_status_objects};
// The caller names the tools through set_discovered_tools() after creation.
constexpr BackendClassOps kToolChangerOps{&AmsBackendToolChanger::kTraits,
                                          construct_backend<AmsBackendToolChanger>};

// Exact spellings: case matters.
constexpr std::string_view kAfcAliases[] = {"afc", "AFC"};
constexpr std::string_view kHappyHareAliases[] = {"mmu", "happy_hare", "Happy Hare"};
constexpr std::string_view kOpenAmsAliases[] = {"openams", "OpenAMS", "oams_manager"};
constexpr std::string_view kIndxAliases[] = {"indx", "INDX"};
constexpr std::string_view kIfsAliases[] = {"ad5x_ifs", "ad5x ifs", "ad5x", "ifs"};
// The bare "box" is Creality's K2 wording; a QIDI Box needs an explicit spelling.
constexpr std::string_view kCfsAliases[] = {"cfs", "CFS", "box"};
constexpr std::string_view kAceAliases[] = {"valgace", "ValgACE", "bunnyace", "BunnyACE",
                                            "duckace", "DuckACE", "ace",      "ACE Pro"};
constexpr std::string_view kQidiAliases[] = {"qidi_box", "QIDI Box", "qidibox"};
constexpr std::string_view kSnapmakerAliases[] = {"snapmaker", "Snapmaker", "snapswap", "SnapSwap"};
constexpr std::string_view kToolChangerAliases[] = {"toolchanger", "tool_changer", "Tool Changer"};

constexpr AmsBackendRow make_row(AmsType type, const BackendClassOps& ops, bool needs_http_poll,
                                 bool tool_changer, bool filament_system,
                                 AmsNameAliases name_aliases,
                                 const char* telemetry_feature = nullptr) {
    return {type,
            ops.traits,
            ops.construct,
            needs_http_poll,
            ops.owns_filament_sensor,
            ops.required_status_objects,
            tool_changer,
            filament_system,
            name_aliases,
            telemetry_feature};
}

// NONE comes first: it answers for a value with no row. AmsBackend::required_status_objects()
// merges in this order, and a later row wins a key two rows share.
//
//       type, class, needs_http_poll, tool_changer, filament_system, aliases, telemetry feature
constexpr AmsBackendRow kBackendRows[] = {
    make_row(AmsType::NONE, {}, false, false, false, {}),
    make_row(AmsType::AFC, kAfcOps, false, false, true, kAfcAliases, "mmu_afc"),
    make_row(AmsType::HAPPY_HARE, kHappyHareOps, false, false, true, kHappyHareAliases,
             "mmu_happy_hare"),
    make_row(AmsType::OPENAMS, kOpenAmsOps, false, false, true, kOpenAmsAliases),
    make_row(AmsType::INDX, kIndxOps, false, true, false, kIndxAliases),
    make_row(AmsType::AD5X_IFS, kIfsOps, true, false, true, kIfsAliases),
    make_row(AmsType::CFS, kCfsOps, false, false, true, kCfsAliases),
    make_row(AmsType::ACE, kAceOps, true, false, true, kAceAliases),
    make_row(AmsType::QIDI_BOX, kQidiOps, false, false, true, kQidiAliases),
    // A U1 is both: it swaps heads and feeds each from a lane.
    make_row(AmsType::SNAPMAKER, kSnapmakerOps, false, true, true, kSnapmakerAliases),
    make_row(AmsType::TOOL_CHANGER, kToolChangerOps, false, true, false, kToolChangerAliases,
             "tool_changer"),
};

} // namespace

const AmsBackendRow& ams_backend_row(AmsType type) {
    for (const auto& row : kBackendRows) {
        if (row.type == type) {
            return row;
        }
    }
    return kBackendRows[0];
}

bool is_tool_changer(AmsType type) {
    return ams_backend_row(type).tool_changer;
}

bool is_filament_system(AmsType type) {
    return ams_backend_row(type).filament_system;
}

AmsType ams_type_from_string(std::string_view str) {
    for (const auto& row : kBackendRows) {
        for (const std::string_view alias : row.name_aliases) {
            if (str == alias) {
                return row.type;
            }
        }
    }
    return AmsType::NONE;
}

bool AmsBackend::sensor_belongs_to_backend(AmsType type, const std::string& bare_name,
                                           const helix::PrinterDiscovery& discovery) {
    // A backend's named (keyword-free) filament sensors, by the patterns it declares
    // in its own translation unit (#1054). Every other sensor is caught only by the
    // keyword substring path in PrinterHardware.
    const auto owns = ams_backend_row(type).owns_filament_sensor;
    return owns && owns(bare_name, discovery);
}

bool AmsBackend::sensor_name_names_filament_system(std::string_view lower_name) {
    const auto has_any = [lower_name](const auto& keywords) {
        for (const char* keyword : keywords) {
            if (lower_name.find(keyword) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    };
    return has_any(AmsBackendAfc::kSensorNameKeywords) ||
           has_any(AmsBackendHappyHare::kSensorNameKeywords);
}

nlohmann::json AmsBackend::required_status_objects(const helix::PrinterDiscovery& hw) {
    nlohmann::json objects = nlohmann::json::object();
    for (const auto& row : kBackendRows) {
        if (!row.required_status_objects) {
            continue;
        }
        const nlohmann::json from = row.required_status_objects(hw);
        for (auto it = from.begin(); it != from.end(); ++it) {
            objects[it.key()] = it.value();
        }
    }
    return objects;
}

void AmsBackend::init_persistent_state(const std::string& config_dir) {
    AfcMessageDedup::instance().init(config_dir);
}

void AmsBackend::shutdown_persistent_state() {
    AfcMessageDedup::instance().shutdown();
}

std::unique_ptr<AmsBackend> AmsBackend::create(AmsType detected_type, IMoonrakerAPI* api,
                                               IMoonrakerClient* client,
                                               const helix::PrinterDiscovery* hw) {
#ifdef HELIX_ENABLE_MOCKS
    if (auto mock = try_create_mock(client)) {
        return mock;
    }
#endif

    const AmsBackendRow& row = ams_backend_row(detected_type);
    const char* const name = ams_type_to_string(row.type);
    if (row.type == AmsType::NONE) {
        spdlog::debug("[AMS Backend] No AMS detected");
        return nullptr;
    }
    if (!row.construct) {
        spdlog::info("[AMS Backend] {} support not compiled in", name);
        return nullptr;
    }
    if (!api || !client) {
        spdlog::error("[AMS Backend] {} requires IMoonrakerAPI and MoonrakerClient", name);
        return nullptr;
    }
    if (row.needs_http_poll && !http_poll_ams_backends_supported()) {
        spdlog::info("[AMS Backend] AMS backend '{}' unsupported on this screen", name);
        return nullptr;
    }
    spdlog::debug("[AMS Backend] Creating {} backend", name);
    return row.construct(api, client, hw);
}

// ============================================================================
// Routing provenance
// ============================================================================

AmsError AmsBackend::set_tool_mapping(int tool_number, int slot_index) {
    AmsError result = set_tool_mapping_impl(tool_number, slot_index);
    if (!result.success()) {
        return result;
    }
    std::lock_guard<std::mutex> lock(authored_mappings_mutex_);
    authored_mappings_[tool_number] = slot_index;
    return result;
}

bool AmsBackend::firmware_reported_tool_mapping() const {
    return reports_firmware_tool_mapping() && firmware_tool_mapping_generation() > 0;
}

helix::FirmwareRouting AmsBackend::firmware_default_routing() const {
    if (firmware_picks_lanes_at_print()) {
        return helix::FirmwareRouting::picked_at_print();
    }
    // Only a table the firmware keeps, and has reported at least once, is what
    // it routes by. A table no frame has stated yet holds nothing but our own
    // optimistic writes, and a per-print map is not a standing table at all.
    if (!firmware_reported_tool_mapping() || get_remap_strategy() != RemapStrategy::Native) {
        return helix::FirmwareRouting::identity();
    }
    // A tool no lane is mapped to has nowhere to run.
    return helix::FirmwareRouting::from_table(get_tool_mapping());
}

helix::printer::ToolMappingOrigin AmsBackend::tool_mapping_origin() const {
    // A printer that publishes its own live table has already said what will
    // print. Nothing about the shape of that answer makes it less true, so it
    // needs no second opinion from us.
    if (firmware_reported_tool_mapping()) {
        return helix::printer::ToolMappingOrigin::Deliberate;
    }

    // Otherwise the routing is vouched for only where it still reflects a lane
    // somebody aimed a tool at here. A table that has moved on since is back to
    // speaking for itself.
    const std::vector<int> routing = get_tool_mapping();
    std::lock_guard<std::mutex> lock(authored_mappings_mutex_);
    for (const auto& [tool, slot] : authored_mappings_) {
        if (tool >= 0 && static_cast<size_t>(tool) < routing.size() &&
            routing[static_cast<size_t>(tool)] == slot) {
            return helix::printer::ToolMappingOrigin::Deliberate;
        }
    }
    return helix::printer::ToolMappingOrigin::Unvouched;
}

const char* filament_op_eligibility_reason(AmsBackend::FilamentOpEligibility e) {
    switch (e) {
    case AmsBackend::FilamentOpEligibility::Eligible:
        return "";
    case AmsBackend::FilamentOpEligibility::Empty:
        return "empty";
    case AmsBackend::FilamentOpEligibility::AlreadyLoaded:
        return "already loaded";
    case AmsBackend::FilamentOpEligibility::NotLoaded:
        return "not loaded";
    case AmsBackend::FilamentOpEligibility::FeederUnavailable:
        return "feeder not in automatic mode";
    case AmsBackend::FilamentOpEligibility::SensorDisabled:
        return "filament sensor disabled";
    case AmsBackend::FilamentOpEligibility::Busy:
        return "busy";
    case AmsBackend::FilamentOpEligibility::Error:
        return "feeder error";
    case AmsBackend::FilamentOpEligibility::NotSupported:
        return "not supported";
    }
    return "";
}

} // namespace helix
