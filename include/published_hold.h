// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <utility>

namespace helix {

/// Points a published image at @p next (or at nothing when it is empty), then
/// lets go of what @p held kept alive. In that order, a widget showing the old
/// image never reads it after it is freed.
template <typename Holder, typename Publish>
void republish_held(Holder& held, Holder next, Publish&& publish) {
    publish(next);
    held = std::move(next);
}

} // namespace helix
