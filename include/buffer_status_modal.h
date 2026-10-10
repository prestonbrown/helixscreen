// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_buffer_slider.h"
#include "ui_modal.h"
#include "ui_observer_guard.h"

#include "ams_types.h"
#include "buffer_reading.h"

#include <memory>

namespace helix::ui {
class UiClogBar;
} // namespace helix::ui

/**
 * @brief Read-only modal for one filament buffer: an upright slider with its
 *        last minute scrolling out to the right, the reading and target, the
 *        lean in words, and the backend's own rows (Happy Hare spool motor, gear
 *        sync, flow; AFC state and distance to fault). Closed by its X; nothing
 *        to confirm.
 *
 * Live while open: every row is re-read from the backend on each AmsState data
 * revision or backend-count change.
 *
 * Subjects are static (shared across instances) because lv_xml_register_subject
 * rejects duplicate names — the first registration wins and the pointer persists.
 * Destroying per-instance subjects would leave the registry with dangling pointers.
 */
class BufferStatusModal : public Modal {
  public:
    BufferStatusModal();
    ~BufferStatusModal() override;

    const char* get_name() const override {
        return "Buffer Status";
    }
    const char* component_name() const override {
        return "buffer_status_modal";
    }

    /// Convenience: create the modal for one unit's buffer and show it; -1 is
    /// the buffer feeding the toolhead. One-shot and stack-owned - ModalStack
    /// frees the instance when its entry goes (#1382).
    static void show_for(int effective_unit);

  protected:
    void on_show() override;

  private:
    friend class TestableBufferStatusModal;
    friend class BufferStatusModalProbe;

    static void init_subjects();
    /// Write the modal's subjects for one unit's buffer, and return the
    /// reading the slider draws.
    helix::BufferReading populate(const helix::AmsSystemInfo& info, int effective_unit);
    /// populate() from the active backend's current snapshot, and put the
    /// slider on the same reading.
    void refresh();

    static bool subjects_initialized_;
    std::unique_ptr<helix::ui::UiBufferSlider> slider_;
    /// The clog reading above the columns. Owned here so it is torn down
    /// before Modal::~Modal() frees the dialog tree it points into.
    helix::ui::UiClogBar* clog_bar_ = nullptr;

    // Static subjects + backing buffers (persist across modal instances)
    static lv_subject_t type_subject_;
    static lv_subject_t show_meter_subject_;
    static lv_subject_t show_espooler_subject_;
    static lv_subject_t show_flow_subject_;
    static lv_subject_t show_distance_subject_;

    static lv_subject_t description_subject_;
    static char description_buf_[128];
    static lv_subject_t show_reading_subject_;
    static lv_subject_t status_subject_; ///< ClogMeterStatus of the reading, for its colour
    static lv_subject_t value_subject_;
    static char value_buf_[64];
    static lv_subject_t target_subject_;
    static char target_buf_[48];
    static lv_subject_t trace_caption_subject_;
    static char trace_caption_buf_[64];
    /// Shown when the filament system reports no buffer/flow data at all, so
    /// the dialog says why instead of rendering an empty box.
    static lv_subject_t unsupported_subject_;
    static char unsupported_buf_[128];
    static lv_subject_t espooler_value_subject_;
    static char espooler_buf_[128];
    static lv_subject_t gear_sync_value_subject_;
    static char gear_sync_buf_[32];
    static lv_subject_t flow_value_subject_;
    static char flow_buf_[32];
    static lv_subject_t afc_state_subject_;
    static char afc_state_buf_[128];
    static lv_subject_t afc_distance_subject_;
    static char afc_distance_buf_[128];

    int effective_unit_ = 0;
    ObserverGuard revision_observer_;
    ObserverGuard backend_observer_;
};
