// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "calibration_result_choices.h"
#include "core/subject.h"
#include "subject_managed_panel.h"

#include <array>
#include <string>
#include <vector>

namespace helix::calibration {

/// One Before / After row of a result page.
struct ResultRow {
    std::string label;
    std::string before;
    std::string after;
};

/// The footer's first line for @p choices: what Keep for Now leaves behind (translated).
[[nodiscard]] std::string result_keep_line(const ResultChoices& choices);
/// The footer's second line: what Save to Config and Discard do (translated).
[[nodiscard]] std::string result_save_line(const ResultChoices& choices);

/**
 * @brief The subjects behind one components/calibration_result.xml
 *
 * Publishes the page's title, summary, up to kRows Before / After rows and the
 * choices' footer, under one prefix (the component's `prefix` prop). The screen
 * owns the callbacks. Main thread only; the view must not move once its
 * subjects are registered.
 */
class CalibrationResultView {
  public:
    static constexpr int kRows = 4;

    /// @p prefix must outlive the view (a string literal).
    explicit CalibrationResultView(const char* prefix);
    CalibrationResultView(const CalibrationResultView&) = delete;
    CalibrationResultView& operator=(const CalibrationResultView&) = delete;

    void init_subjects(SubjectManager& manager);
    void publish(const std::string& title, const std::string& summary,
                 const std::vector<ResultRow>& rows, const ResultChoices& choices);

    [[nodiscard]] const ResultChoices& choices() const {
        return choices_;
    }

  private:
    static constexpr size_t kTextCap = 128;
    struct TextSubject {
        std::string name;
        helix::Subject subject{};
        std::array<char, kTextCap> buf{};
    };
    struct IntSubject {
        std::string name;
        helix::Subject subject{};
    };
    struct Row {
        TextSubject label, before, after;
        IntSubject in_run, keep;
    };

    void init_text(SubjectManager& manager, TextSubject& t, const char* field);
    void init_int(SubjectManager& manager, IntSubject& s, const char* field, int value);
    static void set(TextSubject& t, const std::string& value);

    std::string prefix_;
    TextSubject title_, summary_, status_, status_sub_;
    IntSubject hide_keep_, hide_save_;
    std::array<Row, kRows> rows_;
    ResultChoices choices_;
    bool initialized_ = false;
};

} // namespace helix::calibration
