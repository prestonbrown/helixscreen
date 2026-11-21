// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "color_picker_model.h"
#include "color_recents.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * @file ui_color_picker_view.h
 * @brief The one embeddable color picker: a grid of the caller's palette, HSV
 *        sliders with a hex keypad, and a strip of the caller's colors.
 *
 * The picker is never a modal itself. A host puts it inline in a page, in an
 * overlay page or in a modal (ColorPickerModal), and puts the readout
 * (<color_picker_readout/>) and the Grid | Sliders switch (<color_picker_tabs/>)
 * in its own header when it has one.
 *
 * The XML binds process-wide subjects (color_picker_*), so one view is live
 * at a time: the last one attached. When it detaches, the view attached before
 * it (a page under a modal's picker, say) shows its own state again and works.
 *
 * @code
 * picker_.attach(body, slot_rgb, {ColorPalette::Filament, ColorStrip::FilamentRecents});
 * picker_.on_change([this](uint32_t rgb) { draft_.color = rgb; });
 * @endcode
 */

namespace helix::ui {

using helix::ColorPalette;

/// What the strip under the picker holds.
enum class ColorStrip : uint8_t {
    FilamentRecents, ///< ColorRecents::Kind::Filament, removable by long press
    LedRecents,      ///< ColorRecents::Kind::Led, removable by long press
    Custom,          ///< ColorPickerConfig::custom_strip, fixed
};

struct ColorPickerConfig {
    ColorPalette palette = ColorPalette::Filament;
    ColorStrip strip = ColorStrip::FilamentRecents;
    /// The strip's caption; empty means "Recent".
    std::string strip_caption;
    /// The strip's colors when strip is Custom.
    std::function<std::vector<uint32_t>()> custom_strip;
    /// Show the readout and the switch above the body: a host with no header.
    bool tabs_in_body = false;
    /// A thin strip of small chips, leaving the grid the height (the theme editor).
    bool compact_strip = false;
};

class ColorPickerView {
  public:
    enum class Tab : int { Grid = 0, Sliders = 1 };

    ColorPickerView() = default;
    ~ColorPickerView();
    ColorPickerView(const ColorPickerView&) = delete;
    ColorPickerView& operator=(const ColorPickerView&) = delete;

    /// Build the picker in @p container (it fills it) showing @p initial_rgb.
    /// Starts on the Grid tab with the keypad closed.
    void attach(lv_obj_t* container, uint32_t initial_rgb, const ColorPickerConfig& config);
    /// Give up the shared subjects and stop reporting changes. The widgets
    /// stay in the host's tree, which deletes them.
    void detach();
    bool attached() const {
        return root_ != nullptr;
    }
    lv_obj_t* root() const {
        return root_;
    }

    /// Show @p rgb without firing on_change.
    void set_color(uint32_t rgb);
    uint32_t color() const {
        return model_.rgb();
    }
    /// Fired on every pick, slider drag step, stepper press and keypad OK.
    void on_change(std::function<void(uint32_t rgb)> callback);

    void set_tab(Tab tab);
    Tab tab() const {
        return tab_;
    }

    /// Name the readout shows instead of the color's nearest name (the theme
    /// role being edited); empty restores the color name.
    void set_label(const std::string& label);

    /// Re-read the strip's colors (a Custom strip whose source changed).
    void refresh_strip();

    // --- Interactions, called by the XML callbacks; public so tests can drive them.
    void pick_cell(int index);
    void pick_chip(int index);
    void hold_chip(int index);
    void finish_strip_edit();
    void slider_changed(HsvChannel channel);
    void step(HsvChannel channel, int direction);
    void open_keypad();
    void keypad_key(char c);
    void keypad_backspace();
    void keypad_cancel();
    void keypad_ok();
    bool keypad_open() const {
        return keypad_open_;
    }
    bool strip_editing() const {
        return strip_editing_;
    }
    /// The strip's colors as shown.
    const std::vector<uint32_t>& strip_colors() const {
        return strip_;
    }

    /// The live view the XML callbacks route to, or nullptr.
    static ColorPickerView* active();

  private:
    void apply(uint32_t rgb, bool from_sliders);
    void publish();
    /// Every shared subject from this view's state: on attach, and when a
    /// view attached after it detaches.
    void publish_all();
    void publish_keypad();
    void load_strip();
    /// Shows only the recents that fit the strip whole. A custom strip keeps
    /// every color and scrolls, since each one must stay reachable.
    void fit_strip_to_row();
    void publish_strip_selection();
    bool strip_removable() const;

    /// Nulls itself when the host deletes the picker's widgets.
    WidgetRef root_;
    ColorPickerConfig config_;
    ColorPickerModel model_;
    HexKeypad keypad_;
    Tab tab_ = Tab::Grid;
    bool keypad_open_ = false;
    bool strip_editing_ = false;
    bool publishing_ = false;
    std::string label_;
    std::vector<uint32_t> strip_;
    std::function<void(uint32_t)> on_change_;
};

/// Registers the picker's subjects and XML callbacks. Idempotent; attach() calls it.
void ensure_color_picker_view_subjects();

} // namespace helix::ui
