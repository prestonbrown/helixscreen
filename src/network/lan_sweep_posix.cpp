// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The LAN sweep's platform pieces on Linux and macOS (lan_sweep.h).

#include "core/http_executor.h"
#include "lan_sweep.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace helix {

std::unique_ptr<IMdnsDiscovery> make_moonraker_discovery() {
    return std::make_unique<MoonrakerDiscovery>(std::make_unique<MdnsDiscovery>(),
                                                std::make_unique<LanSweepDiscovery>());
}

LanSweepLimits lan_sweep_default_limits() {
    return LanSweepLimits{};
}

namespace {

/// A sweep's own pool. Stopping it lets a worker that is mid-probe run on by itself
/// instead of holding the UI thread for the probe's timeout.
struct SweepPool {
    explicit SweepPool(size_t workers) : pool("lan-sweep", workers) {}
    ~SweepPool() {
        pool.stop(std::chrono::milliseconds(20));
    }
    http::HttpExecutor pool;
};

} // namespace

LanSweepWorkers start_lan_sweep_workers(size_t count, const LanSweepLimits& /*limits*/,
                                        std::function<void()> body) {
    auto pool = std::make_shared<SweepPool>(count);
    try {
        pool->pool.start();
    } catch (const std::system_error& e) {
        // Out of threads. A pool that started only some workers would still run every
        // body in turn, but which ones started is not known, so none are counted and the
        // sweep ends at once rather than wait on bodies that may never run.
        spdlog::warn("[LanSweep] Could not start the sweep's workers: {}", e.what());
        return LanSweepWorkers{nullptr, 0};
    }
    for (size_t i = 0; i < count; ++i) {
        pool->pool.submit(body);
    }
    return LanSweepWorkers{pool, count};
}

std::optional<LocalIpv4> active_local_ipv4() {
    // A connected UDP socket sends nothing, but makes the kernel pick the source address
    // its route would use.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return std::nullopt;
    }
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    probe.sin_addr.s_addr = htonl(0x08080808u);
    sockaddr_in self{};
    socklen_t len = sizeof(self);
    const bool routed = ::connect(fd, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0 &&
                        ::getsockname(fd, reinterpret_cast<sockaddr*>(&self), &len) == 0;
    ::close(fd);
    if (!routed || self.sin_addr.s_addr == 0) {
        return std::nullopt;
    }

    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return std::nullopt;
    }
    std::optional<LocalIpv4> found;
    for (ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || !it->ifa_netmask || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if (addr->sin_addr.s_addr == self.sin_addr.s_addr) {
            const auto* mask = reinterpret_cast<const sockaddr_in*>(it->ifa_netmask);
            found = LocalIpv4{ntohl(addr->sin_addr.s_addr), ntohl(mask->sin_addr.s_addr)};
            break;
        }
    }
    ::freeifaddrs(list);
    return found;
}

} // namespace helix
