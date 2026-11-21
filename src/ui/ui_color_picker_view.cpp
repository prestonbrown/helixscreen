// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_color_picker_view.h"

#include "ui_callback_helpers.h"
#include "ui_utils.h"

#include "color_utils.h"
#include "helix/xml/indexed_subject_pool.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace helix::ui {

namespace {

// The picker's state, bound by color_picker_view.xml and its parts. Process-wide:
// one picker is live at a time, and the names must outlive any single view.
struct PickerSubjects {
    lv_subject_t tab{};
    lv_subject_t current{};
    lv_subject_t name{};
    lv_subject_t hex{};
    lv_subject_t hue{}, sat{}, val{};
    lv_subject_t hue_text{}, sat_text{}, val_text{};
    lv_subject_t sat_lo{}, sat_hi{}, val_hi{};
    lv_subject_t cell_selected{};
    lv_subject_t chip_selected{};
    lv_subject_t chip_count{};
    lv_subject_t strip_caption{};
    lv_subject_t strip_editing{};
    lv_subject_t keypad_is_open{};
    lv_subject_t keypad_len{};
    lv_subject_t keypad_text{};
    lv_subject_t keypad_preview{};

    char name_buf[64] = {};
    char hex_buf[16] = {};
    char hue_buf[16] = {};
    char sat_buf[16] = {};
    char val_buf[16] = {};
    char caption_buf[48] = {};
    char keypad_buf[16] = {};

    /// The attached views, oldest first. The subjects show the last one's
    /// state; when it detaches, the one before it shows again.
    std::vector<ColorPickerView*> publishers;
};

std::unique_ptr<PickerSubjects> s_subjects;
helix::xml::IndexedSubjectPool s_cells{"color_picker_cell",
                                       helix::xml::IndexedSubjectPool::Type::Color};
helix::xml::IndexedSubjectPool s_chips{"color_picker_chip",
                                       helix::xml::IndexedSubjectPool::Type::Color};

/// The most chips a strip shows: the recents cap, or a theme's 16 roles.
constexpr size_t kMaxChips = 16;
constexpr size_t kMaxCells = 60;

lv_color_t to_color(uint32_t rgb) {
    return lv_color_hex(rgb & 0xFFFFFF);
}

void register_subject(lv_subject_t& s, const char* name) {
    lv_xml_register_subject(nullptr, name, &s);
}

const char* user_data_of(lv_event_t* e) {
    const auto* ud = static_cast<const char*>(lv_event_get_user_data(e));
    return ud ? ud : "";
}

HsvChannel channel_of(char c) {
    switch (c) {
    case 'h':
        return HsvChannel::Hue;
    case 's':
        return HsvChannel::Saturation;
    default:
        return HsvChannel::Brightness;
    }
}

ColorRecents::Kind recents_kind(ColorStrip strip) {
    return strip == ColorStrip::LedRecents ? ColorRecents::Kind::Led : ColorRecents::Kind::Filament;
}

/// The name the view gives its root, so a callback can find it.
constexpr const char* kRootName = "color_picker_view";

ColorPickerView* current_publisher() {
    return s_subjects && !s_subjects->publishers.empty() ? s_subjects->publishers.back() : nullptr;
}

void push_publisher(ColorPickerView* view) {
    auto& v = s_subjects->publishers;
    v.erase(std::remove(v.begin(), v.end(), view), v.end());
    v.push_back(view);
}

/// Drops @p view; true when it was the one the subjects showed.
bool drop_publisher(ColorPickerView* view) {
    if (!s_subjects) {
        return false;
    }
    const bool was_current = current_publisher() == view;
    auto& v = s_subjects->publishers;
    v.erase(std::remove(v.begin(), v.end(), view), v.end());
    return was_current;
}

/// The view a callback's widget belongs to: the picker root at or above the
/// widget, or, for a control in the host's header (the switch, the readout),
/// the picker inside the nearest ancestor that holds one. The root's user data
/// is its view while attached.
ColorPickerView* view_from_event(lv_event_t* e) {
    for (lv_obj_t* o = lv_event_get_current_target_obj(e); o; o = lv_obj_get_parent(o)) {
        const char* name = lv_obj_get_name(o);
        lv_obj_t* root =
            (name && std::strcmp(name, kRootName) == 0) ? o : lv_obj_find_by_name(o, kRootName);
        if (root) {
            return static_cast<ColorPickerView*>(lv_obj_get_user_data(root));
        }
    }
    return nullptr;
}

/// Routes a callback to the view its widget belongs to.
template <typename Fn> void with_view(lv_event_t* e, Fn&& fn) {
    if (ColorPickerView* view = view_from_event(e); view && view->attached()) {
        fn(*view);
    }
}

void register_callbacks() {
    register_xml_callbacks({
        {"color_picker_tab_cb",
         [](lv_event_t* e) {
             const int tab = std::atoi(user_data_of(e));
             with_view(e, [&](ColorPickerView& v) {
                 v.set_tab(tab == 1 ? ColorPickerView::Tab::Sliders : ColorPickerView::Tab::Grid);
             });
         }},
        {"color_picker_cell_cb",
         [](lv_event_t* e) {
             const int i = std::atoi(user_data_of(e));
             with_view(e, [&](ColorPickerView& v) { v.pick_cell(i); });
         }},
        {"color_picker_chip_cb",
         [](lv_event_t* e) {
             const int i = std::atoi(user_data_of(e));
             with_view(e, [&](ColorPickerView& v) { v.pick_chip(i); });
         }},
        {"color_picker_chip_hold_cb",
         [](lv_event_t* e) {
             const int i = std::atoi(user_data_of(e));
             with_view(e, [&](ColorPickerView& v) {
                 const bool was = v.strip_editing();
                 v.hold_chip(i);
                 if (!was && v.strip_editing()) {
                     end_press(e);
                 }
             });
         }},
        {"color_picker_strip_done_cb",
         [](lv_event_t* e) { with_view(e, [](ColorPickerView& v) { v.finish_strip_edit(); }); }},
        {"color_picker_slider_cb",
         [](lv_event_t* e) {
             const char c = user_data_of(e)[0];
             with_view(e, [&](ColorPickerView& v) { v.slider_changed(channel_of(c)); });
         }},
        {"color_picker_step_cb",
         [](lv_event_t* e) {
             const char* ud = user_data_of(e);
             if (std::strlen(ud) < 2) {
                 return;
             }
             with_view(
                 e, [&](ColorPickerView& v) { v.step(channel_of(ud[0]), ud[1] == '-' ? -1 : 1); });
         }},
        {"color_picker_hex_open_cb",
         [](lv_event_t* e) { with_view(e, [](ColorPickerView& v) { v.open_keypad(); }); }},
        {"color_picker_key_cb",
         [](lv_event_t* e) {
             const char* ud = user_data_of(e);
             with_view(e, [&](ColorPickerView& v) {
                 if (std::strcmp(ud, "back") == 0) {
                     v.keypad_backspace();
                 } else if (ud[0] != '\0') {
                     v.keypad_key(ud[0]);
                 }
             });
         }},
        {"color_picker_keypad_cancel_cb",
         [](lv_event_t* e) { with_view(e, [](ColorPickerView& v) { v.keypad_cancel(); }); }},
        {"color_picker_keypad_ok_cb",
         [](lv_event_t* e) { with_view(e, [](ColorPickerView& v) { v.keypad_ok(); }); }},
    });
}

} // namespace

void ensure_color_picker_view_subjects() {
    if (s_subjects) {
        return;
    }
    s_subjects = std::make_unique<PickerSubjects>();
    auto& s = *s_subjects;
    lv_subject_init_int(&s.tab, 0);
    lv_subject_init_color(&s.current, to_color(0x808080));
    lv_subject_init_string(&s.name, s.name_buf, nullptr, sizeof(s.name_buf), "");
    lv_subject_init_string(&s.hex, s.hex_buf, nullptr, sizeof(s.hex_buf), "");
    lv_subject_init_int(&s.hue, 0);
    lv_subject_init_int(&s.sat, 0);
    lv_subject_init_int(&s.val, 0);
    lv_subject_init_string(&s.hue_text, s.hue_buf, nullptr, sizeof(s.hue_buf), "");
    lv_subject_init_string(&s.sat_text, s.sat_buf, nullptr, sizeof(s.sat_buf), "");
    lv_subject_init_string(&s.val_text, s.val_buf, nullptr, sizeof(s.val_buf), "");
    lv_subject_init_color(&s.sat_lo, to_color(0x808080));
    lv_subject_init_color(&s.sat_hi, to_color(0x808080));
    lv_subject_init_color(&s.val_hi, to_color(0x808080));
    lv_subject_init_int(&s.cell_selected, -1);
    lv_subject_init_int(&s.chip_selected, -1);
    lv_subject_init_int(&s.chip_count, 0);
    lv_subject_init_string(&s.strip_caption, s.caption_buf, nullptr, sizeof(s.caption_buf), "");
    lv_subject_init_int(&s.strip_editing, 0);
    lv_subject_init_int(&s.keypad_is_open, 0);
    lv_subject_init_int(&s.keypad_len, 0);
    lv_subject_init_string(&s.keypad_text, s.keypad_buf, nullptr, sizeof(s.keypad_buf), "");
    lv_subject_init_color(&s.keypad_preview, to_color(0x808080));

    register_subject(s.tab, "color_picker_tab");
    register_subject(s.current, "color_picker_current");
    register_subject(s.name, "color_picker_name");
    register_subject(s.hex, "color_picker_hex");
    register_subject(s.hue, "color_picker_hue");
    register_subject(s.sat, "color_picker_sat");
    register_subject(s.val, "color_picker_val");
    register_subject(s.hue_text, "color_picker_hue_text");
    register_subject(s.sat_text, "color_picker_sat_text");
    register_subject(s.val_text, "color_picker_val_text");
    register_subject(s.sat_lo, "color_picker_sat_lo");
    register_subject(s.sat_hi, "color_picker_sat_hi");
    register_subject(s.val_hi, "color_picker_val_hi");
    register_subject(s.cell_selected, "color_picker_cell_selected");
    register_subject(s.chip_selected, "color_picker_chip_selected");
    register_subject(s.chip_count, "color_picker_chip_count");
    register_subject(s.strip_caption, "color_picker_strip_caption");
    register_subject(s.strip_editing, "color_picker_strip_editing");
    register_subject(s.keypad_is_open, "color_picker_keypad_open");
    register_subject(s.keypad_len, "color_picker_keypad_len");
    register_subject(s.keypad_text, "color_picker_keypad_text");
    register_subject(s.keypad_preview, "color_picker_keypad_preview");
    s_cells.ensure_size(kMaxCells);
    s_chips.ensure_size(kMaxChips);
    register_callbacks();

    StaticSubjectRegistry::instance().register_deinit(
        "ColorPickerView",
        []() {
            if (!s_subjects) {
                return;
            }
            for (const char* n : {"color_picker_tab",           "color_picker_current",
                                  "color_picker_name",          "color_picker_hex",
                                  "color_picker_hue",           "color_picker_sat",
                                  "color_picker_val",           "color_picker_hue_text",
                                  "color_picker_sat_text",      "color_picker_val_text",
                                  "color_picker_sat_lo",        "color_picker_sat_hi",
                                  "color_picker_val_hi",        "color_picker_cell_selected",
                                  "color_picker_chip_selected", "color_picker_chip_count",
                                  "color_picker_strip_caption", "color_picker_strip_editing",
                                  "color_picker_keypad_open",   "color_picker_keypad_len",
                                  "color_picker_keypad_text",   "color_picker_keypad_preview"}) {
                lv_xml_unregister_subject(nullptr, n);
            }
            auto& s = *s_subjects;
            for (lv_subject_t* subject : {&s.tab,
                                          &s.current,
                                          &s.name,
                                          &s.hex,
                                          &s.hue,
                                          &s.sat,
                                          &s.val,
                                          &s.hue_text,
                                          &s.sat_text,
                                          &s.val_text,
                                          &s.sat_lo,
                                          &s.sat_hi,
                                          &s.val_hi,
                                          &s.cell_selected,
                                          &s.chip_selected,
                                          &s.chip_count,
                                          &s.strip_caption,
                                          &s.strip_editing,
                                          &s.keypad_is_open,
                                          &s.keypad_len,
                                          &s.keypad_text,
                                          &s.keypad_preview}) {
                lv_subject_deinit(subject);
            }
            s_cells.reclaim();
            s_chips.reclaim();
            s_subjects.reset();
        },
        StaticSubjectRegistry::Lifetime::Process);
}

ColorPickerView* ColorPickerView::active() {
    return current_publisher();
}

ColorPickerView::~ColorPickerView() {
    detach();
}

void ColorPickerView::attach(lv_obj_t* container, uint32_t initial_rgb,
                             const ColorPickerConfig& config) {
    detach();
    if (!container) {
        return;
    }
    ensure_color_picker_view_subjects();
    config_ = config;
    push_publisher(this);
    tab_ = Tab::Grid;
    keypad_open_ = false;
    strip_editing_ = false;
    keypad_.clear();
    model_ = ColorPickerModel(initial_rgb);
    publish_all();

    const std::string palette_attr = std::to_string(static_cast<int>(config_.palette));
    const char* attrs[] = {
        "palette",      palette_attr.c_str(),
        "tabs_in_body", config_.tabs_in_body ? "1" : "0",
        "chip_size",    config_.compact_strip ? "#cpv_chip_compact" : "#cpv_chip",
        nullptr};
    root_.reset(static_cast<lv_obj_t*>(lv_xml_create(container, "color_picker_view", attrs)));
    if (!root_) {
        spdlog::error("[ColorPickerView] color_picker_view failed to build");
        detach();
        return;
    }
    lv_obj_set_name(root_, kRootName);
    lv_obj_set_user_data(root_, this);
    // Only the palette's own grid is built. This engine's <if> cannot test a
    // component prop, so the grid tab is filled here, once per attach.
    static constexpr const char* kGridComponents[] = {
        "color_picker_grid_filament", "color_picker_grid_led", "color_picker_grid_theme"};
    const auto grid_index = static_cast<size_t>(config_.palette);
    lv_obj_t* grid_tab = lv_obj_find_by_name(root_, "cp_grid_tab");
    if (grid_tab && grid_index < std::size(kGridComponents)) {
        lv_xml_create(grid_tab, kGridComponents[grid_index], nullptr);
    }
    fit_strip_to_row();
    spdlog::debug("[ColorPickerView] attached, palette {} color #{:06X}", palette_attr,
                  model_.rgb());
}

void ColorPickerView::detach() {
    // The widgets stay: they belong to the host's tree, which deletes them with
    // its page or dialog, and a delete here could land inside that teardown.
    if (root_) {
        lv_obj_set_user_data(root_, nullptr);
    }
    root_.reset();
    if (drop_publisher(this)) {
        if (ColorPickerView* previous = current_publisher()) {
            previous->publish_all();
        }
    }
}

void ColorPickerView::publish_all() {
    if (current_publisher() != this) {
        return;
    }
    const auto& palette = color_palette(config_.palette);
    for (size_t i = 0; i < palette.size() && i < kMaxCells; ++i) {
        s_cells.set_color(i, palette[i]);
    }
    const std::string caption =
        config_.strip_caption.empty() ? std::string(lv_tr("Recent")) : config_.strip_caption;
    lv_subject_copy_string(&s_subjects->strip_caption, caption.c_str());
    lv_subject_set_int(&s_subjects->tab, static_cast<int>(tab_));
    lv_subject_set_int(&s_subjects->strip_editing, strip_editing_ ? 1 : 0);
    publish_keypad();
    load_strip();
    publish();
}

void ColorPickerView::on_change(std::function<void(uint32_t)> callback) {
    on_change_ = std::move(callback);
}

void ColorPickerView::set_color(uint32_t rgb) {
    model_.set_rgb(rgb);
    if (current_publisher() == this) {
        publish();
    }
}

void ColorPickerView::set_label(const std::string& label) {
    label_ = label;
    if (current_publisher() == this) {
        publish();
    }
}

void ColorPickerView::set_tab(Tab tab) {
    tab_ = tab;
    if (current_publisher() == this) {
        lv_subject_set_int(&s_subjects->tab, static_cast<int>(tab));
    }
}

void ColorPickerView::apply(uint32_t rgb, bool from_sliders) {
    if (!from_sliders) {
        model_.set_rgb(rgb);
    }
    publish();
    if (on_change_) {
        on_change_(model_.rgb());
    }
}

void ColorPickerView::publish() {
    if (!s_subjects || current_publisher() != this) {
        return;
    }
    auto& s = *s_subjects;
    publishing_ = true;
    const uint32_t rgb = model_.rgb();
    const Hsv& hsv = model_.hsv();
    lv_subject_set_color(&s.current, to_color(rgb));
    const std::string name = label_.empty() ? helix::get_color_name_from_hex(rgb) : label_;
    lv_subject_copy_string(&s.name, name.c_str());
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%06X", rgb);
    lv_subject_copy_string(&s.hex, buf);
    lv_subject_set_int(&s.hue, hsv.h);
    lv_subject_set_int(&s.sat, hsv.s);
    lv_subject_set_int(&s.val, hsv.v);
    std::snprintf(buf, sizeof(buf), "%d\xC2\xB0", hsv.h);
    lv_subject_copy_string(&s.hue_text, buf);
    std::snprintf(buf, sizeof(buf), "%d%%", hsv.s);
    lv_subject_copy_string(&s.sat_text, buf);
    std::snprintf(buf, sizeof(buf), "%d%%", hsv.v);
    lv_subject_copy_string(&s.val_text, buf);
    lv_subject_set_color(&s.sat_lo, to_color(hsv_to_rgb({hsv.h, 0, hsv.v})));
    lv_subject_set_color(&s.sat_hi, to_color(hsv_to_rgb({hsv.h, 100, hsv.v})));
    lv_subject_set_color(&s.val_hi, to_color(hsv_to_rgb({hsv.h, hsv.s, 100})));

    const auto& palette = color_palette(config_.palette);
    const auto it = std::find(palette.begin(), palette.end(), rgb);
    lv_subject_set_int(&s.cell_selected,
                       it == palette.end() ? -1 : static_cast<int>(it - palette.begin()));
    publish_strip_selection();
    publishing_ = false;
}

void ColorPickerView::publish_strip_selection() {
    int selected = -1;
    if (!strip_editing_) {
        const auto it = std::find(strip_.begin(), strip_.end(), model_.rgb());
        selected = it == strip_.end() ? -1 : static_cast<int>(it - strip_.begin());
    }
    lv_subject_set_int(&s_subjects->chip_selected, selected);
}

bool ColorPickerView::strip_removable() const {
    return config_.strip != ColorStrip::Custom;
}

void ColorPickerView::load_strip() {
    if (config_.strip == ColorStrip::Custom) {
        strip_ = config_.custom_strip ? config_.custom_strip() : std::vector<uint32_t>{};
        std::vector<uint32_t> unique;
        for (uint32_t c : strip_) {
            if (std::find(unique.begin(), unique.end(), c) == unique.end()) {
                unique.push_back(c);
            }
        }
        strip_ = std::move(unique);
    } else {
        strip_ = ColorRecents::list(recents_kind(config_.strip));
    }
    if (strip_.size() > kMaxChips) {
        strip_.resize(kMaxChips);
    }
    if (current_publisher() != this) {
        return;
    }
    for (size_t i = 0; i < strip_.size(); ++i) {
        s_chips.set_color(i, strip_[i]);
    }
    if (lv_subject_get_int(&s_subjects->chip_count) != static_cast<int>(strip_.size())) {
        lv_subject_set_int(&s_subjects->chip_count, static_cast<int>(strip_.size()));
    }
    fit_strip_to_row();
    publish_strip_selection();
}

void ColorPickerView::fit_strip_to_row() {
    if (!root_ || config_.strip == ColorStrip::Custom || current_publisher() != this) {
        return;
    }
    lv_obj_t* row = lv_obj_find_by_name(root_, "cp_strip_chips");
    if (!row) {
        return;
    }
    lv_obj_update_layout(root_);
    // The chips are the row's <repeat> children, all one size.
    lv_obj_t* chip = lv_obj_get_child(row, 0);
    if (!chip) {
        return;
    }
    const int fit = whole_chips_in(lv_obj_get_content_width(row), lv_obj_get_width(chip),
                                   lv_obj_get_style_pad_column(row, LV_PART_MAIN));
    if (fit < static_cast<int>(strip_.size())) {
        strip_.resize(static_cast<size_t>(fit));
        lv_subject_set_int(&s_subjects->chip_count, fit);
    }
}

void ColorPickerView::refresh_strip() {
    load_strip();
}

void ColorPickerView::pick_cell(int index) {
    const auto& palette = color_palette(config_.palette);
    if (index < 0 || static_cast<size_t>(index) >= palette.size()) {
        return;
    }
    apply(palette[static_cast<size_t>(index)], false);
}

void ColorPickerView::pick_chip(int index) {
    if (index < 0 || static_cast<size_t>(index) >= strip_.size()) {
        return;
    }
    const uint32_t rgb = strip_[static_cast<size_t>(index)];
    if (strip_editing_) {
        ColorRecents::remove(recents_kind(config_.strip), rgb);
        load_strip();
        if (strip_.empty()) {
            finish_strip_edit();
        }
        return;
    }
    apply(rgb, false);
}

void ColorPickerView::hold_chip(int index) {
    if (!strip_removable() || strip_editing_ || index < 0 ||
        static_cast<size_t>(index) >= strip_.size()) {
        return;
    }
    strip_editing_ = true;
    if (current_publisher() == this) {
        lv_subject_set_int(&s_subjects->strip_editing, 1);
        // Done takes room from the row.
        fit_strip_to_row();
        publish_strip_selection();
    }
}

void ColorPickerView::finish_strip_edit() {
    strip_editing_ = false;
    if (current_publisher() == this) {
        lv_subject_set_int(&s_subjects->strip_editing, 0);
        // Done gave its room back.
        load_strip();
    }
}

void ColorPickerView::slider_changed(HsvChannel channel) {
    if (publishing_ || current_publisher() != this) {
        return;
    }
    lv_subject_t* subject = channel == HsvChannel::Hue          ? &s_subjects->hue
                            : channel == HsvChannel::Saturation ? &s_subjects->sat
                                                                : &s_subjects->val;
    const int value = lv_subject_get_int(subject);
    if (value == model_.channel(channel)) {
        return;
    }
    model_.set_channel(channel, value);
    apply(model_.rgb(), true);
}

void ColorPickerView::step(HsvChannel channel, int direction) {
    model_.step(channel, direction);
    apply(model_.rgb(), true);
}

void ColorPickerView::publish_keypad() {
    if (!s_subjects || current_publisher() != this) {
        return;
    }
    auto& s = *s_subjects;
    std::string text = "#" + keypad_.digits();
    for (size_t i = keypad_.digits().size(); i < HexKeypad::kDigits; ++i) {
        text += "\xC2\xB7"; // a middle dot per digit still to type
    }
    lv_subject_copy_string(&s.keypad_text, text.c_str());
    lv_subject_set_int(&s.keypad_len, static_cast<int>(keypad_.digits().size()));
    lv_subject_set_color(&s.keypad_preview, to_color(keypad_.result().value_or(model_.rgb())));
    lv_subject_set_int(&s.keypad_is_open, keypad_open_ ? 1 : 0);
}

void ColorPickerView::open_keypad() {
    keypad_.clear();
    keypad_open_ = true;
    publish_keypad();
}

void ColorPickerView::keypad_key(char c) {
    if (keypad_open_ && keypad_.push(c)) {
        publish_keypad();
    }
}

void ColorPickerView::keypad_backspace() {
    keypad_.backspace();
    publish_keypad();
}

void ColorPickerView::keypad_cancel() {
    keypad_open_ = false;
    keypad_.clear();
    publish_keypad();
}

void ColorPickerView::keypad_ok() {
    const auto rgb = keypad_.result();
    if (!rgb) {
        return;
    }
    keypad_open_ = false;
    keypad_.clear();
    publish_keypad();
    apply(*rgb, false);
}

} // namespace helix::ui
