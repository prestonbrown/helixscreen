// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file i_moonraker_sub_apis.h
 * @brief Pure-virtual interfaces for the ten Moonraker sub-APIs
 *
 * Mirrors the public surface of MoonrakerMotionAPI, MoonrakerJobAPI,
 * MoonrakerFileAPI, MoonrakerQueueAPI, MoonrakerHistoryAPI,
 * MoonrakerAdvancedAPI, MoonrakerRestAPI, MoonrakerFileTransferAPI,
 * MoonrakerSpoolmanAPI, and MoonrakerTimelapseAPI. Each concrete class
 * additionally inherits the matching interface here; production and test
 * consumers that only need polymorphic access can depend on the interface
 * instead of the concrete type.
 *
 * Two small types (JobQueueEntry/JobQueueStatus, IAdvancedAPI::MPCResult)
 * live here rather than in their originating concrete headers because the
 * concrete classes need them for method signatures declared on the
 * interface — see moonraker_queue_api.h and moonraker_advanced_api.h for
 * the compatibility aliases that keep existing qualified references
 * (e.g. `MoonrakerAdvancedAPI::MPCResult`) working unchanged.
 */

#pragma once

#include "advanced_panel_types.h"
#include "belt_tension_types.h"
#include "calibration_types.h"
#include "json_fwd.h"
#include "moonraker_error.h"
#include "moonraker_types.h"
#include "print_history_data.h"
#include "spoolman_types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

/**
 * @brief A single entry in the Moonraker job queue
 */
struct JobQueueEntry {
    std::string job_id;   ///< Unique job identifier
    std::string filename; ///< G-code filename
    double time_added;    ///< Unix timestamp when job was added
    double time_in_queue; ///< Seconds the job has been in queue
};

/**
 * @brief Status of the Moonraker job queue
 */
struct JobQueueStatus {
    std::string queue_state;                ///< "ready", "paused", "loading"
    std::vector<JobQueueEntry> queued_jobs; ///< Jobs currently in queue
};

/**
 * @brief Motion Control API operations via Moonraker
 *
 * Mirrors MoonrakerMotionAPI (include/moonraker_motion_api.h).
 */
class IMotionAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    virtual ~IMotionAPI() = default;

    virtual void home_axes(const std::string& axes, SuccessCallback on_success,
                           ErrorCallback on_error) = 0;

    virtual void move_axis(char axis, double distance, double feedrate, SuccessCallback on_success,
                           ErrorCallback on_error) = 0;

    /// Relative multi-axis move: XY combined on one G0, Z on its own. Zero
    /// deltas complete immediately without an RPC. Used by the jog coalescer.
    virtual void move_relative(double dx, double dy, double dz, double xy_feedrate,
                               double z_feedrate, SuccessCallback on_success,
                               ErrorCallback on_error) = 0;

    virtual void move_to_position(char axis, double position, double feedrate,
                                  SuccessCallback on_success, ErrorCallback on_error) = 0;
};

/**
 * @brief Print Job Control API operations via Moonraker
 *
 * Mirrors MoonrakerJobAPI (include/moonraker_job_api.h).
 */
class IJobAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using BoolCallback = std::function<void(bool)>;
    using ModifiedPrintCallback = std::function<void(const ModifiedPrintResult&)>;

    virtual ~IJobAPI() = default;

    virtual void start_print(const std::string& filename, SuccessCallback on_success,
                             ErrorCallback on_error) = 0;

    virtual void start_modified_print(const std::string& original_filename,
                                      const std::string& temp_file_path,
                                      const std::vector<std::string>& modifications,
                                      ModifiedPrintCallback on_success, ErrorCallback on_error) = 0;

    virtual void check_helix_plugin(BoolCallback on_result, ErrorCallback on_error) = 0;

    virtual void pause_print(SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void resume_print(SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void cancel_print(SuccessCallback on_success, ErrorCallback on_error) = 0;
};

/**
 * @brief File Management API operations via Moonraker
 *
 * Mirrors MoonrakerFileAPI (include/moonraker_file_api.h).
 */
class IFilesAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using FileListCallback = std::function<void(const std::vector<FileInfo>&)>;
    using FileMetadataCallback = std::function<void(const FileMetadata&)>;
    using FileRootsCallback = std::function<void(const std::vector<FileRoot>&)>;

    virtual ~IFilesAPI() = default;

    /**
     * @brief List the file manager's roots and where they live on disk
     *
     * server.files.roots is the only Moonraker call that gives an absolute path for
     * a writable directory. Everything else it reports about its own config is
     * relative to a root the caller is trying to identify in the first place, so
     * without this an absolute `[include /some/where.conf]` cannot be told apart
     * from an unreachable one (stock Creality K2).
     */
    virtual void get_file_roots(FileRootsCallback on_success, ErrorCallback on_error) = 0;

    virtual void list_files(const std::string& root, const std::string& path, bool recursive,
                            FileListCallback on_success, ErrorCallback on_error) = 0;

    virtual void get_directory(const std::string& root, const std::string& path,
                               FileListCallback on_success, ErrorCallback on_error) = 0;

    virtual void get_file_metadata(const std::string& filename, FileMetadataCallback on_success,
                                   ErrorCallback on_error, bool silent = false) = 0;

    virtual void metascan_file(const std::string& filename, FileMetadataCallback on_success,
                               ErrorCallback on_error, bool silent = true) = 0;

    virtual void delete_file(const std::string& filename, SuccessCallback on_success,
                             ErrorCallback on_error) = 0;

    virtual void move_file(const std::string& source, const std::string& dest,
                           SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void copy_file(const std::string& source, const std::string& dest,
                           SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void create_directory(const std::string& path, SuccessCallback on_success,
                                  ErrorCallback on_error) = 0;

    virtual void delete_directory(const std::string& path, bool force, SuccessCallback on_success,
                                  ErrorCallback on_error) = 0;
};

/**
 * @brief Job Queue API operations via Moonraker
 *
 * Mirrors MoonrakerQueueAPI (include/moonraker_queue_api.h).
 */
class IQueueAPI {
  public:
    using StatusCallback = std::function<void(const JobQueueStatus&)>;
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    virtual ~IQueueAPI() = default;

    virtual void get_queue_status(StatusCallback on_success, ErrorCallback on_error) = 0;

    virtual void start_queue(SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void pause_queue(SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void add_job(const std::string& filename, SuccessCallback on_success,
                         ErrorCallback on_error) = 0;

    virtual void remove_jobs(const std::vector<std::string>& job_ids, SuccessCallback on_success,
                             ErrorCallback on_error) = 0;
};

/**
 * @brief Print History API operations via Moonraker
 *
 * Mirrors MoonrakerHistoryAPI (include/moonraker_history_api.h).
 */
class IHistoryAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using HistoryListCallback =
        std::function<void(const std::vector<PrintHistoryJob>&, uint64_t total_count)>;
    using HistoryTotalsCallback = std::function<void(const PrintHistoryTotals&)>;

    virtual ~IHistoryAPI() = default;

    virtual void get_history_list(int limit, int start, double since, double before,
                                  HistoryListCallback on_success, ErrorCallback on_error) = 0;

    virtual void get_history_totals(HistoryTotalsCallback on_success, ErrorCallback on_error) = 0;

    virtual void delete_history_job(const std::string& job_id, SuccessCallback on_success,
                                    ErrorCallback on_error) = 0;
};

/**
 * @brief Advanced Panel Operations API via Moonraker
 *
 * Mirrors MoonrakerAdvancedAPI (include/moonraker_advanced_api.h).
 *
 * @note MPCResult lives here (not on the concrete class) because
 * MoonrakerAdvancedAPI must inherit IAdvancedAPI, which needs the type for
 * start_mpc_calibrate()'s callback signature. MoonrakerAdvancedAPI carries
 * a `using MPCResult = IAdvancedAPI::MPCResult;` alias so existing qualified
 * references (`MoonrakerAdvancedAPI::MPCResult`) keep resolving.
 */
class IAdvancedAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using BedMeshProgressCallback = std::function<void(int current, int total)>;
    using NoiseCheckCallback = std::function<void(float noise_level)>;
    using InputShaperConfigCallback = std::function<void(const InputShaperConfig&)>;
    using HeaterControlTypeCallback = std::function<void(const std::string& control_type)>;
    using PIDProgressCallback = std::function<void(int sample, float tolerance)>;
    using PIDCalibrateCallback = std::function<void(float kp, float ki, float kd)>;

    /// Result struct for MPC calibration
    struct MPCResult {
        float block_heat_capacity = 0;
        float sensor_responsiveness = 0;
        float ambient_transfer = 0;
        std::string fan_ambient_transfer; // Comma-separated values like "0.12, 0.18, 0.25"
    };

    using MPCCalibrateCallback = std::function<void(const MPCResult&)>;
    using MPCProgressCallback =
        std::function<void(int phase, int total_phases, const std::string& description)>;
    using BeltResonanceCallback = std::function<void(const std::string& csv_path)>;
    using BeltHardwareCallback =
        std::function<void(const helix::calibration::BeltTensionHardware&)>;

    virtual ~IAdvancedAPI() = default;

    virtual const BedMeshProfile* get_active_bed_mesh() const = 0;

    virtual void update_bed_mesh(const json& bed_mesh_data) = 0;

    virtual std::vector<std::string> get_bed_mesh_profiles() const = 0;

    virtual bool has_bed_mesh() const = 0;

    virtual const BedMeshProfile* get_bed_mesh_profile(const std::string& profile_name) const = 0;

    virtual void get_excluded_objects(std::function<void(const std::set<std::string>&)> on_success,
                                      ErrorCallback on_error) = 0;

    virtual void
    get_available_objects(std::function<void(const std::vector<std::string>&)> on_success,
                          ErrorCallback on_error) = 0;

    virtual void start_bed_mesh_calibrate(BedMeshProgressCallback on_progress,
                                          SuccessCallback on_complete, ErrorCallback on_error,
                                          int expected_probes = 0, int probe_samples = 1) = 0;

    virtual void calculate_screws_tilt(helix::ScrewTiltCallback on_success,
                                       ErrorCallback on_error) = 0;

    virtual void run_qgl(SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void run_z_tilt_adjust(SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void start_resonance_test(char axis, helix::ShaperProgressCallback on_progress,
                                      helix::InputShaperCallback on_complete,
                                      ErrorCallback on_error) = 0;

    virtual void start_klippain_shaper_calibration(const std::string& axis,
                                                   SuccessCallback on_success,
                                                   ErrorCallback on_error) = 0;

    virtual void set_input_shaper(char axis, const std::string& shaper_type, double freq_hz,
                                  SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void measure_axes_noise(NoiseCheckCallback on_complete, ErrorCallback on_error) = 0;

    virtual void get_input_shaper_config(InputShaperConfigCallback on_success,
                                         ErrorCallback on_error) = 0;

    virtual void get_heater_pid_values(const std::string& heater, PIDCalibrateCallback on_complete,
                                       ErrorCallback on_error) = 0;

    virtual void get_heater_control_type(const std::string& heater,
                                         HeaterControlTypeCallback on_complete,
                                         ErrorCallback on_error) = 0;

    virtual void start_pid_calibrate(const std::string& heater, int target_temp,
                                     PIDCalibrateCallback on_complete, ErrorCallback on_error,
                                     PIDProgressCallback on_progress = nullptr) = 0;

    virtual void start_mpc_calibrate(const std::string& heater, int target_temp,
                                     int fan_breakpoints, MPCCalibrateCallback on_complete,
                                     ErrorCallback on_error,
                                     MPCProgressCallback on_progress = nullptr) = 0;

    virtual void get_machine_limits(helix::MachineLimitsCallback on_success,
                                    ErrorCallback on_error) = 0;

    virtual void set_machine_limits(const MachineLimits& limits, SuccessCallback on_success,
                                    ErrorCallback on_error) = 0;

    virtual void save_config(SuccessCallback on_success, ErrorCallback on_error) = 0;

    /// @param suppress_auto_toast Maps to helix::rpc_error_policy::CallerIntent::silent
    ///        — opts out of the tracker's generic fallback toast, nothing more.
    virtual void execute_macro(const std::string& name,
                               const std::map<std::string, std::string>& params,
                               SuccessCallback on_success, ErrorCallback on_error,
                               uint32_t timeout_ms = 0, bool suppress_auto_toast = false) = 0;

    virtual std::vector<MacroInfo> get_user_macros(bool include_system = false) const = 0;

    virtual void detect_belt_hardware(BeltHardwareCallback on_complete, ErrorCallback on_error) = 0;

    virtual void test_belt_resonance(const std::string& axis_param, const std::string& output_name,
                                     helix::AdvancedProgressCallback on_progress,
                                     BeltResonanceCallback on_complete, ErrorCallback on_error) = 0;

    virtual void excite_belt_at_frequency(const std::string& axis_param, float freq_hz,
                                          SuccessCallback on_complete, ErrorCallback on_error) = 0;

    virtual void download_accel_csv(const std::string& filename,
                                    std::function<void(const std::string& csv_data)> on_complete,
                                    ErrorCallback on_error) = 0;
};

/**
 * @brief Generic REST Endpoint and WLED Control API operations via Moonraker
 *
 * Mirrors MoonrakerRestAPI (include/moonraker_rest_api.h).
 */
class IRestAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using RestCallback = std::function<void(const RestResponse&)>;

    virtual ~IRestAPI() = default;

    virtual void call_rest_get(const std::string& endpoint, RestCallback on_complete) = 0;

    virtual void call_rest_post(const std::string& endpoint, const json& params,
                                RestCallback on_complete) = 0;

    virtual void wled_get_strips(RestCallback on_success, ErrorCallback on_error) = 0;

    virtual void wled_set_strip(const std::string& strip, const std::string& action, int brightness,
                                int preset, SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void wled_get_status(RestCallback on_success, ErrorCallback on_error) = 0;

    virtual void get_server_config(RestCallback on_success, ErrorCallback on_error) = 0;
};

/**
 * @brief HTTP File Transfer API operations via Moonraker
 *
 * Mirrors MoonrakerFileTransferAPI (include/moonraker_file_transfer_api.h).
 */
class ITransfersAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using StringCallback = std::function<void(const std::string&)>;
    using ProgressCallback = std::function<void(size_t current, size_t total)>;

    virtual ~ITransfersAPI() = default;

    virtual void download_file(const std::string& root, const std::string& path,
                               StringCallback on_success, ErrorCallback on_error) = 0;

    virtual void download_file_partial(const std::string& root, const std::string& path,
                                       size_t max_bytes, StringCallback on_success,
                                       ErrorCallback on_error) = 0;

    /// Download only the LAST @p max_bytes of a file (HTTP suffix range).
    ///
    /// The head-range sibling above is for slicer preambles. Slicers that write
    /// their settings block as a footer put it here instead: on an OrcaSlicer
    /// file every `filament_colour` / `extruder_colour` key sits in the last
    /// ~21 KB of a 13 MB print, so a suffix range answers "what colours does
    /// this file use" for the price of one small request rather than a full
    /// download and parse.
    ///
    /// Returns fewer bytes than asked when the file is shorter; a server that
    /// ignores Range and sends the whole file still yields correct content.
    virtual void download_file_tail(const std::string& root, const std::string& path,
                                    size_t max_bytes, StringCallback on_success,
                                    ErrorCallback on_error) = 0;

    virtual void download_file_to_path(const std::string& root, const std::string& path,
                                       const std::string& dest_path, StringCallback on_success,
                                       ErrorCallback on_error,
                                       ProgressCallback on_progress = nullptr) = 0;

    virtual void download_thumbnail(const std::string& thumbnail_path,
                                    const std::string& cache_path, StringCallback on_success,
                                    ErrorCallback on_error) = 0;

    virtual void upload_file(const std::string& root, const std::string& path,
                             const std::string& content, SuccessCallback on_success,
                             ErrorCallback on_error) = 0;

    virtual void upload_file_with_name(const std::string& root, const std::string& path,
                                       const std::string& filename, const std::string& content,
                                       SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void upload_file_from_path(const std::string& root, const std::string& dest_path,
                                       const std::string& local_path, SuccessCallback on_success,
                                       ErrorCallback on_error,
                                       ProgressCallback on_progress = nullptr) = 0;
};

/**
 * @brief Spoolman API operations via Moonraker's server.spoolman.proxy
 *
 * Mirrors MoonrakerSpoolmanAPI (include/moonraker_spoolman_api.h).
 */
class ISpoolmanAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    virtual ~ISpoolmanAPI() = default;

    virtual void
    get_spoolman_status(std::function<void(bool connected, int active_spool_id)> on_success,
                        ErrorCallback on_error, bool silent = false) = 0;

    virtual void get_spoolman_spools(helix::SpoolListCallback on_success,
                                     ErrorCallback on_error) = 0;

    virtual void get_spoolman_spool(int spool_id, helix::SpoolCallback on_success,
                                    ErrorCallback on_error, bool silent = false) = 0;

    virtual void set_active_spool(int spool_id, SuccessCallback on_success,
                                  ErrorCallback on_error) = 0;

    virtual void
    get_spool_usage_history(int spool_id,
                            std::function<void(const std::vector<FilamentUsageRecord>&)> on_success,
                            ErrorCallback on_error) = 0;

    virtual void update_spoolman_spool_weight(int spool_id, double remaining_weight_g,
                                              SuccessCallback on_success,
                                              ErrorCallback on_error) = 0;

    virtual void update_spoolman_spool(int spool_id, const nlohmann::json& spool_data,
                                       SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void update_spoolman_filament(int filament_id, const nlohmann::json& filament_data,
                                          SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void update_spoolman_filament_color(int filament_id, const std::string& color_hex,
                                                SuccessCallback on_success,
                                                ErrorCallback on_error) = 0;

    virtual void get_spoolman_vendors(helix::VendorListCallback on_success,
                                      ErrorCallback on_error) = 0;

    virtual void get_spoolman_filaments(helix::FilamentListCallback on_success,
                                        ErrorCallback on_error) = 0;

    virtual void get_spoolman_filaments(int vendor_id, helix::FilamentListCallback on_success,
                                        ErrorCallback on_error) = 0;

    virtual void create_spoolman_vendor(const nlohmann::json& vendor_data,
                                        helix::VendorCreateCallback on_success,
                                        ErrorCallback on_error) = 0;

    virtual void create_spoolman_filament(const nlohmann::json& filament_data,
                                          helix::FilamentCreateCallback on_success,
                                          ErrorCallback on_error) = 0;

    virtual void create_spoolman_spool(const nlohmann::json& spool_data,
                                       helix::SpoolCreateCallback on_success,
                                       ErrorCallback on_error) = 0;

    virtual void delete_spoolman_spool(int spool_id, SuccessCallback on_success,
                                       ErrorCallback on_error) = 0;

    virtual void delete_spoolman_vendor(int vendor_id, SuccessCallback on_success,
                                        ErrorCallback on_error) = 0;

    virtual void delete_spoolman_filament(int filament_id, SuccessCallback on_success,
                                          ErrorCallback on_error) = 0;

    virtual void get_spoolman_external_vendors(helix::VendorListCallback on_success,
                                               ErrorCallback on_error) = 0;

    virtual void get_spoolman_external_filaments(const std::string& vendor_name,
                                                 helix::FilamentListCallback on_success,
                                                 ErrorCallback on_error) = 0;
};

/**
 * @brief Timelapse & Webcam API operations via Moonraker
 *
 * Mirrors MoonrakerTimelapseAPI (include/moonraker_timelapse_api.h).
 */
class ITimelapseAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using TimelapseSettingsCallback = std::function<void(const TimelapseSettings&)>;
    using WebcamListCallback = std::function<void(const std::vector<WebcamInfo>&)>;

    virtual ~ITimelapseAPI() = default;

    virtual void get_timelapse_settings(TimelapseSettingsCallback on_success,
                                        ErrorCallback on_error) = 0;

    virtual void set_timelapse_settings(const TimelapseSettings& settings,
                                        SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void set_timelapse_enabled(bool enabled, SuccessCallback on_success,
                                       ErrorCallback on_error) = 0;

    virtual void render_timelapse(SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void save_timelapse_frames(SuccessCallback on_success, ErrorCallback on_error) = 0;

    virtual void get_last_frame_info(std::function<void(const LastFrameInfo&)> on_success,
                                     ErrorCallback on_error) = 0;

    virtual void get_webcam_list(WebcamListCallback on_success, ErrorCallback on_error) = 0;
};
