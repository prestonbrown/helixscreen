// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// These headers come first in the TU on purpose: the ESP32 build parses the
// panel headers that hold a LiveSweepPreview, so none of them may pull in
// libhv's event loop.
#include "ui_panel_belt_tension.h"
#include "ui_panel_input_shaper.h"
#include "ui_wizard_input_shaper.h"

#include "live_sweep_preview.h"

#ifdef HV_EVENT_LOOP_THREAD_HPP_
#error "a header the ESP32 build parses includes hv/EventLoopThread.h"
#endif
