// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::ui {

/// `<heater_response_sketch method="pid|mpc"/>`: a heater's temperature after a
/// part-fan change, drawn against its target line. PID swings and settles; MPC
/// barely moves. Illustrative, not measured.
void register_heater_response_sketch_widget();

} // namespace helix::ui
