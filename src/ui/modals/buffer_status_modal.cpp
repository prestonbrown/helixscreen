// SPDX-License-Identifier: GPL-3.0-or-later

#include "buffer_status_modal.h"

#include "theme_manager.h"
#include "ui_buffer_meter.h"

#include <spdlog/fmt/fmt.h>

#include <cmath>

// Static member definitions
bool BufferStatusModal::subjects_initialized_ = false;
lv_subject_t BufferStatusModal::type_subject_;
lv_subject_t BufferStatusModal::show_meter_subject_;
lv_subject_t BufferStatusModal::show_espooler_subject_;
lv_subject_t BufferStatusModal::show_flow_subject_;
lv_subject_t BufferStatusModal::show_distance_subject_;
lv_subject_t BufferStatusModal::description_subject_;
char BufferStatusModal::description_buf_[128]{};
lv_subject_t BufferStatusModal::espooler_value_subject_;
char BufferStatusModal::espooler_buf_[128]{};
lv_subject_t BufferStatusModal::gear_sync_value_subject_;
char BufferStatusModal::gear_sync_buf_[32]{};
lv_subject_t BufferStatusModal::clog_value_subject_;
char BufferStatusModal::clog_buf_[32]{};
lv_subject_t BufferStatusModal::flow_value_subject_;
char BufferStatusModal::flow_buf_[32]{};
lv_subject_t BufferStatusModal::afc_state_subject_;
char BufferStatusModal::afc_state_buf_[128]{};
lv_subject_t BufferStatusModal::afc_distance_subject_;
char BufferStatusModal::afc_distance_buf_[128]{};

BufferStatusModal::BufferStatusModal() {
    init_subjects();
}

BufferStatusModal::~BufferStatusModal() {
    // Delete meter before Modal::~Modal() destroys the dialog tree,
    // so UiBufferMeter can safely remove its event callbacks from root_.
    delete meter_;
    // Subjects are static — never deinited (persist for the process lifetime)
}

void BufferStatusModal::init_subjects() {
    if (subjects_initialized_)
        return;

    lv_subject_init_int(&type_subject_, 0);
    lv_subject_init_int(&show_meter_subject_, 0);
    lv_subject_init_int(&show_espooler_subject_, 0);
    lv_subject_init_int(&show_flow_subject_, 0);
    lv_subject_init_int(&show_distance_subject_, 0);

    lv_subject_init_string(&description_subject_, description_buf_, nullptr,
                           sizeof(description_buf_), "");
    lv_subject_init_string(&espooler_value_subject_, espooler_buf_, nullptr,
                           sizeof(espooler_buf_), "");
    lv_subject_init_string(&gear_sync_value_subject_, gear_sync_buf_, nullptr,
                           sizeof(gear_sync_buf_), "");
    lv_subject_init_string(&clog_value_subject_, clog_buf_, nullptr, sizeof(clog_buf_), "");
    lv_subject_init_string(&flow_value_subject_, flow_buf_, nullptr, sizeof(flow_buf_), "");
    lv_subject_init_string(&afc_state_subject_, afc_state_buf_, nullptr,
                           sizeof(afc_state_buf_), "");
    lv_subject_init_string(&afc_distance_subject_, afc_distance_buf_, nullptr,
                           sizeof(afc_distance_buf_), "");

    lv_xml_register_subject(nullptr, "buf_type", &type_subject_);
    lv_xml_register_subject(nullptr, "buf_show_meter", &show_meter_subject_);
    lv_xml_register_subject(nullptr, "buf_show_espooler", &show_espooler_subject_);
    lv_xml_register_subject(nullptr, "buf_show_flow", &show_flow_subject_);
    lv_xml_register_subject(nullptr, "buf_show_distance", &show_distance_subject_);
    lv_xml_register_subject(nullptr, "buf_description", &description_subject_);
    lv_xml_register_subject(nullptr, "buf_espooler_value", &espooler_value_subject_);
    lv_xml_register_subject(nullptr, "buf_gear_sync_value", &gear_sync_value_subject_);
    lv_xml_register_subject(nullptr, "buf_clog_value", &clog_value_subject_);
    lv_xml_register_subject(nullptr, "buf_flow_value", &flow_value_subject_);
    lv_xml_register_subject(nullptr, "buf_afc_state", &afc_state_subject_);
    lv_xml_register_subject(nullptr, "buf_afc_distance", &afc_distance_subject_);

    subjects_initialized_ = true;
}

void BufferStatusModal::populate(const AmsSystemInfo& info, int effective_unit) {
    if (info.type == AmsType::HAPPY_HARE) {
        lv_subject_set_int(&type_subject_, 1);

        // Description based on bias
        bool has_bias = info.sync_feedback_bias > -1.5f;
        if (has_bias) {
            float abs_bias = std::fabs(info.sync_feedback_bias);
            if (abs_bias < 0.02f) {
                lv_subject_copy_string(&description_subject_,
                                      lv_tr("Filament tension is balanced"));
            } else if (info.sync_feedback_bias < 0) {
                lv_subject_copy_string(&description_subject_,
                                      lv_tr("Filament is pulling tight"));
            } else {
                lv_subject_copy_string(&description_subject_, lv_tr("Filament is loose"));
            }
        } else {
            lv_subject_copy_string(&description_subject_, "");
        }

        // eSpooler
        if (!info.espooler_state.empty()) {
            lv_subject_set_int(&show_espooler_subject_, 1);
            if (info.espooler_state == "rewind") {
                lv_subject_copy_string(&espooler_value_subject_, lv_tr("Rewinding"));
            } else if (info.espooler_state == "assist") {
                lv_subject_copy_string(&espooler_value_subject_, lv_tr("Assisting"));
            } else {
                lv_subject_copy_string(&espooler_value_subject_, info.espooler_state.c_str());
            }
        } else {
            lv_subject_set_int(&show_espooler_subject_, 0);
        }

        // Gear sync
        lv_subject_copy_string(&gear_sync_value_subject_,
                              info.sync_drive ? lv_tr("Active") : lv_tr("Inactive"));

        // Clog detection
        if (info.clog_detection == 2) {
            lv_subject_copy_string(&clog_value_subject_, lv_tr("Automatic"));
        } else if (info.clog_detection == 1) {
            lv_subject_copy_string(&clog_value_subject_, lv_tr("Manual"));
        } else {
            lv_subject_copy_string(&clog_value_subject_, lv_tr("Off"));
        }

        // Flow rate
        if (info.sync_feedback_flow_rate >= 0) {
            auto flow_str = fmt::format("{:.0f}%", info.sync_feedback_flow_rate);
            lv_subject_copy_string(&flow_value_subject_, flow_str.c_str());
            lv_subject_set_int(&show_flow_subject_, 1);
        } else if (info.encoder_flow_rate >= 0) {
            auto flow_str = fmt::format("{}%", info.encoder_flow_rate);
            lv_subject_copy_string(&flow_value_subject_, flow_str.c_str());
            lv_subject_set_int(&show_flow_subject_, 1);
        } else {
            lv_subject_set_int(&show_flow_subject_, 0);
        }

        // Meter visibility
        lv_subject_set_int(&show_meter_subject_, has_bias ? 1 : 0);

    } else if (info.type == AmsType::AFC) {
        lv_subject_set_int(&type_subject_, 2);
        lv_subject_set_int(&show_meter_subject_, 0);

        bool found_health = false;
        if (effective_unit >= 0 && effective_unit < static_cast<int>(info.units.size())) {
            const auto& unit = info.units[effective_unit];
            if (unit.buffer_health.has_value()) {
                const auto& bh = unit.buffer_health.value();
                found_health = true;

                // State
                if (!bh.state.empty()) {
                    if (bh.state == "Advancing") {
                        lv_subject_copy_string(&afc_state_subject_,
                                              lv_tr("Feeding filament forward"));
                    } else if (bh.state == "Trailing") {
                        lv_subject_copy_string(&afc_state_subject_,
                                              lv_tr("Pulling filament back"));
                    } else {
                        auto s = fmt::format("{}: {}", lv_tr("State"), bh.state);
                        lv_subject_copy_string(&afc_state_subject_, s.c_str());
                    }
                } else {
                    lv_subject_copy_string(&afc_state_subject_, "");
                }

                // Distance
                if (bh.fault_detection_enabled && bh.distance_to_fault >= 0) {
                    auto d = fmt::format("{:.1f} mm {}", bh.distance_to_fault,
                                         lv_tr("remaining before clog detection triggers"));
                    lv_subject_copy_string(&afc_distance_subject_, d.c_str());
                    lv_subject_set_int(&show_distance_subject_, 1);
                } else {
                    lv_subject_set_int(&show_distance_subject_, 0);
                }

                // Clog detection
                if (bh.fault_detection_enabled) {
                    lv_subject_copy_string(&clog_value_subject_, lv_tr("Active"));
                } else {
                    lv_subject_copy_string(&clog_value_subject_, lv_tr("Inactive"));
                }
            }
        }
        if (!found_health) {
            lv_subject_copy_string(&afc_state_subject_, lv_tr("No buffer data available"));
            lv_subject_set_int(&show_distance_subject_, 0);
            lv_subject_copy_string(&clog_value_subject_, lv_tr("Unknown"));
        }
    } else {
        lv_subject_set_int(&type_subject_, 0);
        lv_subject_set_int(&show_meter_subject_, 0);
    }
}

void BufferStatusModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_close");
    wire_cancel_button("btn_secondary");

    populate(info_, effective_unit_);

    // Apply label/value color distinction AFTER theme_apply_current_palette_to_tree
    // (which runs in Modal::show and forces all labels white on dark backgrounds)
    if (dialog()) {
        auto muted = theme_manager_get_color("text_muted");
        static const char* label_names[] = {"lbl_espooler",  "lbl_gear_sync",
                                            "lbl_clog",      "lbl_flow",
                                            "lbl_afc_clog",  nullptr};
        for (const char** name = label_names; *name; ++name) {
            lv_obj_t* lbl = lv_obj_find_by_name(dialog(), *name);
            if (lbl)
                lv_obj_set_style_text_color(lbl, muted, 0);
        }
    }

    // Create UiBufferMeter programmatically in the meter column
    bool has_bias = info_.type == AmsType::HAPPY_HARE && info_.sync_feedback_bias > -1.5f;
    if (has_bias && dialog()) {
        lv_obj_t* meter_col = lv_obj_find_by_name(dialog(), "meter_col");
        if (meter_col) {
            meter_ = new helix::ui::UiBufferMeter(meter_col);
            meter_->set_bias(info_.sync_feedback_bias);
        }
    }
}

void BufferStatusModal::show_for(const AmsSystemInfo& info, int effective_unit) {
    // Use a static unique_ptr so the modal is cleaned up on next invocation
    static std::unique_ptr<BufferStatusModal> instance;
    instance = std::make_unique<BufferStatusModal>();
    instance->info_ = info;
    instance->effective_unit_ = effective_unit;
    instance->show(lv_screen_active());
}
