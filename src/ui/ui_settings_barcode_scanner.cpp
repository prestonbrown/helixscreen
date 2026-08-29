// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_barcode_scanner.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "bluetooth_loader.h"
#include "bt_scanner_discovery_utils.h"
#include "input_device_scanner.h"
#include "log_redact.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "usb_scanner_monitor.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace helix::ui {

BarcodeScannerSettingsOverlay* BarcodeScannerSettingsOverlay::s_active_instance_ = nullptr;

namespace {
std::unique_ptr<BarcodeScannerSettingsOverlay> g_barcode_scanner_overlay;

struct RowData {
    std::string vendor_product; // empty = auto-detect
    std::string device_name;
    std::string bt_mac; // empty for USB rows
};
} // namespace

BarcodeScannerSettingsOverlay& get_barcode_scanner_settings_overlay() {
    if (!g_barcode_scanner_overlay) {
        g_barcode_scanner_overlay = std::make_unique<BarcodeScannerSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "BarcodeScannerSettingsOverlay", []() { g_barcode_scanner_overlay.reset(); });
    }
    return *g_barcode_scanner_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

BarcodeScannerSettingsOverlay::BarcodeScannerSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

BarcodeScannerSettingsOverlay::~BarcodeScannerSettingsOverlay() {
    // Singleton lifetime: this runs only at app shutdown via StaticPanelRegistry.
    // stop_bt_discovery() signals the detached discovery thread to exit; the
    // thread may still be inside loader.discover(bt_ctx_) when we reach
    // loader.deinit(bt_ctx_) below. At process exit this is acceptable
    // (kernel tears down); for any future path that destroys the singleton
    // while the process continues, promote the discovery thread to joinable.
    stop_bt_discovery();

    if (subjects_initialized_) {
        lv_subject_deinit(&bt_available_subject_);
        lv_subject_deinit(&bt_discovering_subject_);
        lv_subject_deinit(&keymap_index_subject_);
        lv_subject_deinit(&current_device_label_subject_);
    }

    if (bt_ctx_) {
        auto& loader = helix::bluetooth::BluetoothLoader::instance();
        if (loader.deinit) {
            loader.deinit(bt_ctx_);
        }
        bt_ctx_ = nullptr;
    }

    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void BarcodeScannerSettingsOverlay::init_subjects() {
    if (subjects_initialized_)
        return;

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    lv_subject_init_int(&bt_available_subject_, loader.is_available() ? 1 : 0);
    lv_xml_register_subject(nullptr, "scanner_bt_available", &bt_available_subject_);

    lv_subject_init_int(&bt_discovering_subject_, 0);
    lv_xml_register_subject(nullptr, "scanner_bt_discovering", &bt_discovering_subject_);

    const std::string km = helix::SettingsManager::instance().get_scanner_keymap();
    int km_idx = 0;
    if (km == "qwertz")
        km_idx = 1;
    else if (km == "azerty")
        km_idx = 2;
    lv_subject_init_int(&keymap_index_subject_, km_idx);
    lv_xml_register_subject(nullptr, "scanner_keymap_index", &keymap_index_subject_);

    current_device_label_buf_[0] = '\0';
    lv_subject_init_string(&current_device_label_subject_, current_device_label_buf_, nullptr,
                           sizeof(current_device_label_buf_), current_device_label_buf_);
    lv_xml_register_subject(nullptr, "scanner_current_device_label",
                            &current_device_label_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void BarcodeScannerSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_bs_scan_bluetooth", on_bs_scan_bluetooth},
        {"on_bs_refresh_usb", on_bs_refresh_usb},
        {"on_bs_keymap_changed", on_bs_keymap_changed},
        {"on_bs_row_clicked", on_bs_row_clicked},
        {"on_bs_row_forget", on_bs_row_forget},
        {"on_bs_bt_scanner_selected", on_bs_bt_scanner_selected},
        {"on_bs_bt_pair", on_bs_bt_pair},
        {"on_bs_bt_forget", on_bs_bt_forget},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* BarcodeScannerSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "barcode_scanner_settings", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void BarcodeScannerSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void BarcodeScannerSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    s_active_instance_ = this;

    usb_list_ = lv_obj_find_by_name(overlay_root_, "usb_device_list");

    if (auto* row = lv_obj_find_by_name(overlay_root_, "row_bt_scanners"))
        bt_dropdown_ = lv_obj_find_by_name(row, "dropdown");
    btn_bt_pair_ = lv_obj_find_by_name(overlay_root_, "btn_bt_pair");
    btn_bt_forget_ = lv_obj_find_by_name(overlay_root_, "btn_bt_forget");

    // Seed bt_devices_ from BlueZ's known-devices list (paired + previously
    // seen scanners) so the dropdown is populated before any active scan.
    bt_devices_.clear();
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (loader.is_available() && loader.enumerate_known) {
        if (!bt_ctx_ && loader.init)
            bt_ctx_ = loader.init();
        if (bt_ctx_) {
            loader.enumerate_known(
                bt_ctx_,
                [](const helix_bt_device* dev, void* ud) {
                    if (!dev)
                        return;
                    auto* self = static_cast<BarcodeScannerSettingsOverlay*>(ud);
                    bool looks_like_scanner =
                        helix::bluetooth::is_hid_scanner_uuid(dev->service_uuid) ||
                        helix::bluetooth::is_likely_bt_scanner(dev->name);
                    if (!looks_like_scanner)
                        return;
                    BtDeviceInfo info;
                    info.mac = dev->mac ? dev->mac : "";
                    info.name = dev->name ? dev->name : "Unknown";
                    info.paired = dev->paired;
                    info.is_ble = dev->is_ble;
                    for (const auto& existing : self->bt_devices_)
                        if (existing.mac == info.mac)
                            return;
                    self->bt_devices_.push_back(info);
                },
                this);
        }
    }

    // If the saved scanner isn't in BlueZ's known list (e.g., plugin unavailable),
    // seed from settings so the user still sees it.
    const auto saved_mac = helix::SettingsManager::instance().get_scanner_bt_address();
    const auto saved_name = helix::SettingsManager::instance().get_scanner_device_name();
    if (!saved_mac.empty()) {
        bool present = false;
        for (const auto& d : bt_devices_) {
            if (d.mac == saved_mac) {
                present = true;
                break;
            }
        }
        if (!present) {
            BtDeviceInfo saved;
            saved.mac = saved_mac;
            saved.name = saved_name.empty() ? saved_mac : saved_name;
            saved.paired = true;
            bt_devices_.push_back(saved);
        }
    }

    refresh_current_selection_label();
    populate_device_list();
    populate_bt_dropdown();
}

void BarcodeScannerSettingsOverlay::on_deactivate() {
    stop_bt_discovery();
    s_active_instance_ = nullptr;
    usb_list_ = nullptr;
    bt_dropdown_ = nullptr;
    btn_bt_pair_ = nullptr;
    btn_bt_forget_ = nullptr;
    OverlayBase::on_deactivate();
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

void BarcodeScannerSettingsOverlay::refresh_current_selection_label() {
    auto id = helix::SettingsManager::instance().get_scanner_device_id();
    auto name = helix::SettingsManager::instance().get_scanner_device_name();
    const char* text = id.empty() ? "Auto-detect" : name.c_str();
    lv_subject_copy_string(&current_device_label_subject_, text);
}

// ============================================================================
// DEVICE LIST POPULATION
// ============================================================================

void BarcodeScannerSettingsOverlay::add_usb_row(lv_obj_t* container, const std::string& label,
                                                const std::string& sublabel,
                                                const std::string& vendor_product) {
    if (!container)
        return;

    const bool selected =
        !vendor_product.empty() &&
        vendor_product == helix::SettingsManager::instance().get_scanner_device_id();

    const char* attrs[] = {
        "row_icon",     "usb",
        "row_label",    label.c_str(),
        "row_sublabel", sublabel.c_str(),
        "hide_check",   selected ? "false" : "true",
        "hide_forget",  "true",
        nullptr,
    };
    lv_obj_t* row =
        static_cast<lv_obj_t*>(lv_xml_create(container, "barcode_scanner_device_row", attrs));
    if (!row)
        return;

    auto* data = new RowData{vendor_product, label, std::string{}};
    lv_obj_set_user_data(row, data);

    // LV_EVENT_DELETE cleanup: frees heap-allocated RowData for this row.
    // lv_obj_add_event_cb is intentionally used here — LV_EVENT_DELETE cannot
    // be expressed in XML, and this is the same pattern used in ScannerPickerModal.
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_DELETE)
                return;
            delete static_cast<RowData*>(
                lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e))));
        },
        LV_EVENT_DELETE, nullptr);
}

void BarcodeScannerSettingsOverlay::add_auto_detect_row(lv_obj_t* container) {
    if (!container)
        return;

    const bool selected = helix::SettingsManager::instance().get_scanner_device_id().empty() &&
                          helix::SettingsManager::instance().get_scanner_bt_address().empty();

    const char* attrs[] = {
        "row_icon",     "magnify",
        "row_label",    "Auto-detect",
        "row_sublabel", "Use first HID scanner found",
        "hide_check",   selected ? "false" : "true",
        "hide_forget",  "true",
        nullptr,
    };
    lv_obj_t* row =
        static_cast<lv_obj_t*>(lv_xml_create(container, "barcode_scanner_device_row", attrs));
    if (!row)
        return;

    auto* data = new RowData{std::string{}, "Auto-detect", std::string{}};
    lv_obj_set_user_data(row, data);

    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_DELETE)
                return;
            delete static_cast<RowData*>(
                lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e))));
        },
        LV_EVENT_DELETE, nullptr);
}

void BarcodeScannerSettingsOverlay::populate_device_list() {
    if (usb_list_)
        helix::ui::safe_clean_children(usb_list_);
    if (!usb_list_)
        return;

    // Auto-detect always first
    add_auto_detect_row(usb_list_);

    auto devices = helix::input::enumerate_usb_hid_devices();
    spdlog::debug("[{}] Enumerated {} HID devices", get_name(), devices.size());

    int usb_count = 0;
    for (const auto& dev : devices) {
        if (dev.bus_type == 0x05)
            continue; // BT HIDs handled via dropdown
        const std::string vendor_product = dev.vendor_id + ":" + dev.product_id;
        const std::string sublabel = vendor_product + "  " + dev.event_path;
        add_usb_row(usb_list_, dev.name, sublabel, vendor_product);
        usb_count++;
    }
}

int BarcodeScannerSettingsOverlay::selected_bt_index() const {
    if (!bt_dropdown_)
        return -1;
    if (bt_devices_.empty())
        return -1;
    const uint32_t sel = lv_dropdown_get_selected(bt_dropdown_);
    if (sel >= bt_devices_.size())
        return -1;
    return static_cast<int>(sel);
}

void BarcodeScannerSettingsOverlay::populate_bt_dropdown() {
    if (!bt_dropdown_)
        return;

    const auto saved_mac = helix::SettingsManager::instance().get_scanner_bt_address();

    if (bt_devices_.empty()) {
        lv_dropdown_set_options(bt_dropdown_, "(none)");
        lv_dropdown_set_selected(bt_dropdown_, 0);
        update_bt_action_buttons();
        return;
    }

    std::string options;
    int selected_idx = 0;
    for (size_t i = 0; i < bt_devices_.size(); ++i) {
        const auto& d = bt_devices_[i];
        if (!options.empty())
            options += "\n";
        options += d.name;
        if (d.paired)
            options += " (paired)";
        if (!d.mac.empty() && d.mac == saved_mac)
            selected_idx = static_cast<int>(i);
    }
    lv_dropdown_set_options(bt_dropdown_, options.c_str());
    lv_dropdown_set_selected(bt_dropdown_, selected_idx);
    update_bt_action_buttons();
}

void BarcodeScannerSettingsOverlay::update_bt_action_buttons() {
    const int idx = selected_bt_index();
    const bool has_sel = (idx >= 0);
    const bool paired = has_sel && bt_devices_[idx].paired;

    if (btn_bt_pair_) {
        if (has_sel && !paired)
            lv_obj_remove_state(btn_bt_pair_, LV_STATE_DISABLED);
        else
            lv_obj_add_state(btn_bt_pair_, LV_STATE_DISABLED);
    }
    if (btn_bt_forget_) {
        if (has_sel && paired)
            lv_obj_remove_state(btn_bt_forget_, LV_STATE_DISABLED);
        else
            lv_obj_add_state(btn_bt_forget_, LV_STATE_DISABLED);
    }
}

void BarcodeScannerSettingsOverlay::handle_device_selected(const std::string& vendor_product,
                                                           const std::string& device_name,
                                                           const std::string& bt_mac) {
    auto& s = helix::SettingsManager::instance();

    spdlog::info("[{}] Selected: '{}' ({}{})", get_name(), device_name,
                 vendor_product.empty() ? "auto-detect" : vendor_product,
                 bt_mac.empty() ? "" : " bt:" + helix::redact::mac(bt_mac));

    s.set_scanner_device_id(vendor_product);
    s.set_scanner_device_name(vendor_product.empty() && bt_mac.empty() ? std::string{}
                                                                       : device_name);
    s.set_scanner_bt_address(bt_mac);

    refresh_current_selection_label();
    populate_device_list(); // redraw so checkmark moves to new selection
}

void BarcodeScannerSettingsOverlay::handle_keymap_changed(int dropdown_index) {
    const char* values[] = {"qwerty", "qwertz", "azerty"};
    if (dropdown_index < 0 || dropdown_index >= 3)
        return;
    helix::SettingsManager::instance().set_scanner_keymap(values[dropdown_index]);
    helix::UsbScannerMonitor::set_active_layout(
        helix::UsbScannerMonitor::parse_keymap(values[dropdown_index]));
}

void BarcodeScannerSettingsOverlay::start_bt_discovery() {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available() || !loader.discover) {
        spdlog::warn("[{}] BT discovery unavailable", get_name());
        return;
    }

    if (bt_discovering_) {
        spdlog::debug("[{}] BT discovery already in progress", get_name());
        return;
    }

    // Initialize BT context if needed
    if (!bt_ctx_ && loader.init) {
        bt_ctx_ = loader.init();
        if (!bt_ctx_) {
            spdlog::error("[{}] Failed to init BT context", get_name());
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("Bluetooth initialization failed"));
            return;
        }
    }

    bt_discovering_ = true;

    // Keep already-paired devices; remove unpaired (stale discovery results)
    bt_devices_.erase(std::remove_if(bt_devices_.begin(), bt_devices_.end(),
                                     [](const BtDeviceInfo& d) { return !d.paired; }),
                      bt_devices_.end());

    lv_subject_set_int(&bt_discovering_subject_, 1);

    // Refresh device list immediately so paired BT devices are visible
    // while discovery is in progress.
    populate_device_list();
    populate_bt_dropdown();

    // Set up discovery context (shared_ptr so the detached thread keeps it alive)
    bt_discovery_ctx_ = std::make_shared<BtDiscoveryContext>();
    bt_discovery_ctx_->alive.store(true);
    bt_discovery_ctx_->overlay = this;
    bt_discovery_ctx_->token = lifetime_.token();

    auto disc_ctx = bt_discovery_ctx_; // shared_ptr copy for thread
    auto* ctx = bt_ctx_;

    // Wrap spawn in try/catch per feedback_no_bare_threads_arm.md (#724) —
    // thread creation can fail on memory-constrained ARM targets.
    try {
        std::thread([ctx, disc_ctx]() {
            auto& ldr = helix::bluetooth::BluetoothLoader::instance();
            ldr.discover(
                ctx, 15000,
                [](const helix_bt_device* dev, void* user_data) {
                    auto* dctx = static_cast<BtDiscoveryContext*>(user_data);
                    if (!dctx->alive.load())
                        return;

                    // Filter: only include devices that look like HID scanners
                    bool looks_like_scanner =
                        helix::bluetooth::is_hid_scanner_uuid(dev->service_uuid) ||
                        helix::bluetooth::is_likely_bt_scanner(dev->name);

                    if (!looks_like_scanner) {
                        spdlog::trace("[BarcodeScannerSettings] Skipping non-scanner BT: {}",
                                      dev->name ? dev->name : "(null)");
                        return;
                    }

                    BtDeviceInfo info;
                    info.mac = dev->mac ? dev->mac : "";
                    info.name = dev->name ? dev->name : "Unknown";
                    info.paired = dev->paired;
                    info.is_ble = dev->is_ble;

                    // Use tok.defer() — token holds its own shared_ptr; safe if
                    // overlay is destroyed before the callback fires (#707).
                    dctx->token->defer([dctx, info]() {
                        if (!dctx->alive.load())
                            return;
                        auto* overlay = dctx->overlay;

                        // Avoid duplicates by MAC
                        bool found = false;
                        for (const auto& existing : overlay->bt_devices_) {
                            if (existing.mac == info.mac) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            overlay->bt_devices_.push_back(info);
                            spdlog::debug("[BarcodeScannerSettings] BT discovered: {} ({})",
                                          info.name, helix::redact::mac(info.mac));
                            overlay->populate_bt_dropdown();
                        }
                    });
                },
                disc_ctx.get());

            // Discovery completed (timeout or stopped)
            disc_ctx->token->defer([disc_ctx]() {
                if (!disc_ctx->alive.load())
                    return;
                auto* overlay = disc_ctx->overlay;
                overlay->bt_discovering_ = false;
                lv_subject_set_int(&overlay->bt_discovering_subject_, 0);
                spdlog::info("[BarcodeScannerSettings] BT discovery finished, {} scanner(s) found",
                             overlay->bt_devices_.size());
                overlay->populate_bt_dropdown();
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("[{}] Failed to spawn BT discovery thread: {}", get_name(), e.what());
        bt_discovery_ctx_->alive.store(false);
        bt_discovering_ = false;
        lv_subject_set_int(&bt_discovering_subject_, 0);
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      lv_tr("Could not start Bluetooth discovery"), 3000);
        return;
    }

    spdlog::info("[{}] Started Bluetooth scanner discovery", get_name());
}

void BarcodeScannerSettingsOverlay::stop_bt_discovery() {
    if (!bt_discovering_)
        return;

    if (bt_discovery_ctx_)
        bt_discovery_ctx_->alive.store(false);

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (bt_ctx_ && loader.stop_discovery)
        loader.stop_discovery(bt_ctx_);

    bt_discovering_ = false;
    lv_subject_set_int(&bt_discovering_subject_, 0);

    spdlog::debug("[{}] Stopped Bluetooth discovery", get_name());
}

void BarcodeScannerSettingsOverlay::pair_bt_device(const std::string& mac,
                                                   const std::string& name) {
    auto msg = fmt::format("{} {}?", lv_tr("Pair with"), name);
    // mac/name ride in the capture; the old lv_event_cb_t form carried them in
    // a heap PairData that needed an LV_EVENT_DELETE net to free.
    helix::ui::ConfirmOptions opts;
    opts.owner_token = lifetime_.token(); // gates the confirm on this overlay
    auto* dialog = helix::ui::modal_confirm(
        lv_tr("Pair Bluetooth Scanner"), msg.c_str(), ModalSeverity::Info, lv_tr("Pair"),
        [mac, name] {
            auto& loader = helix::bluetooth::BluetoothLoader::instance();
            if (!loader.is_available() || !loader.pair) {
                ToastManager::instance().show(ToastSeverity::ERROR,
                                              lv_tr("Bluetooth not available"));
            } else if (!s_active_instance_ || !s_active_instance_->bt_ctx_) {
                ToastManager::instance().show(ToastSeverity::ERROR,
                                              lv_tr("Bluetooth not initialized"));
            } else {
                ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Pairing..."), 5000);

                auto* bt_ctx = s_active_instance_->bt_ctx_;
                auto token = s_active_instance_->lifetime_.token();

                // Wrap spawn in try/catch per feedback_no_bare_threads_arm.md (#724).
                try {
                    std::thread([mac, name, bt_ctx, token]() {
                        auto& ldr = helix::bluetooth::BluetoothLoader::instance();
                        int ret = ldr.pair(bt_ctx, mac.c_str());
                        int paired_r = -1;
                        int bonded_r = -1;
                        // sd_bus_call_method returns any non-negative integer on success.
                        if (ret >= 0) {
                            paired_r = ldr.is_paired ? ldr.is_paired(bt_ctx, mac.c_str()) : -1;
                            bonded_r = ldr.is_bonded ? ldr.is_bonded(bt_ctx, mac.c_str()) : -1;
                            spdlog::info("[BarcodeScannerSettings] Post-pair: ret={} is_paired={} "
                                         "is_bonded={}",
                                         ret, paired_r, bonded_r);
                        }

                        // After BlueZ reports Pair success, the kernel still has to
                        // bring up the HID profile and create /dev/input/eventN.
                        // Always poll - even when Bonded=false, operators who set
                        // ClassicBondedOnly=false in /etc/bluetooth/input.conf can
                        // still get HID to attach, and we don't want to falsely
                        // report failure in that case. bonded_r is used below only
                        // to pick the right toast wording when HID does not appear.
                        bool hid_ok = false;
                        if (ret >= 0) {
                            for (int i = 0; i < 25; ++i) {
                                if (helix::input::find_bt_hid_device_by_mac(mac)) {
                                    hid_ok = true;
                                    break;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                            }
                            spdlog::info("[BarcodeScannerSettings] Post-pair HID probe: hid_ok={}",
                                         hid_ok);
                        }

                        // Clean up broken pairing: if we paired but didn't bond,
                        // the device is stuck in a "paired" state that will cause
                        // AlreadyExists on the next attempt and still won't work.
                        // Remove it so the user gets a clean slate on retry.
                        if (ret >= 0 && !hid_ok && bonded_r != 1) {
                            spdlog::info("[BarcodeScannerSettings] Removing unbonded device {} "
                                         "to allow clean re-pair",
                                         helix::redact::mac(mac));
                            if (ldr.remove_device) {
                                ldr.remove_device(bt_ctx, mac.c_str());
                            }
                        }

                        helix::ui::queue_update([ret, mac, name, token, bt_ctx, paired_r, bonded_r,
                                                 hid_ok]() {
                            if (token.expired())
                                return;

                            if (ret >= 0 && hid_ok) {
                                ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                              lv_tr("Paired successfully"), 2000);
                            } else if (ret >= 0 && bonded_r != 1) {
                                // Paired but not bonded - Just-Works SSP left no
                                // persistent key, so BlueZ's input plugin will
                                // refuse HID. Almost always means the scanner
                                // wasn't in pairing mode when Pair ran.
                                spdlog::warn("[BarcodeScannerSettings] Paired but not bonded - "
                                             "scanner needs to be in pairing mode");
                                ToastManager::instance().show(
                                    ToastSeverity::WARNING,
                                    lv_tr("Pairing did not complete — the scanner did not bond. "
                                          "Try turning it off and on, then tap Pair again."),
                                    10000);
                            } else if (ret >= 0) {
                                // Bonded but HID profile still never came up.
                                // Rarer - device-specific firmware quirk or HID
                                // descriptor rejected by the kernel. Keep the
                                // technical escape hatch here for operators who
                                // can edit system config.
                                spdlog::warn("[BarcodeScannerSettings] Bonded but HID profile "
                                             "did not connect within 5s - scanner will not be "
                                             "usable");
                                ToastManager::instance().show(
                                    ToastSeverity::WARNING,
                                    lv_tr("Scanner bonded but didn't attach as a keyboard. Try "
                                          "turning the scanner off and on, then tap Pair again. "
                                          "If that fails, add ClassicBondedOnly=false to "
                                          "/etc/bluetooth/input.conf."),
                                    10000);
                            }

                            if (ret >= 0) {
                                if (s_active_instance_) {
                                    for (auto& dev : s_active_instance_->bt_devices_) {
                                        if (dev.mac == mac) {
                                            dev.paired = (paired_r == 1);
                                            break;
                                        }
                                    }

                                    // Only adopt this scanner as the active one when
                                    // the HID link actually came up. Persisting a
                                    // broken bond means the app would keep "using" a
                                    // scanner that can't send any input.
                                    if (hid_ok) {
                                        helix::SettingsManager::instance().set_scanner_bt_address(
                                            mac);
                                        helix::SettingsManager::instance().set_scanner_device_name(
                                            name);
                                        s_active_instance_->refresh_current_selection_label();
                                    }
                                    s_active_instance_->populate_device_list();
                                    s_active_instance_->populate_bt_dropdown();
                                }
                            } else {
                                auto& ldr2 = helix::bluetooth::BluetoothLoader::instance();
                                const char* err =
                                    ldr2.last_error ? ldr2.last_error(bt_ctx) : "Unknown error";
                                spdlog::error("[BarcodeScannerSettings] Pairing failed: {}", err);
                                ToastManager::instance().show(ToastSeverity::ERROR,
                                                              lv_tr("Pairing failed"), 3000);
                            }
                        });
                    }).detach();
                } catch (const std::system_error& ex) {
                    spdlog::error("[BarcodeScannerSettings] Failed to spawn BT pair thread: {}",
                                  ex.what());
                    // Dismiss the "Pairing..." progress toast shown above so the user
                    // doesn't see a stale "in progress" message alongside the error.
                    ToastManager::instance().hide();
                    ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Pairing failed"),
                                                  3000);
                    // Re-sync Pair/Forget button enable state. Selection + paired
                    // flags are unchanged by the spawn failure, but call this so
                    // any ambient state (e.g. selection updated while the modal
                    // was open) is reflected - matches what the normal failure
                    // path achieves implicitly via populate_bt_dropdown().
                    if (s_active_instance_)
                        s_active_instance_->update_bt_action_buttons();
                }
            }
        },
        opts);
    if (!dialog) {
        spdlog::warn("[{}] Failed to show pairing confirmation modal", get_name());
    }
}

void BarcodeScannerSettingsOverlay::handle_bt_forget(const std::string& mac) {
    spdlog::info("[{}] Forgetting BT scanner {}", get_name(), helix::redact::mac(mac));

    auto tok = lifetime_.token();
    // Wrap spawn in try/catch per feedback_no_bare_threads_arm.md (#724).
    // `this` is captured so the inner tok.defer lambda can access member state;
    // it is never dereferenced on the BG thread, and tok.defer silently drops
    // if the overlay has been destroyed (singleton lifetime + token guard).
    try {
        std::thread([this, tok, mac]() {
            auto& loader = helix::bluetooth::BluetoothLoader::instance();
            if (!loader.is_available() || !loader.remove_device) {
                spdlog::error("[BarcodeScannerSettings] remove_device symbol missing");
            } else {
                auto* ctx = loader.get_or_create_context();
                if (!ctx) {
                    spdlog::error(
                        "[BarcodeScannerSettings] Failed to get BT context for remove_device");
                } else {
                    int r = loader.remove_device(ctx, mac.c_str());
                    if (r < 0) {
                        const char* err = loader.last_error ? loader.last_error(ctx) : "unknown";
                        spdlog::error(
                            "[BarcodeScannerSettings] remove_device failed for {}: r={} err={}",
                            helix::redact::mac(mac), r, err);
                        // Fall through — clear settings so UI doesn't show a stale entry
                    } else {
                        spdlog::info("[BarcodeScannerSettings] BlueZ unpair succeeded for {}",
                                     helix::redact::mac(mac));
                    }
                }
            }

            if (tok.expired())
                return;
            tok.defer([this, mac]() {
                auto& settings = helix::SettingsManager::instance();
                if (settings.get_scanner_bt_address() == mac) {
                    settings.set_scanner_bt_address("");
                    settings.set_scanner_device_id("");
                    settings.set_scanner_device_name("");
                }

                bt_devices_.erase(
                    std::remove_if(bt_devices_.begin(), bt_devices_.end(),
                                   [&mac](const BtDeviceInfo& d) { return d.mac == mac; }),
                    bt_devices_.end());

                refresh_current_selection_label();
                populate_device_list();
                populate_bt_dropdown();

                ToastManager::instance().show(ToastSeverity::SUCCESS,
                                              lv_tr("Bluetooth scanner forgotten"), 2000);
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("[{}] Failed to spawn forget thread: {}", get_name(), e.what());
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Could not forget device"), 3000);
    }
}

// ============================================================================
// Static XML event callbacks
// ============================================================================

void BarcodeScannerSettingsOverlay::on_bs_scan_bluetooth(lv_event_t*) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_scan_bluetooth");
    if (!s_active_instance_)
        return;
    if (s_active_instance_->bt_discovering_)
        s_active_instance_->stop_bt_discovery();
    else
        s_active_instance_->start_bt_discovery();
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_refresh_usb(lv_event_t*) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_refresh_usb");
    if (s_active_instance_)
        s_active_instance_->populate_device_list();
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_keymap_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_keymap_changed");
    if (!s_active_instance_)
        return;
    auto* dd = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    s_active_instance_->handle_keymap_changed(static_cast<int>(lv_dropdown_get_selected(dd)));
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_row_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_row_clicked");
    if (!s_active_instance_)
        return;
    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!row)
        return;
    auto* data = static_cast<RowData*>(lv_obj_get_user_data(row));
    if (!data)
        return;
    s_active_instance_->handle_device_selected(data->vendor_product, data->device_name,
                                               data->bt_mac);
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_row_forget(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_row_forget");
    if (!s_active_instance_)
        return;
    // The forget button is a child of the row; walk up to find the RowData.
    auto* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* row = btn ? lv_obj_get_parent(btn) : nullptr;
    auto* data = row ? static_cast<RowData*>(lv_obj_get_user_data(row)) : nullptr;
    if (!data || data->bt_mac.empty())
        return;
    s_active_instance_->handle_bt_forget(data->bt_mac);
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_bt_scanner_selected(lv_event_t*) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_bt_scanner_selected");
    if (!s_active_instance_)
        return;
    s_active_instance_->update_bt_action_buttons();

    // Persist the selection if the chosen device is already paired — that's
    // the user's "use this scanner" choice. Unpaired entries are skipped here;
    // they get persisted in the post-pair callback instead.
    const int idx = s_active_instance_->selected_bt_index();
    if (idx >= 0 && idx < static_cast<int>(s_active_instance_->bt_devices_.size())) {
        const auto& dev = s_active_instance_->bt_devices_[idx];
        if (dev.paired && !dev.mac.empty()) {
            auto& s = helix::SettingsManager::instance();
            if (s.get_scanner_bt_address() != dev.mac) {
                spdlog::info("[BarcodeScannerSettings] Active BT scanner -> {} ({})", dev.name,
                             helix::redact::mac(dev.mac));
                s.set_scanner_device_id(""); // BT path supersedes USB VID:PID match
                s.set_scanner_device_name(dev.name);
                s.set_scanner_bt_address(dev.mac);
                s_active_instance_->refresh_current_selection_label();
            }
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_bt_pair(lv_event_t*) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_bt_pair");
    if (!s_active_instance_)
        return;
    const int idx = s_active_instance_->selected_bt_index();
    if (idx < 0)
        return;
    const auto& dev = s_active_instance_->bt_devices_[idx];
    if (dev.paired)
        return; // already paired; button should be disabled
    s_active_instance_->pair_bt_device(dev.mac, dev.name);
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_bt_forget(lv_event_t*) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_bt_forget");
    if (!s_active_instance_)
        return;
    const int idx = s_active_instance_->selected_bt_index();
    if (idx < 0)
        return;
    const auto& dev = s_active_instance_->bt_devices_[idx];
    if (dev.mac.empty())
        return;
    s_active_instance_->handle_bt_forget(dev.mac);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
