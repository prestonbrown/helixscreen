// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_notification.h"

#include "core/error_reporting.h"
#ifndef HELIX_SPLASH_ONLY
#include "core/update_queue.h"
#endif

#include "ams_error.h"
#include "moonraker_error.h"
#include "system/notify_tr.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <string>
#include <type_traits>

/**
 * @file ui_error_reporting.h
 * @brief What UI code includes to report an error: the notification API, the
 *        NOTIFY_* macros (core/error_reporting.h), the translated toasts
 *        (system/notify_tr.h) and the AmsError renderers (ams_error.h).
 */
