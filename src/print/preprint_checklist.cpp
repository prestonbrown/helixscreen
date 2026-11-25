// SPDX-License-Identifier: GPL-3.0-or-later

#include "preprint_checklist.h"

#include <algorithm>
#include <iterator>

namespace helix {

namespace {

bool is_listed_phase(PrintStartPhase p) {
    return p != PrintStartPhase::IDLE && p != PrintStartPhase::INITIALIZING &&
           p != PrintStartPhase::COMPLETE;
}

int lookup_seconds(const std::map<int, int>& m, PrintStartPhase p) {
    auto it = m.find(static_cast<int>(p));
    return it == m.end() ? -1 : it->second;
}

bool contains(const std::vector<PreprintStepRow>& rows, PrintStartPhase p) {
    return std::any_of(rows.begin(), rows.end(),
                       [p](const PreprintStepRow& r) { return r.phase == p; });
}

} // namespace

std::vector<PreprintStepRow> build_preprint_checklist(const PreprintChecklistInput& in) {
    std::vector<PreprintStepRow> rows;

    for (PrintStartPhase p : in.done_in_order) {
        if (!is_listed_phase(p) || contains(rows, p))
            continue;
        rows.push_back(
            {p, PreprintStepStatus::Done, std::max(0, lookup_seconds(in.actual_seconds, p))});
    }

    // Expected steps ahead of the current one were run without a signal the
    // collector could see: a G28 on a printer that is already homed never
    // changes homed_axes. They are done, placed by the expected order.
    const auto expected_index = [&in](PrintStartPhase p) {
        const auto it = std::find(in.expected_order.begin(), in.expected_order.end(), p);
        return it == in.expected_order.end()
                   ? -1
                   : static_cast<int>(std::distance(in.expected_order.begin(), it));
    };
    const int current_index = expected_index(in.current);
    for (int i = 0; i < current_index; ++i) {
        const PrintStartPhase p = in.expected_order[static_cast<size_t>(i)];
        if (!is_listed_phase(p) || p == in.current || contains(rows, p))
            continue;
        const auto later = std::find_if(rows.begin(), rows.end(), [&](const PreprintStepRow& r) {
            return expected_index(r.phase) > i;
        });
        rows.insert(later, {p, PreprintStepStatus::Done, -1});
    }

    if (is_listed_phase(in.current) && !contains(rows, in.current)) {
        int predicted = lookup_seconds(in.predicted_seconds, in.current);
        int left = predicted < 0 ? -1 : std::max(0, predicted - in.current_elapsed_seconds);
        rows.push_back({in.current, PreprintStepStatus::Current, left});
    }

    for (PrintStartPhase p : in.expected_order) {
        if (!is_listed_phase(p) || contains(rows, p))
            continue;
        rows.push_back({p, PreprintStepStatus::Upcoming, lookup_seconds(in.predicted_seconds, p)});
    }

    // Drop the oldest done rows first; the current row and upcoming rows stay.
    while (rows.size() > kPreprintChecklistMaxRows) {
        auto it = std::find_if(rows.begin(), rows.end(), [](const PreprintStepRow& r) {
            return r.status == PreprintStepStatus::Done;
        });
        if (it == rows.end())
            break;
        rows.erase(it);
    }
    return rows;
}

PreprintStepPosition preprint_step_position(const std::vector<PreprintStepRow>& rows) {
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].status == PreprintStepStatus::Current)
            return {static_cast<int>(i) + 1, static_cast<int>(rows.size())};
    }
    return {};
}

} // namespace helix
