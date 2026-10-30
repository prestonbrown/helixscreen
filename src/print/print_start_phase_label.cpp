// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_phase_label.h"

#include "lvgl.h"
#include "translation_loader.h"

namespace helix {

const char* print_start_phase_step_key(PrintStartPhase phase) {
    switch (phase) {
    case PrintStartPhase::HOMING:
        return TR_NOOP("Homing");
    case PrintStartPhase::MOTOR_SYNC:
        return TR_NOOP("Motor sync");
    case PrintStartPhase::HEATING_BED:
        return TR_NOOP("Heating bed");
    case PrintStartPhase::SOAKING:
        return TR_NOOP("Heat soak");
    case PrintStartPhase::HEATING_NOZZLE:
        return TR_NOOP("Heating nozzle");
    case PrintStartPhase::QGL:
        return TR_NOOP("Quad gantry level");
    case PrintStartPhase::Z_TILT:
        return TR_NOOP("Z tilt");
    case PrintStartPhase::BED_MESH:
        return TR_NOOP("Bed mesh");
    case PrintStartPhase::CLEANING:
        return TR_NOOP("Cleaning nozzle");
    case PrintStartPhase::PURGING:
        return TR_NOOP("Purge");
    case PrintStartPhase::IDLE:
    case PrintStartPhase::INITIALIZING:
    case PrintStartPhase::COMPLETE:
        return "";
    }
    return "";
}

const char* print_start_phase_step_label(PrintStartPhase phase) {
    const char* key = print_start_phase_step_key(phase);
    return *key ? lv_tr(key) : key;
}

const char* print_start_phase_signal_message(PrintStartPhase phase) {
    // The pre-print banner is hidden outside a print start, so IDLE has no text.
    switch (phase) {
    case PrintStartPhase::IDLE:
        return "";
    case PrintStartPhase::INITIALIZING:
        return lv_tr("Preparing Print...");
    case PrintStartPhase::HOMING:
        return lv_tr("Homing...");
    case PrintStartPhase::MOTOR_SYNC:
        return lv_tr("Syncing Motors...");
    case PrintStartPhase::HEATING_BED:
        return lv_tr("Heating Bed...");
    case PrintStartPhase::SOAKING:
        return lv_tr("Heat Soaking...");
    case PrintStartPhase::HEATING_NOZZLE:
        return lv_tr("Heating Nozzle...");
    case PrintStartPhase::QGL:
        return lv_tr("Leveling Gantry...");
    case PrintStartPhase::Z_TILT:
        return lv_tr("Z Tilt Adjust...");
    case PrintStartPhase::BED_MESH:
        return lv_tr("Loading Bed Mesh...");
    case PrintStartPhase::CLEANING:
        return lv_tr("Cleaning Nozzle...");
    case PrintStartPhase::PURGING:
        return lv_tr("Purging...");
    case PrintStartPhase::COMPLETE:
        return lv_tr("Starting Print...");
    }
    return "";
}

const std::vector<PhaseOption>& print_start_phase_options() {
    static const std::vector<PhaseOption> kOptions = {
        {"bed_mesh", PrintStartPhase::BED_MESH},  {"qgl", PrintStartPhase::QGL},
        {"z_tilt", PrintStartPhase::Z_TILT},      {"nozzle_clean", PrintStartPhase::CLEANING},
        {"purge_line", PrintStartPhase::PURGING},
    };
    return kOptions;
}

std::optional<PrintStartPhase> print_start_phase_of_option(std::string_view option_id) {
    for (const PhaseOption& option : print_start_phase_options()) {
        if (option.id == option_id) {
            return option.phase;
        }
    }
    return std::nullopt;
}

std::optional<PrintStartPhase> phase_of_category(OperationCategory category) {
    switch (category) {
    case OperationCategory::BED_MESH:
        return PrintStartPhase::BED_MESH;
    case OperationCategory::QGL:
        return PrintStartPhase::QGL;
    case OperationCategory::Z_TILT:
        return PrintStartPhase::Z_TILT;
    case OperationCategory::NOZZLE_CLEAN:
        return PrintStartPhase::CLEANING;
    case OperationCategory::PURGE_LINE:
        return PrintStartPhase::PURGING;
    case OperationCategory::HOMING:
        return PrintStartPhase::HOMING;
    case OperationCategory::CHAMBER_SOAK:
        return PrintStartPhase::SOAKING;
    case OperationCategory::MOTOR_SYNC:
        return PrintStartPhase::MOTOR_SYNC;
    case OperationCategory::BED_LEVEL:
    case OperationCategory::SKEW_CORRECT:
    case OperationCategory::START_PRINT:
    case OperationCategory::UNKNOWN:
        break;
    }
    return std::nullopt;
}

} // namespace helix
