// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ethernet_backend.h"

#include <functional>
#include <string>
#include <vector>

/**
 * @brief Ethernet backend that asks the printer's network daemon
 *
 * helix::netd (netd_protocol.h) owns the wire format and the one-shot query;
 * this class maps a daemon snapshot onto EthernetInfo. Query-only and
 * synchronous: no events, no threads, no sockets of its own — every
 * get_info() opens one connection, sends one GET, and closes.
 *
 * Kernel state is the fallback half: adapter identity (interface/MAC) comes
 * from the kernel reader even when the daemon answers (its connected-first
 * preference is the right pick on multi-adapter boxes), and when the daemon
 * is unreachable the kernel reading IS the answer — a daemon that died
 * mid-session must not blank a row whose kernel address is still live.
 *
 * has_interface() is deliberately daemon-free: it scans sysfs directly (the
 * shared ethernet:: classification), so the Ethernet row's visibility never
 * flaps when the daemon restarts, and a downed eth0 (which still exists in
 * sysfs) still counts as present.
 *
 * Both calls are safe to invoke from any thread except the LVGL thread:
 * get_info() blocks for up to ~1 s worst case (2x the 500 ms per-read
 * timeout — a healthy daemon answers in milliseconds) and runs on the
 * shared HttpExecutor fast lane, so that bound is worker-pinning time.
 * EthernetManager already runs it on a worker thread.
 */
class EthernetBackendNetd : public EthernetBackend {
  public:
    /**
     * @param sysfs_root   Sysfs mount to read <root>/class/net/ under.
     *                     Injectable so tests can point at a fake tree;
     *                     production uses the default "/sys".
     * @param kernel_state Kernel-state reader used for adapter identity and
     *                     as the whole answer when the daemon is unreachable.
     *                     Injectable so tests are host-independent; the
     *                     default reads the real kernel through
     *                     EthernetBackendLinux.
     */
    explicit EthernetBackendNetd(const std::string& sysfs_root = "/sys",
                                 std::function<EthernetInfo()> kernel_state = nullptr);
    ~EthernetBackendNetd() override;

    // ========================================================================
    // EthernetBackend Interface Implementation
    // ========================================================================

    bool has_interface() override;
    EthernetInfo get_info() override;

  private:
    std::string sysfs_root_;
    std::function<EthernetInfo()> kernel_state_;
};
