// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "brother_ql_bt_printer.h"

#include "bluetooth_loader.h"
#include "brother_ql_printer.h"
#include "brother_ql_protocol.h"
#include "bt_print_utils.h"
#include "core/update_queue.h"
#include "helix_thread.h"
#include "log_redact.h"

#include <spdlog/spdlog.h>

#include <thread>

namespace helix::label {

void BrotherQLBluetoothPrinter::set_device(const std::string& mac, int channel) {
    mac_ = mac;
    channel_ = channel;
}

std::string BrotherQLBluetoothPrinter::name() const {
    return "Brother QL (Bluetooth)";
}

std::vector<LabelSize> BrotherQLBluetoothPrinter::supported_sizes() const {
    return helix::BrotherQLPrinter::supported_sizes_static();
}

void BrotherQLBluetoothPrinter::print(const LabelBitmap& bitmap, const LabelSize& size,
                                      PrintCallback callback) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        spdlog::error("Brother QL BT: Bluetooth not available");
        helix::ui::queue_update("BrotherQLBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, "Bluetooth not available");
        });
        return;
    }

    if (mac_.empty()) {
        spdlog::error("Brother QL BT: No device configured");
        helix::ui::queue_update("BrotherQLBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, "Bluetooth device not configured");
        });
        return;
    }

    auto commands = brother_ql_build_raster(bitmap, size);
    spdlog::warn("Brother QL BT: sending {} bytes to {} ch{}", commands.size(),
                 helix::redact::mac(mac_), channel_);

    std::string mac = mac_;
    int channel = channel_;

    // Wrap thread spawn in try/catch — pthread_create EAGAIN on resource-constrained
    // ARM (AD5M/CC1) throws std::system_error which aborts with std::terminate
    // if it escapes an LVGL event frame (#724, #837, [L083]).
    try {
        helix::make_thread([mac, channel, commands = std::move(commands), callback]() {
            auto result = helix::bluetooth::rfcomm_send(mac, channel, commands, "Brother QL BT");

            helix::ui::queue_update("BrotherQLBluetoothPrinter::print", [callback, result]() {
                if (callback)
                    callback(result.success, result.error);
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("Brother QL BT: failed to spawn print thread: {}", e.what());
        helix::ui::queue_update("BrotherQLBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, "System busy — please try again");
        });
    }
}

} // namespace helix::label

#endif // HELIX_HAS_LABEL_PRINTER
