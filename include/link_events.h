// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>

namespace helix::net {

/**
 * @brief A non-blocking feed of "an interface's link or address changed"
 *
 * Says only that something changed, never what: the owner re-probes the
 * backend for the new state. poll() never blocks, so the UI thread may ask.
 */
class LinkEvents {
  public:
    virtual ~LinkEvents() = default;

    /// Drain whatever arrived since the last call; true when anything did.
    virtual bool poll() = 0;
};

/**
 * @brief The kernel's link and IPv4 address notifications (rtnetlink)
 *
 * Fires for a cable plugged or pulled and for an address gained or lost on
 * any interface, whichever process manages it.
 *
 * @return nullptr off Linux, or when the socket cannot be opened
 */
std::unique_ptr<LinkEvents> open_kernel_link_events();

/**
 * @brief Events counted by a shared generation number, for mock backends
 *
 * The backend bumps the counter on each change; the counter is shared so an
 * event feed may outlive the backend that handed it out.
 */
class CounterLinkEvents : public LinkEvents {
  public:
    explicit CounterLinkEvents(std::shared_ptr<std::atomic<unsigned>> generation)
        : generation_(std::move(generation)), seen_(generation_->load()) {}

    bool poll() override {
        const unsigned now = generation_->load();
        const bool changed = now != seen_;
        seen_ = now;
        return changed;
    }

  private:
    std::shared_ptr<std::atomic<unsigned>> generation_;
    unsigned seen_;
};

} // namespace helix::net
