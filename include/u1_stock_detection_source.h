// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "detection_source.h"
#include "printer_state.h" // PrintJobState

namespace helix::detection {

/**
 * @brief Detection source backed by Snapmaker U1 stock firmware.
 *
 * Observes the print-state-enum subject. On a transition INTO PAUSED, it reads
 * print_stats.exception (already parsed by PrinterPrintState) and, when the
 * exception is the defect_detection module's spaghetti (code 2) or dirty-bed
 * (code 1) report, emits a DetectionEvent. The firmware self-pauses on the
 * same snapshot, so already_paused is always true.
 *
 * The Spaghetti Detection switch is sent to the module (NOODLE_ENABLE), so
 * switching it off stops the firmware pausing prints, not just the dialog.
 *
 * The module's pre-print bed check is switched with DEFECT_DETECTION_CONFIG
 * CLEAN_BED_ENABLE and read back from the defect_detection status object,
 * whose get_status() returns the module's stored config.
 */
class U1StockSource : public DetectionSource {
  public:
    explicit U1StockSource(helix::PrinterState* state) : state_(state) {}

    static constexpr const char* kSourceId = "u1_stock";

    std::string id() const override {
        return kSourceId;
    }
    bool available() const override {
        return capable_;
    }
    bool can_tune() const override {
        return true;
    }
    /// The stock firmware pauses the print on the same snapshot it reports,
    /// so the pause-on-detect setting cannot govern this source.
    bool self_pauses() const override {
        return true;
    }
    void set_callback(Callback cb) override {
        cb_ = std::move(cb);
    }

    void set_capable(bool v) {
        capable_ = v;
    }
    /// Capable exactly while the printer lists the defect_detection module.
    void on_objects_listed(const std::vector<std::string>& objects) override;

    /// DEFECT_DETECTION_CONFIG NOODLE_SENSITIVITY=low, the stock firmware's
    /// one-step-down sensitivity command.
    void tune() override;

    /// DEFECT_DETECTION_CONFIG NOODLE_ENABLE=0|1: the firmware's own
    /// spaghetti detection, which pauses prints by itself, follows the switch.
    void apply_detection_enabled(bool enabled) override;

    bool explains_current_pause() const override;

    bool has_bed_check() const override {
        return capable_;
    }
    void read_bed_check(BedCheckReadCallback done) override;
    void set_bed_check(bool enabled, BedCheckSetCallback done) override;

    /// Install the print-state observer. Called once.
    void start();

  private:
    void evaluate();
    /// The defect kind the latched exception reports, Unknown when none.
    DetectionKind latched_kind() const;

    helix::PrinterState* state_ = nullptr;
    Callback cb_;
    bool capable_ = false;
    bool fired_this_pause_ = false;
    ObserverGuard state_observer_;
    ObserverGuard exception_observer_;
};

} // namespace helix::detection
