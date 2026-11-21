// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace helix {

/// One label print whose outcome some later presentation reports. The print
/// and that presentation finish in either order: the outcome waits for the
/// presentation, or the presentation waits for the outcome, and it runs once.
/// Main thread only, like the print callback that settles it.
class LabelOutcomeHold {
  public:
    struct Outcome {
        bool printed = false;
        /// The printer's raw error when it did not print.
        std::string error;
    };
    using Present = std::function<void(const Outcome&)>;

    /// The print finished. A presentation already waiting runs now.
    void settle(bool printed, const std::string& error) {
        if (outcome_) {
            return;
        }
        outcome_ = Outcome{printed, error};
        run();
    }

    /// Report the outcome through @p present: at once when the print has
    /// finished, else when it does. Only the first presentation asked for runs.
    void when_settled(Present present) {
        if (present_ || done_) {
            return;
        }
        present_ = std::move(present);
        run();
    }

    [[nodiscard]] bool settled() const {
        return outcome_.has_value();
    }

  private:
    void run() {
        if (!outcome_ || !present_ || done_) {
            return;
        }
        done_ = true;
        Present present = std::move(present_);
        present_ = nullptr;
        present(*outcome_);
    }

    std::optional<Outcome> outcome_;
    Present present_;
    bool done_ = false;
};

} // namespace helix
