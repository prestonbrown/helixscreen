// SPDX-License-Identifier: GPL-3.0-or-later

// The HelixPrint plugin's power-loss endpoints (server.helix.plr.*), so the
// resume wizard can be driven under --test. Off unless HELIX_MOCK_PLR names a
// backend and HELIX_MOCK_HELIX_PLUGIN reports the plugin installed; the knobs
// are documented in docs/devel/MOCK_ENVIRONMENT_VARIABLES.md.

#include "core/text_io.h"
#include "env_knobs.h"
#include "mock_planted_gcodes.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"
#include "plr_backend.h"
#include "plr_profile.h"
#include "plr_wizard.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>

using helix::PlrConfirmation;

namespace {

constexpr const char* kMockPlrFile = "3DBenchy.gcode";
constexpr int kMockPlrLayer = 42;
constexpr double kMockPlrZ = 8.4;
constexpr double kMockPlrOutageS = 720.0;
constexpr double kMockPlrFileFraction = 0.42;
constexpr float kMockPlrBedTarget = 60.0f;
/// HELIX_MOCK_PLR_WARNINGS=cold_bed: the bed reads this far under target.
constexpr float kMockPlrColdBedDropC = 30.0f;
constexpr uint32_t kMockPlrStepIntervalMs = 1500;

struct MockPlrConfig {
    bool active = false;
    std::string backend;
    int tier = 2;
    std::vector<std::string> warnings;
    std::string fail;
};

MockPlrConfig mock_plr_config() {
    MockPlrConfig cfg;
    const char* plugin = std::getenv("HELIX_MOCK_HELIX_PLUGIN");
    const bool plugin_up =
        plugin != nullptr && std::string(plugin) != "disabled" && helix::env_truthy(plugin);
    const char* backend = std::getenv("HELIX_MOCK_PLR");
    if (!plugin_up || backend == nullptr) {
        return cfg;
    }
    cfg.backend = backend;
    if (cfg.backend != "generic" && cfg.backend != "qidi_print_state") {
        return cfg;
    }
    cfg.active = true;
    if (const char* tier = std::getenv("HELIX_MOCK_PLR_TIER")) {
        cfg.tier = helix::text_io::parse_leading<int>(tier).value_or(2) == 1 ? 1 : 2;
    }
    if (const char* warnings = std::getenv("HELIX_MOCK_PLR_WARNINGS")) {
        std::string list = warnings;
        size_t start = 0;
        while (start <= list.size()) {
            size_t comma = list.find(',', start);
            std::string item(helix::text_io::trim(list.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start)));
            if (!item.empty()) {
                cfg.warnings.push_back(item);
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
    }
    if (const char* fail = std::getenv("HELIX_MOCK_PLR_FAIL")) {
        cfg.fail = fail;
    }
    return cfg;
}

bool has_warning(const MockPlrConfig& cfg, const char* name) {
    return std::find(cfg.warnings.begin(), cfg.warnings.end(), name) != cfg.warnings.end();
}

/// The plugin refuses with a coded Moonraker error; the code leads the message.
MoonrakerError plr_error(const std::string& method, const char* code, const std::string& text) {
    MoonrakerError err;
    err.type = MoonrakerErrorType::JSON_RPC_ERROR;
    err.code = 400;
    err.method = method;
    err.message = fmt::format("{}: {}", code, text);
    return err;
}

std::vector<PlrConfirmation> mock_requires(const MockPlrConfig& cfg) {
    helix::PlrPlanView view;
    view.tier = cfg.tier;
    view.warnings = cfg.warnings;
    view.bed_target = kMockPlrBedTarget;
    view.bed_current =
        has_warning(cfg, "cold_bed") ? kMockPlrBedTarget - kMockPlrColdBedDropC : kMockPlrBedTarget;
    return helix::plr_required_confirmations(view, helix::plr_plan_bed_state(view));
}

std::int64_t mock_file_size() {
    struct stat st {};
    const std::string path = helix::mock::gcode_disk_path(kMockPlrFile);
    return stat(path.c_str(), &st) == 0 ? static_cast<std::int64_t>(st.st_size) : 0;
}

} // namespace

json MoonrakerClientMock::mock_plr_feature_entry() {
    if (!mock_plr_config().active) {
        return nullptr;
    }
    return json{{"version", helix::kPlrFeatureMinVersion},
                {"available", true},
                {"enabled", mock_plr_saving() || mock_plr_interrupted()},
                {"saving", mock_plr_saving()},
                {"reason", nullptr}};
}

bool MoonrakerClientMock::mock_plr_saving() {
    const json user = mock_db_get(helix::kPlrDbNamespace, helix::kPlrUserKey);
    return !(user.is_object() && user.contains("enabled") && user["enabled"].is_boolean()) ||
           user["enabled"].get<bool>();
}

bool MoonrakerClientMock::mock_plr_interrupted() const {
    return mock_plr_config().active && !plr_snapshot_cleared_;
}

void MoonrakerClientMock::mock_plr_request(
    const std::string& method, const json& params,
    helix::SharedCallback<void(const json&)> success_cb,
    helix::SharedCallback<void(const MoonrakerError&)> error_cb) {
    const MockPlrConfig cfg = mock_plr_config();
    auto reply = [success_cb](json result) {
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", std::move(result)}});
        }
    };
    auto fail = [error_cb](MoonrakerError err) {
        spdlog::info("[MoonrakerClientMock] PLR refusing: {}", err.message);
        if (error_cb) {
            error_cb(err);
        }
    };
    if (method == helix::kPlrMeshIncludeRpc) {
        // The config-file question stands without the outage simulation, so
        // it answers even when no PLR backend is on.
        const bool had = plr_mesh_include_;
        const std::string action =
            params.is_object() && params.contains("action") && params["action"].is_string()
                ? params["action"].get<std::string>()
                : std::string();
        if (action == "add" || action == "remove") {
            plr_mesh_include_ = action == "add";
            reply({{"present", plr_mesh_include_}, {"changed", plr_mesh_include_ != had}});
            return;
        }
        reply({{"present", plr_mesh_include_}});
        return;
    }

    if (!cfg.active) {
        fail(MoonrakerError::unknown("Method not found", method));
        return;
    }
    const bool snapshot = !plr_snapshot_cleared_;

    if (method == helix::kPlrStatusRpc) {
        const double now =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        json snap = nullptr;
        if (snapshot) {
            snap = {{"job_id", "mock_job_000"},
                    {"filename", kMockPlrFile},
                    {"layer", kMockPlrLayer},
                    {"z", kMockPlrZ},
                    {"saved_at", now - kMockPlrOutageS},
                    {"age_s", kMockPlrOutageS}};
        }
        json blocked_by = json::array();
        if (!snapshot) {
            blocked_by.push_back("no_snapshot");
        }
        if (cfg.fail == "no_lift_room") {
            blocked_by.push_back("no_lift_room");
        }
        reply({{"saving", mock_plr_saving()},
               {"storage",
                {{"storage_class", "emmc"},
                 {"wear_tolerant", true},
                 {"source", "runtime"},
                 {"device", "mmcblk0"}}},
               {"tier", cfg.tier},
               {"backend", cfg.backend},
               {"snapshot", snap},
               {"offer", {{"available", blocked_by.empty()}, {"blocked_by", blocked_by}}}});
        return;
    }

    if (method == helix::kPlrDiscardRpc) {
        plr_snapshot_cleared_ = true;
        plr_plan_id_.clear();
        reply({{"ok", true}});
        return;
    }

    if (method == helix::kPlrPlanRpc) {
        if (cfg.fail == "plan") {
            fail(plr_error(method, "plr_klippy_error", "Mock Klipper error (plan)"));
            return;
        }
        if (!snapshot) {
            fail(plr_error(method, "plr_no_plan", "No power-loss snapshot"));
            return;
        }
        if (cfg.fail == "plan_background" || cfg.fail == "plan_loop") {
            // A long scan answers pending, then reports through plan_ready:
            // with an error, or (plan_loop) with nothing a re-request can use.
            const bool with_error = cfg.fail == "plan_background";
            play_console_steps(
                {[this, with_error]() {
                    json ready = {{"plan_id", with_error ? "" : "mock-plan-pending"}};
                    if (with_error) {
                        ready["error"] = "multi_material: the file uses 2 tools";
                    }
                    dispatch_method_callback(helix::kPlrPlanReadyNotify,
                                             json{{"jsonrpc", "2.0"},
                                                  {"method", helix::kPlrPlanReadyNotify},
                                                  {"params", {ready}}});
                }},
                kMockPlrStepIntervalMs, []() {});
            reply({{"pending", true}});
            return;
        }
        plr_plan_id_ = fmt::format("mock-plan-{}", ++plr_plan_seq_);
        json requires_list = json::array();
        for (PlrConfirmation c : mock_requires(cfg)) {
            requires_list.push_back(helix::plr_confirmation_name(c));
        }
        const float bed_current = has_warning(cfg, "cold_bed")
                                      ? kMockPlrBedTarget - kMockPlrColdBedDropC
                                      : kMockPlrBedTarget;
        reply(
            {{"plan_id", plr_plan_id_},
             {"filename", kMockPlrFile},
             {"method", has_warning(cfg, "layer_start") ? "layer_start" : "measured"},
             {"resume_offset", static_cast<std::int64_t>(mock_file_size() * kMockPlrFileFraction)},
             {"layer", kMockPlrLayer},
             {"z_gcode", kMockPlrZ},
             {"z_physical", kMockPlrZ + 0.12},
             {"retrace_estimate_mm", 3.2},
             {"bed", {{"target", kMockPlrBedTarget}, {"current", bed_current}}},
             {"restores", {"mesh", "pressure_advance", "fans"}},
             {"warnings", cfg.warnings},
             {"requires", requires_list},
             {"tier", cfg.tier}});
        return;
    }

    const std::string plan_id =
        params.is_object() && params.contains("plan_id") && params["plan_id"].is_string()
            ? params["plan_id"].get<std::string>()
            : std::string();
    if (plan_id.empty() || plan_id != plr_plan_id_) {
        fail(plr_error(method, "plr_stale_plan", "Not the latest plan"));
        return;
    }

    if (method == helix::kPlrAbortRpc) {
        set_bed_target(0.0);
        reply({{"ok", true}});
        return;
    }

    auto progress = [this, plan_id](const char* step) {
        return [this, plan_id, step]() {
            dispatch_method_callback(
                helix::kPlrProgressNotify,
                json{{"jsonrpc", "2.0"},
                     {"method", helix::kPlrProgressNotify},
                     {"params", {{{"plan_id", plan_id}, {"step", step}, {"state", "running"}}}}});
        };
    };

    if (method == helix::kPlrPrepareRpc) {
        if (cfg.fail == "prepare") {
            fail(plr_error(method, "plr_klippy_error", "Mock Klipper error (prepare)"));
            return;
        }
        set_bed_target(kMockPlrBedTarget);
        // Bed and nozzle heat where the nozzle stopped before anything moves,
        // and the nozzle drops to standby once it has let go; only the generic
        // backend's prepare moves the toolhead.
        std::vector<helix::SharedCallback<void()>> steps = {progress("bed_wait"),
                                                            progress("nozzle_in_place")};
        if (cfg.backend == "generic") {
            steps.push_back(progress("lift"));
            steps.push_back(progress("nozzle_standby"));
            steps.push_back(progress("home_xy"));
            if (cfg.tier == 1) {
                steps.push_back(progress("position"));
            }
        } else {
            steps.push_back(progress("nozzle_standby"));
        }
        play_console_steps(std::move(steps), kMockPlrStepIntervalMs,
                           [reply]() { reply({{"ok", true}}); });
        return;
    }

    if (method == helix::kPlrCommitRpc) {
        if (cfg.fail == "commit") {
            fail(plr_error(method, "plr_klippy_error", "Mock Klipper error (commit)"));
            return;
        }
        const json* given = params.contains("confirmations") ? &params["confirmations"] : nullptr;
        for (PlrConfirmation c : mock_requires(cfg)) {
            const char* name = helix::plr_confirmation_name(c);
            if (given == nullptr || !given->is_object() || !given->contains(name)) {
                fail(plr_error(method, "plr_missing_confirmation", name));
                return;
            }
        }
        // The plugin re-heats the bed after restoring the print state.
        std::vector<helix::SharedCallback<void()>> steps = {
            progress("z_reference"), progress("restore"), progress("bed_wait"),
            progress("heat_nozzle"), progress("purge"),   progress("move_to_resume"),
            progress("start")};
        play_console_steps(std::move(steps), kMockPlrStepIntervalMs, [this, reply]() {
            plr_snapshot_cleared_ = true;
            plr_plan_id_.clear();
            if (start_print_internal(kMockPlrFile)) {
                print_progress_.store(kMockPlrFileFraction);
            }
            reply({{"ok", true}, {"job_started", true}});
        });
        return;
    }

    fail(MoonrakerError::unknown("Method not found", method));
}

namespace mock_internal {

// NAMESPACE_OK: mock_internal handler registry, alongside its sibling registrars
void register_plr_handlers(std::unordered_map<std::string, MethodHandler>& registry) {
    for (const char* method :
         {helix::kPlrStatusRpc, helix::kPlrPlanRpc, helix::kPlrPrepareRpc, helix::kPlrCommitRpc,
          helix::kPlrAbortRpc, helix::kPlrDiscardRpc, helix::kPlrMeshIncludeRpc}) {
        registry[method] =
            [method](MoonrakerClientMock* self, const json& params,
                     helix::SharedCallback<void(const json&)> success_cb,
                     helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
            self->mock_plr_request(method, params, std::move(success_cb), std::move(error_cb));
            return true;
        };
    }
}

} // namespace mock_internal
