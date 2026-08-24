// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "standard_macros.h"

#include "config.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_discovery.h"
#include "state/subject_macros.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>

using namespace helix;

const char* StandardMacroInfo::translated_name() const {
    return lv_tr(display_name.c_str());
}

// ============================================================================
// Slot Definition Data
// ============================================================================

namespace {

/**
 * @brief Detection patterns for each slot
 *
 * Patterns are matched case-insensitively against available macros.
 * First match wins, so order patterns by specificity.
 */
struct SlotPatterns {
    StandardMacroSlot slot;
    std::vector<std::string> patterns;
};

// clang-format off
const std::vector<SlotPatterns> DETECTION_PATTERNS = {
    {StandardMacroSlot::LoadFilament,   {"LOAD_FILAMENT", "LOAD_MATERIAL", "M701"}},
    // HELIX_UNLOAD_FILAMENT (from our macro pack) deliberately outranks
    // Creality's QUIT_MATERIAL — that stock macro purges filament forward and
    // retracts only part of it (a melt-zone clearer for manually-cut
    // filament), not a true unload. A printer's own native unload macros and
    // the MMU M702 keep priority over the override.
    {StandardMacroSlot::UnloadFilament, {"UNLOAD_FILAMENT", "UNLOAD_MATERIAL", "M702", "HELIX_UNLOAD_FILAMENT", "QUIT_MATERIAL"}},
    {StandardMacroSlot::Purge,          {"PURGE", "PURGE_LINE", "PRIME_LINE", "PURGE_FILAMENT", "LINE_PURGE"}},
    {StandardMacroSlot::Pause,          {"PAUSE", "M601"}},
    {StandardMacroSlot::Resume,         {"RESUME", "M602"}},
    {StandardMacroSlot::Cancel,         {"CANCEL_PRINT"}},
    {StandardMacroSlot::BedMesh,        {"BED_MESH_CALIBRATE", "G29"}},
    {StandardMacroSlot::BedLevel,       {"QUAD_GANTRY_LEVEL", "QGL", "Z_TILT_ADJUST"}},
    {StandardMacroSlot::CleanNozzle,    {"CLEAN_NOZZLE", "NOZZLE_WIPE", "WIPE_NOZZLE", "CLEAR_NOZZLE"}},
    {StandardMacroSlot::HeatSoak,       {"HEAT_SOAK", "CHAMBER_SOAK", "SOAK"}},
};
// clang-format on

/**
 * @brief HELIX fallback macros for each slot
 *
 * These are installed by HelixScreen's macro installer.
 * Empty string means no fallback is available.
 */
// clang-format off
const std::map<StandardMacroSlot, std::string> FALLBACK_MACROS = {
    {StandardMacroSlot::LoadFilament,   ""},
    {StandardMacroSlot::UnloadFilament, "HELIX_UNLOAD_FILAMENT"},
    {StandardMacroSlot::Purge,          ""},
    {StandardMacroSlot::Pause,          ""},
    {StandardMacroSlot::Resume,         ""},
    {StandardMacroSlot::Cancel,         ""},
    {StandardMacroSlot::BedMesh,        "HELIX_BED_MESH_IF_NEEDED"},
    {StandardMacroSlot::BedLevel,       ""},
    {StandardMacroSlot::CleanNozzle,    "HELIX_CLEAN_NOZZLE"},
    {StandardMacroSlot::HeatSoak,       ""},
};
// clang-format on

/**
 * @brief Slot metadata
 */
struct SlotMeta {
    std::string name;         ///< Config key: "load_filament"
    std::string display_name; ///< UI label: "Load Filament"
};

// clang-format off
const std::map<StandardMacroSlot, SlotMeta> SLOT_METADATA = {
    {StandardMacroSlot::LoadFilament,   {"load_filament",   "Load Filament"}},
    {StandardMacroSlot::UnloadFilament, {"unload_filament", "Unload Filament"}},
    {StandardMacroSlot::Purge,          {"purge",           "Purge"}},
    {StandardMacroSlot::Pause,          {"pause",           "Pause Print"}},
    {StandardMacroSlot::Resume,         {"resume",          "Resume Print"}},
    {StandardMacroSlot::Cancel,         {"cancel",          "Cancel Print"}},
    {StandardMacroSlot::BedMesh,        {"bed_mesh",        "Bed Mesh"}},
    {StandardMacroSlot::BedLevel,       {"bed_level",       "Bed Level"}},
    {StandardMacroSlot::CleanNozzle,    {"clean_nozzle",    "Clean Nozzle"}},
    {StandardMacroSlot::HeatSoak,       {"heat_soak",       "Heat Soak"}},
};
// clang-format on

/**
 * @brief Convert string to uppercase for case-insensitive comparison
 */
std::string to_upper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

} // namespace

// ============================================================================
// StandardMacros Implementation
// ============================================================================

StandardMacros& StandardMacros::instance() {
    static StandardMacros instance;
    return instance;
}

StandardMacros::StandardMacros() {
    init_slot_definitions();
}

void StandardMacros::init_slot_definitions() {
    slots_.clear();
    slots_.reserve(static_cast<size_t>(StandardMacroSlot::COUNT));

    // Initialize all slots with metadata
    for (int i = 0; i < static_cast<int>(StandardMacroSlot::COUNT); ++i) {
        auto slot = static_cast<StandardMacroSlot>(i);
        StandardMacroInfo info;
        info.slot = slot;

        auto meta_it = SLOT_METADATA.find(slot);
        if (meta_it != SLOT_METADATA.end()) {
            info.slot_name = meta_it->second.name;
            info.display_name = meta_it->second.display_name;
        }

        auto fallback_it = FALLBACK_MACROS.find(slot);
        if (fallback_it != FALLBACK_MACROS.end()) {
            info.fallback_macro = fallback_it->second;
        }

        slots_.push_back(std::move(info));
    }
}

void StandardMacros::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[StandardMacros] Subjects already initialized, skipping");
        return;
    }

    INIT_SUBJECT_INT(macros_version, 0, subjects_, register_xml);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "StandardMacros", []() { StandardMacros::instance().deinit_subjects(); });

    spdlog::trace("[StandardMacros] Subjects initialized (register_xml={})", register_xml);
}

void StandardMacros::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Death signal BEFORE the subjects go away: deinit frees every observer node
    // on them, so outside ObserverGuards must learn they are gone or their next
    // reset() calls lv_observer_remove() on freed memory.
    if (subjects_lifetime_) {
        *subjects_lifetime_ = false;
    }
    subjects_lifetime_ = std::make_shared<bool>(true);

    subjects_.deinit_all();
    subjects_initialized_ = false;

    spdlog::trace("[StandardMacros] Subjects deinitialized");
}

void StandardMacros::bump_version() {
    if (!subjects_initialized_) {
        return;
    }
    // init() runs inside a queue_update() drain on the main thread (see
    // PrinterDiscovery), so setting the subject here is main-thread safe.
    lv_subject_set_int(&macros_version_, lv_subject_get_int(&macros_version_) + 1);
}

void StandardMacros::reset() {
    spdlog::debug("[StandardMacros] Resetting");
    for (auto& slot : slots_) {
        slot.detected_macro.clear();
        // Don't clear configured_macro - that's user config
        // Don't clear fallback_macro - that's static
        // Don't clear missing_macro - it still holds user config the last
        // connected printer could not resolve; the next init() re-decides.
    }
    initialized_ = false;
    bump_version();
}

const StandardMacroInfo& StandardMacros::get(StandardMacroSlot slot) const {
    auto index = static_cast<size_t>(slot);
    if (index >= slots_.size()) {
        spdlog::error("[StandardMacros] Invalid slot index: {}", index);
        static StandardMacroInfo empty_info;
        return empty_info;
    }
    return slots_[index];
}

std::optional<StandardMacroSlot> StandardMacros::slot_from_name(const std::string& name) {
    for (const auto& [slot, meta] : SLOT_METADATA) {
        if (meta.name == name) {
            return slot;
        }
    }
    return std::nullopt;
}

std::string StandardMacros::slot_to_name(StandardMacroSlot slot) {
    auto it = SLOT_METADATA.find(slot);
    if (it != SLOT_METADATA.end()) {
        return it->second.name;
    }
    return "";
}

void StandardMacros::set_macro(StandardMacroSlot slot, const std::string& macro) {
    auto index = static_cast<size_t>(slot);
    if (index >= slots_.size()) {
        spdlog::error("[StandardMacros] set_macro: invalid slot index {}", index);
        return;
    }

    auto& info = slots_[index];
    info.configured_macro = macro;
    // Settings > Macro Buttons offers only names read off this printer
    // (api->hardware().macros()), so a fresh pick resolves by construction. The
    // next init() re-validates it against whatever the printer reports then.
    info.missing_macro.clear();

    spdlog::info("[StandardMacros] Set {} = '{}'", info.slot_name, macro);
    save_to_config();
    bump_version();
}

void StandardMacros::load_from_config() {
    auto* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[StandardMacros] Config not available");
        return;
    }

    for (auto& slot : slots_) {
        std::string path = "/standard_macros/" + slot.slot_name;
        slot.configured_macro = config->get<std::string>(path, "");
        // The stored name is a candidate again until validate_configured() has
        // checked it against the printer that is connected now.
        slot.missing_macro.clear();
        if (!slot.configured_macro.empty()) {
            spdlog::debug("[StandardMacros] Loaded config: {} = {}", slot.slot_name,
                          slot.configured_macro);
        }
    }
}

void StandardMacros::save_to_config() {
    auto* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[StandardMacros] Config not available for save");
        return;
    }

    for (const auto& slot : slots_) {
        std::string path = "/standard_macros/" + slot.slot_name;
        // Persist what the user asked for, not what resolved: init() demotes a
        // configured macro the printer lacks into missing_macro, and this writes
        // every slot, so using configured_macro alone would silently delete the
        // other slots' settings the first time any one of them is changed.
        config->set<std::string>(path, slot.requested_macro());
    }

    if (!config->save()) {
        spdlog::error("[StandardMacros] Failed to save config");
    }
}

bool StandardMacros::execute(StandardMacroSlot slot, IMoonrakerAPI* api, SuccessCallback on_success,
                             ErrorCallback on_error, uint32_t timeout_ms,
                             bool suppress_auto_toast) {
    return execute(slot, api, {}, std::move(on_success), std::move(on_error), timeout_ms,
                   suppress_auto_toast);
}

bool StandardMacros::execute(StandardMacroSlot slot, IMoonrakerAPI* api,
                             const std::map<std::string, std::string>& params,
                             SuccessCallback on_success, ErrorCallback on_error,
                             uint32_t timeout_ms, bool suppress_auto_toast) {
    const auto& info = get(slot);

    if (info.is_empty()) {
        spdlog::debug("[StandardMacros] Slot {} is empty, cannot execute", info.slot_name);
        return false;
    }

    std::string macro_name = info.get_macro();
    if (!api) {
        spdlog::error("[StandardMacros] Cannot execute {}: API is null", macro_name);
        return false;
    }

    spdlog::info("[StandardMacros] Executing {} via {}", info.slot_name, macro_name);
    // suppress_auto_toast is CallerIntent::silent — see rpc_error_policy.h.
    api->advanced().execute_macro(macro_name, params, std::move(on_success), std::move(on_error),
                                  timeout_ms, suppress_auto_toast);
    return true;
}

void StandardMacros::init(const helix::PrinterDiscovery& hardware) {
    spdlog::debug("[StandardMacros] Initializing with hardware discovery");

    // Reset detected macros and restore fallbacks from static table
    for (auto& slot : slots_) {
        slot.detected_macro.clear();

        // Restore fallback from static definition
        auto fallback_it = FALLBACK_MACROS.find(slot.slot);
        if (fallback_it != FALLBACK_MACROS.end()) {
            slot.fallback_macro = fallback_it->second;
        }
    }

    // Load user configuration
    load_from_config();

    // A configured name is only usable if this printer defines it
    validate_configured(hardware);

    // Run auto-detection
    auto_detect(hardware);

    // Check which fallbacks are actually available on this printer
    for (auto& slot : slots_) {
        if (!slot.fallback_macro.empty()) {
            if (!hardware.has_helix_macro(slot.fallback_macro)) {
                spdlog::trace("[StandardMacros] Fallback {} not installed for {}",
                              slot.fallback_macro, slot.slot_name);
                slot.fallback_macro.clear();
            }
        }
    }

    initialized_ = true;

    // Log summary
    int configured = 0, detected = 0, fallback = 0, empty = 0;
    for (const auto& slot : slots_) {
        switch (slot.get_source()) {
        case MacroSource::CONFIGURED:
            configured++;
            break;
        case MacroSource::DETECTED:
            detected++;
            break;
        case MacroSource::FALLBACK:
            fallback++;
            break;
        case MacroSource::NONE:
            empty++;
            break;
        }
    }
    spdlog::debug("[StandardMacros] Initialized: {} configured, {} detected, {} fallback, {} empty",
                  configured, detected, fallback, empty);

    bump_version();
}

void StandardMacros::validate_configured(const helix::PrinterDiscovery& hardware) {
    // Printer presets seed the configured macros from a template machine, so a
    // Voron set up from the FlashForge AD5M preset carries CLEAR_NOZZLE,
    // AUTO_FULL_BED_LEVEL, LOAD_FILAMENT and UNLOAD_FILAMENT — none of which its
    // Klipper defines. Taken at face value those names outrank auto-detection,
    // the HELIX fallbacks and (for load/unload) the whole dispatch ladder, so
    // the buttons rendered as working and the only feedback on a tap was an
    // "Unknown command" rejection from the printer.
    //
    // Mirrors the fallback check further down init(): the printer's own macro
    // list is the authority for every source, not just the HELIX_* ones.
    for (auto& slot : slots_) {
        if (slot.configured_macro.empty()) {
            continue;
        }
        if (hardware.has_macro(slot.configured_macro)) {
            continue;
        }

        spdlog::warn("[StandardMacros] Configured {} macro '{}' is not defined on this printer — "
                     "treating the slot as unassigned",
                     slot.slot_name, slot.configured_macro);
        slot.missing_macro = slot.configured_macro;
        slot.configured_macro.clear();
    }
}

void StandardMacros::auto_detect(const helix::PrinterDiscovery& hardware) {
    spdlog::debug("[StandardMacros] Running auto-detection on {} macros", hardware.macro_count());

    for (const auto& pattern_def : DETECTION_PATTERNS) {
        auto detected = try_detect(hardware, pattern_def.slot, pattern_def.patterns);
        if (!detected.empty()) {
            auto index = static_cast<size_t>(pattern_def.slot);
            if (index < slots_.size()) {
                slots_[index].detected_macro = detected;
                spdlog::trace("[StandardMacros] Detected {} -> {}", slots_[index].slot_name,
                              detected);
            }
        }
    }
}

std::string StandardMacros::try_detect(const helix::PrinterDiscovery& hardware,
                                       [[maybe_unused]] StandardMacroSlot slot,
                                       const std::vector<std::string>& patterns) {
    const auto& macros = hardware.macros();

    for (const auto& pattern : patterns) {
        std::string upper_pattern = to_upper(pattern);
        // Check if the pattern exists as a macro (both are uppercase)
        if (macros.find(upper_pattern) != macros.end()) {
            // Return the pattern as-is (Klipper macros are case-insensitive)
            return pattern;
        }
    }

    return "";
}
