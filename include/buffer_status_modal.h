// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "ams_types.h"

namespace helix::ui {
class UiBufferMeter;
}

/**
 * @brief Read-only modal showing buffer/sync status for Happy Hare or AFC
 *
 * Subjects are static (shared across instances) because lv_xml_register_subject
 * rejects duplicate names — the first registration wins and the pointer persists.
 * Destroying per-instance subjects would leave the registry with dangling pointers.
 */
class BufferStatusModal : public Modal {
  public:
    BufferStatusModal();
    ~BufferStatusModal() override;

    const char* get_name() const override { return "Buffer Status"; }
    const char* component_name() const override { return "buffer_status_modal"; }

    /// Convenience: create modal, populate from info, and show
    static void show_for(const AmsSystemInfo& info, int effective_unit);

  protected:
    void on_show() override;

  private:
    friend class TestableBufferStatusModal;

    static void init_subjects();
    void populate(const AmsSystemInfo& info, int effective_unit);

    static bool subjects_initialized_;
    helix::ui::UiBufferMeter* meter_ = nullptr;

    // Static subjects + backing buffers (persist across modal instances)
    static lv_subject_t type_subject_;
    static lv_subject_t show_meter_subject_;
    static lv_subject_t show_espooler_subject_;
    static lv_subject_t show_flow_subject_;
    static lv_subject_t show_distance_subject_;

    static lv_subject_t description_subject_;
    static char description_buf_[128];
    static lv_subject_t espooler_value_subject_;
    static char espooler_buf_[128];
    static lv_subject_t gear_sync_value_subject_;
    static char gear_sync_buf_[32];
    static lv_subject_t clog_value_subject_;
    static char clog_buf_[32];
    static lv_subject_t flow_value_subject_;
    static char flow_buf_[32];
    static lv_subject_t afc_state_subject_;
    static char afc_state_buf_[128];
    static lv_subject_t afc_distance_subject_;
    static char afc_distance_buf_[128];

    // Stored info for post-show meter creation
    AmsSystemInfo info_;
    int effective_unit_ = 0;
};
