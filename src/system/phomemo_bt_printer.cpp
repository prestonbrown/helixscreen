// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "phomemo_bt_printer.h"

#include "ui_update_queue.h"

#include "bluetooth_loader.h"
#include "bt_print_utils.h"
#include "helix_thread.h"
#include "log_redact.h"
#include "lvgl.h"
#include "phomemo_printer.h"
#include "phomemo_protocol.h"

#include <spdlog/spdlog.h>

#include <thread>

namespace helix::label {

void PhomemoBluetoothPrinter::set_device(const std::string& mac, const std::string& transport) {
    mac_ = mac;
    transport_ = transport;
}

std::string PhomemoBluetoothPrinter::name() const {
    return "Phomemo (Bluetooth)";
}

std::vector<LabelSize> PhomemoBluetoothPrinter::supported_sizes() const {
    return helix::PhomemoPrinter::supported_sizes_static();
}

void PhomemoBluetoothPrinter::print(const LabelBitmap& bitmap, const LabelSize& size,
                                    PrintCallback callback) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        spdlog::error("Phomemo BT: Bluetooth not available");
        helix::ui::queue_update("PhomemoBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("Bluetooth not available"));
        });
        return;
    }

    if (mac_.empty()) {
        spdlog::error("Phomemo BT: No device configured");
        helix::ui::queue_update("PhomemoBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("Bluetooth device not configured"));
        });
        return;
    }

    auto commands = phomemo_build_raster(bitmap, size);
    spdlog::warn("Phomemo BT: sending {} bytes to {} via {}", commands.size(),
                 helix::redact::mac(mac_), transport_);

    std::string mac = mac_;
    bool use_spp = (transport_ == "spp");

    // Wrap thread spawn in try/catch — pthread_create EAGAIN on resource-constrained
    // ARM (AD5M/CC1) throws std::system_error which aborts with std::terminate
    // if it escapes an LVGL event frame (#724, #837, [L083]).
    try {
        helix::make_thread([mac, use_spp, commands = std::move(commands), callback]() {
            bool success = false;
            std::string error;

            if (use_spp) {
                auto result = helix::bluetooth::rfcomm_send(mac, 1, commands, "Phomemo BT");
                success = result.success;
                error = std::move(result.error);
            } else {
                // BLE GATT path
                auto& loader = helix::bluetooth::BluetoothLoader::instance();
                auto* ctx = loader.init();
                if (!ctx) {
                    error = lv_tr("Failed to initialize Bluetooth context");
                    spdlog::error("Phomemo BT: {}", error);
                } else {
                    int handle = loader.connect_ble(ctx, mac.c_str(), kPhomemoWriteUuid);
                    if (handle < 0) {
                        const char* err =
                            loader.last_error ? loader.last_error(ctx) : "unknown error";
                        error = fmt::format(lv_tr("BLE connect failed: {}"), err);
                        spdlog::error("Phomemo BT: {}", error);
                    } else {
                        int ret = loader.ble_write(ctx, handle, commands.data(),
                                                   static_cast<int>(commands.size()));
                        if (ret < 0) {
                            const char* err =
                                loader.last_error ? loader.last_error(ctx) : "unknown error";
                            error = fmt::format(lv_tr("BLE write failed: {}"), err);
                            spdlog::error("Phomemo BT: {}", error);
                        } else {
                            success = true;
                            spdlog::warn("Phomemo BT: sent {} bytes via BLE", commands.size());
                        }
                        loader.disconnect(ctx, handle);
                    }
                    loader.deinit(ctx);
                }
            }

            helix::ui::queue_update("PhomemoBluetoothPrinter::print", [callback, success, error]() {
                if (callback)
                    callback(success, error);
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("Phomemo BT: failed to spawn print thread: {}", e.what());
        helix::ui::queue_update("PhomemoBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("System busy — please try again"));
        });
    }
}

} // namespace helix::label

#endif // HELIX_HAS_LABEL_PRINTER
