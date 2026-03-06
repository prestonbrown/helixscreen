// SPDX-License-Identifier: GPL-3.0-or-later

#include "clog_detection_widget.h"

#include "ams_state.h"
#include "ams_types.h"
#include "buffer_status_modal.h"
#include "clog_detection_config_modal.h"
#include "panel_widget_registry.h"
#include "ui_buffer_meter.h"
#include "ui_carousel.h"
#include "ui_clog_meter.h"
#include "ui_update_queue.h"

#include "helix-xml/src/xml/lv_xml.h"

#include <spdlog/spdlog.h>

namespace helix {
void register_clog_detection_widget() {
    register_widget_factory("clog_detection",
                            []() { return std::make_unique<ClogDetectionWidget>(); });

    lv_xml_register_event_cb(nullptr, "on_clog_detection_widget_clicked", [](lv_event_t* /*e*/) {
        auto* backend = AmsState::instance().get_backend();
        if (!backend)
            return;
        auto info = backend->get_system_info();
        BufferStatusModal::show_for(info, 0);
    });
}
} // namespace helix

using namespace helix;

ClogDetectionWidget::~ClogDetectionWidget() {
    detach();
}

void ClogDetectionWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    widget_obj_ = widget_obj;
    if (!widget_obj_)
        return;

    lv_obj_set_user_data(widget_obj_, this);

    carousel_ = lv_obj_find_by_name(widget_obj_, "filament_health_carousel");
    if (!carousel_) {
        spdlog::warn("[ClogDetectionWidget] filament_health_carousel not found");
        return;
    }

    build_carousel_pages();
    apply_config();
}

void ClogDetectionWidget::build_carousel_pages() {
    if (!carousel_)
        return;

    // Page 1: Clog arc meter (always added; UiClogMeter auto-hides when mode=0)
    clog_page_ = static_cast<lv_obj_t*>(lv_xml_create(lv_scr_act(), "clog_meter_page", nullptr));
    if (!clog_page_) {
        spdlog::error("[ClogDetectionWidget] Failed to create clog_meter_page XML component");
        return;
    }

    clog_meter_ = std::make_unique<ui::UiClogMeter>(clog_page_);
    clog_meter_->set_fill_mode(true);
    ui_carousel_add_item(carousel_, clog_page_);

    // Page 2: Buffer meter (only if Happy Hare sync feedback bias is available)
    auto* backend = AmsState::instance().get_backend();
    if (backend) {
        auto info = backend->get_system_info();
        if (info.type == AmsType::HAPPY_HARE && info.sync_feedback_bias > -1.5f) {
            buffer_page_ = lv_obj_create(lv_scr_act());
            lv_obj_set_size(buffer_page_, LV_PCT(100), LV_PCT(100));
            lv_obj_set_style_pad_all(buffer_page_, 0, 0);
            lv_obj_set_style_border_width(buffer_page_, 0, 0);
            lv_obj_set_style_bg_opa(buffer_page_, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(buffer_page_, LV_OBJ_FLAG_SCROLLABLE);

            buffer_meter_ = std::make_unique<ui::UiBufferMeter>(buffer_page_);
            buffer_meter_->set_bias(info.sync_feedback_bias);
            ui_carousel_add_item(carousel_, buffer_page_);
            has_buffer_page_ = true;

            spdlog::debug("[ClogDetectionWidget] Added buffer meter page (bias={:.2f})",
                          info.sync_feedback_bias);
        }
    }

    // Only show indicators when there are 2+ pages
    auto* state = ui_carousel_get_state(carousel_);
    if (state) {
        state->show_indicators = has_buffer_page_;
    }
    ui_carousel_rebuild_indicators(carousel_);

    spdlog::debug("[ClogDetectionWidget] Built carousel with {} page(s)",
                  has_buffer_page_ ? 2 : 1);
}

void ClogDetectionWidget::detach() {
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        buffer_meter_.reset();
        clog_meter_.reset();
    }

    carousel_ = nullptr;
    clog_page_ = nullptr;
    buffer_page_ = nullptr;
    has_buffer_page_ = false;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
}

void ClogDetectionWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int /*width_px*/,
                                          int /*height_px*/) {
    if (clog_meter_)
        clog_meter_->resize_arc();
    if (buffer_meter_)
        buffer_meter_->resize();
}

void ClogDetectionWidget::on_activate() {
    // Refresh buffer meter bias when panel becomes visible
    if (buffer_meter_) {
        auto* backend = AmsState::instance().get_backend();
        if (backend) {
            auto info = backend->get_system_info();
            if (info.sync_feedback_bias > -1.5f) {
                buffer_meter_->set_bias(info.sync_feedback_bias);
            }
        }
    }
}

bool ClogDetectionWidget::on_edit_configure() {
    spdlog::info("[ClogDetectionWidget] Configure requested - showing config modal");
    config_modal_ = std::make_unique<ClogDetectionConfigModal>(id(), panel_id());
    config_modal_->show(lv_screen_active());
    return false;
}

void ClogDetectionWidget::set_config(const nlohmann::json& config) {
    config_ = config;
}

void ClogDetectionWidget::apply_config() {
    int source = 0;
    int threshold = 0;
    if (config_.contains("source") && config_["source"].is_number_integer())
        source = config_["source"].get<int>();
    if (config_.contains("danger_threshold") && config_["danger_threshold"].is_number_integer())
        threshold = config_["danger_threshold"].get<int>();

    auto& ams = AmsState::instance();
    ams.set_source_override(source);
    ams.set_danger_threshold_override(threshold);

    spdlog::debug("[ClogDetectionWidget] Applied config: source={}, threshold={}", source, threshold);
}
