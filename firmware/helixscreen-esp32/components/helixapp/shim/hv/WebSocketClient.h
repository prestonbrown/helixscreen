// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parse-only stand-in for libhv's hv/WebSocketClient.h. See hv/Event.h in this
// dir for why it exists: the concrete helix::MoonrakerClient (in the app's
// include/moonraker_client.h) inherits hv::WebSocketClient and is transitively
// included by ~21 app TUs, but is never constructed on the ESP32 path.
//
// MoonrakerClient's header surface uses only the base class name, the
// onopen/onmessage/onclose callback members, hv::EventLoopPtr as a ctor
// parameter type, hv::TimerID, and setReconnect(). Skeletal declarations are
// enough to PARSE the header. Declared-but-undefined is intentional: no ESP32
// TU links against these — the real transport is helixnet's EspMoonrakerClient.
// If a linker error ever demands one of these symbols, that means an ESP32 code
// path is wrongly constructing the libhv client; fix the call site, don't
// define the symbol here.
#pragma once
#include "Event.h"

#include <functional>
#include <memory>
#include <string>

namespace hv {

class EventLoop;
using EventLoopPtr = std::shared_ptr<EventLoop>;

struct reconn_setting_t;

class WebSocketClient {
  public:
    WebSocketClient(EventLoopPtr loop = nullptr);
    virtual ~WebSocketClient();

    void setReconnect(reconn_setting_t* setting);

    std::function<void()> onopen;
    std::function<void()> onclose;
    std::function<void(const std::string&)> onmessage;
};

} // namespace hv
