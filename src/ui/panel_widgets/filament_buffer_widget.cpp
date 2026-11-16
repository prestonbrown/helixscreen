// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_buffer_widget.h"

#include "ui_buffer_slider.h"

#include "ams_state.h"
#include "buffer_status_modal.h"
#include "core/subject.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_registry.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace {

// 1 when the tile is two cells wide, which is what draws the trace and the
// lean in words. Registered before the XML that binds it is parsed.
helix::Subject s_wide_subject{};
helix::WidgetSubjects s_subjects{"FilamentBufferWidgetSubjects"};

void filament_buffer_widget_init_subjects() {
    s_subjects.ensure_init([] {
        s_wide_subject.init_int(0);
        s_subjects.publish("filament_buffer_wide", &s_wide_subject);
    });
}

} // namespace

namespace helix {

void register_filament_buffer_widget() {
    register_widget_factory("filament_buffer", [](const std::string&) {
        return std::make_unique<FilamentBufferWidget>();
    });
    register_widget_subjects("filament_buffer", filament_buffer_widget_init_subjects);

    lv_xml_register_event_cb(nullptr, "on_filament_buffer_widget_clicked", [](lv_event_t* /*e*/) {
        if (!AmsState::instance().get_backend())
            return;
        BufferStatusModal::show_for(-1);
    });
}

FilamentBufferWidget::FilamentBufferWidget() = default;

FilamentBufferWidget::~FilamentBufferWidget() {
    detach();
}

void FilamentBufferWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    if (!widget_obj)
        return;
    slider_ =
        std::make_unique<ui::UiBufferSlider>(lv_obj_find_by_name(widget_obj, "buffer_slider_box"),
                                             lv_obj_find_by_name(widget_obj, "buffer_trace"), -1);
    slider_->follow_system_reading();
}

void FilamentBufferWidget::detach() {
    slider_.reset();
}

void FilamentBufferWidget::on_size_changed(int colspan, int /*rowspan*/, int /*width_px*/,
                                           int /*height_px*/) {
    lv_subject_set_int(&s_wide_subject, colspan >= 2 * GridLayout::TRACKS_PER_CELL ? 1 : 0);
}

bool FilamentBufferWidget::fits_at(int width_px, int /*height_px*/) const {
    // One row of padding, slider, gap and reading, and the reading never wraps.
    // A synced buffer's bias reads up to "+100%" (buffer_short_text).
    const lv_font_t* value_font = theme_manager_get_font("font_body_bold");
    const int reading_px = std::max(ui::measure_text_px("+100%", value_font),
                                    ui::measure_text_px("-100%", value_font));
    const int need = 2 * theme_manager_get_spacing("buffer_tile_pad") +
                     theme_manager_get_spacing("buffer_slider_w") +
                     theme_manager_get_spacing("space_sm") + reading_px;
    return width_px >= need;
}

} // namespace helix
