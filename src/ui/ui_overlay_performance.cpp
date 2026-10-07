// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_overlay_performance.h"

#include "ui_panel_common.h"
#include "ui_utils.h" // helix::ui::safe_clean_children

#include "observer_factory.h"
#include "performance_state.h" // helix::perf::PerformanceState::subjects_lifetime()
#include "text_io.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <string>
#include <string_view>
#include <vector>

namespace helix {
namespace ui {

UiOverlayPerformance& UiOverlayPerformance::instance() {
    static UiOverlayPerformance s;
    return s;
}

lv_obj_t* UiOverlayPerformance::create(lv_obj_t* parent) {
    if (root_)
        return root_;

    root_ = helix::ui::create_xml_hidden(parent, "performance_overlay");
    if (!root_) {
        spdlog::error("[UiOverlayPerformance] Failed to create root from XML");
        return nullptr;
    }

    mcu_card_ = helix::ui::find_required(root_, "mcu_card", "UiOverlayPerformance");

    lv_subject_t* names_subj = lv_xml_get_subject(nullptr, "perf_mcu_names");
    if (names_subj) {
        // perf_mcu_names is NOT a never-freed singleton: PerformanceState destroys
        // and recreates its subjects on reconnect / printer switch (and between
        // tests) via deinit_subjects()/init_subjects(). This observer lives on the
        // singleton, so it can outlive the subject. Pass PerformanceState's subject
        // lifetime so the ObserverGuard releases safely when the subject is torn
        // down — otherwise reset() would call lv_observer_remove() on a freed
        // subject → UAF at lv_observer.c:584 ([L077], mirrors 056bb40e9 for
        // HelixSparkline on perf_history_tick).
        mcu_names_observer_ = helix::ui::observe<const char*>(
            names_subj, this,
            [](UiOverlayPerformance* self, const char* /*v*/) { self->rebuild_mcu_rows(); },
            helix::perf::PerformanceState::instance().subjects_lifetime());
    } else {
        spdlog::warn("[UiOverlayPerformance] perf_mcu_names subject not found");
    }

    return root_;
}

void UiOverlayPerformance::rebuild_mcu_rows() {
    if (!mcu_card_)
        return;

    lv_subject_t* names_subj = lv_xml_get_subject(nullptr, "perf_mcu_names");
    if (!names_subj)
        return;
    const char* names_cstr = lv_subject_get_string(names_subj);
    std::string names_str = (names_cstr ? names_cstr : "");

    // Gate on an actual change. perf_mcu_names is re-published on essentially
    // every perf sample even when the MCU set is identical; rebuilding the rows
    // (each height="content" = LV_SIZE_CONTENT) on every notification both wastes
    // work and, on 32-bit ARM, opens a crash window: a freshly-created row still
    // carries the unresolved LV_COORD_MAX sentinel until the next layout pass,
    // and a render that blits it in that window overflows int32 fill arithmetic
    // → SIGSEGV. Skipping the no-op rebuild eliminates that churn. See
    // (prestonbrown/helixscreen#1061).
    if (names_str == last_mcu_names_)
        return;
    last_mcu_names_ = names_str;

    helix::ui::safe_clean_children(mcu_card_);

    // An empty names string that differs from a previously-non-empty value must
    // still clear the rows (done above) before returning.
    if (names_str.empty())
        return;

    std::vector<std::string> names;
    for (std::string_view sv : helix::text_io::lines(names_str, ',')) {
        std::string item(sv);
        if (!item.empty()) {
            names.push_back(item);
        }
    }

    for (const auto& name : names) {
        // Sanitize name for use as subject key: spaces, slashes, dots → underscores
        std::string safe = name;
        for (auto& c : safe) {
            if (c == ' ' || c == '/' || c == '.')
                c = '_';
        }

        const std::string bind_value = "perf_mcu_" + safe + "_load_pct";
        const std::string bind_text = "perf_mcu_" + safe + "_text";
        const std::string bind_present = "perf_mcu_" + safe + "_present";
        const std::string spark_src = "mcu_" + safe + "_load_pct";

        const char* attrs[] = {"label",
                               name.c_str(),
                               "bind_value",
                               bind_value.c_str(),
                               "bind_text",
                               bind_text.c_str(),
                               "bind_present",
                               bind_present.c_str(),
                               "sparkline_source",
                               spark_src.c_str(),
                               nullptr,
                               nullptr};
        lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(mcu_card_, "perf_metric_row", attrs));
        if (!row) {
            spdlog::warn("[UiOverlayPerformance] Failed to create perf_metric_row for MCU '{}'",
                         name);
        }
    }

    // Resolve the new rows' coordinates synchronously. Each row is sized
    // height="content" (LV_SIZE_CONTENT), so until a layout pass runs its width
    // and height report the LV_COORD_MAX sentinel (0x1FFFFFFF). On 32-bit ARM a
    // render that blits a row carrying that sentinel overflows int32 clip/fill
    // arithmetic and walks off the heap → SIGSEGV. Forcing the layout here means
    // no row is ever handed to a render pass with unresolved coordinates. See
    // (prestonbrown/helixscreen#1061).
    lv_obj_update_layout(mcu_card_);
}

} // namespace ui
} // namespace helix
