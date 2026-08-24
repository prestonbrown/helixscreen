// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_api.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Forward declaration for shared state
class MockPrinterState;

/**
 * @brief Simulated bed screw state for mock bed leveling
 *
 * Tracks the "physical" state of bed screws to simulate a realistic
 * iterative bed leveling session. Each probe shows current deviations,
 * and after each probe the user is assumed to make adjustments that
 * bring the bed closer to level.
 */
struct MockBedScrew {
    std::string name;            ///< Screw identifier (e.g., "front_left")
    float x_pos = 0.0f;          ///< Bed X coordinate (mm)
    float y_pos = 0.0f;          ///< Bed Y coordinate (mm)
    float current_offset = 0.0f; ///< Current Z deviation from level (mm)
    bool is_reference = false;   ///< True for the reference screw (always level)
};

/**
 * @brief Mock bed leveling state machine
 *
 * Simulates a realistic bed leveling session:
 * 1. Initial state has screws out of level (0.05-0.20mm deviations)
 * 2. After each probe, user "adjusts" screws (70-90% correction)
 * 3. Typically reaches level state after 2-4 iterations
 */
class MockScrewsTiltState {
  public:
    MockScrewsTiltState();

    /**
     * @brief Reset bed to initial out-of-level state
     */
    void reset();

    /**
     * @brief Simulate probing the bed and return raw Klipper console lines
     *
     * Returns the "// screw_name : x=… : adjust CW TT:MM" text Klipper would
     * emit, so callers must run it through helix::parse_screws_tilt_line() —
     * the same parser the live collector uses. Returning pre-built structs is
     * what let the mock drift to the opposite sign convention from Klipper
     * and hid the level-verdict bug (prestonbrown/helixscreen#1225).
     *
     * @return One console line per screw, base screw first
     */
    std::vector<std::string> probe_lines();

    /**
     * @brief Simulate user making adjustments based on probe results
     *
     * After seeing probe results, user turns screws. This applies
     * a 70-90% correction with some randomness to simulate imperfect adjustment.
     */
    void simulate_user_adjustments();

    /**
     * @brief Corner-to-corner error of the simulated bed, in mm
     *
     * Highest screw minus lowest, base included — the same quantity
     * evaluate_screw_level() judges, not each screw's distance from the base.
     */
    [[nodiscard]] float spread_mm() const;

    /**
     * @brief Check whether the simulated bed has converged
     * @param tolerance_mm Maximum acceptable corner-to-corner spread (default 0.02mm)
     * @return true if bed is considered level
     */
    [[nodiscard]] bool is_level(float tolerance_mm = 0.02f) const;

    /**
     * @brief Get the number of probe iterations performed
     */
    [[nodiscard]] int get_probe_count() const {
        return probe_count_;
    }

    /**
     * @brief Convert a Klipper base-relative diff to a turns:minutes string
     *
     * Mirrors Klipper's screws_tilt_adjust.py exactly: the diff is
     * `z_base - z`, a positive diff is CW on a CW-M* thread, diffs under 1
     * micron are zeroed before the pitch divide, and a rounded-up 60 minutes is
     * NOT carried into the turn count — Klipper really does print "00:60".
     *
     * Public so the arithmetic can be unit-tested directly; probe_lines() is
     * the only production caller.
     *
     * @param diff_mm z_base minus this screw's probed z, in mm
     * @return Adjustment string like "CW 01:15" or "CCW 00:30"
     */
    static std::string diff_to_adjustment(float diff_mm);

  private:
    std::vector<MockBedScrew> screws_;
    int probe_count_ = 0;
};

/**
 * @brief Mock Spoolman API for testing without a real Spoolman server
 *
 * Overrides all MoonrakerSpoolmanAPI methods to return mock filament
 * inventory data. Also provides mock-specific helpers for AMS slot
 * mapping and filament consumption simulation.
 */
class MoonrakerSpoolmanAPIMock : public MoonrakerSpoolmanAPI {
  public:
    using SuccessCallback = MoonrakerSpoolmanAPI::SuccessCallback;
    using ErrorCallback = MoonrakerSpoolmanAPI::ErrorCallback;

    explicit MoonrakerSpoolmanAPIMock(helix::MoonrakerClient& client);
    ~MoonrakerSpoolmanAPIMock() override = default;

    // ========================================================================
    // Overridden Spoolman Methods (return mock filament inventory)
    // ========================================================================

    void get_spoolman_status(std::function<void(bool, int)> on_success, ErrorCallback on_error,
                             bool silent = false) override;
    void get_spoolman_spools(helix::SpoolListCallback on_success, ErrorCallback on_error) override;
    void get_spoolman_spool(int spool_id, helix::SpoolCallback on_success, ErrorCallback on_error,
                            bool silent = false) override;
    void set_active_spool(int spool_id, SuccessCallback on_success,
                          ErrorCallback on_error) override;
    void update_spoolman_spool_weight(int spool_id, double remaining_weight_g,
                                      SuccessCallback on_success, ErrorCallback on_error) override;
    void update_spoolman_spool(int spool_id, const nlohmann::json& spool_data,
                               SuccessCallback on_success, ErrorCallback on_error) override;
    void update_spoolman_filament(int filament_id, const nlohmann::json& filament_data,
                                  SuccessCallback on_success, ErrorCallback on_error) override;
    void update_spoolman_filament_color(int filament_id, const std::string& color_hex,
                                        SuccessCallback on_success,
                                        ErrorCallback on_error) override;
    void get_spoolman_vendors(helix::VendorListCallback on_success,
                              ErrorCallback on_error) override;
    void get_spoolman_filaments(helix::FilamentListCallback on_success,
                                ErrorCallback on_error) override;
    void get_spoolman_filaments(int vendor_id, helix::FilamentListCallback on_success,
                                ErrorCallback on_error) override;
    void create_spoolman_vendor(const nlohmann::json& vendor_data,
                                helix::VendorCreateCallback on_success,
                                ErrorCallback on_error) override;
    void create_spoolman_filament(const nlohmann::json& filament_data,
                                  helix::FilamentCreateCallback on_success,
                                  ErrorCallback on_error) override;
    void create_spoolman_spool(const nlohmann::json& spool_data,
                               helix::SpoolCreateCallback on_success,
                               ErrorCallback on_error) override;
    void delete_spoolman_spool(int spool_id, SuccessCallback on_success,
                               ErrorCallback on_error) override;
    void delete_spoolman_vendor(int vendor_id, SuccessCallback on_success,
                                ErrorCallback on_error) override;
    void delete_spoolman_filament(int filament_id, SuccessCallback on_success,
                                  ErrorCallback on_error) override;
    void get_spoolman_external_vendors(helix::VendorListCallback on_success,
                                       ErrorCallback on_error) override;
    void get_spoolman_external_filaments(const std::string& vendor_name,
                                         helix::FilamentListCallback on_success,
                                         ErrorCallback on_error) override;

    // ========================================================================
    // Mock-Specific Helpers
    // ========================================================================

    /**
     * @brief Enable or disable mock Spoolman integration
     */
    void set_mock_spoolman_enabled(bool enabled) {
        mock_spoolman_enabled_ = enabled;
    }

    [[nodiscard]] bool is_mock_spoolman_enabled() const {
        return mock_spoolman_enabled_;
    }

    /**
     * @brief Assign a Spoolman spool to an AMS slot
     */
    void assign_spool_to_slot(int slot_index, int spool_id);

    /**
     * @brief Remove spool assignment from an AMS slot
     */
    void unassign_spool_from_slot(int slot_index);

    /**
     * @brief Get the Spoolman spool ID assigned to a slot
     */
    [[nodiscard]] int get_spool_for_slot(int slot_index) const;

    /**
     * @brief Get SpoolInfo for a slot (if assigned)
     */
    [[nodiscard]] std::optional<SpoolInfo> get_spool_info_for_slot(int slot_index) const;

    /**
     * @brief Simulate filament consumption during a print
     */
    void consume_filament(float grams, int slot_index = -1);

    // Test inspection
    struct FilamentUpdateRecord {
        int filament_id = 0;
        nlohmann::json data;
    };
    std::vector<FilamentUpdateRecord> filament_updates;

    /// Captured PATCH payloads from update_spoolman_spool() calls (for test
    /// assertions). Separate from update_spoolman_spool_weight(), which has its
    /// own dedicated path and does not populate this vector.
    struct SpoolUpdateRecord {
        int spool_id = 0;
        nlohmann::json patch;
    };
    std::vector<SpoolUpdateRecord> spool_updates;

    /// Captured (spool_id, remaining_weight_g) from update_spoolman_spool_weight()
    /// calls — the dedicated weight PATCH path. Lets tests count weight PATCHes
    /// issued via this path separately from update_spoolman_spool() combined PATCHes.
    struct WeightUpdateRecord {
        int spool_id = 0;
        double remaining_weight_g = 0.0;
    };
    std::vector<WeightUpdateRecord> weight_updates;

    /// Captured POST payloads from create_spoolman_vendor() calls (for test assertions).
    std::vector<nlohmann::json> created_vendors;

    /// ID to assign to the next vendor created via create_spoolman_vendor().
    /// If 0, the mock falls back to an auto-assigned ID.
    int next_created_vendor_id = 0;

    /// Captured POST payloads from create_spoolman_filament() calls (for test assertions).
    std::vector<nlohmann::json> created_filaments;

    /// ID to assign to the next filament created via create_spoolman_filament().
    /// If 0, the mock falls back to the internal auto-increment counter.
    int next_created_filament_id = 0;

    /// Captured POST payloads from create_spoolman_spool() calls (for test assertions).
    std::vector<nlohmann::json> created_spools;

    /// ID to assign to the next spool created via create_spoolman_spool().
    /// If 0, the mock falls back to an auto-assigned ID based on list size.
    int next_created_spool_id = 0;

    /**
     * @brief Pre-seed a vendor with a known ID and name.
     *
     * Added vendors are returned by get_spoolman_vendors() ahead of
     * vendors synthesized from mock_spools_. Use to test vendor-lookup
     * code with predictable IDs.
     */
    void add_vendor(int id, std::string name) {
        VendorInfo v;
        v.id = id;
        v.name = std::move(name);
        mock_vendors_.push_back(v);
    }

    /**
     * @brief Pre-seed a filament with a known ID, vendor, material, and color hex.
     *
     * Added filaments are returned by both get_spoolman_filaments() overloads.
     * The vendor_id-filtered overload returns only matching entries. Use to test
     * filament-lookup code with predictable IDs.
     */
    void add_filament(int id, int vendor_id, std::string material, std::string color_hex) {
        FilamentInfo f;
        f.id = id;
        f.vendor_id = vendor_id;
        f.material = std::move(material);
        f.color_hex = std::move(color_hex);
        mock_filaments_.push_back(std::move(f));
    }

    /**
     * @brief Get mutable reference to mock spools for testing
     */
    std::vector<SpoolInfo>& get_mock_spools() {
        return mock_spools_;
    }

    /**
     * @brief Get const reference to mock spools
     */
    [[nodiscard]] const std::vector<SpoolInfo>& get_mock_spools() const {
        return mock_spools_;
    }

    /**
     * @brief Get the current active spool ID for test assertions
     * @return Active spool ID, or 0 if no spool is active
     */
    [[nodiscard]] int get_mock_active_spool_id() const {
        return mock_active_spool_id_;
    }

  private:
    bool mock_spoolman_enabled_ = true;
    int mock_active_spool_id_ = 1;
    std::vector<SpoolInfo> mock_spools_;
    std::vector<FilamentInfo> mock_filaments_;
    std::vector<VendorInfo> mock_vendors_;
    int next_filament_id_ = 300;
    std::map<int, int> slot_spool_map_;

    void init_mock_spools();

    /**
     * @brief Invoke on_error the way the real proxy path fails when Spoolman
     * is not connected/configured in Moonraker (JSON-RPC error from the
     * "server.spoolman.proxy" call). Used to keep the mock honest when
     * mock_spoolman_enabled_ is false instead of silently succeeding.
     */
    void fail_spoolman_unavailable(const std::string& method, const ErrorCallback& on_error) const;
};

/**
 * @brief Mock Timelapse API for testing without a real Moonraker connection
 *
 * Overrides all MoonrakerTimelapseAPI methods to return mock data.
 * Render/frame operations are no-ops; settings are not persisted.
 */
class MoonrakerTimelapseAPIMock : public MoonrakerTimelapseAPI {
  public:
    using SuccessCallback = MoonrakerTimelapseAPI::SuccessCallback;
    using ErrorCallback = MoonrakerTimelapseAPI::ErrorCallback;

    explicit MoonrakerTimelapseAPIMock(helix::MoonrakerClient& client,
                                       const std::string& http_base_url);
    ~MoonrakerTimelapseAPIMock() override = default;

    void render_timelapse(SuccessCallback on_success, ErrorCallback on_error) override;
    void save_timelapse_frames(SuccessCallback on_success, ErrorCallback on_error) override;
    void get_last_frame_info(std::function<void(const LastFrameInfo&)> on_success,
                             ErrorCallback on_error) override;
};

/**
 * @brief Mock REST API for testing without real Moonraker REST endpoints
 *
 * Overrides all MoonrakerRestAPI methods to return mock data.
 * WLED state is tracked internally for toggle/brightness/preset testing.
 */
/**
 * @brief Mock Advanced API for testing calibration and macro operations
 *
 * Overrides bed mesh calibration and screws tilt methods with mock implementations
 * that simulate realistic behavior without real hardware.
 */
class MoonrakerAdvancedAPIMock : public MoonrakerAdvancedAPI {
  public:
    using SuccessCallback = MoonrakerAdvancedAPI::SuccessCallback;
    using ErrorCallback = MoonrakerAdvancedAPI::ErrorCallback;
    using BedMeshProgressCallback = MoonrakerAdvancedAPI::BedMeshProgressCallback;

    MoonrakerAdvancedAPIMock(helix::MoonrakerClient& client, MoonrakerAPI& api);
    ~MoonrakerAdvancedAPIMock() override = default;

    // ========================================================================
    // Overridden Calibration Methods (simulate realistic behavior)
    // ========================================================================

    /**
     * @brief Mock bed mesh calibration with progress simulation
     */
    void start_bed_mesh_calibrate(BedMeshProgressCallback on_progress, SuccessCallback on_complete,
                                  ErrorCallback on_error, int expected_probes = 0,
                                  int probe_samples = 1) override;

    /**
     * @brief Simulate SCREWS_TILT_CALCULATE with iterative bed leveling
     */
    void calculate_screws_tilt(helix::ScrewTiltCallback on_success,
                               ErrorCallback on_error) override;

    /**
     * @brief Reset the mock bed to initial out-of-level state
     */
    void reset_mock_bed_state();

    /**
     * @brief Get the mock bed state for inspection/testing
     */
    MockScrewsTiltState& get_mock_bed_state() {
        return mock_bed_state_;
    }

  private:
    /// Mock bed state for screws tilt simulation
    MockScrewsTiltState mock_bed_state_;
};

class MoonrakerRestAPIMock : public MoonrakerRestAPI {
  public:
    using SuccessCallback = MoonrakerRestAPI::SuccessCallback;
    using ErrorCallback = MoonrakerRestAPI::ErrorCallback;
    using RestCallback = MoonrakerRestAPI::RestCallback;

    explicit MoonrakerRestAPIMock(helix::MoonrakerClient& client, const std::string& http_base_url);
    ~MoonrakerRestAPIMock() override = default;

    // ========================================================================
    // Overridden REST Methods (return mock responses)
    // ========================================================================

    void call_rest_get(const std::string& endpoint, RestCallback on_complete) override;
    void call_rest_post(const std::string& endpoint, const nlohmann::json& params,
                        RestCallback on_complete) override;

    // ========================================================================
    // Overridden WLED Methods (return mock data with tracked state)
    // ========================================================================

    void wled_get_strips(RestCallback on_success, ErrorCallback on_error) override;
    void wled_set_strip(const std::string& strip, const std::string& action, int brightness,
                        int preset, SuccessCallback on_success, ErrorCallback on_error) override;
    void wled_get_status(RestCallback on_success, ErrorCallback on_error) override;
    void get_server_config(RestCallback on_success, ErrorCallback on_error) override;

    // ========================================================================
    // Test Spies — record outbound POSTs so unit tests can assert on payloads
    // ========================================================================

    /// Recorded POST: endpoint and body. Tests use mock_get_post_history()
    /// to verify a backend issued the expected request.
    struct PostRecord {
        std::string endpoint;
        nlohmann::json body;
    };

    /// All POST calls observed since construction (or last clear).
    [[nodiscard]] std::vector<PostRecord> mock_get_post_history() const {
        return post_history_;
    }

    /// Drop the recorded POST history.
    void mock_clear_post_history() {
        post_history_.clear();
    }

    /// Configure a fake response for the next POST to `endpoint`. If a
    /// response is queued for an endpoint, the mock returns it (instead of
    /// the default {"result":"ok"}) so tests can drive both success and
    /// "state":"error" / 404 paths through the same code.
    void mock_queue_post_response(const std::string& endpoint, RestResponse response) {
        post_responses_[endpoint] = std::move(response);
    }

    /// Configure a persistent fake response for GET `endpoint`. Checked at the
    /// top of call_rest_get (before the built-in ACE canned responses) so tests
    /// can drive failure paths — e.g. a Klipper fork that ships /server/ace/status
    /// + /slots but NOT /server/ace/info (#1069) — or inject custom payloads.
    /// Unlike POST responses these are NOT consumed; they persist across polls.
    void mock_set_get_response(const std::string& endpoint, RestResponse response) {
        get_responses_[endpoint] = std::move(response);
    }

  private:
    /// Mock WLED strip on/off states (strip_id -> is_on)
    std::map<std::string, bool> mock_wled_states_;
    /// Mock WLED active presets (strip_id -> preset_id, -1 = none)
    std::map<std::string, int> mock_wled_presets_;
    /// Mock WLED brightness per strip (strip_id -> 0-255)
    std::map<std::string, int> mock_wled_brightness_;

    /// Recorded outbound POSTs (test spy)
    std::vector<PostRecord> post_history_;
    /// Per-endpoint canned responses (test spy)
    std::map<std::string, RestResponse> post_responses_;
    /// Per-endpoint canned GET responses (test spy). Persistent, not consumed.
    std::map<std::string, RestResponse> get_responses_;
};

/**
 * @brief Mock File Transfer API for testing without real Moonraker HTTP
 *
 * Overrides HTTP file transfer methods to use local test files instead
 * of making actual HTTP requests to a Moonraker server.
 *
 * Path Resolution:
 * The mock tries multiple paths to find test files, supporting both:
 * - Running from project root: assets/test_gcodes/
 * - Running from build/bin/: ../../assets/test_gcodes/
 */
class MoonrakerFileTransferAPIMock : public MoonrakerFileTransferAPI {
  public:
    using SuccessCallback = MoonrakerFileTransferAPI::SuccessCallback;
    using ErrorCallback = MoonrakerFileTransferAPI::ErrorCallback;
    using StringCallback = MoonrakerFileTransferAPI::StringCallback;

    explicit MoonrakerFileTransferAPIMock(helix::MoonrakerClient& client,
                                          const std::string& http_base_url);
    ~MoonrakerFileTransferAPIMock() override = default;

    // ========================================================================
    // Overridden HTTP File Transfer Methods (use local files instead of HTTP)
    // ========================================================================

    void download_file(const std::string& root, const std::string& path, StringCallback on_success,
                       ErrorCallback on_error) override;

    void download_file_partial(const std::string& root, const std::string& path, size_t max_bytes,
                               StringCallback on_success, ErrorCallback on_error) override;

    void download_file_tail(const std::string& root, const std::string& path, size_t max_bytes,
                            StringCallback on_success, ErrorCallback on_error) override;

    void download_file_to_path(const std::string& root, const std::string& path,
                               const std::string& dest_path, StringCallback on_success,
                               ErrorCallback on_error,
                               ProgressCallback on_progress = nullptr) override;

    void upload_file(const std::string& root, const std::string& path, const std::string& content,
                     SuccessCallback on_success, ErrorCallback on_error) override;

    void upload_file_with_name(const std::string& root, const std::string& path,
                               const std::string& filename, const std::string& content,
                               SuccessCallback on_success, ErrorCallback on_error) override;

    void download_thumbnail(const std::string& thumbnail_path, const std::string& cache_path,
                            StringCallback on_success, ErrorCallback on_error) override;

  private:
    /**
     * @brief Find test file using fallback path search
     *
     * Tries multiple paths to locate test files:
     * - assets/test_gcodes/ (from project root)
     * - ../assets/test_gcodes/ (from build/)
     * - ../../assets/test_gcodes/ (from build/bin/)
     *
     * @param filename Filename to find
     * @return Full path to file if found, empty string otherwise
     */
    std::string find_test_file(const std::string& filename) const;

    /// Fallback path prefixes to search (from various CWDs)
    /// Note: Base directory is RuntimeConfig::TEST_GCODE_DIR (defined in runtime_config.h)
    static const std::vector<std::string> PATH_PREFIXES;
};

/**
 * @brief Mock MoonrakerAPI for testing without real printer connection
 *
 * Overrides connection, database, and calibration methods for mock mode.
 * File transfer mocking is handled by MoonrakerFileTransferAPIMock (sub-API).
 *
 * Usage:
 *   MoonrakerClientMock mock_client;
 *   helix::PrinterState state;
 *   MoonrakerAPIMock mock_api(mock_client, state);
 *   // mock_api.transfers().download_file() now reads from assets/test_gcodes/
 */
class MoonrakerAPIMock : public MoonrakerAPI {
  public:
    /**
     * @brief Construct mock API
     *
     * @param client helix::MoonrakerClient instance (typically MoonrakerClientMock)
     * @param state helix::PrinterState instance
     */
    MoonrakerAPIMock(helix::MoonrakerClient& client, helix::PrinterState& state);

    ~MoonrakerAPIMock() override = default;

    // ========================================================================
    // Overridden Connection/Subscription/Database Proxies (no-ops for mock)
    // ========================================================================

    helix::SubscriptionId
    subscribe_notifications(std::function<void(const json&)> callback) override;
    bool unsubscribe_notifications(helix::SubscriptionId id) override;
    void register_method_callback(const std::string& method, const std::string& name,
                                  std::function<void(const json&)> callback) override;
    bool unregister_method_callback(const std::string& method, const std::string& name) override;
    void suppress_disconnect_modal(uint32_t duration_ms) override;

    /// Test spy: how many times suppress_disconnect_modal() was called, and
    /// the duration handed to the last call - lets expected-restart tests
    /// assert the api-level suppression arm.
    [[nodiscard]] size_t suppress_disconnect_modal_calls() const {
        return suppress_disconnect_modal_calls_;
    }
    [[nodiscard]] uint32_t last_suppress_disconnect_modal_ms() const {
        return last_suppress_disconnect_modal_ms_;
    }
    void get_gcode_store(int count,
                         std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                         std::function<void(const MoonrakerError&)> on_error) override;
    void database_get_item(const std::string& namespace_name, const std::string& key,
                           std::function<void(const json&)> on_success,
                           ErrorCallback on_error = nullptr) override;
    void database_post_item(const std::string& namespace_name, const std::string& key,
                            const json& value, std::function<void()> on_success = nullptr,
                            ErrorCallback on_error = nullptr) override;
    void database_get_namespace(const std::string& namespace_name,
                                std::function<void(const json&)> on_success,
                                ErrorCallback on_error = nullptr) override;
    void database_delete_item(const std::string& namespace_name, const std::string& key,
                              std::function<void()> on_success = nullptr,
                              ErrorCallback on_error = nullptr) override;

    // ========================================================================
    // Overridden Helix Plugin Methods (return mock data)
    // ========================================================================

    void get_phase_tracking_status(std::function<void(bool enabled)> on_success,
                                   ErrorCallback on_error = nullptr) override;
    void set_phase_tracking_enabled(bool enabled, std::function<void(bool success)> on_success,
                                    ErrorCallback on_error = nullptr) override;

    // ========================================================================
    // Overridden Power Device Methods (return mock data)
    // ========================================================================

    /**
     * @brief Get mock power devices for testing
     *
     * Returns a predefined list of power devices to test the Power Panel UI
     * without needing a real Moonraker connection.
     *
     * @param on_success Callback with list of mock power devices
     * @param on_error Error callback (never called - mock always succeeds)
     */
    void get_power_devices(PowerDevicesCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Mock set power device (logs but doesn't control hardware)
     *
     * Logs the command and updates internal mock state for testing.
     * Always calls success callback.
     *
     * @param device Device name
     * @param action Action ("on", "off", "toggle")
     * @param on_success Success callback (always called)
     * @param on_error Error callback (never called)
     */
    void set_device_power(const std::string& device, const std::string& action,
                          SuccessCallback on_success, ErrorCallback on_error) override;

    // ========================================================================
    // Overridden Sensor Methods (return mock data)
    // ========================================================================

    void get_sensors(SensorsCallback on_success, ErrorCallback on_error) override;

    // ========================================================================
    // Shared State Methods
    // ========================================================================

    /**
     * @brief Set shared mock state for coordination with MoonrakerClientMock
     *
     * When set, queries for excluded objects and available objects will
     * return data from the shared state, which is also updated by
     * MoonrakerClientMock when processing G-code commands.
     *
     * @param state Shared state pointer (can be nullptr to disable)
     */
    void set_mock_state(std::shared_ptr<MockPrinterState> state);

    /**
     * @brief Get shared mock state (may be nullptr)
     *
     * @return Shared state pointer, or nullptr if not set
     */
    std::shared_ptr<MockPrinterState> get_mock_state() const {
        return mock_state_;
    }

    /**
     * @brief Get excluded objects from shared state
     *
     * Returns objects excluded via EXCLUDE_OBJECT commands processed by
     * MoonrakerClientMock. If no shared state is set, returns empty set.
     *
     * @return Set of excluded object names
     */
    std::set<std::string> get_excluded_objects_from_mock() const;

    /**
     * @brief Get available objects from shared state
     *
     * Returns objects defined via EXCLUDE_OBJECT_DEFINE commands.
     * If no shared state is set, returns empty vector.
     *
     * @return Vector of available object names
     */
    std::vector<std::string> get_available_objects_from_mock() const;

    // ========================================================================
    // Advanced Mock Access
    // ========================================================================

    /**
     * @brief Get the Advanced mock sub-API for mock-specific helpers
     *
     * Provides access to mock-only methods like reset_mock_bed_state().
     *
     * @return Reference to MoonrakerAdvancedAPIMock
     */
    MoonrakerAdvancedAPIMock& advanced_mock();

    // ========================================================================
    // File Transfer Mock Access
    // ========================================================================

    /**
     * @brief Get the File Transfer mock sub-API for mock-specific access
     *
     * @return Reference to MoonrakerFileTransferAPIMock
     */
    MoonrakerFileTransferAPIMock& transfers_mock();

    // ========================================================================
    // Spoolman Mock Access
    // ========================================================================

    /**
     * @brief Get the Spoolman mock sub-API for mock-specific helpers
     *
     * Provides access to mock-only methods like assign_spool_to_slot(),
     * consume_filament(), get_mock_spools(), etc.
     *
     * @return Reference to MoonrakerSpoolmanAPIMock
     */
    MoonrakerSpoolmanAPIMock& spoolman_mock();

    /**
     * @brief Get the Timelapse mock sub-API for mock-specific helpers
     *
     * @return Reference to MoonrakerTimelapseAPIMock
     */
    MoonrakerTimelapseAPIMock& timelapse_mock();

    // ========================================================================
    // REST Mock Access
    // ========================================================================

    /**
     * @brief Get the REST mock sub-API for mock-specific helpers
     *
     * @return Reference to MoonrakerRestAPIMock
     */
    MoonrakerRestAPIMock& rest_mock();

    /// Set a mock database value for testing
    void mock_set_db_value(const std::string& namespace_name, const std::string& key,
                           const nlohmann::json& value);

    /// Fetch a mock database value for test assertions. Returns null JSON if the
    /// key is absent.
    nlohmann::json mock_get_db_value(const std::string& namespace_name,
                                     const std::string& key) const;

    /// Number of times database_post_item() was called on this mock instance.
    /// Counts every invocation — including calls later rejected via
    /// mock_reject_next_db_post() or captured via mock_defer_next_db_post() — so
    /// tests can assert "a write was attempted" or "no write happened".
    [[nodiscard]] int mock_db_post_count() const {
        return db_post_count_;
    }

    /// Number of times database_delete_item() was called on this mock instance.
    /// Counts every invocation (see mock_db_post_count() for rejection/defer
    /// semantics).
    [[nodiscard]] int mock_db_delete_count() const {
        return db_delete_count_;
    }

    /// Cause the next database_post_item() call to fire its on_error callback
    /// with the given MoonrakerError, and skip writing to the mock DB. The
    /// rejection is consumed on the first call — subsequent posts succeed
    /// normally unless this is called again. The no-arg overload uses a
    /// generic UNKNOWN error with a descriptive message.
    /// Not thread-safe: call from the main test thread before the rejection
    /// is consumed. Catch2 runs tests sequentially so this is safe today.
    void mock_reject_next_db_post();
    void mock_reject_next_db_post(MoonrakerError err);

    /// Cause the next database_delete_item() call to fire its on_error callback
    /// with the given MoonrakerError, and skip erasing from the mock DB. The
    /// rejection is consumed on the first call — subsequent deletes succeed
    /// normally unless this is called again. The no-arg overload uses a
    /// generic UNKNOWN error with a descriptive message.
    ///
    /// Note: the mock mirrors MoonrakerAPI's missing-key normalization. If the
    /// injected error has code == 404 or its message contains "not found",
    /// on_success is called instead of on_error — faithfully simulating the
    /// real API's contract. Tests relying on this remap can inject specific
    /// errors and verify callers handle normalized results.
    ///
    /// Not thread-safe: call from the main test thread before the rejection
    /// is consumed.
    void mock_reject_next_db_delete();
    void mock_reject_next_db_delete(MoonrakerError err);

    /// Cause the next database_get_namespace() call to fire its on_error callback
    /// with the given MoonrakerError, and skip returning any value. The rejection
    /// is consumed on the first call — subsequent gets succeed normally unless
    /// this is called again. The no-arg overload uses a generic UNKNOWN error.
    /// Used to exercise load_blocking()'s "MR DB unreachable → fall back to
    /// local cache" path.
    /// Not thread-safe: call from the main test thread before the rejection
    /// is consumed.
    void mock_reject_next_db_get();
    void mock_reject_next_db_get(MoonrakerError err);

    /// Cause the next database_post_item() call to capture its callbacks without
    /// firing them. The captured callbacks can later be fired via
    /// fire_deferred_db_post_success() or fire_deferred_db_post_error(err).
    /// Used to simulate the "callback fires after caller destroyed" window so
    /// tests can prove the caller's lifetime discipline (value-capture + shared
    /// state) actually prevents UAF.
    /// When deferred, the mock also skips writing to the DB until the success
    /// callback fires — matching real-API semantics (no durable state until ACK).
    /// If fire_deferred_*() is called with no captured callbacks, it is a no-op.
    /// Not thread-safe: call from the main test thread.
    void mock_defer_next_db_post();
    void fire_deferred_db_post_success();
    void fire_deferred_db_post_error(MoonrakerError err);

    /// Same mechanism for database_delete_item(). When deferred, the mock also
    /// skips erasing from the DB until the success callback fires.
    void mock_defer_next_db_delete();
    void fire_deferred_db_delete_success();
    void fire_deferred_db_delete_error(MoonrakerError err);

    /// Same mechanism for database_get_namespace() (NOT database_get_item()).
    /// Used to exercise load_blocking()'s cv.wait_for timeout path.
    void mock_defer_next_db_get();
    void fire_deferred_db_get_success(const nlohmann::json& value);
    void fire_deferred_db_get_error(MoonrakerError err);

    /// Ensure a namespace/key is absent from the mock database so subsequent
    /// database_get_item() calls route to on_error.
    void set_database_empty(const std::string& namespace_name, const std::string& key);

  private:
    // Shared mock state for coordination with MoonrakerClientMock
    std::shared_ptr<MockPrinterState> mock_state_;

    // Mock power device states (for toggle testing)
    std::map<std::string, bool> mock_power_states_;

    // Mock subscription ID counter
    helix::SubscriptionId mock_next_subscription_id_ = 100;

    // Test spy state for suppress_disconnect_modal()
    size_t suppress_disconnect_modal_calls_ = 0;
    uint32_t last_suppress_disconnect_modal_ms_ = 0;

    /// Mock database storage: key = "namespace:key", value = JSON
    std::map<std::string, nlohmann::json> mock_db_;

    /// Call counters for database write ops. Incremented at the top of each
    /// override, before any rejection/defer branch, so a rejected or deferred
    /// call still counts as an attempt. Exposed via mock_db_post_count() /
    /// mock_db_delete_count().
    int db_post_count_ = 0;
    int db_delete_count_ = 0;

    /// One-shot rejection for database_post_item (set by mock_reject_next_db_post).
    std::optional<MoonrakerError> next_db_post_rejection_;

    /// One-shot rejection for database_delete_item (set by mock_reject_next_db_delete).
    std::optional<MoonrakerError> next_db_delete_rejection_;

    /// One-shot rejection for database_get_namespace (set by mock_reject_next_db_get).
    std::optional<MoonrakerError> next_db_get_rejection_;

    // One-shot deferred captures. Post/delete share the same shape (void()
    // success, error with MoonrakerError). Get captures a nlohmann::json value.
    struct DeferredDbPost {
        std::function<void()> on_success;
        std::function<void(const MoonrakerError&)> on_error;
        // Post/delete need the captured write/erase info so fire_*_success can
        // apply the same DB mutation the synchronous path would have applied.
        // (For delete, `value` is ignored — the erase is unconditional.)
        std::string namespace_name;
        std::string key;
        nlohmann::json value;
    };
    struct DeferredDbGet {
        std::function<void(const nlohmann::json&)> on_success;
        std::function<void(const MoonrakerError&)> on_error;
        std::string namespace_name;
    };
    bool defer_next_db_post_ = false;
    std::optional<DeferredDbPost> deferred_db_post_;
    bool defer_next_db_delete_ = false;
    std::optional<DeferredDbPost> deferred_db_delete_;
    bool defer_next_db_get_ = false;
    std::optional<DeferredDbGet> deferred_db_get_;
};
