// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_console.h"

#include "async_lifetime_guard.h"
#include "console_filter_engine.h"
#include "core/subject.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"
#include "subject_managed_panel.h"

#include <deque>
#include <string>
#include <vector>

namespace helix {

/// Console tile. Below two cells tall it is the adaptive icon tile; at two
/// cells or taller it shows a live tail of the G-code console output. Either
/// face opens the full console overlay when tapped.
class GCodeConsoleWidget : public TiledPanelWidget {
    friend class GCodeConsoleWidgetTestAccess;

  public:
    /// What the tile draws, published on its view subject for XML to bind.
    enum class View : int { Icon = 0, TailEmpty = 1, Tail = 2 };

    /// Lines kept for the tail. More than any tile shows, so a taller resize
    /// fills from what is already here.
    static constexpr size_t MAX_LINES = 50;

    GCodeConsoleWidget();
    ~GCodeConsoleWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_deactivate() override;
    const char* id() const override {
        return "gcode_console";
    }

    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;

    const char** xml_attrs() const override {
        return const_cast<const char**>(attrs_.data());
    }

    static void clicked_cb(lv_event_t* e);

  protected:
    void on_hooked_root_deleted() override;

  private:
    using Entry = ConsolePanel::GcodeEntry;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* rows_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Static: multiple widget instances share the same global ConsolePanel singleton

    void handle_click();

    void set_tail_active(bool active);
    void subscribe();
    void unsubscribe();
    void fetch_history();
    void replace_lines(const std::vector<Entry>& history);
    void append_line(Entry entry);
    void rebuild_rows();
    void create_row(const Entry& entry);
    void publish_view();

    bool tail_active_ = false;
    size_t max_rows_ = MAX_LINES;
    std::deque<Entry> lines_;
    std::string handler_name_;
    helix::ui::ConsoleFilterEngine filter_;
    helix::AsyncLifetimeGuard lifetime_;

    std::string view_name_{"gcode_console_tail_view"};
    helix::Subject view_subject_{};
    SubjectManager subjects_;
    std::vector<std::string> attr_storage_;
    std::vector<const char*> attrs_;
};

void register_gcode_console_widget();

} // namespace helix
