// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parse-only stand-in for libhv's hv/Event.h. include/moonraker_client.h uses
// exactly one type from it (hv::TimerID, a uint64_t in real libhv). ~21 app
// TUs transitively include moonraker_client.h — the concrete libhv-backed
// helix::MoonrakerClient — even though on ESP32 the app never instantiates it
// (MoonrakerManager's ESP arm builds helixnet's EspMoonrakerClient via the
// create_platform_moonraker_client() factory). This header lets those TUs
// PARSE; it is never a real transport. No implementation ships: nothing on the
// ESP32 path constructs helix::MoonrakerClient, so no hv:: symbols are linked.
#pragma once
#include <cstdint>

namespace hv {
typedef uint64_t TimerID;
}
