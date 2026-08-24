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
 * exception code maps to a spaghetti defect (U1 code 2), emits a DetectionEvent.
 * The firmware self-pauses on the same snapshot, so already_paused is always true.
 */
class U1StockSource : public DetectionSource {
  public:
    explicit U1StockSource(helix::PrinterState* state) : state_(state) {}

    /// Unique registration key. Exposed so DetectionManager can match on id()
    /// and static_cast instead of dynamic_cast — the firmware builds -fno-rtti.
    static constexpr const char* SOURCE_ID = "u1_stock";

    std::string id() const override {
        return SOURCE_ID;
    }
    bool available() const override {
        return capable_;
    }
    bool can_tune() const override {
        return true;
    }
    void set_callback(Callback cb) override {
        cb_ = std::move(cb);
    }

    void set_capable(bool v) {
        capable_ = v;
    }

    /// Install the print-state observer. Called once.
    void start();

  private:
    void on_print_state(int state_enum);

    helix::PrinterState* state_ = nullptr;
    Callback cb_;
    bool capable_ = false;
    int last_state_ = -1;
    ObserverGuard state_observer_;
};

} // namespace helix::detection
