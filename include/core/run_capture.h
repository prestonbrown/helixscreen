// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix {

/// Run @p cmd through popen and return all of its stdout. @p exit_code receives
/// the command's exit status, or -1 when popen failed or the command did not
/// exit normally. Append "2>/dev/null" to @p cmd to silence stderr.
/// BLOCKING: never call it from the LVGL thread.
std::string run_capture(const std::string& cmd, int& exit_code);

} // namespace helix
