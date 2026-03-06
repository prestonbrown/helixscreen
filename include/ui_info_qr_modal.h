// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"
#include <string>

namespace helix::ui {

/// Reusable info modal with icon, description text, and adaptive QR code.
class InfoQrModal : public Modal {
  public:
    struct Config {
        std::string icon;
        std::string title;
        std::string message;
        std::string url;
        std::string url_text;
    };

    explicit InfoQrModal(Config config);

    const char* get_name() const override { return "Info QR"; }
    const char* component_name() const override { return "info_qr_modal"; }

    bool show_modal(lv_obj_t* parent);

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    Config config_;

    void create_qr_code();
};

} // namespace helix::ui
