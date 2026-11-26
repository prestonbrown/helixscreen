// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "src/ui/panel_widgets/clock_face_picker.h"

#include "ui_event_safety.h"

#include "core/subject.h"
#include "core/text_io.h"
#include "helix/xml/indexed_subject_pool.h"
#include "screensaver_clock.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

/// The painted faces' rows, after Text. One picker is open at a time, so these are process-wide.
constexpr size_t kPaintedFaces = kClockFaceCount - 1;

struct PickerSubjects {
    xml::IndexedSubjectPool names{"clock_face_name", xml::IndexedSubjectPool::Type::String};
    helix::Subject count{};
    helix::Subject selected{};
};

helix::WidgetSubjects s_picker_subjects{"ClockFacePicker"};

/// Created on the first open, before the picker's XML binds them.
PickerSubjects& picker_subjects() {
    static PickerSubjects s_picker;
    s_picker_subjects.ensure_init(
        [] {
            UI_MANAGED_SUBJECT_INT(s_picker.count, 0, "clock_face_count", s_picker_subjects);
            UI_MANAGED_SUBJECT_INT(s_picker.selected, 0, "clock_face_selected", s_picker_subjects);
        },
        [] { s_picker.names.reclaim(); });
    return s_picker;
}

ClockFace painted_face(size_t i) {
    return static_cast<ClockFace>(i + 1);
}

} // namespace

void register_clock_face_picker() {
    lv_xml_register_event_cb(nullptr, "clock_face_row_cb", ClockFacePicker::row_cb);
}

ClockFacePicker::~ClockFacePicker() {
    if (lv_obj_t* m = menu(); m != nullptr && lv_is_initialized()) {
        lv_obj_remove_event_cb_with_user_data(m, on_menu_deleted, this);
    }
    free_previews();
}

void ClockFacePicker::on_menu_deleted(lv_event_t* e) {
    auto* self = static_cast<ClockFacePicker*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    // The canvases go with the menu; only their buffers are freed here.
    for (ClockFaceView& view : self->previews_) {
        view.release_dying();
    }
}

void ClockFacePicker::show_for(lv_obj_t* parent, lv_obj_t* anchor, ClockFace current,
                               OnPick on_pick) {
    if (is_visible() || parent == nullptr || anchor == nullptr) {
        return;
    }
    auto& subs = picker_subjects();
    subs.names.ensure_size(kPaintedFaces);
    for (size_t i = 0; i < kPaintedFaces; ++i) {
        subs.names.set_string(i, lv_tr(clock_face_label(painted_face(i))));
    }
    lv_subject_set_int(&subs.selected, static_cast<int>(current));
    lv_subject_set_int(&subs.count, static_cast<int>(kPaintedFaces));
    on_pick_ = std::move(on_pick);
    show_below_widget(parent, anchor, helix::ui::ContextMenu::AnchorAlign::Center);
}

void ClockFacePicker::on_created(lv_obj_t* menu_obj) {
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup. A screen swap deletes the menu without hide(),
    // and the previews' buffers must go with it.
    lv_obj_add_event_cb(menu_obj, on_menu_deleted, LV_EVENT_DELETE, this);
    // DECLARATIVE_OK: measured cap. A share of the screen, so the eight rows scroll on a short
    // panel instead of growing the card past it.
    if (lv_obj_t* list = lv_obj_find_by_name(menu_obj, "clock_face_list")) {
        lv_obj_set_style_max_height(list, screen_height_pct(70), 0);
    }
    lv_obj_update_layout(menu_obj);

    const ui::ClockInput now = ui::saver_clock_now();
    // Each preview sits on the menu's card.
    const lv_obj_t* menu_card = card();
    const lv_color_t surround = menu_card != nullptr
                                    ? lv_obj_get_style_bg_color(menu_card, LV_PART_MAIN)
                                    : theme_manager_get_color("card_bg");
    int32_t box_w = 0;
    for (size_t i = 0; i < kPaintedFaces; ++i) {
        const std::string name = "clock_face_preview_" + std::to_string(i);
        lv_obj_t* box = lv_obj_find_by_name(menu_obj, name.c_str());
        if (box == nullptr) {
            continue;
        }
        const ClockFace face = painted_face(i);
        const int32_t h = lv_obj_get_content_height(box);
        // A dial is square; a row of four cards or tubes is wider than tall.
        const bool analog = face != ClockFace::FLIP && face != ClockFace::NIXIE;
        const int32_t w = analog ? h : h * 2;
        box_w = h * 2;
        // DECLARATIVE_OK: the preview box takes the size of the widest face painted into it.
        lv_obj_set_width(box, box_w);
        ClockFaceTempo still;
        previews_[i].show(box, face, w, h, still, now,
                          ui::Rgb{surround.red, surround.green, surround.blue},
                          /*run_timer=*/false);
    }
    // The Text row's preview box lines its name up with the painted rows' names.
    if (lv_obj_t* text_box = lv_obj_find_by_name(menu_obj, "clock_face_preview_text")) {
        // DECLARATIVE_OK: matches the painted previews' measured width.
        lv_obj_set_width(text_box, box_w);
    }
}

const ClockFaceView* ClockFacePicker::preview(ClockFace face) const {
    if (face == ClockFace::TEXT) {
        return nullptr;
    }
    const auto i = static_cast<size_t>(face) - 1;
    return i < previews_.size() ? &previews_[i] : nullptr;
}

void ClockFacePicker::free_previews() {
    for (ClockFaceView& view : previews_) {
        view.hide();
    }
}

void ClockFacePicker::on_hide() {
    // The menu is deleted later, maybe after this picker: its delete handler goes now.
    if (lv_obj_t* m = menu(); m != nullptr) {
        lv_obj_remove_event_cb_with_user_data(m, on_menu_deleted, this);
    }
    free_previews();
}

void ClockFacePicker::row_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ClockFacePicker] row_cb");
    auto* picker = helix::ui::ContextMenu::active_as<ClockFacePicker>();
    const char* ud = static_cast<const char*>(lv_event_get_user_data(e));
    if (picker == nullptr || ud == nullptr) {
        return;
    }
    const int index = helix::text_io::parse_leading<int>(ud).value_or(0);
    const ClockFace face = index > 0 && static_cast<size_t>(index) < kClockFaceCount
                               ? static_cast<ClockFace>(index)
                               : ClockFace::TEXT;
    // hide() may run the owner's teardown; the callback is taken out first.
    OnPick on_pick = std::move(picker->on_pick_);
    picker->hide();
    if (on_pick) {
        on_pick(face);
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
