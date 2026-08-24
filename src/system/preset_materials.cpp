// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "preset_materials.h"

#include "filament_database.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "material_settings_manager.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>

namespace helix::presets {

namespace {

/// Longest label we render: "<brand> <material>" on a preset button.
constexpr size_t LABEL_BUF_SIZE = 48;
/// "260°C / 100°C" plus slack.
constexpr size_t TEMP_BUF_SIZE = 32;

struct SubjectState {
    std::array<lv_subject_t, PRESET_COUNT> name_subjects{};
    std::array<lv_subject_t, PRESET_COUNT> temp_subjects{};
    std::array<std::array<char, LABEL_BUF_SIZE>, PRESET_COUNT> name_bufs{};
    std::array<std::array<char, TEMP_BUF_SIZE>, PRESET_COUNT> temp_bufs{};
    lv_subject_t count_subject{};
    bool ready = false;
};

SubjectState& state() {
    static SubjectState s;
    return s;
}

bool slot_valid(int slot) {
    return slot >= 0 && slot < PRESET_COUNT;
}

void deinit_subjects() {
    auto& s = state();
    if (!s.ready) {
        return;
    }
    for (int i = 0; i < PRESET_COUNT; ++i) {
        lv_subject_deinit(&s.name_subjects[i]);
        lv_subject_deinit(&s.temp_subjects[i]);
    }
    lv_subject_deinit(&s.count_subject);
    s.ready = false;
    spdlog::debug("[PresetMaterials] Subjects deinitialized");
}

} // namespace

std::string name(int slot) {
    if (!slot_valid(slot)) {
        return {};
    }
    return MaterialSettingsManager::instance().get_preset_materials()[static_cast<size_t>(slot)];
}

std::array<std::string, PRESET_COUNT> all() {
    return MaterialSettingsManager::instance().get_preset_materials();
}

std::string display_label(int slot) {
    if (!slot_valid(slot)) {
        return {};
    }
    const std::string material = name(slot);
    auto branded = MaterialSettingsManager::instance().get_preset_filament(slot);
    if (branded && branded->is_branded() && !branded->brand.empty()) {
        // e.g. "Bambu PLA" — brand + generic type are kept in lockstep by
        // MaterialSettingsManager::set_preset_filament()/reassign.
        return branded->brand + " " + material;
    }
    return material;
}

std::string temp_label(int slot) {
    if (!slot_valid(slot)) {
        return {};
    }
    char buf[TEMP_BUF_SIZE];
    auto branded = MaterialSettingsManager::instance().get_preset_filament(slot);
    if (branded && branded->is_branded()) {
        // Exact branded product temps (whole °C ints, same unit as MaterialInfo).
        std::snprintf(buf, sizeof(buf), "%d°C / %d°C", branded->nozzle, branded->bed);
        return buf;
    }
    auto mat = filament::find_material(name(slot));
    if (!mat) {
        return "---";
    }
    std::snprintf(buf, sizeof(buf), "%d°C / %d°C", mat->nozzle_recommended(), mat->bed_temp);
    return buf;
}

void init_subjects() {
    auto& s = state();
    if (s.ready) {
        return; // idempotent
    }

    // Slot-indexed names on purpose. The old material-named subjects
    // ("filament_preset_pla_name") implied slot 0 was always PLA, which is
    // exactly the assumption this module exists to remove.
    static constexpr const char* NAME_SUBJECTS[PRESET_COUNT] = {
        "preset_material_0_name", "preset_material_1_name", "preset_material_2_name",
        "preset_material_3_name"};
    static constexpr const char* TEMP_SUBJECTS[PRESET_COUNT] = {
        "preset_material_0_temps", "preset_material_1_temps", "preset_material_2_temps",
        "preset_material_3_temps"};

    for (int i = 0; i < PRESET_COUNT; ++i) {
        s.name_bufs[i][0] = '\0';
        s.temp_bufs[i][0] = '\0';

        lv_subject_init_string(&s.name_subjects[i], s.name_bufs[i].data(), nullptr,
                               s.name_bufs[i].size(), s.name_bufs[i].data());
        lv_xml_register_subject(nullptr, NAME_SUBJECTS[i], &s.name_subjects[i]);
        SubjectDebugRegistry::instance().register_subject(
            &s.name_subjects[i], NAME_SUBJECTS[i], LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

        lv_subject_init_string(&s.temp_subjects[i], s.temp_bufs[i].data(), nullptr,
                               s.temp_bufs[i].size(), s.temp_bufs[i].data());
        lv_xml_register_subject(nullptr, TEMP_SUBJECTS[i], &s.temp_subjects[i]);
        SubjectDebugRegistry::instance().register_subject(
            &s.temp_subjects[i], TEMP_SUBJECTS[i], LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    }

    lv_subject_init_int(&s.count_subject, PRESET_COUNT);
    lv_xml_register_subject(nullptr, "preset_material_count", &s.count_subject);
    SubjectDebugRegistry::instance().register_subject(&s.count_subject, "preset_material_count",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    s.ready = true;

    // Co-locate init + cleanup so the registration can't be forgotten (CLAUDE.md
    // subject shutdown safety rule).
    StaticSubjectRegistry::instance().register_deinit("PresetMaterials", deinit_subjects);

    refresh_subjects();
    spdlog::debug("[PresetMaterials] Subjects initialized ({} slots)", PRESET_COUNT);
}

void refresh_subjects() {
    auto& s = state();
    if (!s.ready) {
        return; // plain-accessor-only contexts (unit tests) never init subjects
    }
    for (int i = 0; i < PRESET_COUNT; ++i) {
        const std::string label = display_label(i);
        const std::string temps = temp_label(i);
        std::snprintf(s.name_bufs[i].data(), s.name_bufs[i].size(), "%s", label.c_str());
        std::snprintf(s.temp_bufs[i].data(), s.temp_bufs[i].size(), "%s", temps.c_str());
        lv_subject_copy_string(&s.name_subjects[i], s.name_bufs[i].data());
        lv_subject_copy_string(&s.temp_subjects[i], s.temp_bufs[i].data());
    }
}

bool subjects_ready() {
    return state().ready;
}

} // namespace helix::presets
