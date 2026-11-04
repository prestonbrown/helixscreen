// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_spoolman_api.h
 * @brief Spoolman filament tracking operations via Moonraker proxy
 *
 * Extracted from MoonrakerAPI to encapsulate all Spoolman-related functionality
 * in a dedicated class. Uses MoonrakerClient for JSON-RPC transport.
 */

#pragma once

#include "helix_shared_callback.h"
#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"
#include "spoolman_types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class IMoonrakerClient;
} // namespace helix

// Forward declare json
#include "json_fwd.h"

namespace helix::spoolman_detail {
/**
 * @brief Parse a single Spoolman spool JSON object into SpoolInfo.
 *
 * Exposed for testing. Must tolerate present-but-null numeric fields
 * (Spoolman serializes optional temps etc. as null) without throwing — a
 * throw here aborts the whole spool-list parse (#1087).
 */
SpoolInfo parse_spool_info(const nlohmann::json& spool_json);

/// Spoolman stores every extra value as a JSON-encoded string: "0.042" for a
/// float field, "\"0.042\"" for a text one. The number either way, or nullopt
/// when the key is absent or holds no number.
std::optional<double> parse_extra_number(const nlohmann::json& extra, const std::string& key);

/// The usable OrcaSlicer preset id in @p extra's kSpoolmanOrcaIdExtraKey, or
/// empty. Only a JSON-encoded string counts: a number (Spoolman's own numeric
/// filament id is no Orca preset), an empty or malformed value and a `GF` code
/// are each refused, with one debug line per distinct refused value.
std::string parse_extra_orca_filament_id(const nlohmann::json& extra);

/// @p value encoded for an extra field of @p field_type, the inverse of
/// parse_extra_number(): a text field gets a quoted string, every other type
/// the bare number.
std::string encode_extra_number(const std::string& field_type, double value);

/**
 * @brief Parse one SpoolmanDB filament, or nullopt for an entry that is not an
 * object or carries no id. Never throws.
 */
std::optional<ExternalFilament> parse_external_filament(const nlohmann::json& filament_json);
} // namespace helix::spoolman_detail

/**
 * @brief Spoolman API operations via Moonraker's server.spoolman.proxy
 *
 * Provides high-level operations for interacting with Spoolman through
 * Moonraker's built-in proxy. All methods are asynchronous with callbacks.
 *
 * Usage:
 *   MoonrakerSpoolmanAPI spoolman(client);
 *   spoolman.get_spoolman_spools(
 *       [](const auto& spools) { ... },
 *       [](const auto& err) { ... });
 */
class MoonrakerSpoolmanAPI : public helix::ISpoolmanAPI {
  public:
    using SuccessCallback = helix::SharedCallback<void()>;
    using ErrorCallback = helix::SharedCallback<void(const MoonrakerError&)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     */
    explicit MoonrakerSpoolmanAPI(helix::IMoonrakerClient& client);
    virtual ~MoonrakerSpoolmanAPI() = default;

    // ========================================================================
    // Spoolman Status & Spool Operations
    // ========================================================================

    /**
     * @brief Get Spoolman connection status
     *
     * @param on_success Called with (connected, active_spool_id)
     * @param on_error Called on failure
     */
    void get_spoolman_status(std::function<void(bool connected, int active_spool_id)> on_success,
                             ErrorCallback on_error, bool silent = false) override;

    /**
     * @brief Get list of spools from Spoolman
     *
     * @param on_success Called with spool list
     * @param on_error Called on failure
     */
    void get_spoolman_spools(helix::SpoolListCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Get a single spool's details by ID
     *
     * @param spool_id Spoolman spool ID
     * @param on_success Called with spool info (empty optional if not found)
     * @param on_error Called on failure
     */
    void get_spoolman_spool(int spool_id, helix::SpoolCallback on_success, ErrorCallback on_error,
                            bool silent = false) override;

    /**
     * @brief Set the active spool for filament tracking
     *
     * @param spool_id Spoolman spool ID (0 to clear)
     * @param on_success Called when spool is set
     * @param on_error Called on failure
     */
    void set_active_spool(int spool_id, SuccessCallback on_success,
                          ErrorCallback on_error) override;

    /**
     * @brief Get usage history for a spool
     *
     * @param spool_id Spoolman spool ID
     * @param on_success Called with usage records
     * @param on_error Called on failure
     */
    void
    get_spool_usage_history(int spool_id,
                            std::function<void(const std::vector<FilamentUsageRecord>&)> on_success,
                            ErrorCallback on_error) override;

    // ========================================================================
    // Spool Update Operations
    // ========================================================================

    /**
     * @brief Update a spool's remaining weight in Spoolman
     *
     * @param spool_id Spoolman spool ID
     * @param remaining_weight_g New remaining weight in grams
     * @param on_success Called when update succeeds
     * @param on_error Called on failure
     */
    void update_spoolman_spool_weight(int spool_id, double remaining_weight_g,
                                      SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Update a spool's properties in Spoolman
     *
     * @param spool_id Spoolman spool ID
     * @param spool_data JSON object with fields to update
     * @param on_success Called when update succeeds
     * @param on_error Called on failure
     */
    void update_spoolman_spool(int spool_id, const nlohmann::json& spool_data,
                               SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Update a filament definition in Spoolman
     *
     * WARNING: This affects ALL spools using this filament definition.
     *
     * @param filament_id Spoolman filament ID (not spool ID!)
     * @param filament_data JSON object with fields to update
     * @param on_success Called when update succeeds
     * @param on_error Called on failure
     */
    void update_spoolman_filament(int filament_id, const nlohmann::json& filament_data,
                                  SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Update a filament's color in Spoolman
     *
     * WARNING: This affects ALL spools using this filament definition.
     *
     * @param filament_id Spoolman filament ID (not spool ID!)
     * @param color_hex New color as hex string (e.g., "#FF0000")
     * @param on_success Called when update succeeds
     * @param on_error Called on failure
     */
    void update_spoolman_filament_color(int filament_id, const std::string& color_hex,
                                        SuccessCallback on_success,
                                        ErrorCallback on_error) override;

    void set_spoolman_filament_extra_number(int filament_id, const std::string& key,
                                            const std::string& display_name, double value,
                                            SuccessCallback on_success,
                                            ErrorCallback on_error) override;

    // ========================================================================
    // Vendor & Filament Operations
    // ========================================================================

    /**
     * @brief Get list of vendors from Spoolman
     */
    void get_spoolman_vendors(helix::VendorListCallback on_success,
                              ErrorCallback on_error) override;

    /**
     * @brief Get list of filaments from Spoolman
     */
    void get_spoolman_filaments(helix::FilamentListCallback on_success,
                                ErrorCallback on_error) override;

    /**
     * @brief Get list of filaments from Spoolman filtered by vendor ID
     */
    void get_spoolman_filaments(int vendor_id, helix::FilamentListCallback on_success,
                                ErrorCallback on_error) override;

    // ========================================================================
    // CRUD Operations
    // ========================================================================

    /**
     * @brief Create a new vendor in Spoolman
     */
    void create_spoolman_vendor(const nlohmann::json& vendor_data,
                                helix::VendorCreateCallback on_success,
                                ErrorCallback on_error) override;

    /**
     * @brief Create a new filament in Spoolman
     */
    void create_spoolman_filament(const nlohmann::json& filament_data,
                                  helix::FilamentCreateCallback on_success,
                                  ErrorCallback on_error) override;

    /**
     * @brief Create a new spool in Spoolman
     */
    void create_spoolman_spool(const nlohmann::json& spool_data,
                               helix::SpoolCreateCallback on_success,
                               ErrorCallback on_error) override;

    /**
     * @brief Delete a spool from Spoolman
     */
    void delete_spoolman_spool(int spool_id, SuccessCallback on_success,
                               ErrorCallback on_error) override;

    /**
     * @brief Delete a vendor from Spoolman
     */
    void delete_spoolman_vendor(int vendor_id, SuccessCallback on_success,
                                ErrorCallback on_error) override;

    /**
     * @brief Delete a filament from Spoolman
     */
    void delete_spoolman_filament(int filament_id, SuccessCallback on_success,
                                  ErrorCallback on_error) override;

    void search_spoolman_external_filaments(const std::string& query, int limit,
                                            helix::ExternalFilamentListCallback on_success,
                                            ErrorCallback on_error) override;

  protected:
    helix::IMoonrakerClient& client_;
};
