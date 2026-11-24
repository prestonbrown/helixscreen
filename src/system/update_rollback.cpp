// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/update_rollback.h"

namespace helix::update {

namespace {
std::optional<bool>& rollback_supported_override() {
    static std::optional<bool> value;
    return value;
}
} // namespace

bool auto_rollback_supported() {
    if (const auto& forced = rollback_supported_override()) {
        return *forced;
    }
    return false;
}

void set_auto_rollback_supported_for_test(std::optional<bool> value) {
    rollback_supported_override() = value;
}

} // namespace helix::update
