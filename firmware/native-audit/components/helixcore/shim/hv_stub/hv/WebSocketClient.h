// SPDX-License-Identifier: GPL-3.0-or-later
// AUDIT-ONLY stand-in for libhv's hv/WebSocketClient.h — sweep pass 2.
//
// MoonrakerClient's header surface uses only: the base class name, the
// onopen/onmessage/onclose callback members (assigned in the .cpp, which is
// outside the sweep), hv::EventLoopPtr as a ctor parameter type, and
// hv::TimerID. No inline method in moonraker_client.h calls into the base, so
// a skeletal declaration is sufficient for compile-only categorization.
// Declared-but-undefined members are fine: the sweep compiles with -c, never
// links. Signatures mirror real libhv (onmessage takes const std::string&).
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "Event.h"

namespace hv {

class EventLoop;
using EventLoopPtr = std::shared_ptr<EventLoop>;

struct reconn_setting_t;

class WebSocketClient {
  public:
    WebSocketClient(EventLoopPtr loop = nullptr);
    virtual ~WebSocketClient();

    // Part of libhv's public surface that leaks through MoonrakerClient by
    // inheritance: two UI files call setReconnect() directly (found in pass 2).
    void setReconnect(reconn_setting_t* setting);

    std::function<void()> onopen;
    std::function<void()> onclose;
    std::function<void(const std::string&)> onmessage;
};

} // namespace hv
