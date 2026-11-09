// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The setters that give the mock backend a particular unit's identity.

#include "ams_backend_mock.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <string>

namespace helix {

void AmsBackendMock::set_afc_unit_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!system_info_.units.empty())
        system_info_.units[0].name = name;
}

void AmsBackendMock::set_unit_model(const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.units.empty())
        return;
    system_info_.units[0].model = model;
    system_info_.units[0].topology = PathTopology::LINEAR;
}

void AmsBackendMock::rename_unit(int unit_index, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_index >= 0 && unit_index < static_cast<int>(system_info_.units.size()))
        system_info_.units[static_cast<size_t>(unit_index)].name = name;
}

void AmsBackendMock::set_ace_identity() {
    std::lock_guard<std::mutex> lock(mutex_);
    system_info_.type = AmsType::ACE;
    system_info_.type_name = "ACE";
}

void AmsBackendMock::set_endless_spool_supported(bool supported) {
    std::lock_guard<std::mutex> lock(mutex_);
    endless_spool_supported_ = supported;
    // Update system_info to reflect the new capability
    system_info_.endless_spool_enabled = supported;
    spdlog::debug("[AmsBackendMock] Endless spool supported set to {}", supported);
}

void AmsBackendMock::set_endless_spool_editable(bool editable) {
    std::lock_guard<std::mutex> lock(mutex_);
    endless_spool_editable_ = editable;
    spdlog::debug("[AmsBackendMock] Endless spool editable set to {}", editable);
}

} // namespace helix
