// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_buffer_widget.h"

#include "ui_buffer_slider.h"

#include "ams_state.h"
#include "buffer_status_modal.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_registry.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

namespace {

// 1 when the tile is two cells wide, which is what draws the trace and the
// lean in words. Registered before the XML that binds it is parsed.
lv_subject_t s_wide_subject{};
bool s_subjects_initialized = false;
SubjectManager s_subjects;

void filament_buffer_widget_init_subjects() {
    if (s_subjects_initialized)
        return;

    lv_subject_init_int(&s_wide_subject, 0);
    s_subjects.publish("filament_buffer_wide", &s_wide_subject);
    s_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("FilamentBufferWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            s_subjects.deinit_all();
            s_subjects_initialized = false;
        }
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

} // namespace helix
