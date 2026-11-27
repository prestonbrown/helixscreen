// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "notice.h"

#include <cstddef>
#include <string>
#include <vector>

namespace helix::test {

/// Swaps in the one notice subscriber for its lifetime, so a case reads what
/// production published on the thread that published it. The subscriber is a
/// single slot: two of these must never be alive at once.
class ScopedNoticeCapture {
  public:
    ScopedNoticeCapture() : previous_(helix::set_notice_subscriber(&ScopedNoticeCapture::record)) {
        published().clear();
    }
    ~ScopedNoticeCapture() {
        helix::set_notice_subscriber(previous_);
    }
    ScopedNoticeCapture(const ScopedNoticeCapture&) = delete;
    ScopedNoticeCapture& operator=(const ScopedNoticeCapture&) = delete;

    /// How many notices carry exactly @p message, whatever their severity.
    std::size_t count(const std::string& message) const {
        std::size_t n = 0;
        for (const auto& notice : published()) {
            n += notice.message == message ? 1 : 0;
        }
        return n;
    }

    /// How many carry exactly @p delivery and @p message.
    std::size_t count(helix::NoticeDelivery delivery, const std::string& message) const {
        std::size_t n = 0;
        for (const auto& notice : published()) {
            n += notice.delivery == delivery && notice.message == message ? 1 : 0;
        }
        return n;
    }

    /// Every notice captured since this object was made, oldest first.
    [[nodiscard]] const std::vector<helix::Notice>& all() const {
        return published();
    }

    /// How many ERROR notices end in @p suffix, as a "<name> failed" toast does.
    std::size_t count_errors_ending_with(const std::string& suffix) const {
        std::size_t n = 0;
        for (const auto& notice : published()) {
            const bool ends_with = notice.message.size() >= suffix.size() &&
                                   notice.message.compare(notice.message.size() - suffix.size(),
                                                          suffix.size(), suffix) == 0;
            n += notice.severity == helix::NoticeSeverity::ERROR && ends_with ? 1 : 0;
        }
        return n;
    }

  private:
    static std::vector<helix::Notice>& published() {
        static std::vector<helix::Notice> notices;
        return notices;
    }
    static void record(const helix::Notice& notice) {
        published().push_back(notice);
    }

    helix::NoticeSubscriber previous_;
};

} // namespace helix::test
