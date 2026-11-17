// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "i_moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_error.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/// Answers the three calls PrinterRecoveryService makes from scripted values,
/// on the calling thread, and counts them.
class ScriptedRecoveryApi : public MoonrakerAPIMock {
  public:
    using MoonrakerAPIMock::MoonrakerAPIMock;

    void restart_firmware(SuccessCallback on_success, ErrorCallback on_error) override {
        ++firmware_restarts;
        if (hold_firmware_restart) {
            held_firmware_ok = std::move(on_success);
        } else if (firmware_error) {
            on_error(*firmware_error);
        } else {
            on_success();
        }
    }

    void restart_service(const std::string& /*service_name*/, SuccessCallback on_success,
                         ErrorCallback on_error) override {
        ++service_restarts;
        if (service_error) {
            on_error(*service_error);
        } else {
            on_success();
        }
    }

    void get_klippy_status(KlippyStatusCallback on_result, ErrorCallback on_error) override {
        const size_t i = static_cast<size_t>(*status_queries);
        ++*status_queries;
        if (i < status_errors) {
            on_error(MoonrakerError::connection_lost("printer.info"));
            return;
        }
        on_result(statuses[std::min(i - status_errors, statuses.size() - 1)]);
    }

    /// printer.info answers in order, after the first status_errors fail; the last repeats.
    std::vector<KlippyStatus> statuses{{"startup", ""}};
    size_t status_errors = 0;
    std::optional<MoonrakerError> firmware_error;
    std::optional<MoonrakerError> service_error;
    /// Keep the firmware_restart acceptance in held_firmware_ok instead of answering.
    bool hold_firmware_restart = false;
    SuccessCallback held_firmware_ok;
    int firmware_restarts = 0;
    int service_restarts = 0;
    /// Shared so a test can still read it after the API is destroyed.
    std::shared_ptr<int> status_queries = std::make_shared<int>(0);
};

} // namespace helix
