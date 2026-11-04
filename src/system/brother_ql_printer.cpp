// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "brother_ql_printer.h"

#include "brother_ql_protocol.h"
#include "core/update_queue.h"
#include "http_executor.h"
#include "safe_resolve.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace helix {

// Brother QL raster row width: 90 bytes = 720 pixels for QL-800/810W/820NWB
static constexpr int kRasterRowBytes = 90;

struct BrotherQLPrinter::Impl {};

BrotherQLPrinter::BrotherQLPrinter() : impl_(std::make_unique<Impl>()) {}

BrotherQLPrinter::BrotherQLPrinter(std::string host, int port)
    : impl_(std::make_unique<Impl>()), host_(std::move(host)), port_(port) {}

BrotherQLPrinter::~BrotherQLPrinter() = default;

std::string BrotherQLPrinter::name() const {
    return "Brother QL";
}

void BrotherQLPrinter::print(const LabelBitmap& bitmap, const LabelSize& size,
                             PrintCallback callback) {
    print_label(host_, port_, bitmap, size, std::move(callback));
}

std::vector<LabelSize> BrotherQLPrinter::supported_sizes() const {
    return supported_sizes_static();
}

std::vector<LabelSize> BrotherQLPrinter::supported_sizes_static() {
    return {
        {"29mm", 306, 0, 300, 0x0A, 29, 0},          {"38mm", 413, 0, 300, 0x0A, 38, 0},
        {"62mm", 696, 0, 300, 0x0A, 62, 0},          {"23x23mm", 202, 202, 300, 0x0B, 23, 23},
        {"29x90mm", 306, 991, 300, 0x0B, 29, 90},    {"62x29mm", 696, 271, 300, 0x0B, 62, 29},
        {"62x100mm", 696, 1164, 300, 0x0B, 62, 100},
    };
}

std::vector<uint8_t> BrotherQLPrinter::build_raster_commands(const LabelBitmap& bitmap,
                                                             const LabelSize& size) {
    return helix::label::brother_ql_build_raster(bitmap, size);
}

void BrotherQLPrinter::print_label(const std::string& host, int port, const LabelBitmap& bitmap,
                                   const LabelSize& size, PrintCallback callback) {
    auto commands = build_raster_commands(bitmap, size);
    spdlog::warn("Brother QL: sending {} bytes to {}:{}", commands.size(), host, port);

    // Route TCP send through HttpExecutor::fast() — raw detached std::thread
    // spawned per print was the second half of the nested-thread chain that
    // crashed the Test Print flow on CC1 (the outer thread lives in
    // label_printer_utils.cpp). See #724 for ARM thread exhaustion context.
    helix::http::HttpExecutor::fast().submit([host, port, commands = std::move(commands),
                                              callback]() {
        bool success = false;
        std::string error;

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            error = fmt::format("socket() failed: {}", strerror(errno));
            spdlog::error("Brother QL: {}", error);
        } else {
            // Set send timeout
            struct timeval tv {};
            tv.tv_sec = 10;
            setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            // Resolve hostname (safe on ARM — avoids glibc __check_pf crash)
            struct sockaddr_in addr {};
            int resolve_err = helix::safe_resolve(host, port, addr);
            if (resolve_err != 0) {
                error = fmt::format("DNS resolve failed for {}", host);
                spdlog::error("Brother QL: {}", error);
            } else {
                // Connect
                if (connect(sockfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
                    error = fmt::format("connect to {}:{} failed: {}", host, port, strerror(errno));
                    spdlog::error("Brother QL: {}", error);
                } else {
                    // Send all data
                    size_t total_sent = 0;
                    while (total_sent < commands.size()) {
                        ssize_t sent = send(sockfd, commands.data() + total_sent,
                                            commands.size() - total_sent, MSG_NOSIGNAL);
                        if (sent < 0) {
                            error = fmt::format("send failed: {}", strerror(errno));
                            spdlog::error("Brother QL: {}", error);
                            break;
                        }
                        total_sent += static_cast<size_t>(sent);
                    }
                    if (total_sent == commands.size()) {
                        success = true;
                        spdlog::warn("Brother QL: sent {} bytes successfully", total_sent);
                    }
                }
            }
            close(sockfd);
        }

        // Dispatch callback to UI thread
        helix::ui::queue_update("BrotherQLPrinter::print_label",
                                [callback, success, error]() { callback(success, error); });
    });
}

} // namespace helix

#endif // HELIX_HAS_LABEL_PRINTER
