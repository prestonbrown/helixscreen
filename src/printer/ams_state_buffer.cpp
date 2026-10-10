// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_state.h"
#include "ams_state_internal.h"
#include "buffer_reading.h"

#include <cmath>
#include <iterator>

namespace helix {
using ams_state_detail::assert_main_thread;
using ams_state_detail::copy_string_if_changed;

void AmsState::sync_buffer_from_info(const AmsSystemInfo& info, int64_t now_ms) {
    const int unit_count = static_cast<int>(info.units.size());
    // A unit that is gone has no trace to show.
    for (auto it = buffer_traces_.begin(); it != buffer_traces_.end();) {
        it = it->first >= unit_count ? buffer_traces_.erase(it) : std::next(it);
    }

    const BufferReading system = buffer_reading(info, -1);
    publish_buffer_reading(system);
    buffer_traces_[-1].record(now_ms, system);
    for (int u = 0; u < unit_count; ++u) {
        const BufferReading r = buffer_reading(info, u);
        buffer_traces_[u].record(now_ms, r);
    }
}

void AmsState::publish_buffer_reading(const BufferReading& r) {
    lv_subject_set_int(&buffer_present_, r.present() ? 1 : 0);
    lv_subject_set_int(&buffer_slider_, r.has_slider ? 1 : 0);
    lv_subject_set_int(&buffer_bias_pct_, static_cast<int>(std::lround(r.bias * 100.0f)));
    lv_subject_set_int(&buffer_status_, static_cast<int>(r.status));
    lv_subject_set_int(&buffer_gauge_, static_cast<int>(r.gauge));
    lv_subject_set_int(&buffer_value_pct_, r.is_fill() ? r.value_pct : 0);
    lv_subject_set_int(&buffer_target_pct_, r.is_fill() ? r.target_pct : -1);
    copy_string_if_changed(&buffer_label_, buffer_label(r));
    copy_string_if_changed(&buffer_value_text_, buffer_value_text(r).c_str());
    copy_string_if_changed(&buffer_short_text_, buffer_short_text(r).c_str());
    copy_string_if_changed(&buffer_lean_text_, buffer_lean_text(r));
    copy_string_if_changed(&buffer_target_text_, buffer_target_text(r).c_str());
}

const BufferTrace& AmsState::buffer_trace(int unit) const {
    assert_main_thread();
    static const BufferTrace kEmpty;
    const auto it = buffer_traces_.find(unit);
    return it == buffer_traces_.end() ? kEmpty : it->second;
}

} // namespace helix
