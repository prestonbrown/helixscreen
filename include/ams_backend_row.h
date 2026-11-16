// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_type.h"
#include "json_fwd.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace helix {

class AmsBackend;
class IMoonrakerAPI;
class IMoonrakerClient;
class PrinterDiscovery;

/// The spellings of one type's name that ams_type_from_string() accepts.
class AmsNameAliases {
  public:
    constexpr AmsNameAliases() = default;
    template <std::size_t N>
    constexpr AmsNameAliases(const std::string_view (&names)[N]) : first_(names), count_(N) {}

    [[nodiscard]] constexpr const std::string_view* begin() const {
        return first_;
    }
    [[nodiscard]] constexpr const std::string_view* end() const {
        return first_ + count_;
    }

  private:
    const std::string_view* first_ = nullptr;
    std::size_t count_ = 0;
};

/**
 * @brief What the factory and generic code need to know about one filament backend.
 *
 * One row per AmsType, in src/printer/ams_backend.cpp: a new backend is one row
 * plus its entry in kAmsTypeTraits. A backend this build leaves out through its
 * HELIX_HAS_* gate keeps its row with `construct`,
 * `owns_filament_sensor` and `required_status_objects` null, so the type's
 * classification and names still answer.
 *
 * Not rows: discovery's cross-vendor yield rules and precedence, which compare
 * two candidates; each vendor's object name collection; the mock persona ladder.
 */
struct AmsBackendRow {
    using Construct = std::unique_ptr<AmsBackend> (*)(IMoonrakerAPI*, IMoonrakerClient*,
                                                      const PrinterDiscovery*);
    using OwnsFilamentSensor = bool (*)(const std::string& bare_name, const PrinterDiscovery&);
    using RequiredStatusObjects = nlohmann::json (*)(const PrinterDiscovery&);

    AmsType type;
    Construct construct;
    /// Polls raw HTTP instead of riding the WebSocket, so a screen without that
    /// transport cannot host it.
    bool needs_http_poll;
    /// Null for a backend with no named, keyword-free filament sensors.
    OwnsFilamentSensor owns_filament_sensor;
    /// Null for a backend that subscribes to nothing of its own at this level.
    RequiredStatusObjects required_status_objects;
    bool tool_changer;
    bool filament_system;
    AmsNameAliases name_aliases;
    /// The entry in the telemetry `features` list; nullptr for none. A published
    /// schema: never rename one.
    const char* telemetry_feature;
};

/// The row for @p type; the NONE row for a value with none.
[[nodiscard]] const AmsBackendRow& ams_backend_row(AmsType type);

} // namespace helix
