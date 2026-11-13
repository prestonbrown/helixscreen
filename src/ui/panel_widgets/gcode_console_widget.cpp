// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_console_widget.h"

#include "ui_event_safety.h"
#include "ui_panel_console.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "console_line.h"
#include "grid_layout.h"
#include "i_moonraker_api.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <atomic>

namespace helix {

namespace {

/// server.gcode_store rows asked for. More than MAX_LINES because filtering
/// runs before trimming, and a chatty printer's temperature reports would
/// otherwise crowd the real lines out.
constexpr int kFetchCount = 100;

const lv_font_t* row_font() {
    const lv_font_t* font = theme_manager_get_font("font_mono");
    return font ? font : theme_manager_get_font("font_small");
}

} // namespace

void register_gcode_console_widget() {
    register_widget_factory(
        "gcode_console", [](const std::string&) { return std::make_unique<GCodeConsoleWidget>(); });

    lv_xml_register_event_cb(nullptr, "gcode_console_clicked_cb", GCodeConsoleWidget::clicked_cb);
}

GCodeConsoleWidget::GCodeConsoleWidget()
    : TiledPanelWidget("gcode_console", TileSizing::Content{"", "", "Console", false}) {
    // Registered here, not in attach(): the manager parses this tile's XML
    // before attach() runs, and the parser drops a binding whose subject is
    // missing at parse time.
    UI_MANAGED_SUBJECT_INT(view_subject_, static_cast<int>(View::Icon), view_name_.c_str(),
                           subjects_);

    for (const char** attr = sizing_.subject_attrs(); *attr; ++attr) {
        attr_storage_.emplace_back(*attr);
    }
    attr_storage_.emplace_back("tail_view_subject");
    attr_storage_.push_back(view_name_);
    for (const auto& s : attr_storage_) {
        attrs_.push_back(s.c_str());
    }
    attrs_.push_back(nullptr);
}

GCodeConsoleWidget::~GCodeConsoleWidget() {
    detach();
    subjects_.deinit_all();
}

void GCodeConsoleWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    rows_ = lv_obj_find_by_name(widget_obj_, "gcode_console_tail_rows");
    install_delete_hook(widget_obj_);
}

void GCodeConsoleWidget::detach() {
    unsubscribe();
    lifetime_.invalidate();
    lines_.clear();
    tail_active_ = false;
    if (widget_obj_) {
        publish_view();
    }
    uninstall_delete_hook();
    widget_obj_ = nullptr;
    rows_ = nullptr;
    parent_screen_ = nullptr;
}

void GCodeConsoleWidget::on_hooked_root_deleted() {
    lifetime_.invalidate();
    widget_obj_ = nullptr;
    rows_ = nullptr;
    parent_screen_ = nullptr;
}

void GCodeConsoleWidget::on_activate() {
    if (!tail_active_) {
        return;
    }
    // Filter settings may have changed in the console overlay, and a
    // reconnect leaves a gap the store fills.
    ConsolePanel::load_firmware_filter(filter_);
    subscribe();
    fetch_history();
}

void GCodeConsoleWidget::on_deactivate() {
    // Off screen the tail needs no live lines; on_activate() refetches the store.
    unsubscribe();
    lifetime_.invalidate();
}

void GCodeConsoleWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    sizing_.measure_and_publish(width_px, height_px);

    const lv_font_t* font = row_font();
    const int line_h = line_px(font);
    // One extra so a partial row fills the top edge instead of leaving a gap.
    max_rows_ = line_h > 0 ? static_cast<size_t>(height_px / line_h) + 1 : MAX_LINES;

    constexpr int kCell = GridLayout::TRACKS_PER_CELL;
    set_tail_active(rowspan >= 2 * kCell && colspan >= kCell);
}

void GCodeConsoleWidget::set_tail_active(bool active) {
    active = active && rows_ != nullptr;
    if (active == tail_active_) {
        if (active) {
            rebuild_rows();
        }
        return;
    }

    tail_active_ = active;
    if (active) {
        ConsolePanel::load_firmware_filter(filter_);
        subscribe();
        fetch_history();
    } else {
        unsubscribe();
        // Drops a history reply or line already queued for the tail.
        lifetime_.invalidate();
        lines_.clear();
        helix::ui::safe_clean_children(rows_);
    }
    publish_view();
}

void GCodeConsoleWidget::subscribe() {
    if (!handler_name_.empty()) {
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::debug("[GCodeConsoleWidget] No API, tail not subscribed");
        return;
    }

    static std::atomic<uint64_t> s_handler_id{0};
    handler_name_ = "gcode_console_widget_" + std::to_string(++s_handler_id);
    api->register_method_callback("notify_gcode_response", handler_name_,
                                  [this, token = lifetime_.token()](const nlohmann::json& msg) {
                                      auto entry = ConsolePanel::entry_from_gcode_response(msg);
                                      if (!entry) {
                                          return;
                                      }
                                      token.defer(
                                          "GCodeConsoleWidget::gcode_line",
                                          [this, e = std::move(*entry)]() { append_line(e); });
                                  });
}

void GCodeConsoleWidget::unsubscribe() {
    if (handler_name_.empty()) {
        return;
    }
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        api->unregister_method_callback("notify_gcode_response", handler_name_);
    }
    handler_name_.clear();
}

void GCodeConsoleWidget::fetch_history() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return;
    }
    api->get_gcode_store(
        kFetchCount,
        [this, token = lifetime_.token()](const std::vector<GcodeStoreEntry>& stored) {
            std::vector<Entry> entries;
            entries.reserve(stored.size());
            for (const auto& s : stored) {
                entries.push_back(ConsolePanel::entry_from_store(s));
            }
            token.defer("GCodeConsoleWidget::history",
                        [this, entries = std::move(entries)]() { replace_lines(entries); });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[GCodeConsoleWidget] gcode_store fetch failed: {}", err.message);
        });
}

void GCodeConsoleWidget::replace_lines(const std::vector<Entry>& history) {
    lines_.clear();
    // Filter before trimming, so noise never spends the line budget.
    for (const auto& entry : history) {
        if (ConsolePanel::accepts(entry, filter_)) {
            lines_.push_back(entry);
        }
    }
    while (lines_.size() > MAX_LINES) {
        lines_.pop_front();
    }
    rebuild_rows();
    publish_view();
}

void GCodeConsoleWidget::append_line(Entry entry) {
    if (!ConsolePanel::accepts(entry, filter_)) {
        return;
    }
    lines_.push_back(std::move(entry));
    if (lines_.size() > MAX_LINES) {
        lines_.pop_front();
    }
    if (tail_active_ && rows_) {
        create_row(lines_.back());
        // Rows are oldest-first, so the one past the visible budget is child 0.
        // Bounded because the deferred delete is a no-op during shutdown.
        const uint32_t count = lv_obj_get_child_count(rows_);
        for (uint32_t i = static_cast<uint32_t>(max_rows_); i < count; ++i) {
            helix::ui::safe_delete_deferred_raw(lv_obj_get_child(rows_, 0));
        }
    }
    publish_view();
}

void GCodeConsoleWidget::rebuild_rows() {
    if (!rows_) {
        return;
    }
    helix::ui::safe_clean_children(rows_);
    const size_t start = lines_.size() > max_rows_ ? lines_.size() - max_rows_ : 0;
    for (size_t i = start; i < lines_.size(); ++i) {
        create_row(lines_[i]);
    }
}

void GCodeConsoleWidget::create_row(const Entry& entry) {
    // Streaming log rows: count and content come from the printer as it talks,
    // so they are built here rather than declared, as in the console overlay.
    lv_obj_t* row = lv_spangroup_create(rows_);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_style_text_font(row, row_font(), 0);
    lv_spangroup_set_max_lines(row, 1);
    lv_spangroup_set_overflow(row, LV_SPAN_OVERFLOW_ELLIPSIS);
    lv_obj_set_clickable(row, false);
    lv_obj_set_event_bubble(row, true);

    const bool is_command = entry.type == Entry::Type::COMMAND;
    for (const auto& run :
         helix::ui::console_line_spans(entry.message, is_command, entry.is_error)) {
        lv_span_t* span = lv_spangroup_add_span(row);
        lv_span_set_text(span, run.text.c_str());
        lv_style_set_text_color(lv_span_get_style(span), theme_manager_get_color(run.color_token));
    }
    lv_spangroup_refresh(row);
}

void GCodeConsoleWidget::publish_view() {
    View view = View::Icon;
    if (tail_active_) {
        view = lines_.empty() ? View::TailEmpty : View::Tail;
    }
    lv_subject_set_int(&view_subject_, static_cast<int>(view));
}

void GCodeConsoleWidget::handle_click() {
    get_global_console_panel().show(parent_screen_);
}

void GCodeConsoleWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GCodeConsoleWidget] clicked_cb");
    // Both faces share this callback, so the owner comes from the tile root
    // rather than from whichever child was tapped.
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    while (obj && !lv_obj_has_flag(obj, kPanelWidgetTileFlag)) {
        obj = lv_obj_get_parent(obj);
    }
    auto* self = obj ? static_cast<GCodeConsoleWidget*>(lv_obj_get_user_data(obj)) : nullptr;
    if (self) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
