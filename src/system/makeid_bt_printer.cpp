// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "makeid_bt_printer.h"

#include "ui_update_queue.h"

#include "bluetooth_loader.h"
#include "bt_print_utils.h"
#include "helix_thread.h"
#include "label_printer_settings.h"
#include "log_redact.h"
#include "lvgl.h"
#include "makeid_protocol.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace helix::label {

static std::mutex s_print_mutex;
static int s_rfcomm_fd = -1;
static std::string s_connected_mac;

static std::string hex_dump(const uint8_t* data, int len) {
    std::string hex;
    for (int i = 0; i < std::min(len, 40); i++) {
        if (!hex.empty())
            hex += ' ';
        hex += fmt::format("{:02X}", data[i]);
    }
    if (len > 40)
        hex += "...";
    return hex;
}

static bool rfcomm_write(int fd, const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = ::write(fd, data + offset, len - offset);
        if (n <= 0) {
            spdlog::error("MakeID BT: RFCOMM write failed: {}", strerror(errno));
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

static int rfcomm_read(int fd, uint8_t* buf, int buf_len, int timeout_ms) {
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return ret;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        return -EIO;
    ssize_t n = ::read(fd, buf, static_cast<size_t>(buf_len));
    return (n >= 0) ? static_cast<int>(n) : -errno;
}

/// Drain any stale data from the RFCOMM buffer
static void rfcomm_drain(int fd) {
    uint8_t junk[256];
    while (true) {
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 50) <= 0)
            break;
        if (::read(fd, junk, sizeof(junk)) <= 0)
            break;
    }
}

static MakeIdResponse read_response(int fd, const char* label, int timeout_ms = 3000) {
    uint8_t resp[128];
    int n = rfcomm_read(fd, resp, sizeof(resp), timeout_ms);
    if (n > 0) {
        spdlog::debug("MakeID BT: {} response ({} bytes): {}", label, n, hex_dump(resp, n));
        return makeid_parse_response(resp, static_cast<size_t>(n));
    } else if (n == 0) {
        spdlog::warn("MakeID BT: {} response timeout", label);
        return {MakeIdResponseStatus::Resend};
    } else {
        spdlog::warn("MakeID BT: {} response read error: {}", label, n);
        return {MakeIdResponseStatus::Error};
    }
}

static void cleanup_connection() {
    if (s_rfcomm_fd >= 0) {
        ::close(s_rfcomm_fd);
        s_rfcomm_fd = -1;
    }
    s_connected_mac.clear();
}

static int ensure_connected(helix::bluetooth::BluetoothLoader& loader, const std::string& mac) {
    if (s_connected_mac == mac && s_rfcomm_fd >= 0) {
        // Drain stale data, then check keepalive
        rfcomm_drain(s_rfcomm_fd);
        auto pkt = makeid_build_handshake(MakeIdHandshakeState::Search);
        if (rfcomm_write(s_rfcomm_fd, pkt.data(), pkt.size())) {
            auto resp = read_response(s_rfcomm_fd, "keepalive", 1000);
            if (resp.status != MakeIdResponseStatus::Error) {
                spdlog::debug("MakeID BT: reusing RFCOMM (fd={})", s_rfcomm_fd);
                return s_rfcomm_fd;
            }
        }
        spdlog::debug("MakeID BT: existing connection dead, reconnecting");
    }

    cleanup_connection();

    // Use the shared BT context to avoid D-Bus agent conflicts.
    // The UI's pairing already established the BlueZ managed link via the
    // same shared context. Creating a second context via loader.init()
    // causes ECONNABORTED.
    if (!loader.connect_rfcomm) {
        spdlog::error("MakeID BT: BT plugin has no connect_rfcomm");
        return -1;
    }

    auto* ctx = loader.get_or_create_context();
    if (!ctx) {
        spdlog::error("MakeID BT: failed to get BT context");
        return -1;
    }

    int channel = helix::label::resolve_label_printer_channel(mac, 1);
    if (channel <= 0) {
        spdlog::error("MakeID BT: no RFCOMM channel resolved for {}", helix::redact::mac(mac));
        return -1;
    }
    spdlog::info("MakeID BT: RFCOMM connect to {} ch{}", helix::redact::mac(mac), channel);
    int fd = loader.connect_rfcomm(ctx, mac.c_str(), channel);
    if (fd < 0) {
        spdlog::error("MakeID BT: RFCOMM connect failed ({})", fd);
        // If we used a cached channel, invalidate it so the next print tries
        // a fresh SDP lookup. MakeID's async dispatcher makes in-call retry
        // impractical, so we rely on the next print to reconnect cleanly.
        int cached = helix::LabelPrinterSettingsManager::instance().get_bt_channel();
        if (cached > 0 && cached == channel) {
            spdlog::warn("MakeID BT: cached channel {} failed; invalidating for next print",
                         channel);
            helix::LabelPrinterSettingsManager::instance().set_bt_channel(0);
        }
        return -1;
    }
    s_rfcomm_fd = fd;

    s_connected_mac = mac;
    spdlog::warn("MakeID BT: RFCOMM connected (fd={})", s_rfcomm_fd);

    // Drain any stale data, then handshake
    rfcomm_drain(s_rfcomm_fd);
    auto handshake = makeid_build_handshake(MakeIdHandshakeState::Search);
    rfcomm_write(s_rfcomm_fd, handshake.data(), handshake.size());
    auto resp = read_response(s_rfcomm_fd, "init-handshake", 2000);
    spdlog::info("MakeID BT: handshake err={}", resp.error_code);

    return s_rfcomm_fd;
}

static bool wait_for_ready(int fd, int max_polls = 10) {
    auto pkt = makeid_build_handshake(MakeIdHandshakeState::Search);
    for (int i = 0; i < max_polls; i++) {
        if (!rfcomm_write(fd, pkt.data(), pkt.size()))
            return false;
        auto resp = read_response(fd, "ready-poll", 2000);
        if (resp.status == MakeIdResponseStatus::Success)
            return true;
        if (resp.status == MakeIdResponseStatus::Error ||
            resp.status == MakeIdResponseStatus::Exit) {
            spdlog::warn("MakeID BT: not ready (err={})", resp.error_code);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return false;
}

void MakeIdBluetoothPrinter::set_device(const std::string& mac, const std::string& device_name) {
    mac_ = mac;
    name_ = device_name;
}

std::string MakeIdBluetoothPrinter::name() const {
    return "MakeID (Bluetooth)";
}

std::vector<LabelSize> MakeIdBluetoothPrinter::supported_sizes() const {
    return makeid_default_sizes();
}

void MakeIdBluetoothPrinter::print(const LabelBitmap& bitmap, const LabelSize& size,
                                   PrintCallback callback) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        helix::ui::queue_update("MakeIdBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("Bluetooth not available"));
        });
        return;
    }
    if (mac_.empty()) {
        helix::ui::queue_update("MakeIdBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("Bluetooth device not configured"));
        });
        return;
    }

    // E1 head is 96px (12 bytes) regardless of tape width
    static constexpr int kMakeidE1HeadBytes = 12;
    int printer_width_bytes = kMakeidE1HeadBytes;

    MakeIdPrintJobConfig config;
    config.printer_width_bytes = printer_width_bytes;
    config.max_rows_per_chunk = 170;

    auto job = makeid_build_print_job(bitmap, size, config);
    spdlog::warn("MakeID BT: {} chunks to {} via RFCOMM", job.chunks.size(),
                 helix::redact::mac(mac_));

    std::string mac = mac_;
    // Wrap thread spawn in try/catch — pthread_create EAGAIN on resource-constrained
    // ARM (AD5M/CC1) throws std::system_error which aborts with std::terminate
    // if it escapes an LVGL event frame (#724, #837, [L083]).
    try {
        helix::make_thread([mac, job = std::move(job), callback]() {
            std::lock_guard<std::mutex> lock(s_print_mutex);
            auto& loader = helix::bluetooth::BluetoothLoader::instance();
            bool success = false;
            std::string error;

            int fd = ensure_connected(loader, mac);
            if (fd < 0) {
                error = lv_tr("RFCOMM connect failed");
            } else if (!wait_for_ready(fd)) {
                error = lv_tr("Printer not ready");
            } else {
                bool ok = true;
                // Send ALL chunks rapidly — no delay. The printer buffers all
                // data before physically printing (confirmed via snoop of the
                // MakeID-Life app). Inter-chunk delays cause gaps.
                for (size_t i = 0; i < job.chunks.size(); i++) {
                    const auto& frame = job.chunks[i];
                    spdlog::debug("MakeID BT: chunk[{}] ({} bytes)", i, frame.size());

                    if (!rfcomm_write(fd, frame.data(), frame.size())) {
                        error = fmt::format(lv_tr("Write failed at chunk {}"), i);
                        ok = false;
                        cleanup_connection();
                        break;
                    }

                    // Read ACK for this chunk
                    auto resp = read_response(fd, fmt::format("chunk[{}]", i).c_str(), 5000);
                    if (resp.status == MakeIdResponseStatus::Resend) {
                        // Resend once
                        if (!rfcomm_write(fd, frame.data(), frame.size())) {
                            error = fmt::format(lv_tr("Resend write failed at chunk {}"), i);
                            ok = false;
                            cleanup_connection();
                            break;
                        }
                        resp = read_response(fd, fmt::format("chunk[{}] resend", i).c_str(), 5000);
                    }
                    if (resp.status == MakeIdResponseStatus::Error) {
                        error =
                            fmt::format(lv_tr("Error at chunk {} (code={})"), i, resp.error_code);
                        ok = false;
                        break;
                    }
                }

                // After last chunk: poll with handshake until printer finishes
                // (snoop shows app polls ~20-30 times during physical printing)
                if (ok) {
                    constexpr int max_print_polls = 60;
                    bool print_finished = false;
                    auto poll_pkt = makeid_build_handshake(MakeIdHandshakeState::Search);
                    for (int poll = 0; poll < max_print_polls; poll++) {
                        rfcomm_write(fd, poll_pkt.data(), poll_pkt.size());
                        auto resp = read_response(fd, "print-wait", 2000);
                        if (resp.status == MakeIdResponseStatus::Success && !resp.is_printing) {
                            spdlog::info("MakeID BT: print finished after {} polls", poll);
                            print_finished = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                    if (!print_finished) {
                        spdlog::error("MakeID BT: print-wait timed out after {} polls",
                                      max_print_polls);
                        ok = false;
                        error = lv_tr("Print timed out waiting for completion");
                    }
                }
                if (ok) {
                    auto cancel = makeid_build_handshake(MakeIdHandshakeState::Cancel);
                    rfcomm_write(fd, cancel.data(), cancel.size());
                    read_response(fd, "cancel", 2000);
                    success = true;
                    spdlog::warn("MakeID BT: print complete");
                }
            }

            if (!error.empty())
                spdlog::error("MakeID BT: {}", error);
            helix::ui::queue_update("MakeIdBluetoothPrinter::print", [callback, success, error]() {
                if (callback)
                    callback(success, error);
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("MakeID BT: failed to spawn print thread: {}", e.what());
        helix::ui::queue_update("MakeIdBluetoothPrinter::print", [callback]() {
            if (callback)
                callback(false, lv_tr("System busy — please try again"));
        });
    }
}

} // namespace helix::label

#endif // HELIX_HAS_LABEL_PRINTER
