// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_search_debounce.h"

namespace helix::ui {

SearchDebounce::SearchDebounce(Callback callback, uint32_t delay_ms)
    : timer_(delay_ms), callback_(std::move(callback)) {}

void SearchDebounce::schedule(std::string query) {
    if (query.empty()) {
        // Clearing a filter shows everything again - apply now, and drop any
        // pending trigger so it cannot fire on top of the immediate one.
        cancel();
        if (callback_) {
            callback_(query);
        }
        return;
    }
    query_ = std::move(query);
    timer_.schedule([this]() {
        if (callback_) {
            callback_(query_);
        }
    });
}

} // namespace helix::ui
