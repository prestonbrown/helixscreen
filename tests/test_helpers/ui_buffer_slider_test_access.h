// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_buffer_slider.h"

namespace helix::ui {

// Friend access to UiBufferSlider's paint record. The painters write which
// gauge they drew; no getter on the production class exposes it, because
// nothing in production reads it. Read-only.
//
// The header declares this class as a friend inside helix::ui, so the
// definition lives in the same namespace and in ONE place, or two test
// translation units defining their own copy would be an ODR violation.
//
class UiBufferSliderTestAccess {
  public:
    /// Whether the slider object has been painted at least once.
    [[nodiscard]] static bool has_painted(const UiBufferSlider& slider) {
        return slider.painted_;
    }

    /// The gauge the most recent paint drew.
    [[nodiscard]] static BufferGauge painted_gauge(const UiBufferSlider& slider) {
        return slider.painted_gauge_;
    }
};

} // namespace helix::ui
