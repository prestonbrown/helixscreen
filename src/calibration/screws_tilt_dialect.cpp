// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screws_tilt_dialect.h"

#include "printer_discovery.h"
#include "snapmaker_screws_tilt.h"

namespace helix::screws_tilt {

void reconcile_on_connect(IMoonrakerClient& client, const PrinterDiscovery& hw,
                          const nlohmann::json& initial_status) {
    if (hw.screws_tilt_dialect() == ScrewsTiltDialect::SnapmakerAuto) {
        snapmaker::screws_tilt::reconcile_on_connect(client, initial_status);
    }
}

void request_exit(IMoonrakerClient& client, const PrinterDiscovery& hw) {
    if (hw.screws_tilt_dialect() == ScrewsTiltDialect::SnapmakerAuto) {
        snapmaker::screws_tilt::request_exit(client);
    }
}

bool start_dialect_sequence(IMoonrakerClient& client, IMoonrakerAPI& api,
                            const PrinterDiscovery& hw, ScrewTiltCallback on_success,
                            helix::SharedCallback<void(const MoonrakerError&)> on_error) {
    if (hw.screws_tilt_dialect() == ScrewsTiltDialect::SnapmakerAuto) {
        snapmaker::screws_tilt::start_sequence(client, api, std::move(on_success),
                                               std::move(on_error));
        return true;
    }
    return false;
}

const std::vector<const char*>& config_section_names() {
    static const std::vector<const char*> names{"screws_tilt_adjust",
                                                snapmaker::screws_tilt::kModuleName};
    return names;
}

} // namespace helix::screws_tilt
