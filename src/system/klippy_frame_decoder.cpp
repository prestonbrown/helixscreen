// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "klippy_frame_decoder.h"

#include <spdlog/spdlog.h>

namespace helix {

void KlippyFrameDecoder::feed(const char* data, size_t len,
                              const std::function<void(std::string_view)>& on_frame) {
    if (!data || len == 0) {
        return;
    }

    pending_.append(data, len);

    size_t search_from = 0;
    for (;;) {
        const size_t term = pending_.find(FRAME_TERMINATOR, search_from);
        if (term == std::string::npos) {
            break;
        }
        if (term > search_from) {
            if (on_frame) {
                on_frame(std::string_view(pending_).substr(search_from, term - search_from));
            }
        }
        search_from = term + 1;
    }

    if (search_from > 0) {
        pending_.erase(0, search_from);
    }

    if (pending_.size() > MAX_PENDING_BYTES) {
        spdlog::warn("[KlippyFrame] Discarding {} unterminated bytes - stream is unreliable",
                     pending_.size());
        pending_.clear();
        pending_.shrink_to_fit();
        overflowed_ = true;
    }
}

void KlippyFrameDecoder::reset() {
    pending_.clear();
    pending_.shrink_to_fit();
    overflowed_ = false;
}

} // namespace helix
