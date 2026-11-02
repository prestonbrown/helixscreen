// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "brother_pt_bt_printer.h"

#include "ui_update_queue.h"

#include "bluetooth_loader.h"
#include "brother_pt_protocol.h"
#include "bt_print_utils.h"
#include "helix_thread.h"
#include "label_printer_settings.h"
#include "label_renderer.h"
#include "log_redact.h"
#include "lvgl.h"
#include "spoolman_types.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace helix::label {

void BrotherPTBluetoothPrinter::set_device(const std::string& mac) {
    mac_ = mac;
}

std::string BrotherPTBluetoothPrinter::name() const {
    return "Brother PT (Bluetooth)"; // i18n: do not translate - product name
}

std::vector<LabelSize> BrotherPTBluetoothPrinter::supported_sizes() const {
    return supported_sizes_static();
}

std::vector<LabelSize> BrotherPTBluetoothPrinter::supported_sizes_static() {
    std::vector<LabelSize> sizes;
    for (int w : {4, 6, 9, 12, 18, 24}) {
        auto s = brother_pt_label_size_for_tape(w);
        if (s)
            sizes.push_back(*s);
    }
    return sizes;
}

void BrotherPTBluetoothPrinter::print(const LabelBitmap& bitmap, const LabelSize& size,
                                      PrintCallback callback) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        spdlog::error("[Brother PT BT] Bluetooth not available");
        helix::ui::queue_update("BrotherPTBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("Bluetooth not available"));
        });
        return;
    }

    if (mac_.empty()) {
        spdlog::error("[Brother PT BT] No device configured");
        helix::ui::queue_update("BrotherPTBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("Bluetooth device not configured"));
        });
        return;
    }

    auto commands = brother_pt_build_raster(bitmap, size.width_mm);
    if (commands.empty()) {
        helix::ui::queue_update("BrotherPTBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("Unsupported tape width"));
        });
        return;
    }

    spdlog::warn("[Brother PT BT] Sending {} bytes to {} (channel resolved at send time)",
                 commands.size(), helix::redact::mac(mac_));

    std::string mac = mac_;

    // Wrap thread spawn in try/catch — pthread_create EAGAIN on resource-constrained
    // ARM (AD5M/CC1) throws std::system_error which aborts with std::terminate
    // if it escapes an LVGL event frame (#724, #837, [L083]).
    try {
        helix::make_thread([mac, commands = std::move(commands), callback]() {
            // fallback_channel=1 preserves first-run behavior on SDP-less builds.
            // Brother PT-E550W / P750W historically advertise SPP on channel 1.
            auto result = helix::bluetooth::rfcomm_send(mac, 1, commands, "Brother PT BT");
            helix::ui::queue_update("BrotherPTBluetoothPrinter::print", [callback, result]() {
                if (callback)
                    callback(result.success, result.error);
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("[Brother PT BT] Failed to spawn print thread: {}", e.what());
        helix::ui::queue_update("BrotherPTBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("System busy — please try again"));
        });
    }
}

void BrotherPTBluetoothPrinter::print_spool(const SpoolInfo& spool, LabelPreset preset,
                                            PrintCallback callback) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        spdlog::error("[Brother PT BT] Bluetooth not available");
        helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
            if (callback)
                callback(false, lv_tr("Bluetooth not available"));
        });
        return;
    }

    if (mac_.empty()) {
        spdlog::error("[Brother PT BT] No device configured");
        helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
            if (callback)
                callback(false, lv_tr("Bluetooth device not configured"));
        });
        return;
    }

    std::string mac = mac_;

    // Wrap thread spawn in try/catch — pthread_create EAGAIN on resource-constrained
    // ARM (AD5M/CC1) throws std::system_error which aborts with std::terminate
    // if it escapes an LVGL event frame (#724, #837, [L083]).
    try {
        helix::make_thread([mac, spool, preset, callback]() {
            auto& loader = helix::bluetooth::BluetoothLoader::instance();
            auto* ctx = loader.get_or_create_context();
            if (!ctx) {
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                    if (callback)
                        callback(false, lv_tr("Failed to initialize Bluetooth"));
                });
                return;
            }

            // Resolve RFCOMM channel via cache + SDP. Fallback 1 matches Brother PT
            // historical SPP channel for builds without a working SDP symbol.
            int channel = helix::label::resolve_label_printer_channel(mac, 1);
            if (channel <= 0) {
                spdlog::error("[Brother PT BT] No RFCOMM channel resolved for {}",
                              helix::redact::mac(mac));
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                    if (callback)
                        callback(false, lv_tr("Could not resolve printer RFCOMM channel"));
                });
                return;
            }

            // Single RFCOMM connection for both status query and raster send
            spdlog::info("[Brother PT BT] Connecting to {} ch{}", helix::redact::mac(mac), channel);
            int fd = loader.connect_rfcomm(ctx, mac.c_str(), channel);
            if (fd < 0) {
                // If we used a cached channel, invalidate it so the next print retries
                // with fresh SDP. Matches the invalidate-on-failure pattern in MakeID.
                int cached = helix::LabelPrinterSettingsManager::instance().get_bt_channel();
                if (cached > 0 && cached == channel) {
                    spdlog::warn(
                        "[Brother PT BT] cached channel {} failed; invalidating for next print",
                        channel);
                    helix::LabelPrinterSettingsManager::instance().set_bt_channel(0);
                }
                const char* err = loader.last_error ? loader.last_error(ctx) : "Unknown error";
                helix::ui::queue_update(
                    "BrotherPTBluetoothPrinter::print_spool", [callback, e = std::string(err)]() {
                        if (callback)
                            callback(false, lv_tr("RFCOMM connect failed: ") + e);
                    });
                return;
            }

            auto cleanup = [&]() { loader.disconnect(ctx, fd); };

            // Step 1: Query status to detect loaded tape
            auto status_cmd = brother_pt_build_status_request();
            ssize_t written = write(fd, status_cmd.data(), status_cmd.size());
            if (written < 0) {
                spdlog::error("[Brother PT BT] Status write failed: {}", strerror(errno));
                cleanup();
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                    if (callback)
                        callback(false, lv_tr("Failed to send status request"));
                });
                return;
            }

            // Read 32-byte status response with timeout
            uint8_t status_buf[32] = {};
            struct timeval tv = {5, 0}; // 5 second timeout
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t nread = read(fd, status_buf, 32);
            if (nread < 32) {
                spdlog::error("[Brother PT BT] Status read: got {} bytes (expected 32)", nread);
                cleanup();
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                    if (callback)
                        callback(false, lv_tr("Failed to read printer status"));
                });
                return;
            }

            auto media = brother_pt_parse_status(status_buf, 32);
            if (!media.valid) {
                cleanup();
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                    if (callback)
                        callback(false, lv_tr("Invalid status response from printer"));
                });
                return;
            }

            auto error = brother_pt_error_string(media);
            if (!error.empty()) {
                cleanup();
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool",
                                        [callback, error]() {
                                            if (callback)
                                                callback(false, error);
                                        });
                return;
            }

            if (media.width_mm == 0) {
                cleanup();
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                    if (callback)
                        callback(false, lv_tr("No tape detected in printer"));
                });
                return;
            }

            // Step 2: Build label size from detected tape
            auto label_size = brother_pt_label_size_for_tape(media.width_mm);
            if (!label_size) {
                cleanup();
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool",
                                        [callback, w = media.width_mm]() {
                                            if (callback)
                                                callback(false, lv_tr("Unsupported tape width: ") +
                                                                    std::to_string(w) + "mm");
                                        });
                return;
            }

            spdlog::warn("[Brother PT BT] Detected {} tape ({} printable pixels)", label_size->name,
                         label_size->width_px);

            // Step 3: Render label at detected tape dimensions
            auto actual_preset = preset;
            if (label_size->width_px <= 50) {
                actual_preset = LabelPreset::MINIMAL;
            }

            auto bitmap = helix::LabelRenderer::render(spool, actual_preset, *label_size);
            if (bitmap.empty()) {
                cleanup();
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                    if (callback)
                        callback(false, lv_tr("Failed to render label"));
                });
                return;
            }

            // Step 4: Build raster and send on same connection
            auto commands = brother_pt_build_raster(bitmap, media.width_mm);
            if (commands.empty()) {
                cleanup();
                helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                    if (callback)
                        callback(false, lv_tr("Failed to build raster data"));
                });
                return;
            }

            spdlog::info("[Brother PT BT] Sending {} bytes on fd {}", commands.size(), fd);

            // Write raster data in chunks
            size_t total = commands.size();
            size_t sent = 0;
            while (sent < total) {
                size_t chunk = std::min(total - sent, size_t(4096));
                ssize_t w = write(fd, commands.data() + sent, chunk);
                if (w < 0) {
                    spdlog::error("[Brother PT BT] Raster write failed at byte {}: {}", sent,
                                  strerror(errno));
                    cleanup();
                    helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                        if (callback)
                            callback(false, lv_tr("Failed to send print data"));
                    });
                    return;
                }
                sent += static_cast<size_t>(w);
            }

            spdlog::warn("[Brother PT BT] Sent {} bytes, waiting for print completion", sent);

            // Brief wait for print completion status (printer sends 32-byte response)
            struct timeval tv2 = {3, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
            uint8_t completion_buf[32] = {};
            nread = read(fd, completion_buf, 32);
            if (nread == 32) {
                auto completion = brother_pt_parse_status(completion_buf, 32);
                if (completion.valid && completion.status_type == 0x02) {
                    auto cerr = brother_pt_error_string(completion);
                    spdlog::error("[Brother PT BT] Print error: {}", cerr);
                    cleanup();
                    helix::ui::queue_update(
                        "BrotherPTBluetoothPrinter::print_spool", [callback, cerr]() {
                            if (callback)
                                callback(false,
                                         cerr.empty() ? std::string(lv_tr("Print error")) : cerr);
                        });
                    return;
                }
                spdlog::debug("[Brother PT BT] Completion status: type={}", completion.status_type);
            }

            cleanup();
            helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
                if (callback)
                    callback(true, "");
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("[Brother PT BT] Failed to spawn print thread: {}", e.what());
        helix::ui::queue_update("BrotherPTBluetoothPrinter::print_spool", [callback]() {
            if (callback)
                callback(false, lv_tr("System busy — please try again"));
        });
    }
}

} // namespace helix::label

#endif // HELIX_HAS_LABEL_PRINTER
