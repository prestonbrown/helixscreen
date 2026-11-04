// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/static_panel_registry.h"
#include "helix/xml/indexed_subject_pool.h"
#include "overlay_base.h"
#include "printer_event_journal.h"
#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

namespace helix::ui {

/// "Printer Events": the printer event journal, newest first. Reads the
/// plugin's server.helix.journal when its event_journal feature is ready and
/// the screen's own journal otherwise (decide_journal_source), each time it
/// opens.
class PrinterEventsOverlay : public OverlayBase {
  public:
    /// Rows shown; the stores hold more.
    static constexpr std::size_t kMaxRows = 100;

    ~PrinterEventsOverlay() override;

    void init_subjects() override;
    void on_activate() override;

    /// Reclaim the row pools once the widgets are gone, never while rows
    /// still observe them.
    void on_ui_destroyed() override;

    const char* get_name() const override {
        return "Printer Events";
    }

    const char* xml_component() const override {
        return "printer_events_overlay";
    }
    // Keeps its tree: opened during a print on slow boards, where rebuild cost is unmeasured.
    bool destroy_on_close() const override {
        return false;
    }

    /// Fill the list from @p entries, which came from @p source.
    void show_entries(JournalSource source, std::vector<JournalEntry> entries);

  protected:
    void before_show() override;

  private:
    void refresh();

    bool ui_alive_ = false;
    SubjectManager subjects_;
    lv_subject_t count_subject_{};
    lv_subject_t source_subject_{};
    char source_buf_[96] = {};
    /// 1 when there is no source at all (JournalSource::None)
    lv_subject_t unavailable_subject_{};

    helix::xml::IndexedSubjectPool severity_pool_{"printer_event_severity",
                                                  helix::xml::IndexedSubjectPool::Type::Int};
    helix::xml::IndexedSubjectPool title_pool_{"printer_event_title",
                                               helix::xml::IndexedSubjectPool::Type::String};
    helix::xml::IndexedSubjectPool message_pool_{"printer_event_message",
                                                 helix::xml::IndexedSubjectPool::Type::String, 256};
    helix::xml::IndexedSubjectPool has_message_pool_{"printer_event_has_message",
                                                     helix::xml::IndexedSubjectPool::Type::Int};
    helix::xml::IndexedSubjectPool time_pool_{"printer_event_time",
                                              helix::xml::IndexedSubjectPool::Type::String};
};

inline PrinterEventsOverlay& get_printer_events_overlay() {
    return lazy_global<PrinterEventsOverlay>("PrinterEventsOverlay");
}

} // namespace helix::ui
