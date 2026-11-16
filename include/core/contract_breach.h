// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file contract_breach.h
 * @brief One switch that turns a broken XML or threading contract into an abort.
 *
 * Any layer may report a breach; the UI's required-widget lookups are the
 * main reporters, and AmsState reports reads made off the main thread.
 */

namespace helix::ui {

/**
 * @brief Make a breached UI contract fatal
 *
 * On in unit tests and under --test: a breach prints to stderr and aborts, so
 * the run fails where the XML and the C++ disagree. Off (the default) it only
 * logs. Release builds never abort.
 */
void set_strict_ui_checks(bool enabled) noexcept;

/// Whether set_strict_ui_checks() turned breaches fatal.
[[nodiscard]] bool strict_ui_checks() noexcept;

/// Log @p message at error, then abort when strict UI checks are on.
void report_ui_contract_breach(const char* message);

} // namespace helix::ui
