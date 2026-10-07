// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "ui_settings_label_printer.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "bluetooth_loader.h"
#include "brother_pt_bt_printer.h"
#include "brother_ql_printer.h"
#include "bt_discovery_run.h"
#include "bt_discovery_utils.h"
#include "helix_thread.h"
#include "ipp_printer.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"
#include "log_redact.h"
#include "makeid_protocol.h"
#include "niimbot_protocol.h"
#include "phomemo_printer.h"
#include "runtime_config.h"
#include "sheet_label_layout.h"
#include "spoolman_types.h"
#include "ui/ui_widget_helpers.h"
#include "usb_printer_detector.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <thread>

namespace helix::settings {

using helix::ui::event_selected;
using helix::ui::find_required;

// ============================================================================
// HELPERS
// ============================================================================

/// Map cryptic BLE chip names to user-friendly product names.
/// Some printers advertise their SoC name rather than the product brand.
static std::string friendly_bt_name(const std::string& name) {
    // MakeID E1 uses Yichip BLE SoC — advertises as "YichipFPGA-XXXX"
    if (strncasecmp(name.c_str(), "YichipFPGA", 10) == 0) {
        return "MakeID (" + name + ")"; // i18n: do not translate — product name
    }
    return name;
}

/// Build dropdown label for a BT device showing connection status.
/// @param paired  BlueZ confirms device is paired
/// @param saved   Device address is saved in our settings (may not be BlueZ-paired)
/// @param connected  Active BLE/RFCOMM connection
static std::string bt_device_label(const std::string& name, bool paired, bool saved,
                                   bool connected) {
    std::string label = friendly_bt_name(name);
    if (connected) {
        label += " (Connected)";
    } else if (paired) {
        label += " (Paired)";
    } else if (saved) {
        label += " (Saved)";
    }
    return label;
}

/// Check BLE connection state for a device via BlueZ D-Bus.
static bool check_bt_connected(helix_bt_context* ctx, const std::string& mac) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!ctx || !loader.is_connected)
        return false;
    return loader.is_connected(ctx, mac.c_str()) == 1;
}

/// Return the correct label sizes for the current printer configuration.
/// Returns true if the current printer auto-detects label/tape size (no manual selection).
static bool current_printer_auto_detects_size() {
    auto& settings = LabelPrinterSettingsManager::instance();
    if (settings.get_printer_type() == "bluetooth") {
        const auto bt_name = settings.get_bt_name();
        return helix::bluetooth::is_brother_pt_printer(bt_name.c_str());
    }
    return false;
}

/// Mirrors the logic in label_printer_utils.cpp print_spool_label().
static std::vector<helix::LabelSize> get_sizes_for_current_printer() {
    auto& settings = LabelPrinterSettingsManager::instance();
    const auto type = settings.get_printer_type();

    if (type == "network" && settings.get_printer_protocol() == "ipp") {
        return helix::IppPrinter::supported_sizes_static();
    }
    if (type == "usb") {
        return helix::PhomemoPrinter::supported_sizes_static();
    }
    if (type == "bluetooth") {
        const auto bt_name = settings.get_bt_name();
        if (helix::bluetooth::is_brother_pt_printer(bt_name.c_str())) {
            return helix::label::BrotherPTBluetoothPrinter::supported_sizes_static();
        }
        if (helix::bluetooth::is_brother_printer(bt_name.c_str())) {
            return BrotherQLPrinter::supported_sizes_static();
        }
        if (helix::bluetooth::is_niimbot_printer(bt_name.c_str())) {
            return helix::label::niimbot_sizes_for_model(bt_name);
        }
        if (helix::bluetooth::is_makeid_printer(bt_name.c_str())) {
            return helix::label::makeid_default_sizes();
        }
        return helix::PhomemoPrinter::supported_sizes_static();
    }
    return BrotherQLPrinter::supported_sizes_static();
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

LabelPrinterSettingsOverlay::~LabelPrinterSettingsOverlay() {
    stop_label_printer_discovery();
    stop_usb_detection();
    stop_bt_discovery();
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void LabelPrinterSettingsOverlay::init_subjects() {
    // Ensure manager subjects are initialized (reads config for initial value)
    LabelPrinterSettingsManager::instance().init_subjects();

    // Register C++-owned subject globally so XML bind_flag_if_not_eq can find it.
    // Deliberately NOT registered with subjects_: the name aliases
    // LabelPrinterSettingsManager's subject (owner registered as
    // "label_printer_type"), a process-lifetime singleton — its storage
    // outlives this overlay, so the name never dangles (prestonbrown/helixscreen#1538).
    lv_xml_register_subject(nullptr, "printer_type_subject",
                            LabelPrinterSettingsManager::instance().subject_printer_type());

    // BT scanning state subject (0=idle, 1=scanning)
    UI_MANAGED_SUBJECT_INT(bt_scanning_subject_, 0, "bt_scanning", subjects_);
    UI_MANAGED_SUBJECT_INT(test_printing_subject_, 0, "test_printing", subjects_);

    // IPP selected subject (0=not IPP, 1=IPP protocol selected within network type)
    int ipp_initial = (LabelPrinterSettingsManager::instance().get_printer_type() == "network" &&
                       LabelPrinterSettingsManager::instance().get_printer_protocol() == "ipp")
                          ? 1
                          : 0;
    UI_MANAGED_SUBJECT_INT(ipp_selected_subject_, ipp_initial, "ipp_selected", subjects_);
}

void LabelPrinterSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_lp_label_size_changed",
         [](lv_event_t* e) {
             get_label_printer_settings_overlay().handle_label_size_changed(event_selected(e));
         }},
        {"on_lp_preset_changed",
         [](lv_event_t* e) {
             get_label_printer_settings_overlay().handle_preset_changed(event_selected(e));
         }},
        {"on_lp_test_print",
         [](lv_event_t*) { get_label_printer_settings_overlay().handle_test_print(); }},
        {"on_lp_printer_selected",
         [](lv_event_t* e) {
             get_label_printer_settings_overlay().handle_printer_selected(event_selected(e));
         }},
        {"on_lp_type_changed",
         [](lv_event_t* e) {
             get_label_printer_settings_overlay().handle_type_changed(event_selected(e));
         }},
        {"on_lp_usb_printer_selected",
         [](lv_event_t* e) {
             get_label_printer_settings_overlay().handle_usb_printer_selected(event_selected(e));
         }},
        {"on_lp_bt_printer_selected",
         [](lv_event_t* e) {
             get_label_printer_settings_overlay().handle_bt_printer_selected(event_selected(e));
         }},
        {"on_lp_bt_scan",
         [](lv_event_t*) { get_label_printer_settings_overlay().handle_bt_scan(); }},
        {"on_lp_bt_connect",
         [](lv_event_t*) { get_label_printer_settings_overlay().handle_bt_connect(); }},
        {"on_lp_bt_forget",
         [](lv_event_t*) { get_label_printer_settings_overlay().handle_bt_forget(); }},
        {"on_lp_label_count_changed",
         [](lv_event_t* e) {
             get_label_printer_settings_overlay().handle_label_count_changed(event_selected(e));
         }},
    });
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void LabelPrinterSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_printer_type_dropdown();
    init_address_input();
    init_port_input();
    init_label_size_dropdown();
    init_preset_dropdown();
    init_discovery_dropdown();
    init_usb_printer_dropdown();
    init_bt_printer_dropdown();
    init_label_count_dropdown();

    // Update IPP selected subject based on current config
    update_ipp_selected_subject();

    // Start detection based on current printer type
    auto& settings = LabelPrinterSettingsManager::instance();
    const auto type = settings.get_printer_type();
    if (type == "usb") {
        start_usb_detection();
    } else if (type == "bluetooth") {
        // BT discovery is on-demand (scan button), not automatic
    } else {
        start_label_printer_discovery();
    }

    inputs_initialized_ = true;
}

void LabelPrinterSettingsOverlay::on_deactivating(DeactivateReason) {
    stop_label_printer_discovery();
    stop_usb_detection();
    stop_bt_discovery();
}

// ============================================================================
// INTERNAL METHODS
// ============================================================================

void LabelPrinterSettingsOverlay::init_address_input() {
    if (!overlay_root_)
        return;

    auto& settings = LabelPrinterSettingsManager::instance();
    lv_obj_t* row = find_required(overlay_root_, "row_address_port", get_name());
    if (!row)
        return;

    lv_obj_t* input = helix::ui::find_required(row, "input_address", get_name());
    if (input) {
        lv_textarea_set_text(input, settings.get_printer_address().c_str());
        if (!inputs_initialized_) {
            lv_obj_add_event_cb(
                input,
                [](lv_event_t*) {
                    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] address_done");
                    get_label_printer_settings_overlay().handle_address_changed();
                    LVGL_SAFE_EVENT_CB_END();
                },
                LV_EVENT_DEFOCUSED, nullptr);
        }
        spdlog::trace("[{}] Address input initialized", get_name());
    }
}

void LabelPrinterSettingsOverlay::init_port_input() {
    if (!overlay_root_)
        return;

    auto& settings = LabelPrinterSettingsManager::instance();
    lv_obj_t* row = find_required(overlay_root_, "row_address_port", get_name());
    if (!row)
        return;

    lv_obj_t* input = helix::ui::find_required(row, "input_port", get_name());
    if (input) {
        auto port_str = fmt::format("{}", settings.get_printer_port());
        lv_textarea_set_text(input, port_str.c_str());
        if (!inputs_initialized_) {
            lv_obj_add_event_cb(
                input,
                [](lv_event_t*) {
                    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] port_done");
                    get_label_printer_settings_overlay().handle_port_changed();
                    LVGL_SAFE_EVENT_CB_END();
                },
                LV_EVENT_DEFOCUSED, nullptr);
        }
        spdlog::trace("[{}] Port input initialized", get_name());
    }
}

void LabelPrinterSettingsOverlay::init_label_size_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* size_row = find_required(overlay_root_, "row_label_size", get_name());
    if (!size_row)
        return;

    lv_obj_t* dropdown = helix::ui::find_required(size_row, "dropdown", get_name());
    if (dropdown) {
        // PT printers auto-detect tape — disable size selection
        if (current_printer_auto_detects_size()) {
            lv_dropdown_set_options(dropdown, lv_tr("Auto-detect"));
            lv_dropdown_set_selected(dropdown, 0);
            lv_obj_add_state(dropdown, LV_STATE_DISABLED);
            spdlog::trace("[{}] Label size dropdown disabled (auto-detect)", get_name());
            return;
        }

        lv_obj_remove_state(dropdown, LV_STATE_DISABLED);
        auto& settings = LabelPrinterSettingsManager::instance();
        auto sizes = get_sizes_for_current_printer();

        std::string options;
        for (size_t i = 0; i < sizes.size(); i++) {
            if (i > 0)
                options += "\n";
            options += sizes[i].name;
        }

        if (!options.empty()) {
            lv_dropdown_set_options(dropdown, options.c_str());
            int current_idx = settings.get_label_size_index();
            int max_idx = static_cast<int>(sizes.size()) - 1;
            if (current_idx > max_idx) {
                current_idx = 0;
                settings.set_label_size_index(0);
            }
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(current_idx));
        }
        spdlog::trace("[{}] Label size dropdown initialized ({} sizes)", get_name(), sizes.size());
    }
}

void LabelPrinterSettingsOverlay::init_preset_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* preset_row = find_required(overlay_root_, "row_preset", get_name());
    if (!preset_row)
        return;

    lv_obj_t* dropdown = helix::ui::find_required(preset_row, "dropdown", get_name());
    if (dropdown) {
        auto& settings = LabelPrinterSettingsManager::instance();
        // Build translated preset options
        auto options =
            fmt::format("{}\n{}\n{}", lv_tr("Standard"), lv_tr("Compact"), lv_tr("QR Only"));
        lv_dropdown_set_options(dropdown, options.c_str());
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(settings.get_label_preset()));

        // For small square labels, only QR-only is available
        auto sizes = get_sizes_for_current_printer();
        int size_idx =
            std::clamp(settings.get_label_size_index(), 0, static_cast<int>(sizes.size()) - 1);
        const auto& sz = sizes[size_idx];
        if (sz.width_px <= 250 && sz.height_px > 0 && sz.height_px <= 250) {
            lv_dropdown_set_options(dropdown, lv_tr("QR Only"));
            lv_dropdown_set_selected(dropdown, 0);
            lv_obj_add_state(dropdown, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(dropdown, LV_STATE_DISABLED);
        }

        spdlog::trace("[{}] Preset dropdown initialized", get_name());
    }
}

// ============================================================================
// MDNS DISCOVERY
// ============================================================================

void LabelPrinterSettingsOverlay::init_discovery_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = find_required(overlay_root_, "row_discovered_printers", get_name());
    if (!row)
        return;

    // Hide the empty description text to vertically center the label
    lv_obj_t* desc = helix::ui::find_required(row, "description", get_name());
    if (desc) {
        lv_obj_add_flag(desc, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
    if (dropdown) {
        lv_dropdown_set_options(dropdown, lv_tr("Searching..."));
    }
}

void LabelPrinterSettingsOverlay::start_label_printer_discovery() {
    // Start raw TCP discovery (_pdl-datastream._tcp)
    if (!mdns_discovery_ || !mdns_discovery_->is_discovering()) {
        mdns_discovery_ = std::make_unique<MdnsDiscovery>("_pdl-datastream._tcp.local");
        auto token = lifetime_.token();
        mdns_discovery_->start_discovery(
            [this, token](const std::vector<DiscoveredPrinter>& printers) {
                // L081 Mechanism C: merge_and_update_discovery() touches LVGL
                // (lv_dropdown_set_options) — must run on main thread.
                token.defer("LabelPrinterSettings::raw_discovery_apply", [this, printers]() {
                    raw_printers_ = printers;
                    merge_and_update_discovery();
                });
            });
    }

    // Start IPP discovery (_ipp._tcp)
    if (!ipp_mdns_discovery_ || !ipp_mdns_discovery_->is_discovering()) {
        ipp_mdns_discovery_ = std::make_unique<MdnsDiscovery>("_ipp._tcp.local");
        auto token = lifetime_.token();
        ipp_mdns_discovery_->start_discovery(
            [this, token](const std::vector<DiscoveredPrinter>& printers) {
                // L081 Mechanism C: merge_and_update_discovery() touches LVGL
                // (lv_dropdown_set_options) — must run on main thread.
                token.defer("LabelPrinterSettings::ipp_discovery_apply", [this, printers]() {
                    ipp_printers_ = printers;
                    merge_and_update_discovery();
                });
            });
    }

    spdlog::debug("[{}] Started network printer mDNS discovery (raw TCP + IPP)", get_name());
}

void LabelPrinterSettingsOverlay::stop_label_printer_discovery() {
    if (mdns_discovery_) {
        mdns_discovery_->stop_discovery();
        mdns_discovery_.reset();
    }
    if (ipp_mdns_discovery_) {
        ipp_mdns_discovery_->stop_discovery();
        ipp_mdns_discovery_.reset();
    }
    spdlog::debug("[{}] Stopped network printer mDNS discovery", get_name());
}

void LabelPrinterSettingsOverlay::merge_and_update_discovery() {
    // Score and merge results from both raw TCP and IPP discoveries
    discovered_network_printers_.clear();

    for (const auto& p : raw_printers_) {
        int score = helix::label_printer_score(p);
        if (score > 0) {
            discovered_network_printers_.push_back({p, "raw", score});
        } else {
            spdlog::debug("[{}] Filtered out non-label printer: {} ({})", get_name(), p.name,
                          p.ip_address);
        }
    }

    for (const auto& p : ipp_printers_) {
        int score = helix::ipp_printer_score(p);
        if (score > 0) {
            discovered_network_printers_.push_back({p, "ipp", score});
        } else {
            spdlog::debug("[{}] Filtered out non-page printer: {} ({})", get_name(), p.name,
                          p.ip_address);
        }
    }

    // Sort by score descending (most likely printers first)
    std::sort(discovered_network_printers_.begin(), discovered_network_printers_.end(),
              [](const DiscoveredNetworkPrinter& a, const DiscoveredNetworkPrinter& b) {
                  return a.score > b.score;
              });

    if (!overlay_root_)
        return;

    lv_obj_t* row = find_required(overlay_root_, "row_discovered_printers", get_name());
    if (!row)
        return;

    lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
    if (!dropdown)
        return;

    // Dedup by IP+protocol — keep highest-scored per unique combination
    {
        std::vector<DiscoveredNetworkPrinter> deduped;
        for (const auto& np : discovered_network_printers_) {
            bool duplicate = false;
            for (const auto& existing : deduped) {
                if (existing.printer.ip_address == np.printer.ip_address &&
                    existing.protocol == np.protocol) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                deduped.push_back(np);
            }
        }
        discovered_network_printers_ = std::move(deduped);
    }

    std::string options;
    if (discovered_network_printers_.empty()) {
        options = lv_tr("No printers found");
    } else {
        for (const auto& np : discovered_network_printers_) {
            if (!options.empty()) {
                options += "\n";
            }
            options += np.printer.name + " (" + np.printer.ip_address + ")";
            if (np.protocol == "ipp") {
                options += " [IPP]"; // i18n: do not translate — protocol name
            }
        }
    }

    lv_dropdown_set_options(dropdown, options.c_str());

    spdlog::debug("[{}] Discovery update: {} raw + {} ipp = {} merged printers", get_name(),
                  raw_printers_.size(), ipp_printers_.size(), discovered_network_printers_.size());

    // Auto-select the first discovered printer if no address is configured yet
    if (!discovered_network_printers_.empty()) {
        auto& settings = LabelPrinterSettingsManager::instance();
        if (!settings.is_configured()) {
            handle_printer_selected(0);
            spdlog::info("[{}] Auto-selected first discovered printer: {}", get_name(),
                         discovered_network_printers_[0].printer.name);
        }
    }
}

void LabelPrinterSettingsOverlay::handle_printer_selected(int index) {
    if (index < 0 || index >= static_cast<int>(discovered_network_printers_.size())) {
        return;
    }

    const auto& np = discovered_network_printers_[index];
    const auto& printer = np.printer;
    int port = (np.protocol == "ipp") ? (printer.port > 0 ? printer.port : 631) : printer.port;

    spdlog::info("[{}] Selected printer: {} ({}:{}, protocol={})", get_name(), printer.name,
                 printer.ip_address, port, np.protocol);

    auto& settings = LabelPrinterSettingsManager::instance();
    settings.set_printer_address(printer.ip_address);
    settings.set_printer_port(port);
    settings.set_printer_protocol(np.protocol);

    // Update IPP selected subject and refresh dependent UI
    update_ipp_selected_subject();
    init_label_size_dropdown();
    init_label_count_dropdown();

    // Update address and port input fields
    if (overlay_root_) {
        lv_obj_t* addr_row = find_required(overlay_root_, "row_address_port", get_name());
        if (addr_row) {
            lv_obj_t* addr_input = helix::ui::find_required(addr_row, "input_address", get_name());
            if (addr_input) {
                lv_textarea_set_text(addr_input, printer.ip_address.c_str());
            }

            lv_obj_t* port_input = helix::ui::find_required(addr_row, "input_port", get_name());
            if (port_input) {
                auto port_str = fmt::format("{}", port);
                lv_textarea_set_text(port_input, port_str.c_str());
            }
        }
    }
}

void LabelPrinterSettingsOverlay::update_ipp_selected_subject() {
    if (!subjects_initialized_)
        return;
    auto& settings = LabelPrinterSettingsManager::instance();
    int ipp_val =
        (settings.get_printer_type() == "network" && settings.get_printer_protocol() == "ipp") ? 1
                                                                                               : 0;
    lv_subject_set_int(&ipp_selected_subject_, ipp_val);
}

// ============================================================================
// PRINTER TYPE
// ============================================================================

void LabelPrinterSettingsOverlay::init_printer_type_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = find_required(overlay_root_, "row_printer_type", get_name());
    if (!row)
        return;

    lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
    if (dropdown) {
        const bool bt_available = helix::bluetooth::BluetoothLoader::instance().is_available();
        std::string options;
        if (bt_available) {
            options = fmt::format("{}\n{}\n{}", lv_tr("Network"), lv_tr("USB"), lv_tr("Bluetooth"));
        } else {
            options = fmt::format("{}\n{}", lv_tr("Network"), lv_tr("USB"));
        }
        lv_dropdown_set_options(dropdown, options.c_str());

        auto& settings = LabelPrinterSettingsManager::instance();
        const auto type = settings.get_printer_type();
        int type_idx = 0;
        if (type == "usb")
            type_idx = 1;
        else if (type == "bluetooth" && bt_available)
            type_idx = 2;
        else if (type == "bluetooth" && !bt_available) {
            spdlog::warn("[{}] Saved type is bluetooth but BT unavailable, falling back to network",
                         get_name());
            settings.set_printer_type("network");
        }
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(type_idx));
    }
}

void LabelPrinterSettingsOverlay::handle_type_changed(int index) {
    const bool bt_available = helix::bluetooth::BluetoothLoader::instance().is_available();
    std::string type;
    if (bt_available) {
        // 0=network, 1=usb, 2=bluetooth
        if (index == 0)
            type = "network";
        else if (index == 1)
            type = "usb";
        else if (index == 2)
            type = "bluetooth";
        else
            type = "network";
    } else {
        // 0=network, 1=usb (no bluetooth)
        if (index == 0)
            type = "network";
        else if (index == 1)
            type = "usb";
        else
            type = "network";
    }

    spdlog::info("[{}] Printer type changed: {} (index={})", get_name(), type, index);

    auto& settings = LabelPrinterSettingsManager::instance();
    settings.set_printer_type(type);

    // Reset protocol to raw when switching away from network
    if (type != "network") {
        settings.set_printer_protocol("raw");
    }

    // Update IPP selected subject
    update_ipp_selected_subject();

    // Refresh label size dropdown for new backend
    init_label_size_dropdown();

    // Reset label size to first option for new backend
    settings.set_label_size_index(0);

    // Stop all discoveries, then start the appropriate one
    stop_label_printer_discovery();
    stop_usb_detection();
    stop_bt_discovery();

    if (type == "usb") {
        detected_usb_printers_.clear();
        init_usb_printer_dropdown();
        start_usb_detection();
    } else if (type == "bluetooth") {
        bt_devices_.clear();
        init_bt_printer_dropdown();
        // BT discovery is on-demand via scan button
    } else {
        discovered_network_printers_.clear();
        raw_printers_.clear();
        ipp_printers_.clear();
        init_discovery_dropdown();
        start_label_printer_discovery();
    }
}

// ============================================================================
// LABEL COUNT
// ============================================================================

void LabelPrinterSettingsOverlay::init_label_count_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = find_required(overlay_root_, "row_label_count", get_name());
    if (!row)
        return;

    lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
    if (!dropdown)
        return;

    // Determine max labels from current sheet template
    auto& settings = LabelPrinterSettingsManager::instance();
    const auto& templates = helix::label::get_sheet_templates();
    int tmpl_idx =
        std::clamp(settings.get_label_size_index(), 0, static_cast<int>(templates.size()) - 1);
    int max_labels = templates[tmpl_idx].labels_per_sheet();

    std::string options;
    for (int i = 1; i <= max_labels; i++) {
        if (i > 1)
            options += "\n";
        options += fmt::format("{}", i);
    }
    lv_dropdown_set_options(dropdown, options.c_str());

    int saved_count = std::clamp(settings.get_label_count(), 1, max_labels);
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(saved_count - 1));

    spdlog::trace("[{}] Label count dropdown initialized (max={})", get_name(), max_labels);
}

void LabelPrinterSettingsOverlay::handle_label_count_changed(int index) {
    int count = index + 1; // dropdown index 0 = "1 label"
    spdlog::info("[{}] Label count changed: {} (index {})", get_name(), count, index);
    LabelPrinterSettingsManager::instance().set_label_count(count);
}

// ============================================================================
// USB DETECTION
// ============================================================================

void LabelPrinterSettingsOverlay::init_usb_printer_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = find_required(overlay_root_, "row_usb_printers", get_name());
    if (!row)
        return;

    lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
    if (dropdown) {
        lv_dropdown_set_options(dropdown, lv_tr("Searching..."));
    }
}

void LabelPrinterSettingsOverlay::start_usb_detection() {
    if (usb_detector_ && usb_detector_->is_polling())
        return;

    usb_detector_ = std::make_unique<helix::UsbPrinterDetector>();
    auto token = lifetime_.token();
    usb_detector_->start_polling([this, token](const std::vector<helix::UsbPrinterInfo>& printers) {
        // L081 Mechanism C: on_usb_printers_detected() calls
        // lv_obj_find_by_name + lv_dropdown_set_options — must run on main.
        token.defer("LabelPrinterSettings::usb_apply",
                    [this, printers]() { on_usb_printers_detected(printers); });
    });
    spdlog::debug("[{}] Started USB printer detection", get_name());
}

void LabelPrinterSettingsOverlay::stop_usb_detection() {
    if (usb_detector_) {
        usb_detector_->stop_polling();
        usb_detector_.reset();
        spdlog::debug("[{}] Stopped USB printer detection", get_name());
    }
}

void LabelPrinterSettingsOverlay::on_usb_printers_detected(
    const std::vector<helix::UsbPrinterInfo>& printers) {
    detected_usb_printers_ = printers;

    if (!overlay_root_)
        return;

    lv_obj_t* row = find_required(overlay_root_, "row_usb_printers", get_name());
    if (!row)
        return;
    lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
    if (!dropdown)
        return;

    std::string options;
    if (detected_usb_printers_.empty()) {
        options = lv_tr("No USB printers found");
    } else {
        for (const auto& p : detected_usb_printers_) {
            if (!options.empty())
                options += "\n";
            if (get_runtime_config()->is_test_mode()) {
                options += fmt::format("{} (Bus {}, Dev {})", p.product_name, p.bus, p.address);
            } else {
                options += p.product_name;
            }
        }
    }

    lv_dropdown_set_options(dropdown, options.c_str());
    spdlog::debug("[{}] USB detection: {} printers found", get_name(), printers.size());

    // Auto-select first if not configured yet
    if (!detected_usb_printers_.empty()) {
        auto& settings = LabelPrinterSettingsManager::instance();
        if (settings.get_usb_vid() == 0) {
            handle_usb_printer_selected(0);
            spdlog::info("[{}] Auto-selected USB printer: {}", get_name(),
                         detected_usb_printers_[0].product_name);
        }
    }
}

void LabelPrinterSettingsOverlay::handle_usb_printer_selected(int index) {
    if (index < 0 || index >= static_cast<int>(detected_usb_printers_.size()))
        return;

    const auto& printer = detected_usb_printers_[index];
    spdlog::info("[{}] Selected USB printer: {} ({:04x}:{:04x})", get_name(), printer.product_name,
                 printer.vid, printer.pid);

    auto& settings = LabelPrinterSettingsManager::instance();
    settings.set_usb_vid(printer.vid);
    settings.set_usb_pid(printer.pid);
    settings.set_usb_serial(printer.serial);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void LabelPrinterSettingsOverlay::handle_address_changed() {
    lv_obj_t* row = find_required(overlay_root_, "row_address_port", get_name());
    if (!row)
        return;

    lv_obj_t* input = helix::ui::find_required(row, "input_address", get_name());
    if (input) {
        const char* text = lv_textarea_get_text(input);
        std::string addr = text ? text : "";
        spdlog::info("[{}] Address changed: {}", get_name(), addr);
        LabelPrinterSettingsManager::instance().set_printer_address(addr);
    }
}

void LabelPrinterSettingsOverlay::handle_port_changed() {
    lv_obj_t* row = find_required(overlay_root_, "row_address_port", get_name());
    if (!row)
        return;

    lv_obj_t* input = helix::ui::find_required(row, "input_port", get_name());
    if (input) {
        const char* text = lv_textarea_get_text(input);
        if (text && text[0] != '\0') {
            int port = std::atoi(text);
            if (port > 0 && port <= 65535) {
                spdlog::info("[{}] Port changed: {}", get_name(), port);
                LabelPrinterSettingsManager::instance().set_printer_port(port);
            } else {
                spdlog::warn("[{}] Invalid port: {}", get_name(), text);
                ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Invalid port number"));
            }
        }
    }
}

void LabelPrinterSettingsOverlay::handle_label_size_changed(int index) {
    auto& settings = LabelPrinterSettingsManager::instance();
    auto sizes = get_sizes_for_current_printer();
    if (index >= 0 && index < static_cast<int>(sizes.size())) {
        spdlog::info("[{}] Label size changed: {} (index {})", get_name(), sizes[index].name,
                     index);
        settings.set_label_size_index(index);

        // Force QR-only for small square labels where text won't fit
        const auto& sz = sizes[index];
        bool force_qr = (sz.width_px <= 250 && sz.height_px > 0 && sz.height_px <= 250);
        lv_obj_t* preset_row = find_required(overlay_root_, "row_preset", get_name());
        if (preset_row) {
            lv_obj_t* dd = helix::ui::find_required(preset_row, "dropdown", get_name());
            if (dd) {
                if (force_qr) {
                    lv_dropdown_set_options(dd, lv_tr("QR Only"));
                    lv_dropdown_set_selected(dd, 0);
                    lv_obj_add_state(dd, LV_STATE_DISABLED);
                    settings.set_label_preset(static_cast<int>(LabelPreset::MINIMAL));
                } else {
                    // Restore full options
                    auto opts = fmt::format("{}\n{}\n{}", lv_tr("Standard"), lv_tr("Compact"),
                                            lv_tr("QR Only"));
                    lv_dropdown_set_options(dd, opts.c_str());
                    lv_dropdown_set_selected(dd,
                                             static_cast<uint32_t>(settings.get_label_preset()));
                    lv_obj_remove_state(dd, LV_STATE_DISABLED);
                }
            }
        }
    } else {
        spdlog::warn("[{}] Label size index {} out of range ({})", get_name(), index, sizes.size());
    }
}

void LabelPrinterSettingsOverlay::handle_preset_changed(int index) {
    if (index >= 0 && index <= 2) {
        spdlog::info("[{}] Preset changed: {} (index {})", get_name(),
                     label_preset_name(static_cast<LabelPreset>(index)), index);
        LabelPrinterSettingsManager::instance().set_label_preset(index);
    } else {
        spdlog::warn("[{}] Preset index {} out of range", get_name(), index);
    }
}

void LabelPrinterSettingsOverlay::handle_test_print() {
    auto& settings = LabelPrinterSettingsManager::instance();

    if (!settings.is_configured()) {
        const auto type = settings.get_printer_type();
        const char* msg;
        if (type == "usb") {
            msg = lv_tr("Connect a USB label printer first");
        } else if (type == "bluetooth") {
            msg = lv_tr("Scan and select a Bluetooth printer first");
        } else {
            msg = lv_tr("Enter printer IP address first");
        }
        ToastManager::instance().show(ToastSeverity::WARNING, msg);
        return;
    }

    spdlog::warn("[{}] Test print requested", get_name());

    // Disable test print button via subject binding
    lv_subject_set_int(&test_printing_subject_, 1);

    // Create a mock spool for the test label. Use a negative ID so the
    // embedded QR code never decodes to a real Spoolman spool — if a user
    // accidentally scans the test label, the decoder rejects it instead of
    // silently switching the active spool to whatever "42" happens to be.
    SpoolInfo mock_spool;
    mock_spool.id = -1;
    mock_spool.vendor = "Hatchbox";
    mock_spool.material = "PLA";
    mock_spool.filament_name = "Red";
    mock_spool.color_hex = "#FF0000";
    mock_spool.remaining_weight_g = 800;
    mock_spool.initial_weight_g = 1000;
    mock_spool.remaining_length_m = 265;
    mock_spool.lot_nr = "LOT-2026A";
    mock_spool.comment = "Sample spool";
    mock_spool.nozzle_temp_recommended = 210;
    mock_spool.bed_temp_recommended = 60;

    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Printing test label..."), 2000);

    auto token = lifetime_.token();
    helix::print_spool_label(mock_spool, [token](bool success, const std::string& error) {
        if (success) {
            ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Test label printed"),
                                          2000);
        } else {
            spdlog::error("[LabelPrinterSettings] Test print failed: {}", error);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          helix::friendly_label_printer_error(error).c_str(), 5000);
        }

        // Re-enable test print button via subject
        if (!token.expired()) {
            get_label_printer_settings_overlay().test_printing_subject_set(0);
        }
    });
}

// ============================================================================
// BLUETOOTH DISCOVERY
// ============================================================================

void LabelPrinterSettingsOverlay::init_bt_printer_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = find_required(overlay_root_, "row_bt_printers", get_name());
    if (!row)
        return;

    lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
    if (!dropdown)
        return;

    // Pre-populate with saved paired device if available
    auto& settings = LabelPrinterSettingsManager::instance();
    std::string saved_addr = settings.get_bt_address();
    std::string saved_name = settings.get_bt_name();

    if (!saved_addr.empty()) {
        // Add saved device to bt_devices_ so selection index works
        BtDeviceInfo saved_dev;
        saved_dev.mac = saved_addr;
        saved_dev.name = saved_name.empty() ? saved_addr : saved_name;
        // Name-based BLE detection overrides saved transport setting —
        // some devices were incorrectly saved as "spp" before the fix
        saved_dev.is_ble = helix::bluetooth::name_suggests_ble(saved_dev.name.c_str()) ||
                           (settings.get_bt_transport() == "ble");
        if (saved_dev.is_ble && settings.get_bt_transport() != "ble") {
            spdlog::info("[{}] Correcting saved transport from spp to ble for {}", get_name(),
                         saved_dev.name);
            settings.set_bt_transport("ble");
        }

        // Show saved device immediately (assume saved = usable)
        saved_dev.paired = false;
        saved_dev.connected = false;

        bt_devices_.clear();
        bt_devices_.push_back(saved_dev);

        // Show "(Saved)" immediately — BLE devices don't persist pairing so
        // our settings ARE the "saved" state
        lv_dropdown_set_options(dropdown,
                                bt_device_label(saved_dev.name, false, true, false).c_str());
        lv_dropdown_set_selected(dropdown, 0);

        // Enable connect button (will update after async check)
        lv_obj_t* btn = find_required(overlay_root_, "btn_bt_connect", get_name());
        if (btn) {
            lv_obj_remove_state(btn, LV_STATE_DISABLED);
        }

        // Enable Forget button whenever a BT MAC is configured
        lv_obj_t* forget_btn = find_required(overlay_root_, "btn_bt_forget", get_name());
        if (forget_btn) {
            lv_obj_remove_state(forget_btn, LV_STATE_DISABLED);
        }

        // Check actual paired/connected state asynchronously to avoid
        // blocking the UI thread on D-Bus calls (25-second default timeout)
        auto& loader = helix::bluetooth::BluetoothLoader::instance();
        if (loader.is_available() && loader.is_paired) {
            auto bt_token = lifetime_.token();
            auto shared_ctx = bt_ctx_;
            std::string addr = saved_addr;
            // Wrap spawn per feedback_no_bare_threads_arm.md (#724, #837, [L083]).
            try {
                helix::make_thread([bt_token, shared_ctx, addr]() {
                    auto& ldr = helix::bluetooth::BluetoothLoader::instance();
                    auto* ctx = shared_ctx->get();
                    if (!ctx)
                        return;
                    int paired_r = ldr.is_paired ? ldr.is_paired(ctx, addr.c_str()) : -1;
                    bool connected = check_bt_connected(ctx, addr);
                    spdlog::debug("[Label Printer] Async BT state: is_paired={} connected={}",
                                  paired_r, connected);

                    helix::ui::queue_update(
                        "LabelPrinterSettingsOverlay::init_bt_printer_dropdown",
                        [bt_token, paired_r, connected, addr]() {
                            if (bt_token.expired())
                                return;
                            auto& ov = get_label_printer_settings_overlay();
                            for (auto& dev : ov.bt_devices_) {
                                if (dev.mac == addr) {
                                    dev.paired = (paired_r == 1);
                                    dev.connected = connected;
                                    break;
                                }
                            }
                            // Refresh dropdown with actual state
                            if (ov.overlay_root_) {
                                lv_obj_t* row = helix::ui::find_required(
                                    ov.overlay_root_, "row_bt_printers", "Label Printer");
                                if (row) {
                                    lv_obj_t* dd =
                                        helix::ui::find_required(row, "dropdown", "Label Printer");
                                    if (dd) {
                                        std::string options;
                                        for (const auto& d : ov.bt_devices_) {
                                            if (!options.empty())
                                                options += "\n";
                                            options += bt_device_label(d.name, d.paired,
                                                                       !d.mac.empty(), d.connected);
                                        }
                                        lv_dropdown_set_options(dd, options.c_str());
                                    }
                                }
                                // Update connect button
                                lv_obj_t* b = helix::ui::find_required(
                                    ov.overlay_root_, "btn_bt_connect", "Label Printer");
                                if (b) {
                                    if (connected)
                                        lv_obj_add_state(b, LV_STATE_DISABLED);
                                    else
                                        lv_obj_remove_state(b, LV_STATE_DISABLED);
                                }
                            }
                        });
                }).detach();
            } catch (const std::system_error& e) {
                spdlog::warn("[Label Printer] Failed to spawn BT state-probe thread: {}", e.what());
                // Non-fatal — UI will show cached state; user can retry manually
            }
        }
    } else {
        lv_dropdown_set_options(dropdown, lv_tr("Press Scan to search"));
        // No saved BT printer — ensure Forget button is disabled
        lv_obj_t* forget_btn = find_required(overlay_root_, "btn_bt_forget", get_name());
        if (forget_btn) {
            lv_obj_add_state(forget_btn, LV_STATE_DISABLED);
        }
    }
}

void LabelPrinterSettingsOverlay::start_bt_discovery() {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available() || !loader.discover) {
        spdlog::warn("[{}] BT discovery unavailable", get_name());
        return;
    }

    if (bt_discovering_) {
        spdlog::debug("[{}] BT discovery already in progress", get_name());
        return;
    }

    bt_discovering_ = true;
    // Keep saved paired device so it stays visible during scan
    bt_devices_.erase(std::remove_if(bt_devices_.begin(), bt_devices_.end(),
                                     [](const BtDeviceInfo& d) { return !d.paired; }),
                      bt_devices_.end());

    // Update UI to show scanning state
    lv_subject_set_int(&bt_scanning_subject_, 1);
    if (overlay_root_) {
        lv_obj_t* row = find_required(overlay_root_, "row_bt_printers", get_name());
        if (row) {
            lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
            if (dropdown) {
                lv_dropdown_set_options(dropdown, lv_tr("Scanning..."));
            }
        }
    }

    helix::bluetooth::DiscoveryRun::Callbacks callbacks;
    callbacks.accept = [](const helix_bt_device& dev) {
        if (!dev.is_scanner)
            return true;
        spdlog::debug("[Label Printer] Skipping scanner: {} ({})", dev.name ? dev.name : "",
                      helix::redact::mac(dev.mac ? dev.mac : ""));
        return false;
    };
    callbacks.on_device = [this](const helix::bluetooth::DiscoveredDevice& found) {
        BtDeviceInfo info;
        info.mac = found.mac;
        info.name = found.name;
        info.paired = found.paired;
        info.is_ble = found.is_ble;
        info.is_scanner = found.is_scanner;

        // Exact-MAC dedup: ignore if BlueZ repeats the same device.
        for (const auto& existing : bt_devices_) {
            if (existing.mac == info.mac)
                return;
        }

        // Name-based transport resolution. Dual-mode printers (e.g.
        // Niimbot D110) enumerate as two BlueZ entries with the same
        // name: one BR/EDR half exposing only SPP/PnP, one BLE half
        // exposing the vendor GATT service. The brand table knows
        // which transport actually prints; drop the mismatched half.
        bool replaced = false;
        if (!info.name.empty() && helix::bluetooth::find_brand(info.name.c_str()) != nullptr) {
            const bool brand_prefers_ble = helix::bluetooth::name_suggests_ble(info.name.c_str());
            const bool new_matches_brand = (info.is_ble == brand_prefers_ble);
            for (auto it = bt_devices_.begin(); it != bt_devices_.end(); ++it) {
                if (it->name != info.name)
                    continue;
                const bool existing_matches_brand = (it->is_ble == brand_prefers_ble);
                if (new_matches_brand && !existing_matches_brand) {
                    // Genuine transport migration: the existing entry was
                    // the wrong-transport half of a dual-mode device.
                    // Replace it and clear the saved address so the user
                    // re-pairs on the correct transport.
                    spdlog::info("[Label Printer] Migrating {} from {} {} "
                                 "to brand-preferred {} {}",
                                 it->name, helix::redact::mac(it->mac),
                                 it->is_ble ? "BLE" : "Classic", helix::redact::mac(info.mac),
                                 info.is_ble ? "BLE" : "Classic");
                    auto& settings_mgr = LabelPrinterSettingsManager::instance();
                    if (settings_mgr.get_bt_address() == it->mac && it->mac != info.mac) {
                        spdlog::warn("[Label Printer] Saved BT address {} is the "
                                     "wrong transport for {}; clearing so user "
                                     "re-pairs with {}",
                                     helix::redact::mac(it->mac), it->name,
                                     helix::redact::mac(info.mac));
                        settings_mgr.set_bt_address("");
                    }
                    *it = info;
                    replaced = true;
                } else if (new_matches_brand && existing_matches_brand) {
                    // Same name, same transport, different MAC — same
                    // device re-advertising with a different address
                    // (BLE random-address rotation, e.g. Niimbot B1).
                    // Keep the existing entry so the user's saved
                    // pairing sticks; drop the duplicate.
                    spdlog::debug("[Label Printer] Ignoring duplicate {} "
                                  "advertisement for {} (existing {}, new {}) "
                                  "— same transport, treating as RPA rotation",
                                  info.is_ble ? "BLE" : "Classic", info.name,
                                  helix::redact::mac(it->mac), helix::redact::mac(info.mac));
                    return;
                } else {
                    // New entry is on the non-preferred transport; drop.
                    spdlog::debug("[Label Printer] Ignoring {} ({} {}): "
                                  "brand prefers {} transport already present",
                                  info.name, helix::redact::mac(info.mac),
                                  info.is_ble ? "BLE" : "Classic",
                                  brand_prefers_ble ? "BLE" : "Classic");
                    return;
                }
                break;
            }
        }

        if (!replaced) {
            bt_devices_.push_back(info);
            spdlog::debug("[Label Printer] BT discovered: {} ({})", info.name,
                          helix::redact::mac(info.mac));
        }

        // Update dropdown
        if (overlay_root_) {
            lv_obj_t* row = helix::ui::find_required(overlay_root_, "row_bt_printers", get_name());
            if (row) {
                lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
                if (dropdown) {
                    std::string options;
                    for (const auto& d : bt_devices_) {
                        if (!options.empty())
                            options += "\n";
                        options += bt_device_label(d.name, d.paired, !d.mac.empty(), d.connected);
                    }
                    lv_dropdown_close(dropdown);
                    lv_dropdown_set_options(dropdown, options.c_str());
                }
            }
        }
    };
    callbacks.on_finished = [this](bool ok) {
        if (!ok)
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("Could not start Bluetooth discovery"), 3000);
        bt_discovering_ = false;
        lv_subject_set_int(&bt_scanning_subject_, 0);

        if (overlay_root_) {
            lv_obj_t* row = helix::ui::find_required(overlay_root_, "row_bt_printers", get_name());
            if (row) {
                lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
                if (dropdown) {
                    lv_dropdown_close(dropdown);
                    if (bt_devices_.empty()) {
                        lv_dropdown_set_options(dropdown, lv_tr("No Bluetooth printers found"));
                    } else {
                        // Refresh dropdown with final device list (handles case where
                        // all discovered devices were already known — dropdown still
                        // shows "Scanning..." without this)
                        std::string options;
                        for (const auto& d : bt_devices_) {
                            if (!options.empty())
                                options += "\n";
                            options += bt_device_label(
                                d.name, d.paired,
                                d.mac == LabelPrinterSettingsManager::instance().get_bt_address(),
                                d.connected);
                        }
                        lv_dropdown_set_options(dropdown, options.c_str());
                    }
                }
            }
        }

        spdlog::info("[Label Printer] BT discovery finished, {} devices found", bt_devices_.size());
    };

    if (!bt_discovery_.start(bt_ctx_, 15000, lifetime_.token(), std::move(callbacks))) {
        bt_discovering_ = false;
        lv_subject_set_int(&bt_scanning_subject_, 0);
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      lv_tr("Could not start Bluetooth discovery"), 3000);
        return;
    }

    spdlog::info("[{}] Started Bluetooth discovery", get_name());
}

void LabelPrinterSettingsOverlay::stop_bt_discovery() {
    if (!bt_discovering_)
        return;

    bt_discovery_.cancel();
    bt_discovering_ = false;
    lv_subject_set_int(&bt_scanning_subject_, 0);
    spdlog::debug("[{}] Stopped Bluetooth discovery", get_name());
}

void LabelPrinterSettingsOverlay::handle_bt_printer_selected(int index) {
    if (index < 0 || index >= static_cast<int>(bt_devices_.size()))
        return;

    const auto& device = bt_devices_[index];
    if (device.is_scanner) {
        spdlog::warn("[{}] Ignoring selection of scanner device: {} ({})", get_name(), device.name,
                     helix::redact::mac(device.mac));
        return;
    }
    spdlog::info("[{}] Selected BT printer: {} ({})", get_name(), device.name,
                 helix::redact::mac(device.mac));

    // If not paired and not already saved (i.e. a newly discovered device), prompt for pairing.
    // Saved devices (address matches settings) can skip re-pairing — BLE devices don't
    // persist pairing between sessions but the saved address is enough to reconnect.
    auto& settings = LabelPrinterSettingsManager::instance();
    bool is_saved = (device.mac == settings.get_bt_address());
    if (!device.paired && !is_saved) {
        auto msg = fmt::format("{} {}?", lv_tr("Pair with"), device.name);
        // The MAC rides in the capture; the old lv_event_cb_t form kept it in a
        // heap std::string that only the button callbacks freed, so a dismissal
        // leaked it (#1380).
        helix::ui::ConfirmOptions opts;
        opts.owner_token = lifetime_.token(); // gates the confirm on this overlay
        auto* dialog = helix::ui::modal_confirm(
            lv_tr("Pair Bluetooth Printer"), msg.c_str(), ModalSeverity::Info, lv_tr("Pair"),
            [this, mac = device.mac] {
                auto& loader = helix::bluetooth::BluetoothLoader::instance();
                if (!loader.is_available() || !loader.pair) {
                    ToastManager::instance().show(ToastSeverity::ERROR,
                                                  lv_tr("Bluetooth not available"));
                    return;
                }

                ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Pairing..."), 5000);

                auto token = lifetime_.token();
                auto shared_ctx = bt_ctx_;

                // Pair on a detached thread
                // Wrap spawn per feedback_no_bare_threads_arm.md (#724, #837, [L083]).
                try {
                    helix::make_thread([mac, shared_ctx, token]() {
                        auto& ldr = helix::bluetooth::BluetoothLoader::instance();
                        auto* bt_ctx = shared_ctx->get();
                        int ret = bt_ctx ? ldr.pair(bt_ctx, mac.c_str()) : -ENODEV;
                        std::string err;
                        if (ret != 0)
                            err = bt_ctx && ldr.last_error ? ldr.last_error(bt_ctx)
                                                           : "Bluetooth not initialized";

                        // Check paired/connected state on this worker thread (D-Bus
                        // calls can take seconds - must not block the UI thread)
                        int paired_r = -1;
                        bool connected = false;
                        if (ret == 0) {
                            paired_r = ldr.is_paired ? ldr.is_paired(bt_ctx, mac.c_str()) : -1;
                            connected = check_bt_connected(bt_ctx, mac);
                            spdlog::info("[LabelPrinterSettings] Post-pair: is_paired={} "
                                         "connected={}",
                                         paired_r, connected);
                        }

                        helix::ui::queue_update(
                            "LabelPrinterSettingsOverlay::handle_bt_printer_selected",
                            [ret, mac, token, err, paired_r, connected]() {
                                if (token.expired())
                                    return;

                                if (ret == 0) {
                                    ToastManager::instance().show(
                                        ToastSeverity::SUCCESS, lv_tr("Paired successfully"), 2000);

                                    // Update device info and save settings
                                    auto& ov = get_label_printer_settings_overlay();
                                    for (auto& dev : ov.bt_devices_) {
                                        if (dev.mac == mac) {
                                            dev.paired = (paired_r == 1);
                                            dev.connected = connected;
                                            auto& settings =
                                                LabelPrinterSettingsManager::instance();
                                            settings.set_bt_address(mac);
                                            settings.set_bt_name(dev.name);
                                            settings.set_bt_transport(dev.is_ble ? "ble" : "spp");
                                            ov.init_label_size_dropdown();
                                            break;
                                        }
                                    }

                                    // Refresh dropdown to show paired checkmark
                                    if (ov.overlay_root_) {
                                        lv_obj_t* row = helix::ui::find_required(
                                            ov.overlay_root_, "row_bt_printers", "Label Printer");
                                        if (row) {
                                            lv_obj_t* dropdown = helix::ui::find_required(
                                                row, "dropdown", "Label Printer");
                                            if (dropdown) {
                                                lv_dropdown_close(dropdown);
                                                std::string options;
                                                for (const auto& d : ov.bt_devices_) {
                                                    if (!options.empty())
                                                        options += "\n";
                                                    options += bt_device_label(
                                                        d.name, d.paired,
                                                        d.mac ==
                                                            LabelPrinterSettingsManager::instance()
                                                                .get_bt_address(),
                                                        d.connected);
                                                }
                                                lv_dropdown_set_options(dropdown, options.c_str());
                                            }
                                        }
                                    }
                                } else {
                                    spdlog::error("[LabelPrinterSettings] Pairing failed: {}", err);
                                    ToastManager::instance().show(ToastSeverity::ERROR,
                                                                  lv_tr("Pairing failed"), 3000);
                                }
                            });
                    }).detach();
                } catch (const std::system_error& e) {
                    spdlog::error("[LabelPrinterSettings] Failed to spawn pair thread: {}",
                                  e.what());
                    ToastManager::instance().hide();
                    ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Pairing failed"),
                                                  3000);
                }
            },
            opts);

        if (!dialog) {
            spdlog::warn("[{}] Failed to show pairing modal", get_name());
        }
        return;
    }

    // Already paired or saved — update settings
    settings.set_bt_address(device.mac);
    settings.set_bt_name(device.name);
    settings.set_bt_transport(device.is_ble ? "ble" : "spp");

    // Enable connect button when not connected
    if (overlay_root_) {
        lv_obj_t* btn = find_required(overlay_root_, "btn_bt_connect", get_name());
        if (btn) {
            if (!device.connected) {
                lv_obj_remove_state(btn, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(btn, LV_STATE_DISABLED);
            }
        }
    }

    // Refresh label size dropdown for the selected printer's transport
    init_label_size_dropdown();
}

void LabelPrinterSettingsOverlay::handle_bt_scan() {
    if (bt_discovering_) {
        stop_bt_discovery();
    } else {
        start_bt_discovery();
    }
}

void LabelPrinterSettingsOverlay::handle_bt_connect() {
    auto& settings = LabelPrinterSettingsManager::instance();
    std::string mac = settings.get_bt_address();
    if (mac.empty())
        return;

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available())
        return;

    // Disable button while connecting
    if (overlay_root_) {
        lv_obj_t* btn = find_required(overlay_root_, "btn_bt_connect", get_name());
        if (btn)
            lv_obj_add_state(btn, LV_STATE_DISABLED);
    }

    auto token = lifetime_.token();
    auto shared_ctx = bt_ctx_;

    // Wrap spawn per feedback_no_bare_threads_arm.md (#724, #837, [L083]).
    try {
        helix::make_thread([mac, shared_ctx, token]() {
            auto& ldr = helix::bluetooth::BluetoothLoader::instance();
            auto* init_ctx = shared_ctx->get();
            int ret = -1;

            if (init_ctx && ldr.pair) {
                ret = ldr.pair(init_ctx, mac.c_str());
            }

            // If pair failed (device may have been removed from BlueZ cache),
            // try a brief scan to rediscover, then retry
            if (init_ctx && ret < 0 && ldr.discover && ldr.pair) {
                spdlog::info("[LabelPrinterSettings] Pair failed, scanning to rediscover {}...",
                             helix::redact::mac(mac));
                struct ScanCtx {
                    std::string target;
                    bool found = false;
                };
                ScanCtx scan_ctx{mac};
                ldr.discover(
                    init_ctx, 8000,
                    [](const helix_bt_device* dev, void* user_data) {
                        auto* sc = static_cast<ScanCtx*>(user_data);
                        if (dev->mac && sc->target == dev->mac) {
                            sc->found = true;
                        }
                    },
                    &scan_ctx, nullptr);

                if (scan_ctx.found) {
                    spdlog::info("[LabelPrinterSettings] Rediscovered {}, retrying pair",
                                 helix::redact::mac(mac));
                    ret = ldr.pair(init_ctx, mac.c_str());
                } else {
                    spdlog::warn("[LabelPrinterSettings] Device {} not found during rescan",
                                 helix::redact::mac(mac));
                }
            }

            // For SPP printers (Brother QL, etc.), BlueZ "Connected" only reflects
            // an active RFCOMM/BLE session — pair success is sufficient. Only check
            // is_connected for BLE devices that maintain persistent connections.
            bool paired_ok = (ret == 0);
            bool connected = false;
            if (paired_ok && ldr.is_paired) {
                paired_ok = (ldr.is_paired(init_ctx, mac.c_str()) == 1);
            }

            helix::ui::queue_update("LabelPrinterSettingsOverlay::handle_bt_connect", [mac,
                                                                                       paired_ok,
                                                                                       connected,
                                                                                       token]() {
                if (token.expired())
                    return;

                auto& ov = get_label_printer_settings_overlay();

                // Update device state
                for (auto& dev : ov.bt_devices_) {
                    if (dev.mac == mac) {
                        dev.paired = paired_ok;
                        dev.connected = connected;
                        break;
                    }
                }

                // Refresh dropdown labels
                if (ov.overlay_root_) {
                    lv_obj_t* row = helix::ui::find_required(ov.overlay_root_, "row_bt_printers",
                                                             "Label Printer");
                    if (row) {
                        lv_obj_t* dropdown =
                            helix::ui::find_required(row, "dropdown", "Label Printer");
                        if (dropdown) {
                            std::string options;
                            for (const auto& d : ov.bt_devices_) {
                                if (!options.empty())
                                    options += "\n";
                                options += bt_device_label(
                                    d.name, d.paired,
                                    d.mac ==
                                        LabelPrinterSettingsManager::instance().get_bt_address(),
                                    d.connected);
                            }
                            lv_dropdown_set_options(dropdown, options.c_str());
                        }
                    }

                    // Update connect button state
                    lv_obj_t* btn = helix::ui::find_required(ov.overlay_root_, "btn_bt_connect",
                                                             "Label Printer");
                    if (btn) {
                        if (paired_ok) {
                            lv_obj_add_state(btn, LV_STATE_DISABLED);
                        } else {
                            lv_obj_remove_state(btn, LV_STATE_DISABLED);
                        }
                    }
                }

                if (paired_ok) {
                    ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Paired"), 2000);
                } else {
                    ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Pairing failed"),
                                                  2000);
                }
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("[LabelPrinterSettings] Failed to spawn pair thread: {}", e.what());
        if (overlay_root_) {
            lv_obj_t* btn = find_required(overlay_root_, "btn_bt_connect", get_name());
            if (btn)
                lv_obj_remove_state(btn, LV_STATE_DISABLED);
        }
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Pairing failed"), 2000);
    }
}

void LabelPrinterSettingsOverlay::handle_bt_forget() {
    auto& settings = LabelPrinterSettingsManager::instance();
    std::string mac = settings.get_bt_address();
    if (mac.empty()) {
        spdlog::warn("[{}] Forget clicked but no BT MAC configured", get_name());
        return;
    }

    spdlog::info("[{}] Forgetting BT printer {}", get_name(), helix::redact::mac(mac));

    // Disable Forget + Connect buttons while the unpair is in flight
    if (overlay_root_) {
        if (auto* fbtn = find_required(overlay_root_, "btn_bt_forget", get_name())) {
            lv_obj_add_state(fbtn, LV_STATE_DISABLED);
        }
        if (auto* cbtn = find_required(overlay_root_, "btn_bt_connect", get_name())) {
            lv_obj_add_state(cbtn, LV_STATE_DISABLED);
        }
    }

    auto tok = lifetime_.token();
    // Wrap the std::thread spawn in try/catch per feedback_no_bare_threads_arm.md
    // (#724) — thread creation can fail on AD5M/CC1 due to tight thread limits.
    try {
        helix::make_thread([this, tok, mac]() {
            bool bluez_ok = false;
            auto& loader = helix::bluetooth::BluetoothLoader::instance();
            if (!loader.is_available() || !loader.remove_device) {
                spdlog::error("[LabelPrinterSettings] remove_device symbol missing");
            } else {
                auto* ctx = loader.get_or_create_context();
                if (!ctx) {
                    spdlog::error(
                        "[LabelPrinterSettings] Failed to get BT context for remove_device");
                } else {
                    int r = loader.remove_device(ctx, mac.c_str());
                    if (r < 0) {
                        const char* err = loader.last_error ? loader.last_error(ctx) : "unknown";
                        spdlog::error(
                            "[LabelPrinterSettings] remove_device failed for {}: r={} err={}",
                            helix::redact::mac(mac), r, err);
                        // Fall through and clear settings anyway so the UI doesn't
                        // show a stale config.
                    } else {
                        spdlog::info("[LabelPrinterSettings] BlueZ unpair succeeded for {}",
                                     helix::redact::mac(mac));
                        bluez_ok = true;
                    }
                }
            }

            if (tok.expired())
                return;
            tok.defer([this, mac, bluez_ok]() {
                auto& s = LabelPrinterSettingsManager::instance();
                s.set_bt_address("");
                s.set_bt_name("");
                s.set_bt_channel(0);
                s.set_bt_transport("");

                // Drop the forgotten device from the local discovery list so the
                // dropdown no longer lists it.
                bt_devices_.erase(
                    std::remove_if(bt_devices_.begin(), bt_devices_.end(),
                                   [&mac](const BtDeviceInfo& d) { return d.mac == mac; }),
                    bt_devices_.end());

                // Rebuild the BT printer dropdown to match the "no saved printer" state.
                init_bt_printer_dropdown();
                // Refresh label size dropdown (selected printer/transport just cleared).
                init_label_size_dropdown();

                if (bluez_ok) {
                    ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                  lv_tr("Bluetooth printer forgotten"), 2000);
                } else {
                    // Settings cleared, but BlueZ refused/timed out unpair — the
                    // bond is still on the system. Tell the user so they can
                    // remove it via OS Bluetooth settings if needed.
                    ToastManager::instance().show(
                        ToastSeverity::WARNING,
                        lv_tr(
                            "Cleared from settings — system unpair failed, may need OS Bluetooth"),
                        5000);
                }
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("[LabelPrinterSettings] Failed to spawn forget thread: {}", e.what());
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Could not forget device"), 3000);
    }
}

} // namespace helix::settings

#endif // HELIX_HAS_LABEL_PRINTER
