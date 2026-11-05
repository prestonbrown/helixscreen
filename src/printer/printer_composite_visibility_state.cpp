// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_composite_visibility_state.cpp
 * @brief Aggregate `has_any_preprint_options` visibility subject
 *
 * Manages the single aggregate subject driving the PRINT OPTIONS card's
 * visibility in print_file_detail.xml. Computed from the PrePrintOption
 * framework's option count, the plugin-gated PRINT_START macro rows, and the
 * (un-gated) timelapse capability.
 */

#include "printer_composite_visibility_state.h"

#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

namespace helix {

void PrinterCompositeVisibilityState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterCompositeVisibilityState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterCompositeVisibilityState] Initializing subjects (register_xml={})",
                  register_xml);

    INIT_SUBJECT_INT(has_any_preprint_options, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(timelapse_owner, static_cast<int>(TimelapseOwner::None), subjects_,
                     register_xml);
    INIT_SUBJECT_INT(timelapse_enabled, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterCompositeVisibilityState] Subjects initialized successfully");
}

void PrinterCompositeVisibilityState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterCompositeVisibilityState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterCompositeVisibilityState::set_timelapse_owner(TimelapseOwner owner) {
    const int value = static_cast<int>(owner);
    if (lv_subject_get_int(&timelapse_owner_) != value) {
        lv_subject_set_int(&timelapse_owner_, value);
    }
}

void PrinterCompositeVisibilityState::set_timelapse_enabled(bool enabled) {
    const int value = enabled ? 1 : 0;
    if (lv_subject_get_int(&timelapse_enabled_) != value) {
        lv_subject_set_int(&timelapse_enabled_, value);
    }
}

void PrinterCompositeVisibilityState::update_visibility(
    bool plugin_installed, const PrinterCapabilitiesState& capabilities,
    size_t framework_option_count, size_t macro_option_count) {
    const bool any_visible = (plugin_installed && macro_option_count > 0) ||
                             lv_subject_get_int(capabilities.subject(Capability::HasTimelapse)) ||
                             framework_option_count > 0;

    const int new_any = any_visible ? 1 : 0;
    lv_subject_set_int(&has_any_preprint_options_, new_any);

    if (!last_log_state_initialized_ || new_any != last_any_ || plugin_installed != last_plugin_) {
        spdlog::debug("[PrinterCompositeVisibilityState] has_any_preprint_options={} (plugin={}, "
                      "framework={}, macro={})",
                      new_any, plugin_installed, framework_option_count, macro_option_count);
        last_any_ = new_any;
        last_plugin_ = plugin_installed;
        last_log_state_initialized_ = true;
    }
}

} // namespace helix
