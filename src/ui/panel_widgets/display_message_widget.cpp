// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_message_widget.h"

#include "ui_icon_codepoints.h"

#include "core/subject.h"
#include "helix_psram_attr.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

namespace {

// The verdict panel_widget_display_message.xml binds. Module-level because the widget is
// single-instance and registration runs before any XML that binds them is parsed.
HELIX_PSRAM_BSS helix::Subject s_multiline_subject{};
HELIX_PSRAM_BSS helix::Subject s_show_icon_subject{};
helix::WidgetSubjects s_subjects{"DisplayMessageWidgetSubjects"};
// Only the instance bound to the live tile publishes; a rebuild's outgoing instance
// must not overwrite the tile's verdict.
helix::LiveTileOwner<helix::DisplayMessageWidget> s_tile;

void display_message_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_multiline_subject, 0, "display_message_widget_multiline",
                               s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_icon_subject, 1, "display_message_widget_icon", s_subjects);
    });
}

} // namespace

namespace helix {

void register_display_message_widget() {
    register_widget_factory("display_message", [](const std::string&) {
        return std::make_unique<DisplayMessageWidget>();
    });
    register_widget_subjects("display_message", display_message_init_subjects);
}

DisplayMessageWidget::~DisplayMessageWidget() {
    detach();
}

DisplayMessageVerdict DisplayMessageWidget::decide(int width_px, int height_px) const {
    // The root's padding alone: the gap between the icon and the text is charged in the
    // decision, only when the icon is drawn.
    const ContentChrome chrome = content_padding_or_estimate(root(), "space_sm", "space_sm");
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* icon_font = theme_manager_get_font("icon_font_sm");

    DisplayMessageInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    in.line_h = line_px(body);
    in.icon_w = ui::measure_text_px(ui::icon::lookup_codepoint("message"), icon_font);
    in.icon_h = line_px(icon_font);
    in.gap = theme_manager_get_spacing("space_sm");
    // Roughly eight characters of body text: below that the icon costs more than it says.
    in.min_text_w = 4 * in.line_h;
    return decide_display_message_layout(in);
}

void DisplayMessageWidget::attach(lv_obj_t* /*widget_obj*/, lv_obj_t* /*parent_screen*/) {
    s_tile.claim(this);
    // Instances are recycled across rebuilds: the verdict is republished here too.
    relayout_for_granted_size();
    publish();
}

void DisplayMessageWidget::detach() {
    if (DisplayMessageWidget* displaced = s_tile.release(this)) {
        displaced->relayout_for_granted_size();
        displaced->publish();
    }
}

void DisplayMessageWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                           int height_px) {
    verdict_ = decide(width_px, height_px);
    publish();
    spdlog::debug("[DisplayMessageWidget] on_size_changed {}x{} multiline={} icon={} fits={}",
                  width_px, height_px, verdict_.multiline, verdict_.show_icon, verdict_.fits);
}

bool DisplayMessageWidget::fits_at(int width_px, int height_px) const {
    return decide(width_px, height_px).fits;
}

void DisplayMessageWidget::publish() {
    if (!s_tile.is_owner(this))
        return;
    lv_subject_set_int(&s_multiline_subject, verdict_.multiline ? 1 : 0);
    lv_subject_set_int(&s_show_icon_subject, verdict_.show_icon ? 1 : 0);
}

} // namespace helix
