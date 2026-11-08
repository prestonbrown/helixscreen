// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

/**
 * @brief What anonymous telemetry collects, opened from the wizard's info button
 *
 * One-shot: shown through Modal::show_owned(). Its one button closes it.
 */
class TelemetryInfoModal : public Modal {
  public:
    HELIX_KIND_TAG(TelemetryInfoModal)

    const char* get_name() const override {
        return "Telemetry Info";
    }
    const char* component_name() const override {
        return "telemetry_info_modal";
    }

  protected:
    void on_show() override {
        wire_ok_button("btn_primary");
    }
};
