// SPDX-License-Identifier: GPL-3.0-or-later
// AUDIT-ONLY stand-in for libhv's hv/Event.h — sweep pass 2 ("seam carved").
//
// include/moonraker_client.h needs exactly one type from this header
// (hv::TimerID, a uint64_t in real libhv). This stub exists so the sweep can
// measure what blocks the ~150 files whose ONLY failure is the transitive
// moonraker_client.h → libhv include; it is never linked and never shipped.
#pragma once
#include <cstdint>

namespace hv {
typedef uint64_t TimerID;
}
