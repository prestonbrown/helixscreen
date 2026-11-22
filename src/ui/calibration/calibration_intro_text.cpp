// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_intro_text.h"

#include "calibration_history_store.h"
#include "locale_formats.h"

namespace helix::calibration {

std::optional<std::string> last_run_month_day(const std::string& step_id) {
    const HistoryEntry* last = CalibrationHistory::instance().entry(step_id);
    if (!last) {
        return std::nullopt;
    }
    const auto date = parse_iso_date(last->date);
    return date ? locale::format_localized_month_day(&*date) : last->date;
}

} // namespace helix::calibration
