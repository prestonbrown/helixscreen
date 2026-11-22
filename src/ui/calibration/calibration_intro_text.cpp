// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_intro_text.h"

#include "calibration_history_store.h"
#include "core/text_io.h"
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

int intro_chip_index(lv_event_t* e) {
    const auto* text = static_cast<const char*>(lv_event_get_user_data(e));
    if (!text) {
        return -1;
    }
    return text_io::parse_leading<int>(text).value_or(-1);
}

} // namespace helix::calibration
