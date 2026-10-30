// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "ipp_print_modal.h"

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ipp_printer.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"
#include "label_renderer.h"
#include "lvgl.h"
#include "sheet_label_layout.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

namespace helix {

// ============================================================================
// Static callback registration
// ============================================================================

// ============================================================================
// IppPrintModal
// ============================================================================

IppPrintModal::IppPrintModal() = default;

void IppPrintModal::show_for_spool(const SpoolInfo& spool, PrintCallback callback) {
    spool_ = spool;
    callback_ = std::move(callback);

    // Determine max labels from current sheet template
    const auto& templates = helix::label::get_sheet_templates();
    auto& settings = LabelPrinterSettingsManager::instance();
    int tmpl_idx =
        std::clamp(settings.get_label_size_index(), 0, static_cast<int>(templates.size()) - 1);
    max_labels_ = templates[tmpl_idx].labels_per_sheet();

    show(lv_screen_active());
}

void IppPrintModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    init_dropdowns();
}

void IppPrintModal::init_dropdowns() {
    auto& settings = LabelPrinterSettingsManager::instance();
    const auto& templates = helix::label::get_sheet_templates();
    int tmpl_idx =
        std::clamp(settings.get_label_size_index(), 0, static_cast<int>(templates.size()) - 1);
    const auto& tmpl = templates[tmpl_idx];

    // Set template name display
    auto* name_label = find_widget("template_name");
    if (name_label) {
        lv_label_set_text(name_label, tmpl.name.c_str());
    }

    // Populate label count dropdown (1 through max_labels_)
    auto* count_dd = find_widget("dropdown_count");
    if (count_dd) {
        std::string options;
        for (int i = 1; i <= max_labels_; i++) {
            if (i > 1)
                options += "\n";
            options += std::to_string(i);
        }
        lv_dropdown_set_options(count_dd, options.c_str());

        // Default from saved settings (label_count is 1-indexed, dropdown is 0-indexed)
        int saved = std::clamp(settings.get_label_count() - 1, 0, max_labels_ - 1);
        lv_dropdown_set_selected(count_dd, saved);
    }

    // Populate start position dropdown (Position 1 through max)
    auto* start_dd = find_widget("dropdown_start");
    if (start_dd) {
        std::string options;
        for (int i = 1; i <= max_labels_; i++) {
            if (i > 1)
                options += "\n";
            options += fmt::format("{} {}", lv_tr("Position"), i);
        }
        lv_dropdown_set_options(start_dd, options.c_str());
        lv_dropdown_set_selected(start_dd, 0); // Default to Position 1
    }

    spdlog::debug("[IppPrintModal] Template: {} ({} labels/sheet)", tmpl.name, max_labels_);
}

void IppPrintModal::on_ok() {
    // Read dropdown values
    auto* count_dd = find_widget("dropdown_count");
    auto* start_dd = find_widget("dropdown_start");
    int count = count_dd ? static_cast<int>(lv_dropdown_get_selected(count_dd)) + 1 : 1;
    int start = start_dd ? static_cast<int>(lv_dropdown_get_selected(start_dd)) : 0;

    spdlog::info("[IppPrintModal] Printing {} label(s) starting at position {}", count, start + 1);

    // Save label count preference for next time
    auto& settings = LabelPrinterSettingsManager::instance();
    settings.set_label_count(count);

    // Get label size and render
    auto sizes = IppPrinter::supported_sizes_static();
    int size_idx =
        std::clamp(settings.get_label_size_index(), 0, static_cast<int>(sizes.size()) - 1);
    const auto& label_size = sizes[size_idx];
    auto preset = static_cast<LabelPreset>(
        std::clamp(settings.get_label_preset(), 0, static_cast<int>(LabelPreset::MINIMAL)));

    auto bitmap = LabelRenderer::render(spool_, preset, label_size);
    if (bitmap.empty()) {
        spdlog::error("[IppPrintModal] Failed to render label bitmap");
        if (callback_)
            callback_(false, "Failed to render label");
        hide();
        return;
    }

    // Configure and print via IPP
    static IppPrinter ipp_printer;
    apply_ipp_target(ipp_printer);
    ipp_printer.set_sheet_template(size_idx);
    ipp_printer.set_label_count(count);
    ipp_printer.set_start_position(start);

    auto cb = callback_;
    ipp_printer.print(bitmap, label_size, cb);

    hide();
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Printing label..."), 2000);
}

void IppPrintModal::on_cancel() {
    hide();
}

// ============================================================================
// Free function
// ============================================================================

bool maybe_show_ipp_print_modal(const SpoolInfo& spool, PrintCallback callback) {
    auto& settings = LabelPrinterSettingsManager::instance();
    if (settings.get_printer_type() != "network" || settings.get_printer_protocol() != "ipp")
        return false;

    static IppPrintModal modal;
    modal.show_for_spool(spool, std::move(callback));
    return true;
}

} // namespace helix

#endif // HELIX_HAS_LABEL_PRINTER
