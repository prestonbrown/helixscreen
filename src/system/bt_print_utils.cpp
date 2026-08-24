// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "bt_print_utils.h"

#include "bluetooth_loader.h"
#include "label_printer_settings.h"
#include "log_redact.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace helix::bluetooth {

// Single mutex for ALL Bluetooth RFCOMM prints — only one connection at a time
static std::mutex s_rfcomm_mutex;

RfcommSendResult rfcomm_send(const std::string& mac, int fallback_channel,
                             const std::vector<uint8_t>& data, const std::string& log_tag) {
    std::lock_guard<std::mutex> lock(s_rfcomm_mutex);

    RfcommSendResult result;
    auto& loader = BluetoothLoader::instance();

    for (int attempt = 0; attempt < 2; ++attempt) {
        int cached_before = helix::LabelPrinterSettingsManager::instance().get_bt_channel();
        int channel = helix::label::resolve_label_printer_channel(mac, fallback_channel);
        if (channel <= 0) {
            result.error = "failed to resolve RFCOMM channel";
            spdlog::error("{}: {}", log_tag, result.error);
            return result;
        }

        spdlog::info("[{}] RFCOMM connect to {} ch{} (attempt {})", log_tag,
                     helix::redact::mac(mac), channel, attempt + 1);

        auto* ctx = loader.get_or_create_context();
        if (!ctx) {
            result.error = "Failed to initialize Bluetooth context";
            spdlog::error("{}: {}", log_tag, result.error);
            return result;
        }

        int fd = loader.connect_rfcomm(ctx, mac.c_str(), channel);
        if (fd < 0) {
            const char* err = loader.last_error ? loader.last_error(ctx) : "unknown error";
            bool channel_was_cached = (cached_before == channel && cached_before > 0);
            if (channel_was_cached && attempt == 0) {
                spdlog::warn("[{}] Cached channel {} failed ({}); invalidating and retrying",
                             log_tag, channel, err);
                helix::LabelPrinterSettingsManager::instance().set_bt_channel(0);
                continue;
            }
            result.error = fmt::format("RFCOMM connect failed: {}", err);
            spdlog::error("{}: {}", log_tag, result.error);
            return result;
        }

        size_t total_sent = 0;
        while (total_sent < data.size()) {
            ssize_t sent = ::write(fd, data.data() + total_sent, data.size() - total_sent);
            if (sent < 0) {
                result.error = fmt::format("Write failed: {}", strerror(errno));
                spdlog::error("{}: {}", log_tag, result.error);
                break;
            }
            total_sent += static_cast<size_t>(sent);
        }

        if (total_sent == data.size()) {
            result.success = true;
            spdlog::info("{}: sent {} bytes via RFCOMM, draining...", log_tag, total_sent);
            // RFCOMM write() returns when data is copied to the kernel buffer,
            // not when it's delivered over the air. Wait for transmission.
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        loader.disconnect(ctx, fd);
        return result;
    }

    result.error = "retry exhausted";
    return result;
}

RfcommSendResult rfcomm_send_receive(const std::string& mac, int fallback_channel,
                                     const std::vector<uint8_t>& data, size_t response_len,
                                     const std::string& log_tag) {
    std::lock_guard<std::mutex> lock(s_rfcomm_mutex);

    RfcommSendResult result;
    auto& loader = BluetoothLoader::instance();

    for (int attempt = 0; attempt < 2; ++attempt) {
        int cached_before = helix::LabelPrinterSettingsManager::instance().get_bt_channel();
        int channel = helix::label::resolve_label_printer_channel(mac, fallback_channel);
        if (channel <= 0) {
            result.error = "failed to resolve RFCOMM channel";
            spdlog::error("{}: {}", log_tag, result.error);
            return result;
        }

        spdlog::info("[{}] RFCOMM connect to {} ch{} (attempt {})", log_tag,
                     helix::redact::mac(mac), channel, attempt + 1);

        auto* ctx = loader.get_or_create_context();
        if (!ctx) {
            result.error = "Failed to initialize Bluetooth context";
            spdlog::error("{}: {}", log_tag, result.error);
            return result;
        }

        int fd = loader.connect_rfcomm(ctx, mac.c_str(), channel);
        if (fd < 0) {
            const char* err = loader.last_error ? loader.last_error(ctx) : "unknown error";
            bool channel_was_cached = (cached_before == channel && cached_before > 0);
            if (channel_was_cached && attempt == 0) {
                spdlog::warn("[{}] Cached channel {} failed ({}); invalidating and retrying",
                             log_tag, channel, err);
                helix::LabelPrinterSettingsManager::instance().set_bt_channel(0);
                continue;
            }
            result.error = fmt::format("RFCOMM connect failed: {}", err);
            spdlog::error("{}: {}", log_tag, result.error);
            return result;
        }

        // Send request
        size_t total_sent = 0;
        bool write_failed = false;
        while (total_sent < data.size()) {
            ssize_t sent = ::write(fd, data.data() + total_sent, data.size() - total_sent);
            if (sent < 0) {
                result.error = fmt::format("Write failed: {}", strerror(errno));
                spdlog::error("{}: {}", log_tag, result.error);
                write_failed = true;
                break;
            }
            total_sent += static_cast<size_t>(sent);
        }
        if (write_failed) {
            loader.disconnect(ctx, fd);
            return result;
        }

        // Wait for printer to process the command before reading
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Read response with poll + timeout
        result.response.resize(response_len);
        size_t total_read = 0;

        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;

        // Poll for up to 3 seconds total
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (total_read < response_len) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                break;
            int ready = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
            if (ready <= 0)
                break;
            ssize_t n = ::read(fd, result.response.data() + total_read, response_len - total_read);
            if (n <= 0)
                break;
            total_read += static_cast<size_t>(n);
        }

        result.response.resize(total_read);
        result.success = (total_read >= response_len);

        spdlog::debug("{}: sent {} bytes, received {} bytes", log_tag, total_sent, total_read);

        loader.disconnect(ctx, fd);
        return result;
    }

    result.error = "retry exhausted";
    return result;
}

} // namespace helix::bluetooth

namespace helix::label {

namespace {
constexpr int MIN_CHANNEL = 1;
constexpr int MAX_CHANNEL = 30;
} // namespace

int resolve_label_printer_channel(const std::string& mac, int fallback_channel) {
    auto& settings = helix::LabelPrinterSettingsManager::instance();
    int cached = settings.get_bt_channel();
    if (cached >= MIN_CHANNEL && cached <= MAX_CHANNEL) {
        spdlog::debug("[BT] Using cached RFCOMM channel {} for {}", cached,
                      helix::redact::mac(mac));
        return cached;
    }

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.sdp_find_rfcomm_channel) {
        spdlog::warn("[BT] SDP symbol missing; using fallback channel {} for {}", fallback_channel,
                     helix::redact::mac(mac));
        return (fallback_channel >= MIN_CHANNEL && fallback_channel <= MAX_CHANNEL)
                   ? fallback_channel
                   : -1;
    }

    auto* ctx = loader.get_or_create_context();
    int channel = -1;
    int r = loader.sdp_find_rfcomm_channel(ctx, mac.c_str(), SPP_UUID16, &channel);
    if (r == 0 && channel >= MIN_CHANNEL && channel <= MAX_CHANNEL) {
        spdlog::info("[BT] SDP resolved RFCOMM channel {} for {} (caching)", channel,
                     helix::redact::mac(mac));
        settings.set_bt_channel(channel);
        return channel;
    }

    spdlog::warn("[BT] SDP lookup failed for {} (r={}, ch={}); falling back to {}",
                 helix::redact::mac(mac), r, channel, fallback_channel);
    if (fallback_channel >= MIN_CHANNEL && fallback_channel <= MAX_CHANNEL) {
        return fallback_channel; // do NOT cache fallback
    }
    return -1;
}

} // namespace helix::label

#endif // HELIX_HAS_LABEL_PRINTER
