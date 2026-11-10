// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "led/led_backend.h"
#include "led/led_devices.h"
#include "src/ui/panel_widgets/tile_sizing.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"
#include "subject_managed_panel.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace helix {
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

namespace helix {

/// Whether a light tile this many tracks wide has room for its › zone.
bool light_tile_is_wide(int colspan);

/// Width a wide light tile gives its › zone: at least #button_height, and the
/// zone's glyph at the widest it draws (one rung under the bulb's top rung).
int light_chevron_reserve_px();

/// The devices the light-button picker lists, before its fixed "All lights" row.
std::vector<led::LedStripInfo> light_picker_devices();

/// How a light button's bulb reads for the devices it drives.
struct LightIconLook {
    bool unknown = true;  ///< no target reports a state
    int brightness = 0;   ///< 0 while nothing is on
    bool has_rgb = false; ///< false: the theme's light color
    uint32_t rgb = 0xFFFFFF;
};

/// On while any target is on, at the brightest one's level, in the first lit
/// color-capable target's hue.
LightIconLook light_icon_look(const std::vector<led::DeviceState>& states);

/// Acknowledge a tap on a light whose state nothing will report: @p icon shows
/// the lamp color, fades, then returns to the color it had before the flash.
void flash_light_icon(lv_obj_t* icon);

/// One light button: on the home grid it drives the device its `led` config
/// names, "all" for every switchable device, or the chamber light when unset.
class LedWidget : public TiledPanelWidget {
  public:
    LedWidget(const std::string& instance_id, PrinterState& printer_state, IMoonrakerAPI* api);
    ~LedWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    std::string get_component_name() const override {
        return "panel_widget_led";
    }
    const char* id() const override {
        return instance_id_.c_str();
    }
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;

    /// Measured against the width the bulb keeps, as on_size_changed() does:
    /// a box two cells wide gives the › zone its reserve.
    bool fits_at(int width_px, int height_px) const override {
        const bool wide = width_px >= 2 * sizing_.whole_cell_px();
        return sizing_.fits(wide ? width_px - light_chevron_reserve_px() : width_px, height_px);
    }

    const char** xml_attrs() const override {
        return const_cast<const char**>(attrs_.data());
    }

    /// Point this button at @p key (a device id or kLightButtonAll) and save it.
    void select_light(const std::string& key);

    /// The device ids a tap switches.
    std::vector<std::string> targets() const;

    /// The device the › zone opens the LEDs overlay on.
    std::string overlay_device() const;

    const std::string& light_key() const {
        return led_key_;
    }

    /// The bulb for this button's targets. All lights has no one device to take
    /// a hue from, so it lights in the theme's lamp color.
    LightIconLook icon_look() const;

    // XML event callbacks (public for early registration in register_led_widget)
    static void light_toggle_cb(lv_event_t* e);
    static void light_more_cb(lv_event_t* e);
    static void led_picker_row_cb(lv_event_t* e);

  private:
    /// "This button controls": one row per switchable device, then All lights.
    class LedPicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(LedPicker)

      public:
        explicit LedPicker(LedWidget& owner) : owner_(owner) {}
        LedWidget& owner() {
            return owner_;
        }
        /// The device behind each row as the picker drew it, so a tap picks
        /// what the user saw even if the device list changed since.
        std::vector<std::string> row_ids;

      protected:
        const char* xml_component_name() const override {
            return "led_picker";
        }
        /// Same clamp as the fan picker: readable on a 480px panel, not sprawling on 1024px.
        CardWidth card_width() const override {
            return {30, 160, 240};
        }
        void on_created(lv_obj_t* menu) override;

      private:
        LedWidget& owner_;
    };

    /// Widgets that are attached now; an event's user_data is trusted only when
    /// it is one of these.
    static std::unordered_set<LedWidget*>& live_instances();
    static LedWidget* from_event(lv_event_t* e);

    std::string instance_id_;
    std::string led_key_;

    helix::ui::WidgetRef widget_obj_;
    helix::ui::WidgetRef parent_screen_;
    helix::ui::WidgetRef light_icon_;

    SubjectManager subjects_;
    lv_subject_t name_subject_{};
    char name_buf_[64] = {};
    lv_subject_t wide_subject_{};
    std::string name_subject_name_;
    std::string wide_subject_name_;
    std::vector<std::string> attr_storage_;
    std::vector<const char*> attrs_;

    ObserverGuard led_version_observer_;
    ObserverGuard led_state_observer_;

    LedPicker picker_{*this};

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs, so a queued observer callback never reaches a
    // half-destroyed widget (see temp_stack_widget.h).
    helix::AsyncLifetimeGuard lifetime_;

    void handle_light_toggle();
    void update_light_icon();
    void bind_led();
    void show_led_picker();
};

} // namespace helix
