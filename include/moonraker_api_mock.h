// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_api.h"
#include "moonraker_client.h"

#include <functional>
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
    void start_bed_mesh_calibrate(const BedMeshCommand& command,
                                  BedMeshProgressCallback on_progress, SuccessCallback on_complete,
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

    /// Hold every wled_get_strips answer until mock_release_wled_strips(), so a
    /// test can order WLED's reply against other startup events.
    void mock_hold_wled_strips() {
        hold_wled_strips_ = true;
    }
    void mock_release_wled_strips();

  private:
    bool hold_wled_strips_ = false;
    std::vector<std::function<void()>> held_wled_strips_;

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
    // In-memory config root (test injection)
    // ========================================================================

    /**
     * @brief Seed an in-memory "config" root keyed by FULL relative path
     *
     * The on-disk fallback resolves by basename, so a nested path such as
     * conf.d/options.cfg can never be served from it. When this map is
     * non-empty, download_file(root == "config", path) looks the path up
     * exactly, and upload_file records what was written back into it.
     *
     * @param files Map of path (relative to the config root) -> file content
     */
    void set_config_files(std::map<std::string, std::string> files);

    /// Content the mock currently holds for a config path, including anything
    /// uploaded since seeding. std::nullopt when the path is unknown.
    std::optional<std::string> get_uploaded_config(const std::string& path) const;

    /// Every config path the mock currently holds (for list_files mirroring)
    std::map<std::string, std::string> get_config_files() const;

    // ========================================================================
    // Overridden HTTP File Transfer Methods (use local files instead of HTTP)
    // ========================================================================

    void download_file(const std::string& root, const std::string& path, StringCallback on_success,
                       ErrorCallback on_error) override;

    void download_file_partial(const std::string& root, const std::string& path, size_t max_bytes,
                               StringCallback on_success, ErrorCallback on_error,
                               CancelFlag cancelled = nullptr) override;

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

    void upload_file_from_path(const std::string& root, const std::string& dest_path,
                               const std::string& local_path, SuccessCallback on_success,
                               ErrorCallback on_error,
                               ProgressCallback on_progress = nullptr) override;

    void download_thumbnail(const std::string& thumbnail_path, const std::string& cache_path,
                            StringCallback on_success, ErrorCallback on_error) override;

    // ========================================================================
    // upload_file_from_path spy (test injection)
    // ========================================================================

    /// One recorded upload_file_from_path() call. @a content is what was on
    /// disk at @a local_path when the call was made — the producer deletes that
    /// file in its success callback, so reading it later is not an option.
    struct PathUploadRecord {
        std::string root;
        std::string dest_path;
        std::string local_path;
        std::string content;
    };

    /// Every upload_file_from_path() the mock has served, in call order.
    [[nodiscard]] const std::vector<PathUploadRecord>& path_uploads() const {
        return path_uploads_;
    }

    /// Local destinations handed to download_file_to_path(), in call order.
    /// A caller that deletes its download cannot be asked where it put it, so
    /// the mock is the only place that path survives.
    [[nodiscard]] const std::vector<std::string>& download_destinations() const {
        return download_destinations_;
    }

    /// Make every later upload_file_from_path() call on_error instead of
    /// on_success. The call is still recorded, so a test can assert both that
    /// the upload was attempted and that its failure was handled.
    void mock_fail_path_uploads(bool fail = true) {
        fail_path_uploads_ = fail;
    }

    /// Make later upload_file_from_path() calls record and then wait: neither
    /// callback runs until release_held_path_uploads().
    void mock_hold_path_uploads(bool hold = true) {
        hold_path_uploads_ = hold;
    }

    /// Complete every held upload with success.
    void release_held_path_uploads() {
        auto held = std::move(held_path_uploads_);
        held_path_uploads_.clear();
        for (auto& cb : held) {
            if (cb) {
                cb();
            }
        }
    }

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

    /// One config-root download lookup shared by the transfer entry points.
    /// HELIX_MOCK_PLUGINS_DIR serves helixscreen/plugins/<rel> from a local folder; the
    /// injected config root serves full paths from memory. `owned` false means neither
    /// source claims the config root for this path and the caller keeps its normal
    /// behaviour; `owned` true with no content is a known-absent path (not-found error).
    struct ConfigRootFile {
        bool owned = false;
        std::optional<std::string> content;
    };
    [[nodiscard]] ConfigRootFile lookup_config_root(const std::string& root,
                                                    const std::string& path) const;

    /// Injected config root: full relative path -> content. Empty = use disk.
    std::map<std::string, std::string> config_files_;

    /// Recorded upload_file_from_path() calls (test spy)
    std::vector<PathUploadRecord> path_uploads_;
    /// Recorded download_file_to_path() destinations (test spy)
    std::vector<std::string> download_destinations_;
    /// When set, upload_file_from_path() reports failure instead of success
    bool fail_path_uploads_ = false;
    bool hold_path_uploads_ = false;
    std::vector<SuccessCallback> held_path_uploads_;
};

/**
 * @brief Mock File Management API for testing without a Moonraker connection
 *
 * Only list_files() on the "config" root is mocked, and only when config files
 * have been injected via set_config_files(). Everything else falls through to
 * the real implementation.
 */
class MoonrakerFileAPIMock : public MoonrakerFileAPI {
  public:
    using ErrorCallback = MoonrakerFileAPI::ErrorCallback;

    explicit MoonrakerFileAPIMock(helix::IMoonrakerClient& client);
    ~MoonrakerFileAPIMock() override = default;

    void list_files(const std::string& root, const std::string& path, bool recursive,
                    FileListCallback on_success, ErrorCallback on_error) override;

    void delete_file(const std::string& filename, SuccessCallback on_success,
                     ErrorCallback on_error) override;

    /// Seed the paths list_files("config", ...) reports. See
    /// MoonrakerFileTransferAPIMock::set_config_files().
    void set_config_files(std::map<std::string, std::string> files);

    /// Every path delete_file() was asked to remove, in call order. Root-qualified
    /// exactly as the caller spelled it (e.g. "gcodes/.helix_temp/modified_1_a.gcode").
    [[nodiscard]] const std::vector<std::string>& deleted_files() const {
        return deleted_files_;
    }

  private:
    std::map<std::string, std::string> config_files_;
    std::vector<std::string> deleted_files_;
};

/**
 * @brief Mock Job API covering the endpoints MoonrakerClientMock does not serve
 *
 * Only start_modified_print() is intercepted: it reaches Moonraker through the
 * HelixPrint plugin's server.helix.print_modified, which the client mock's RPC
 * registry has no handler for. start_print() is recorded and then forwarded to
 * the real implementation, so the mock print simulation still runs.
 */
namespace helix {

class MoonrakerJobAPIMock : public MoonrakerJobAPI {
  public:
    using SuccessCallback = MoonrakerJobAPI::SuccessCallback;
    using ErrorCallback = MoonrakerJobAPI::ErrorCallback;
    using ModifiedPrintCallback = MoonrakerJobAPI::ModifiedPrintCallback;

    explicit MoonrakerJobAPIMock(helix::IMoonrakerClient& client,
                                 const helix::PrinterState* state = nullptr);
    ~MoonrakerJobAPIMock() override = default;

    /// One recorded start_modified_print() call
    struct ModifiedPrintRecord {
        std::string original_filename;
        std::string temp_file_path;
        std::vector<std::string> modifications;
    };

    void start_print(const std::string& filename, SuccessCallback on_success,
                     ErrorCallback on_error) override;

    void start_modified_print(const std::string& original_filename,
                              const std::string& temp_file_path,
                              const std::vector<std::string>& modifications,
                              ModifiedPrintCallback on_success, ErrorCallback on_error) override;

    /// Filenames handed to start_print(), in call order
    [[nodiscard]] const std::vector<std::string>& started_prints() const {
        return started_prints_;
    }

    /// Recorded start_modified_print() calls, in call order
    [[nodiscard]] const std::vector<ModifiedPrintRecord>& modified_prints() const {
        return modified_prints_;
    }

    /// Make every later start_modified_print() call on_error instead of
    /// on_success. The call is still recorded.
    void mock_fail_modified_prints(bool fail = true) {
        fail_modified_prints_ = fail;
    }

  private:
    std::vector<std::string> started_prints_;
    std::vector<ModifiedPrintRecord> modified_prints_;
    bool fail_modified_prints_ = false;
};

} // namespace helix

/**
 * @brief Mock MoonrakerAPI for testing without real printer connection
 *
 * Overrides connection and calibration methods for mock mode. The Moonraker
 * database is served by MoonrakerClientMock.
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
    // Overridden Connection/Subscription Proxies (no-ops for mock)
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

    /**
     * @brief Get the File Management mock sub-API for mock-specific access
     *
     * @return Reference to MoonrakerFileAPIMock
     */
    MoonrakerFileAPIMock& files_mock();

    /**
     * @brief Get the Job mock sub-API for mock-specific access
     *
     * @return Reference to MoonrakerJobAPIMock
     */
    helix::MoonrakerJobAPIMock& job_mock();

    /**
     * @brief Seed an in-memory "config" root for both file sub-APIs
     *
     * Wires the same map into list_files() and download_file()/upload_file() so
     * the async config-editing paths (KlipperConfigEditor, include resolution)
     * can be exercised without a printer or on-disk fixtures.
     *
     * @param files Map of path relative to the config root -> file content
     */
    void set_config_files(std::map<std::string, std::string> files);

    /// Content the config root currently holds for @p path, including anything
    /// uploaded since seeding. std::nullopt when the path is unknown.
    std::optional<std::string> get_uploaded_config(const std::string& path) const;

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
};
