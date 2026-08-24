// SPDX-License-Identifier: GPL-3.0-or-later

#include "clog_detection_config_modal.h"

#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"

#include <spdlog/spdlog.h>

namespace {

ClogDetectionConfigModal* get_modal(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* dialog = lv_obj_get_parent(target);
    while (dialog && !lv_obj_get_user_data(dialog))
        dialog = lv_obj_get_parent(dialog);
    return dialog ? static_cast<ClogDetectionConfigModal*>(lv_obj_get_user_data(dialog)) : nullptr;
}

} // namespace

ClogDetectionConfigModal::ClogDetectionConfigModal(const std::string& widget_id,
                                                   const std::string& panel_id)
    : widget_id_(widget_id), panel_id_(panel_id) {
    // Register callbacks before show() parses the XML
    lv_xml_register_event_cb(nullptr, "on_clog_source_auto", on_source_auto);
    lv_xml_register_event_cb(nullptr, "on_clog_source_encoder", on_source_encoder);
    lv_xml_register_event_cb(nullptr, "on_clog_source_flowguard", on_source_flowguard);
    lv_xml_register_event_cb(nullptr, "on_clog_source_afc", on_source_afc);
    lv_xml_register_event_cb(nullptr, "on_clog_mode_auto", on_mode_auto);
    lv_xml_register_event_cb(nullptr, "on_clog_mode_manual", on_mode_manual);
    lv_xml_register_event_cb(nullptr, "on_clog_threshold_changed", on_threshold_changed);
    lv_xml_register_event_cb(nullptr, "on_clog_det_length_changed", on_det_length_changed);

    init_subjects();
}

ClogDetectionConfigModal::~ClogDetectionConfigModal() {
    deinit_subjects();
}

void ClogDetectionConfigModal::init_subjects() {
    if (subjects_initialized_)
        return;

    lv_subject_init_int(&mode_subject_, 2); // default auto

    lv_subject_init_string(&threshold_text_subject_, threshold_text_buf_, nullptr,
                           sizeof(threshold_text_buf_), "Default");

    lv_subject_init_string(&det_length_text_subject_, det_length_text_buf_, nullptr,
                           sizeof(det_length_text_buf_), "---");

    // Default 0 (hidden): on_show() raises it only once the active backend is
    // known to take the detection-mode gcode. Failing closed keeps the controls
    // out of the way when there is no backend at all.
    lv_subject_init_int(&mode_supported_subject_, 0);

    // Per-button boolean subjects for bind_style selected/unselected
    lv_subject_init_int(&src_auto_active_, 1); // auto selected by default
    lv_subject_init_int(&src_encoder_active_, 0);
    lv_subject_init_int(&src_flowguard_active_, 0);
    lv_subject_init_int(&src_afc_active_, 0);
    lv_subject_init_int(&mode_auto_active_, 1); // auto selected by default
    lv_subject_init_int(&mode_manual_active_, 0);

    // Register globally so XML bindings find them
    lv_xml_register_subject(nullptr, "clog_cfg_mode", &mode_subject_);
    lv_xml_register_subject(nullptr, "clog_cfg_threshold_text", &threshold_text_subject_);
    lv_xml_register_subject(nullptr, "clog_cfg_det_length_text", &det_length_text_subject_);
    lv_xml_register_subject(nullptr, "clog_cfg_mode_supported", &mode_supported_subject_);
    lv_xml_register_subject(nullptr, "clog_src_auto_active", &src_auto_active_);
    lv_xml_register_subject(nullptr, "clog_src_encoder_active", &src_encoder_active_);
    lv_xml_register_subject(nullptr, "clog_src_flowguard_active", &src_flowguard_active_);
    lv_xml_register_subject(nullptr, "clog_src_afc_active", &src_afc_active_);
    lv_xml_register_subject(nullptr, "clog_mode_auto_active", &mode_auto_active_);
    lv_xml_register_subject(nullptr, "clog_mode_manual_active", &mode_manual_active_);

    subjects_initialized_ = true;
}

void ClogDetectionConfigModal::deinit_subjects() {
    if (!subjects_initialized_)
        return;
    lv_subject_deinit(&mode_subject_);
    lv_subject_deinit(&threshold_text_subject_);
    lv_subject_deinit(&det_length_text_subject_);
    lv_subject_deinit(&mode_supported_subject_);
    lv_subject_deinit(&src_auto_active_);
    lv_subject_deinit(&src_encoder_active_);
    lv_subject_deinit(&src_flowguard_active_);
    lv_subject_deinit(&src_afc_active_);
    lv_subject_deinit(&mode_auto_active_);
    lv_subject_deinit(&mode_manual_active_);
    subjects_initialized_ = false;
}

void ClogDetectionConfigModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    // Store self pointer on dialog for static callbacks
    if (dialog())
        lv_obj_set_user_data(dialog(), this);

    // Load current config
    auto& wc = helix::PanelWidgetManager::instance().get_widget_config(panel_id_);
    auto config = wc.get_widget_config(widget_id_);

    if (config.contains("source") && config["source"].is_number_integer())
        source_ = config["source"].get<int>();
    if (config.contains("danger_threshold") && config["danger_threshold"].is_number_integer())
        danger_threshold_ = config["danger_threshold"].get<int>();

    // Read current state from AmsState backend
    auto& ams = AmsState::instance();
    auto* backend = ams.get_backend();
    AmsType backend_type = backend ? backend->get_type() : AmsType::NONE;
    if (backend) {
        auto info = backend->get_system_info();
        detection_mode_ = info.encoder_info.detection_mode;
        detection_length_ = info.encoder_info.detection_length;

        has_encoder_ = info.encoder_info.enabled;
        has_flowguard_ = info.flowguard_info.enabled;
        for (const auto& unit : info.units) {
            if (unit.buffer_health && unit.buffer_health->fault_detection_enabled) {
                has_afc_ = true;
                break;
            }
        }
    }
    original_detection_mode_ = detection_mode_;

    // Clamp detection length to valid range
    if (detection_length_ < 2.0f)
        detection_length_ = 10.0f;
    if (detection_length_ > 30.0f)
        detection_length_ = 30.0f;

    // Hide source buttons that aren't available
    update_source_visibility();

    // If current source override points to an unavailable source, reset to auto
    if ((source_ == 1 && !has_encoder_) || (source_ == 2 && !has_flowguard_) ||
        (source_ == 3 && !has_afc_)) {
        source_ = 0;
    }

    // Set slider initial values
    auto* slider = lv_obj_find_by_name(dialog(), "threshold_slider");
    if (slider)
        lv_slider_set_value(slider, danger_threshold_, LV_ANIM_OFF);

    auto* det_slider = lv_obj_find_by_name(dialog(), "det_length_slider");
    if (det_slider)
        lv_slider_set_value(det_slider, static_cast<int>(detection_length_ + 0.5f), LV_ANIM_OFF);

    // Push state to subjects — XML bindings react automatically
    lv_subject_set_int(&mode_subject_, detection_mode_);
    // Mode/length are written with MMU_TEST_CONFIG; hide them on backends that
    // have no such command rather than let Save emit gcode they cannot run.
    bool mode_supported = build_detection_mode_gcode(backend_type, 2, 0.0f).has_value();
    lv_subject_set_int(&mode_supported_subject_, mode_supported ? 1 : 0);
    sync_threshold_text();
    sync_det_length_text();

    // Push button selection to subjects — XML bind_style reacts automatically
    sync_source_subjects();
    sync_mode_subjects();

    spdlog::debug("[ClogConfig] Opened: source={}, mode={}, threshold={}, det_length={:.1f}mm",
                  source_, detection_mode_, danger_threshold_, detection_length_);
}

void ClogDetectionConfigModal::on_ok() {
    nlohmann::json config;
    config["source"] = source_;
    config["danger_threshold"] = danger_threshold_;

    auto& wc = helix::PanelWidgetManager::instance().get_widget_config(panel_id_);
    wc.set_widget_config(widget_id_, config);

    auto& ams = AmsState::instance();
    ams.set_source_override(source_);
    ams.set_danger_threshold_override(danger_threshold_);

    if (detection_mode_ != original_detection_mode_ || detection_mode_ == 1)
        send_detection_mode_gcode(detection_mode_, detection_length_);

    spdlog::info("[ClogConfig] Saved: source={}, mode={}, threshold={}, det_length={:.1f}", source_,
                 detection_mode_, danger_threshold_, detection_length_);
    hide();
}

void ClogDetectionConfigModal::sync_source_subjects() {
    lv_subject_set_int(&src_auto_active_, source_ == 0 ? 1 : 0);
    lv_subject_set_int(&src_encoder_active_, source_ == 1 ? 1 : 0);
    lv_subject_set_int(&src_flowguard_active_, source_ == 2 ? 1 : 0);
    lv_subject_set_int(&src_afc_active_, source_ == 3 ? 1 : 0);
}

void ClogDetectionConfigModal::sync_mode_subjects() {
    lv_subject_set_int(&mode_auto_active_, detection_mode_ == 2 ? 1 : 0);
    lv_subject_set_int(&mode_manual_active_, detection_mode_ == 1 ? 1 : 0);
}

void ClogDetectionConfigModal::update_source_visibility() {
    if (!dialog())
        return;
    auto* btn_enc = lv_obj_find_by_name(dialog(), "btn_source_encoder");
    auto* btn_fg = lv_obj_find_by_name(dialog(), "btn_source_flowguard");
    auto* btn_afc = lv_obj_find_by_name(dialog(), "btn_source_afc");

    if (btn_enc) {
        if (has_encoder_)
            lv_obj_remove_flag(btn_enc, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(btn_enc, LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_fg) {
        if (has_flowguard_)
            lv_obj_remove_flag(btn_fg, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(btn_fg, LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_afc) {
        if (has_afc_)
            lv_obj_remove_flag(btn_afc, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(btn_afc, LV_OBJ_FLAG_HIDDEN);
    }
}

void ClogDetectionConfigModal::sync_threshold_text() {
    if (danger_threshold_ == 0)
        snprintf(threshold_text_buf_, sizeof(threshold_text_buf_), "Default");
    else
        snprintf(threshold_text_buf_, sizeof(threshold_text_buf_), "%d%%", danger_threshold_);
    lv_subject_copy_string(&threshold_text_subject_, threshold_text_buf_);
}

void ClogDetectionConfigModal::sync_det_length_text() {
    snprintf(det_length_text_buf_, sizeof(det_length_text_buf_), "%.0fmm", detection_length_);
    lv_subject_copy_string(&det_length_text_subject_, det_length_text_buf_);
}

std::optional<std::string>
ClogDetectionConfigModal::build_detection_mode_gcode(AmsType type, int mode, float det_length) {
    // MMU_TEST_CONFIG is a Happy Hare command. AFC, ACE, CFS, QIDI Box and the
    // tool changers reach this modal too (the clog widget is offered whenever
    // clog_meter_mode > 0, which includes AFC buffer fault detection), and would
    // answer with "Unknown command".
    if (type != AmsType::HAPPY_HARE)
        return std::nullopt;

    char cmd[96];
    if (mode == 1 && det_length > 0) {
        snprintf(cmd, sizeof(cmd), "MMU_TEST_CONFIG clog_detection=%d detection_length=%.1f", mode,
                 det_length);
    } else {
        snprintf(cmd, sizeof(cmd), "MMU_TEST_CONFIG clog_detection=%d", mode);
    }
    return std::string(cmd);
}

void ClogDetectionConfigModal::send_detection_mode_gcode(int mode, float det_length) {
    // Re-read the backend rather than trust what on_show() saw: the UI gate hides
    // these controls, but the send must refuse on its own too.
    auto* backend = AmsState::instance().get_backend();
    AmsType type = backend ? backend->get_type() : AmsType::NONE;

    auto cmd = build_detection_mode_gcode(type, mode, det_length);
    if (!cmd) {
        spdlog::warn("[ClogConfig] Detection mode is Happy Hare only (MMU_TEST_CONFIG); "
                     "active backend is {} — not sending",
                     ams_type_to_string(type));
        return;
    }

    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[ClogConfig] No API available to send detection mode gcode");
        return;
    }
    api->execute_gcode(
        *cmd, [mode]() { spdlog::info("[ClogConfig] Detection mode set to {}", mode); },
        // Log-only error handler, so the report stays with GcodeErrorRouter's
        // `!!` broadcast (include/rpc_error_policy.h).
        [](const MoonrakerError& err) {
            spdlog::error("[ClogConfig] Detection mode gcode failed: {}", err.message);
        },
        /*timeout_ms=*/0, /*silent=*/false, /*on_queued=*/nullptr,
        /*caller_surfaces_errors=*/false);
}

// Static event callbacks
void ClogDetectionConfigModal::on_source_auto(lv_event_t* e) {
    if (auto* m = get_modal(e)) {
        m->source_ = 0;
        m->sync_source_subjects();
    }
}

void ClogDetectionConfigModal::on_source_encoder(lv_event_t* e) {
    if (auto* m = get_modal(e)) {
        m->source_ = 1;
        m->sync_source_subjects();
    }
}

void ClogDetectionConfigModal::on_source_flowguard(lv_event_t* e) {
    if (auto* m = get_modal(e)) {
        m->source_ = 2;
        m->sync_source_subjects();
    }
}

void ClogDetectionConfigModal::on_source_afc(lv_event_t* e) {
    if (auto* m = get_modal(e)) {
        m->source_ = 3;
        m->sync_source_subjects();
    }
}

void ClogDetectionConfigModal::on_mode_auto(lv_event_t* e) {
    if (auto* m = get_modal(e)) {
        m->detection_mode_ = 2;
        lv_subject_set_int(&m->mode_subject_, 2); // XML binding disables det length slider
        m->sync_mode_subjects();
    }
}

void ClogDetectionConfigModal::on_mode_manual(lv_event_t* e) {
    if (auto* m = get_modal(e)) {
        m->detection_mode_ = 1;
        lv_subject_set_int(&m->mode_subject_, 1); // XML binding enables det length slider
        m->sync_mode_subjects();
    }
}

void ClogDetectionConfigModal::on_threshold_changed(lv_event_t* e) {
    if (auto* m = get_modal(e)) {
        auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
        m->danger_threshold_ = lv_slider_get_value(slider);
        m->sync_threshold_text(); // subject update → XML bind_text reacts
    }
}

void ClogDetectionConfigModal::on_det_length_changed(lv_event_t* e) {
    if (auto* m = get_modal(e)) {
        auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
        int val = lv_slider_get_value(slider);
        val = std::max(2, std::min(30, val));
        m->detection_length_ = static_cast<float>(val);
        m->sync_det_length_text(); // subject update → XML bind_text reacts
    }
}
