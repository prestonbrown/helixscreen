// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wizard_step_logic.h" // helix::WizardPresetPlan
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
namespace helix {
class Config;
}
class IMoonrakerAPI;

namespace helix::wizard {

enum class StepId {
    TouchCalibration = 0,
    Language,
    Wifi,
    Connection,
    PrinterIdentify,
    HeaterSelect,
    FanSelect,
    AmsIdentify,
    LedSelect,
    FilamentSensor,
    InputShaper,
    Summary,
    Telemetry,
};
inline constexpr int STEP_COUNT = 13;

// Plain data needed to decide skips — no LVGL, constructible in tests.
struct StepContext {
    helix::Config* config = nullptr;
    IMoonrakerAPI* api = nullptr;
    WizardPresetPlan preset{}; // {skip_hardware, first_run}
    bool is_subsequent_printer = false;
    // Note: touch-calibration's fbdev check and language's force-step flag live in
    // those steps' own legacy should_skip() (they read process-static state), so
    // they intentionally do NOT need fields here.
};

class Step {
  public:
    virtual ~Step() = default;
    virtual StepId id() const = 0;
    virtual const char* component_name() const = 0; // e.g. "wizard_heater_select"
    virtual const char* log_name() const = 0;
    virtual bool should_skip(const StepContext&) const {
        return false;
    }
    virtual void init_subjects() = 0;
    virtual void register_callbacks() = 0;
    virtual lv_obj_t* create(lv_obj_t* parent) = 0;
    virtual void cleanup() = 0;
    virtual bool is_validated() const {
        return true;
    }
};

const char* to_string(StepId); // for logs/debug

} // namespace helix::wizard
