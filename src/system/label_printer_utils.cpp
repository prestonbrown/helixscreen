// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "label_printer_utils.h"

#include "ui_update_queue.h"

#include "bluetooth_loader.h"
#include "brother_pt_bt_printer.h"
#include "brother_pt_protocol.h"
#include "brother_ql_bt_printer.h"
#include "brother_ql_printer.h"
#include "brother_ql_protocol.h"
#include "bt_discovery_utils.h"
#include "bt_print_utils.h"
#include "http_executor.h"
#include "ipp_printer.h"
#include "label_printer_settings.h"
#include "label_renderer.h"
#include "lvgl.h"
#include "makeid_bt_printer.h"
#include "makeid_protocol.h"
#include "niimbot_bt_printer.h"
#include "niimbot_protocol.h"
#include "phomemo_bt_printer.h"
#include "phomemo_printer.h"
#include "safe_resolve.h"
#include "sheet_label_layout.h"
#include "text_io.h"
#include "usb_printer_detector.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace helix {

/// Query Brother QL printer media over TCP. Returns detected media info.
/// Connects, sends status request, reads 32-byte response, disconnects.
static helix::label::BrotherQLMedia query_brother_media_tcp(const std::string& host, int port) {
    helix::label::BrotherQLMedia media{};

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return media;

    struct timeval tv {};
    tv.tv_sec = 5;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Safe DNS resolution (avoids glibc __check_pf crash on ARM)
    struct sockaddr_in addr {};
    if (helix::safe_resolve(host, port, addr) != 0) {
        close(sockfd);
        return media;
    }

    if (connect(sockfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sockfd);
        return media;
    }

    auto req = helix::label::brother_ql_build_status_request();
    send(sockfd, req.data(), req.size(), MSG_NOSIGNAL);

    uint8_t status[32] = {};
    ssize_t n = recv(sockfd, status, sizeof(status), MSG_WAITALL);
    close(sockfd);

    if (n == 32) {
        media = helix::label::brother_ql_parse_status(status, 32);
        if (media.valid) {
            spdlog::info("[LabelPrinter] Detected Brother QL media: {}mm{}", media.width_mm,
                         media.length_mm > 0 ? fmt::format("x{}mm", media.length_mm)
                                             : " continuous");
        }
    }
    return media;
}

void print_spool_label(const SpoolInfo& spool, PrintCallback callback) {
    auto& settings = LabelPrinterSettingsManager::instance();
    const std::string type = settings.get_printer_type();
    const std::string protocol = settings.get_printer_protocol();
    const bool is_usb = (type == "usb");
    const bool is_bt = (type == "bluetooth");
    const bool is_ipp = (type == "network" && protocol == "ipp");

    std::vector<LabelSize> sizes;
    if (is_ipp) {
        sizes = IppPrinter::supported_sizes_static();
    } else if (is_usb) {
        sizes = PhomemoPrinter::supported_sizes_static();
    } else if (is_bt) {
        const auto bt_name = settings.get_bt_name();
        if (helix::bluetooth::is_brother_pt_printer(bt_name.c_str())) {
            sizes = helix::label::BrotherPTBluetoothPrinter::supported_sizes_static();
        } else if (helix::bluetooth::is_brother_printer(bt_name.c_str())) {
            sizes = BrotherQLPrinter::supported_sizes_static();
        } else if (helix::bluetooth::is_niimbot_printer(bt_name.c_str())) {
            sizes = helix::label::niimbot_sizes_for_model(bt_name);
        } else if (helix::bluetooth::is_makeid_printer(bt_name.c_str())) {
            sizes = helix::label::makeid_default_sizes();
        } else {
            sizes = PhomemoPrinter::supported_sizes_static();
        }
    } else {
        sizes = BrotherQLPrinter::supported_sizes_static();
    }

    int size_idx =
        std::clamp(settings.get_label_size_index(), 0, static_cast<int>(sizes.size()) - 1);
    const auto& label_size = sizes[size_idx];
    auto preset = static_cast<LabelPreset>(
        std::clamp(settings.get_label_preset(), 0, static_cast<int>(LabelPreset::MINIMAL)));

    // Brother QL network: detect media + render on worker thread
    const bool is_brother_net = !is_ipp && !is_usb && !is_bt;
    if (is_brother_net) {
        // Handled below — rendering deferred to worker thread after media detection
    } else {
        // Force QR-only for small square/narrow labels where text won't fit
        if (label_size.width_px <= 250 && label_size.height_px > 0 && label_size.height_px <= 250) {
            preset = LabelPreset::MINIMAL;
        }
    }

    // Render bitmap for non-Brother-network paths
    LabelBitmap bitmap;
    if (!is_brother_net) {
        bitmap = LabelRenderer::render(spool, preset, label_size);
        if (bitmap.empty()) {
            if (callback)
                callback(false, lv_tr("Failed to render label"));
            return;
        }
    }

    if (is_ipp) {
        static IppPrinter ipp_printer;
        ipp_printer.set_target(settings.get_printer_address(), settings.get_printer_port(),
                               "ipp/print");
        ipp_printer.set_sheet_template(size_idx);
        ipp_printer.set_label_count(settings.get_label_count());
        ipp_printer.print(bitmap, label_size, callback);
    } else if (is_usb) {
        // libusb opens every matching device, too slow for the UI thread.
        UsbPrinterDetector::scan_async(
            [bitmap, label_size = label_size, callback](std::vector<UsbPrinterInfo> detected) {
                auto& usb_settings = LabelPrinterSettingsManager::instance();
                uint16_t vid = usb_settings.get_usb_vid();
                uint16_t pid = usb_settings.get_usb_pid();

                bool found = false;
                for (const auto& d : detected) {
                    if (d.vid == vid && d.pid == pid) {
                        found = true;
                        break;
                    }
                }

                if (!found && !detected.empty()) {
                    vid = detected[0].vid;
                    pid = detected[0].pid;
                    spdlog::info("[LabelPrinter] Configured USB printer not found, using {}",
                                 detected[0].product_name);
                } else if (!found) {
                    if (callback)
                        callback(false, lv_tr("No USB printer detected"));
                    return;
                }

                static PhomemoPrinter usb_printer;
                usb_printer.set_device(vid, pid, usb_settings.get_usb_serial());
                usb_printer.print(bitmap, label_size, callback);
            });
    } else if (is_bt) {
        const auto bt_address = settings.get_bt_address();
        const auto bt_name = settings.get_bt_name();

        if (helix::bluetooth::is_brother_pt_printer(bt_name.c_str())) {
            // Brother PT: deferred render — tape width detected via status query
            helix::label::BrotherPTBluetoothPrinter printer;
            printer.set_device(bt_address);
            printer.print_spool(spool, preset, callback);
        } else if (helix::bluetooth::is_brother_printer(bt_name.c_str())) {
            // Brother QL BT: RFCOMM only supports one connection at a time,
            // so media detection requires a single-connection flow. For now,
            // use the selected label size from settings.
            helix::label::BrotherQLBluetoothPrinter printer;
            printer.set_device(bt_address);
            printer.print(bitmap, label_size, callback);
        } else if (helix::bluetooth::is_niimbot_printer(bt_name.c_str())) {
            helix::label::NiimbotBluetoothPrinter printer;
            printer.set_device(bt_address, bt_name);
            printer.print(bitmap, label_size, callback);
        } else if (helix::bluetooth::is_makeid_printer(bt_name.c_str())) {
            helix::label::MakeIdBluetoothPrinter printer;
            printer.set_device(bt_address, bt_name);
            printer.print(bitmap, label_size, callback);
        } else {
            helix::label::PhomemoBluetoothPrinter printer;
            printer.set_device(bt_address, settings.get_bt_transport());
            printer.print(bitmap, label_size, callback);
        }
    } else {
        // Brother QL network: detect loaded media on worker thread, then render + print.
        // Route through HttpExecutor::fast() instead of spawning a detached std::thread
        // per click — raw thread creation crashed on thread-constrained ARM devices
        // (CC1 / AD5M, see #724) and the inner chain's joinable-thread destruction
        // hit std::terminate when a rapid click cancelled a partial send.
        auto host = settings.get_printer_address();
        auto port = settings.get_printer_port();
        helix::http::HttpExecutor::fast().submit([spool, preset, sizes, host, port, callback]() {
            // Query printer for loaded media
            auto media = query_brother_media_tcp(host, port);
            LabelSize actual_size;
            if (media.valid) {
                auto* matched = helix::label::brother_ql_match_media(media, sizes);
                if (matched) {
                    actual_size = *matched;
                    spdlog::info("[LabelPrinter] Auto-detected label: {}", actual_size.name);
                } else {
                    spdlog::warn("[LabelPrinter] Unknown media {}mm{}, using selected size",
                                 media.width_mm,
                                 media.length_mm > 0 ? fmt::format("x{}mm", media.length_mm) : "");
                    // Fall back to user-selected size
                    int idx =
                        std::clamp(LabelPrinterSettingsManager::instance().get_label_size_index(),
                                   0, static_cast<int>(sizes.size()) - 1);
                    actual_size = sizes[idx];
                }
            } else {
                spdlog::warn("[LabelPrinter] Could not detect media, using selected size");
                int idx = std::clamp(LabelPrinterSettingsManager::instance().get_label_size_index(),
                                     0, static_cast<int>(sizes.size()) - 1);
                actual_size = sizes[idx];
            }

            // Apply preset override for small labels
            auto actual_preset = preset;
            if (actual_size.width_px <= 250 && actual_size.height_px > 0 &&
                actual_size.height_px <= 250) {
                actual_preset = LabelPreset::MINIMAL;
            }

            auto bitmap = LabelRenderer::render(spool, actual_preset, actual_size);
            if (bitmap.empty()) {
                helix::ui::queue_update("label_printer_utils::print_spool_label", [callback]() {
                    if (callback)
                        callback(false, lv_tr("Failed to render label"));
                });
                return;
            }

            static BrotherQLPrinter net_printer;
            net_printer.print_label(host, port, bitmap, actual_size, callback);
        });
    }
}

std::string friendly_label_printer_error(const std::string& raw) {
    if (raw.empty()) {
        return lv_tr("Print failed");
    }

    // Normalize to lowercase so case variation across backends doesn't cause
    // the intended branch to be skipped (makeid emits "Write failed at chunk"
    // with a capital W; niimbot/phomemo BT emit "BLE write failed" lowercase).
    std::string lower = raw;
    lower = helix::text_io::to_lower(lower);

    // BR/EDR transport wedged — stale pairing or dual-mode confusion. The
    // ConnectProfile fallback in bt_ble.cpp already retries LE once, so if we
    // still see this the device is genuinely unreachable on both transports.
    if (lower.find("br-connection-") != std::string::npos) {
        return lv_tr("Can't connect — try forgetting and re-pairing the printer");
    }
    // BlueZ Connect() re-entered while a previous attempt is still pending.
    // User tapped Test Print twice; wait and retry.
    if (lower.find("in progress") != std::string::npos ||
        lower.find("inprogress") != std::string::npos) {
        return lv_tr("Already connecting — wait a moment and try again");
    }
    // Peripheral off / out of range / asleep.
    if (lower.find("timed out") != std::string::npos ||
        lower.find("timeout") != std::string::npos ||
        lower.find("host is down") != std::string::npos) {
        return lv_tr("Printer didn't respond — make sure it's on and in range");
    }
    // GATT service missing — device advertises but doesn't expose the write
    // characteristic we expect. Usually means wrong device selected.
    if (lower.find("gatt characteristic not found") != std::string::npos) {
        return lv_tr("Printer doesn't expose the expected service — wrong device?");
    }
    // Not paired / authorization dropped.
    if (lower.find("not authorized") != std::string::npos ||
        lower.find("notauthorized") != std::string::npos ||
        lower.find("not paired") != std::string::npos) {
        return lv_tr("Printer is not paired — pair it first");
    }
    // Host-side setup problems.
    if (lower.find("bluetooth not available") != std::string::npos) {
        return lv_tr("Bluetooth is not available on this device");
    }
    // Bluetooth-specific "not configured" only. USB/IPP "not configured"
    // strings fall through to the generic config branch below.
    if (lower.find("bluetooth device not configured") != std::string::npos) {
        return lv_tr("No Bluetooth printer selected — pick one in settings");
    }
    if (lower.find("not configured") != std::string::npos) {
        return lv_tr("No printer configured — check printer settings");
    }
    // USB access issues (phomemo USB path emits "Cannot open /dev/usb/...").
    if (lower.find("/dev/usb") != std::string::npos ||
        lower.find("check permissions") != std::string::npos) {
        return lv_tr("USB printer access denied — check permissions");
    }
    // Mid-print write failure — connection dropped / USB write error.
    if (lower.find("write failed") != std::string::npos ||
        lower.find("write to ") != std::string::npos) {
        return lv_tr("Lost connection during print — try again");
    }
    // CUPS/IPP network errors.
    if (lower.find("http") != std::string::npos || lower.find("ipp") != std::string::npos) {
        return lv_tr("Print server rejected the job — check printer status");
    }
    // Niimbot job buffered but printer isn't moving — almost always cover, paper,
    // or label-type/RFID (B1/B18 reject non-genuine rolls). Surface verbatim so the
    // firmware status/error bytes reach the user for diagnosis.
    if (lower.find("isn't printing") != std::string::npos ||
        lower.find("didn't start") != std::string::npos) {
        return raw;
    }
    return lv_tr("Print failed");
}

} // namespace helix

#endif // HELIX_HAS_LABEL_PRINTER
