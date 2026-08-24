// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_spoolman_api.h"

#include "json_utils.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdio>
#include <string>

using namespace helix;

// Aliases for json_utils.h helpers
using helix::json_util::safe_double;
using helix::json_util::safe_float;
using helix::json_util::safe_int;
using helix::json_util::safe_string;

// Bring the header-declared parser into this translation unit's unqualified
// scope so the in-file call sites below stay unchanged.
using helix::spoolman_detail::parse_spool_info;

// Helper to parse a Spoolman spool JSON object into SpoolInfo.
// Non-static + declared in the header so tests can exercise the real parser (#1087).
SpoolInfo helix::spoolman_detail::parse_spool_info(const nlohmann::json& spool_json) {
    SpoolInfo info;

    info.id = safe_int(spool_json, "id", 0);
    info.remaining_weight_g = safe_double(spool_json, "remaining_weight");
    info.initial_weight_g = safe_double(spool_json, "initial_weight");
    info.spool_weight_g = safe_double(spool_json, "spool_weight");
    info.price = safe_double(spool_json, "price");
    info.lot_nr = safe_string(spool_json, "lot_nr");
    info.comment = safe_string(spool_json, "comment");
    info.location = safe_string(spool_json, "location");
    info.last_used = safe_string(spool_json, "last_used");
    info.registered = safe_string(spool_json, "registered");

    // used_weight for fallback initial weight calculation
    double used_weight_g = safe_double(spool_json, "used_weight");

    // Length is in mm from Spoolman, convert to meters
    double remaining_length_mm = safe_double(spool_json, "remaining_length");
    info.remaining_length_m = remaining_length_mm / 1000.0;

    // Parse nested filament object
    if (spool_json.contains("filament") && spool_json["filament"].is_object()) {
        const auto& filament = spool_json["filament"];

        info.filament_id = safe_int(filament, "id", 0);
        info.material = safe_string(filament, "material");
        info.filament_name = safe_string(filament, "name");
        info.color_hex = safe_string(filament, "color_hex");
        info.multi_color_hexes = safe_string(filament, "multi_color_hexes");

        // Temperature settings. Spoolman serializes these optional fields as
        // present-but-null, so a raw .value() would throw type_error.302 on the
        // whole list; safe_int null-guards each read.
        info.nozzle_temp_recommended = safe_int(filament, "settings_extruder_temp", 0);
        info.bed_temp_recommended = safe_int(filament, "settings_bed_temp", 0);

        // The min/max pair is what apply_spool_to_slot() copies onto a slot's
        // nozzle range; without it every Spoolman-linked slot reads 0/0 while
        // the bed temperature is real. Same four keys parse_filament_info()
        // reads off the filament endpoint.
        info.nozzle_temp_min = safe_int(filament, "settings_extruder_temp_min", 0);
        info.nozzle_temp_max = safe_int(filament, "settings_extruder_temp_max", 0);
        info.bed_temp_min = safe_int(filament, "settings_bed_temp_min", 0);
        info.bed_temp_max = safe_int(filament, "settings_bed_temp_max", 0);

        // Fallback: use filament definition weight when spool initial_weight is null/0.
        // Spoolman's initial_weight is optional; filament.weight is the canonical
        // net weight for this filament type.
        if (info.initial_weight_g <= 0) {
            info.initial_weight_g = safe_double(filament, "weight");
        }

        // Nested vendor
        if (filament.contains("vendor") && filament["vendor"].is_object()) {
            info.vendor = safe_string(filament["vendor"], "name");
            info.vendor_id = safe_int(filament["vendor"], "id", 0);
        }
    }

    // Final fallback: compute initial weight from remaining + used
    if (info.initial_weight_g <= 0 && used_weight_g > 0) {
        info.initial_weight_g = info.remaining_weight_g + used_weight_g;
    }

    return info;
}

static VendorInfo parse_vendor_info(const nlohmann::json& vendor_json) {
    VendorInfo info;
    info.id = safe_int(vendor_json, "id", 0);
    info.name = safe_string(vendor_json, "name");
    info.url = safe_string(vendor_json, "url");
    return info;
}

static FilamentInfo parse_filament_info(const nlohmann::json& filament_json) {
    FilamentInfo info;
    info.id = safe_int(filament_json, "id", 0);
    info.material = safe_string(filament_json, "material");
    info.filament_name = safe_string(filament_json, "name");
    info.color_hex = safe_string(filament_json, "color_hex");
    info.density = safe_float(filament_json, "density", 0.0f);
    info.diameter = safe_float(filament_json, "diameter", 1.75f);
    info.weight = safe_float(filament_json, "weight", 0.0f);
    info.spool_weight = safe_float(filament_json, "spool_weight", 0.0f);
    info.nozzle_temp_min = safe_int(filament_json, "settings_extruder_temp_min", 0);
    info.nozzle_temp_max = safe_int(filament_json, "settings_extruder_temp_max", 0);
    info.bed_temp_min = safe_int(filament_json, "settings_bed_temp_min", 0);
    info.bed_temp_max = safe_int(filament_json, "settings_bed_temp_max", 0);

    // Extract vendor_id from top-level field (always present in Spoolman response)
    info.vendor_id = safe_int(filament_json, "vendor_id", 0);

    // Nested vendor object (may override vendor_id, adds vendor_name)
    if (filament_json.contains("vendor") && filament_json["vendor"].is_object()) {
        info.vendor_id = safe_int(filament_json["vendor"], "id", info.vendor_id);
        info.vendor_name = safe_string(filament_json["vendor"], "name");
    }

    return info;
}

// ============================================================================
// MoonrakerSpoolmanAPI Implementation
// ============================================================================

MoonrakerSpoolmanAPI::MoonrakerSpoolmanAPI(IMoonrakerClient& client) : client_(client) {}

void MoonrakerSpoolmanAPI::get_spoolman_status(std::function<void(bool, int)> on_success,
                                               ErrorCallback on_error, bool silent) {
    spdlog::debug("[SpoolmanAPI] get_spoolman_status()");

    client_.send_jsonrpc(
        "server.spoolman.status", json::object(),
        [on_success](json response) {
            bool connected = false;
            int active_spool_id = 0;

            if (response.contains("result")) {
                const auto& result = response["result"];
                connected = result.value("spoolman_connected", false);
                if (result.contains("spool_id") && !result["spool_id"].is_null()) {
                    active_spool_id = result["spool_id"].get<int>();
                }
            }

            spdlog::debug("[SpoolmanAPI] Spoolman status: connected={}, active_spool={}", connected,
                          active_spool_id);

            if (on_success) {
                on_success(connected, active_spool_id);
            }
        },
        on_error, 0, silent);
}

void MoonrakerSpoolmanAPI::get_spoolman_spools(SpoolListCallback on_success,
                                               ErrorCallback on_error) {
    spdlog::debug("[SpoolmanAPI] get_spoolman_spools()");

    // Use Moonraker's Spoolman proxy to GET /v1/spool
    json params;
    params["request_method"] = "GET";
    params["path"] = "/v1/spool?limit=1000";

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success](json response) {
            std::vector<SpoolInfo> spools;

            // The proxy returns the Spoolman response in "result"
            if (response.contains("result") && response["result"].is_array()) {
                for (const auto& spool_json : response["result"]) {
                    // Parse each spool independently: a single malformed entry
                    // must not abort the whole list (a null numeric field once
                    // threw type_error and left the picker stuck loading, #1087).
                    try {
                        spools.push_back(parse_spool_info(spool_json));
                    } catch (const std::exception& e) {
                        spdlog::warn("[SpoolmanAPI] Skipping unparseable spool: {}", e.what());
                    }
                }
            }

            sort_spools_by_recency(spools);

            spdlog::debug("[SpoolmanAPI] Got {} spools from Spoolman", spools.size());

            if (on_success) {
                on_success(spools);
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::get_spoolman_spool(int spool_id, SpoolCallback on_success,
                                              ErrorCallback on_error, bool silent) {
    spdlog::trace("[SpoolmanAPI] get_spoolman_spool({})", spool_id);

    // Use Moonraker's Spoolman proxy to GET /v1/spool/{id}
    json params;
    params["request_method"] = "GET";
    params["path"] = "/v1/spool/" + std::to_string(spool_id);

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, spool_id](json response) {
            if (response.contains("result") && response["result"].is_object()) {
                SpoolInfo spool = parse_spool_info(response["result"]);
                spdlog::trace("[SpoolmanAPI] Got spool {}: {} {}", spool_id, spool.vendor,
                              spool.material);
                if (on_success) {
                    on_success(spool);
                }
            } else {
                spdlog::debug("[SpoolmanAPI] Spool {} not found", spool_id);
                if (on_success) {
                    on_success(std::nullopt);
                }
            }
        },
        on_error, 0, silent);
}

void MoonrakerSpoolmanAPI::set_active_spool(int spool_id, SuccessCallback on_success,
                                            ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] set_active_spool({})", spool_id);

    // POST to server.spoolman.post_spool_id
    json params;
    params["spool_id"] = spool_id;

    client_.send_jsonrpc(
        "server.spoolman.post_spool_id", params,
        [on_success, spool_id](json /*response*/) {
            spdlog::debug("[SpoolmanAPI] Active spool set to {}", spool_id);
            if (on_success) {
                on_success();
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::get_spool_usage_history(
    int /*spool_id*/, std::function<void(const std::vector<FilamentUsageRecord>&)> /*on_success*/,
    ErrorCallback on_error) {
    spdlog::warn("[SpoolmanAPI] get_spool_usage_history() not yet implemented");
    if (on_error) {
        MoonrakerError err = MoonrakerError::unknown("Spoolman usage history not yet implemented");
        on_error(err);
    }
}

void MoonrakerSpoolmanAPI::update_spoolman_spool_weight(int spool_id, double remaining_weight_g,
                                                        SuccessCallback on_success,
                                                        ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Updating spool {} remaining weight to {:.1f}g", spool_id,
                 remaining_weight_g);

    nlohmann::json body;
    body["remaining_weight"] = remaining_weight_g;

    nlohmann::json params;
    params["request_method"] = "PATCH";
    params["path"] = "/v1/spool/" + std::to_string(spool_id);
    params["body"] = body;

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, spool_id](json /*response*/) {
            spdlog::debug("[SpoolmanAPI] Spool {} weight updated successfully", spool_id);
            if (on_success) {
                on_success();
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::update_spoolman_spool(int spool_id, const nlohmann::json& spool_data,
                                                 SuccessCallback on_success,
                                                 ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Updating spool {} with {} fields", spool_id, spool_data.size());

    nlohmann::json params;
    params["request_method"] = "PATCH";
    params["path"] = "/v1/spool/" + std::to_string(spool_id);
    params["body"] = spool_data;

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, spool_id](json /*response*/) {
            spdlog::debug("[SpoolmanAPI] Spool {} updated successfully", spool_id);
            if (on_success) {
                on_success();
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::update_spoolman_filament(int filament_id,
                                                    const nlohmann::json& filament_data,
                                                    SuccessCallback on_success,
                                                    ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Updating filament {} with {} fields", filament_id,
                 filament_data.size());

    nlohmann::json params;
    params["request_method"] = "PATCH";
    params["path"] = "/v1/filament/" + std::to_string(filament_id);
    params["body"] = filament_data;

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, filament_id](json /*response*/) {
            spdlog::debug("[SpoolmanAPI] Filament {} updated successfully", filament_id);
            if (on_success) {
                on_success();
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::update_spoolman_filament_color(int filament_id,
                                                          const std::string& color_hex,
                                                          SuccessCallback on_success,
                                                          ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Updating filament {} color to {}", filament_id, color_hex);

    nlohmann::json body;
    body["color_hex"] = color_hex;

    nlohmann::json params;
    params["request_method"] = "PATCH";
    params["path"] = "/v1/filament/" + std::to_string(filament_id);
    params["body"] = body;

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, filament_id, color_hex](json /*response*/) {
            spdlog::debug("[SpoolmanAPI] Filament {} color updated to {}", filament_id, color_hex);
            if (on_success) {
                on_success();
            }
        },
        on_error);
}

// ============================================================================
// Spoolman CRUD Operations
// ============================================================================

void MoonrakerSpoolmanAPI::get_spoolman_vendors(VendorListCallback on_success,
                                                ErrorCallback on_error) {
    spdlog::debug("[SpoolmanAPI] get_spoolman_vendors()");

    json params;
    params["request_method"] = "GET";
    params["path"] = "/v1/vendor?limit=1000";

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success](json response) {
            std::vector<VendorInfo> vendors;

            if (response.contains("result") && response["result"].is_array()) {
                for (const auto& vendor_json : response["result"]) {
                    vendors.push_back(parse_vendor_info(vendor_json));
                }
            }

            spdlog::debug("[SpoolmanAPI] Got {} vendors from Spoolman", vendors.size());

            if (on_success) {
                on_success(vendors);
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::get_spoolman_filaments(FilamentListCallback on_success,
                                                  ErrorCallback on_error) {
    spdlog::debug("[SpoolmanAPI] get_spoolman_filaments()");

    json params;
    params["request_method"] = "GET";
    params["path"] = "/v1/filament?limit=1000";

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success](json response) {
            std::vector<FilamentInfo> filaments;

            if (response.contains("result") && response["result"].is_array()) {
                for (const auto& filament_json : response["result"]) {
                    filaments.push_back(parse_filament_info(filament_json));
                }
            }

            spdlog::debug("[SpoolmanAPI] Got {} filaments from Spoolman", filaments.size());

            if (on_success) {
                on_success(filaments);
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::create_spoolman_vendor(const nlohmann::json& vendor_data,
                                                  VendorCreateCallback on_success,
                                                  ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Creating Spoolman vendor: {}",
                 vendor_data.value("name", "unknown"));

    json params;
    params["request_method"] = "POST";
    params["path"] = "/v1/vendor";
    params["body"] = vendor_data;

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, on_error](json response) {
            if (response.contains("result") && response["result"].is_object()) {
                VendorInfo vendor = parse_vendor_info(response["result"]);
                spdlog::debug("[SpoolmanAPI] Created vendor {}: {}", vendor.id, vendor.name);
                if (on_success) {
                    on_success(vendor);
                }
            } else {
                spdlog::error("[SpoolmanAPI] create_spoolman_vendor: unexpected response format");
                if (on_error) {
                    on_error(MoonrakerError::unknown("Unexpected response format",
                                                     "create_spoolman_vendor"));
                }
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::create_spoolman_filament(const nlohmann::json& filament_data,
                                                    FilamentCreateCallback on_success,
                                                    ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Creating Spoolman filament: {} {}",
                 filament_data.value("material", "?"), filament_data.value("name", "?"));

    json params;
    params["request_method"] = "POST";
    params["path"] = "/v1/filament";
    params["body"] = filament_data;

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, on_error](json response) {
            if (response.contains("result") && response["result"].is_object()) {
                FilamentInfo filament = parse_filament_info(response["result"]);
                spdlog::debug("[SpoolmanAPI] Created filament {}: {}", filament.id,
                              filament.display_name());
                if (on_success) {
                    on_success(filament);
                }
            } else {
                spdlog::error("[SpoolmanAPI] create_spoolman_filament: unexpected response format");
                if (on_error) {
                    on_error(MoonrakerError::unknown("Unexpected response format",
                                                     "create_spoolman_filament"));
                }
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::create_spoolman_spool(const nlohmann::json& spool_data,
                                                 SpoolCreateCallback on_success,
                                                 ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Creating Spoolman spool");

    json params;
    params["request_method"] = "POST";
    params["path"] = "/v1/spool";
    params["body"] = spool_data;

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, on_error](json response) {
            if (response.contains("result") && response["result"].is_object()) {
                SpoolInfo spool = parse_spool_info(response["result"]);
                spdlog::debug("[SpoolmanAPI] Created spool {}: {}", spool.id, spool.display_name());
                if (on_success) {
                    on_success(spool);
                }
            } else {
                spdlog::error("[SpoolmanAPI] create_spoolman_spool: unexpected response format");
                if (on_error) {
                    on_error(MoonrakerError::unknown("Unexpected response format",
                                                     "create_spoolman_spool"));
                }
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::delete_spoolman_spool(int spool_id, SuccessCallback on_success,
                                                 ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Deleting Spoolman spool {}", spool_id);

    json params;
    params["request_method"] = "DELETE";
    params["path"] = "/v1/spool/" + std::to_string(spool_id);

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, spool_id](json /*response*/) {
            spdlog::debug("[SpoolmanAPI] Spool {} deleted successfully", spool_id);
            if (on_success) {
                on_success();
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::get_spoolman_external_vendors(VendorListCallback on_success,
                                                         ErrorCallback on_error) {
    spdlog::debug("[SpoolmanAPI] get_spoolman_external_vendors()");

    json params;
    params["request_method"] = "GET";
    params["path"] = "/v1/external/vendor";

    // Silent: /v1/external/ endpoints require SpoolmanDB integration which
    // is not available on all Spoolman versions (e.g. v0.22.x)
    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success](json response) {
            std::vector<VendorInfo> vendors;

            if (response.contains("result") && response["result"].is_array()) {
                for (const auto& vendor_json : response["result"]) {
                    vendors.push_back(parse_vendor_info(vendor_json));
                }
            }

            spdlog::debug("[SpoolmanAPI] Got {} external vendors from SpoolmanDB", vendors.size());

            if (on_success) {
                on_success(vendors);
            }
        },
        on_error, 0, /*silent=*/true);
}

void MoonrakerSpoolmanAPI::get_spoolman_external_filaments(const std::string& vendor_name,
                                                           FilamentListCallback on_success,
                                                           ErrorCallback on_error) {
    spdlog::debug("[SpoolmanAPI] get_spoolman_external_filaments(vendor={})", vendor_name);

    // URL-encode the vendor name
    std::string encoded;
    for (char c : vendor_name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += "%20";
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            encoded += buf;
        }
    }

    json params;
    params["request_method"] = "GET";
    params["path"] = "/v1/external/filament?vendor_name=" + encoded;

    // Silent: /v1/external/ endpoints require SpoolmanDB integration which
    // is not available on all Spoolman versions (e.g. v0.22.x)
    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, vendor_name](json response) {
            std::vector<FilamentInfo> filaments;

            if (response.contains("result") && response["result"].is_array()) {
                for (const auto& filament_json : response["result"]) {
                    filaments.push_back(parse_filament_info(filament_json));
                }
            }

            spdlog::debug("[SpoolmanAPI] Got {} external filaments for vendor '{}'",
                          filaments.size(), vendor_name);

            if (on_success) {
                on_success(filaments);
            }
        },
        on_error, 0, /*silent=*/true);
}

void MoonrakerSpoolmanAPI::get_spoolman_filaments(int vendor_id, FilamentListCallback on_success,
                                                  ErrorCallback on_error) {
    spdlog::debug("[SpoolmanAPI] get_spoolman_filaments(vendor_id={})", vendor_id);

    json params;
    params["request_method"] = "GET";
    params["path"] = "/v1/filament?vendor.id=" + std::to_string(vendor_id);

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, vendor_id](json response) {
            std::vector<FilamentInfo> filaments;

            if (response.contains("result") && response["result"].is_array()) {
                for (const auto& filament_json : response["result"]) {
                    filaments.push_back(parse_filament_info(filament_json));
                }
            }

            spdlog::debug("[SpoolmanAPI] Got {} filaments for vendor_id {}", filaments.size(),
                          vendor_id);

            if (on_success) {
                on_success(filaments);
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::delete_spoolman_vendor(int vendor_id, SuccessCallback on_success,
                                                  ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Deleting Spoolman vendor {}", vendor_id);

    json params;
    params["request_method"] = "DELETE";
    params["path"] = "/v1/vendor/" + std::to_string(vendor_id);

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, vendor_id](json /*response*/) {
            spdlog::debug("[SpoolmanAPI] Vendor {} deleted successfully", vendor_id);
            if (on_success) {
                on_success();
            }
        },
        on_error);
}

void MoonrakerSpoolmanAPI::delete_spoolman_filament(int filament_id, SuccessCallback on_success,
                                                    ErrorCallback on_error) {
    spdlog::info("[SpoolmanAPI] Deleting Spoolman filament {}", filament_id);

    json params;
    params["request_method"] = "DELETE";
    params["path"] = "/v1/filament/" + std::to_string(filament_id);

    client_.send_jsonrpc(
        "server.spoolman.proxy", params,
        [on_success, filament_id](json /*response*/) {
            spdlog::debug("[SpoolmanAPI] Filament {} deleted successfully", filament_id);
            if (on_success) {
                on_success();
            }
        },
        on_error);
}
