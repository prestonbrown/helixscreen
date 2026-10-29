// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_result_store.h"

#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

#include <ctime>

namespace helix::pa {

SpoolLookup classify_spool(const std::optional<SpoolInfo>& spool) {
    return spool && spool->filament_id > 0 ? SpoolLookup::Found : SpoolLookup::NoSpool;
}

namespace {

void fetch_spool(IMoonrakerAPI& api, int spool_id,
                 std::function<void(SpoolLookup, const std::optional<SpoolInfo>&)> done) {
    api.spoolman().get_spoolman_spool(
        spool_id,
        [done](const std::optional<SpoolInfo>& spool) { done(classify_spool(spool), spool); },
        [done](const MoonrakerError&) { done(SpoolLookup::NoSpool, std::nullopt); });
}

} // namespace

void find_result_spool(IMoonrakerAPI& api, int tool_spool_id,
                       std::function<void(SpoolLookup, const std::optional<SpoolInfo>&)> done) {
    if (tool_spool_id > 0) {
        fetch_spool(api, tool_spool_id, std::move(done));
        return;
    }
    api.spoolman().get_spoolman_status(
        [&api, done](bool connected, int spool_id) {
            if (!connected || spool_id <= 0) {
                done(SpoolLookup::NoSpool, std::nullopt);
                return;
            }
            fetch_spool(api, spool_id, done);
        },
        [done](const MoonrakerError&) { done(SpoolLookup::Unavailable, std::nullopt); });
}

SavedK with_spool(SavedK saved, const std::optional<SpoolInfo>& spool) {
    if (spool) {
        saved.spool_id = spool->id;
        saved.filament_id = spool->filament_id;
        saved.filament_name = spool->display_name();
    }
    return saved;
}

void publish_result(IMoonrakerAPI& api, const SavedK& saved) {
    // Read-modify-write so results for other filaments and extruders survive.
    auto post = [&api, saved](const nlohmann::json& existing) {
        api.database_post_item(
            kMoonrakerNamespace, kMoonrakerKey, merge_moonraker_record(existing, saved),
            [source = saved.source]() {
                spdlog::debug("[PA] Result ({}) published to Moonraker", source);
            },
            [](const MoonrakerError& err) {
                spdlog::warn("[PA] Could not publish to Moonraker: {}", err.message);
            });
    };
    api.database_get_item(kMoonrakerNamespace, kMoonrakerKey, post,
                          [post](const MoonrakerError&) { post(nullptr); });
}

void save_to_filament(IMoonrakerAPI& api, int filament_id, float k, const std::string& field_label,
                      std::function<void()> on_saved,
                      std::function<void(const MoonrakerError&)> on_error) {
    api.spoolman().set_spoolman_filament_extra_number(filament_id, kSpoolmanPaExtraKey, field_label,
                                                      k, std::move(on_saved), std::move(on_error));
}

std::string utc_now_iso() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace helix::pa
