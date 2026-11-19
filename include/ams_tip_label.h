// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "translation_loader.h"

namespace helix {

/**
 * @brief Get user-friendly step label for tip handling
 * @param method The tip method enum value
 * @return Untranslated English key; the display site passes it through lv_tr()
 */
inline const char* tip_method_step_label(TipMethod method) {
    switch (method) {
    case TipMethod::CUT:
        return TR_NOOP("Cut & retract");
    case TipMethod::TIP_FORM:
        return TR_NOOP("Form tip & retract");
    case TipMethod::NONE:
    default:
        return TR_NOOP("Retract");
    }
}

} // namespace helix
