// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_calibration.h"

#include "flashforge_pa_sweep.h"
#include "i_moonraker_api.h"
#include "i_moonraker_sub_apis.h"
#include "json_utils.h"
#include "pa_calibration_run.h"
#include "printer_discovery.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::pacal {
namespace {

/// One firmware that can measure pressure advance without a human judging a
/// printed part.
enum class Kind : uint8_t {
    /// One firmware command runs the whole procedure and prints the result.
    FirmwareCommand,
    /// This app drives the stock FlashForge eBoard sweep (flashforge_pa_sweep.h).
    FlashforgeSweep,
};

struct Provider {
    const char* name;
    Kind kind;

    /// Macro whose presence identifies the firmware. This is evidence, not a
    /// guess: a printer that does not define it cannot run the procedure, and
    /// the capability is reported absent rather than attempted and refused.
    /// Null when the firmware is detected by object instead.
    const char* detect_macro;

    /// Printer object whose presence identifies the firmware, for commands
    /// registered by a klippy Python extra: those never appear in the object
    /// list as "gcode_macro X". Null when the firmware is detected by macro.
    const char* detect_object;

    /// Detection that needs more than one name, or a config section. Null when
    /// detect_macro or detect_object decides.
    bool (*detect)(const PrinterDiscovery& hw);

    /// Command template. {tool} is the tool index and {temp} the nozzle
    /// temperature; a template may use either, both or neither.
    const char* gcode;

    /// Whether each tool is calibrated separately.
    bool per_tool;

    /// See Procedure: regexes over the firmware's console output.
    const char* result_pattern;
    const char* attempt_pattern;
    const char* failure_pattern;
    int expected_attempts;

    Kept kept;
    /// See Traits::firmware_runs_procedure.
    bool firmware_runs_procedure;
    int estimate_minutes;

    /// The extruder kind this firmware's machines use, which is what makes the
    /// plausibility band meaningful. Detected extruder geometry does not exist
    /// in PrinterDiscovery, and inferring it from kinematics would be a guess:
    /// the firmware entry knows its own hardware.
    SaneRange range;

    uint32_t timeout_ms;
};

/// Klipper acknowledges SET_PRESSURE_ADVANCE by echoing the applied value in
/// this shape. A firmware that installs candidates through that command prints
/// it once per candidate, so it can mark progress but never the result.
constexpr const char* kKlipperPaEcho = R"(pressure_advance:\s*([0-9]*\.?[0-9]+))";

/// Direct-drive extruders, the geometry on every machine below. The band is
/// the one every Klipper tuning guide quotes; a result outside it is not
/// rejected, only flagged as worth measuring again.
constexpr SaneRange kDirectDrive{0.02f, 0.08f, "direct drive"};

// --- Snapmaker U1 flow calibrator --------------------------------------------
//
// The U1 carries an eddy-current inductance coil per toolhead and uses it to
// measure extrusion back-pressure directly. FLOW_CALIBRATE (the U1's
// klippy/extras/flow_calibrator.py) purges in a repeating slow/fast pattern for
// each candidate K on the MOUNTED extruder, asks its native solver for the
// signed flow mismatch, root-finds the zero crossing through about five points,
// then applies the winner and saves it to flow_calibrator.json. It sets K
// through the extruder stepper directly, so no SET_PRESSURE_ADVANCE echo ever
// reaches the console. Its own lines, from that source:
//   measure k: 0.02000           once per candidate
//   Got pressure advance: 0.0412 the result
//   flow k is out of range, use default value:0.02
//   abort calibration: <reason>
//
// SM_PRINT_FLOW_CALIBRATE is the print-job wrapper around it and returns
// without a word outside a print, so it cannot drive this screen.
//
// The flow calibrator has no status object, so the firmware is recognised by
// filament_parameters, the Snapmaker extra it cannot run without.
//
// Snapmaker calls this "flow calibration", but the number it computes IS the
// Klipper pressure-advance K. See docs/devel/printers/SNAPMAKER_U1_SUPPORT.md
// § Pre-Print Flow Calibration.
constexpr Provider SNAPMAKER_U1{
    "Snapmaker U1 flow calibrator",
    Kind::FirmwareCommand,
    /*detect_macro=*/nullptr,
    "filament_parameters",
    /*detect=*/nullptr,
    "FLOW_CALIBRATE TEMP={temp}",
    /*per_tool=*/true,
    R"(Got pressure advance:\s*([0-9]*\.?[0-9]+))",
    R"(measure k:\s*([0-9]*\.?[0-9]+))",
    R"(flow k is out of range|abort calibration:)",
    /*expected_attempts=*/5,
    Kept::Permanently,
    /*firmware_runs_procedure=*/true,
    /*estimate_minutes=*/3,
    kDirectDrive,
    /*timeout_ms=*/600000, // 10 min: several purge cycles plus the heat-up
};

// --- Klipper4FlashForge eBoard PA calibrator ---------------------------------
//
// The Creator 5's closed eBoard MCU scores each candidate K live from a
// transducer stream. On the Klipper4FlashForge firmware
// (github.com/Klipper4FlashForge/firmware), FF_PA_CALIBRATE (its
// klippy/extras/ff_pa.py) replays the stock app's sweep inside Klipper: seven
// scrambled candidates per pass, smallest passing verdict wins, mean of three
// pass winners. The command reports, it does not apply:
//   ff_pa: T0 pressure_advance = 0.021667   (mean of 3 sweep winners: ...)
//
// The '=' result shape is deliberate. The sweep installs every candidate via
// SET_PRESSURE_ADVANCE, whose "pressure_advance: <k>" echo would satisfy the
// ':' shape on the FIRST candidate, minutes before the real result. Matching
// '=' skips those echoes, and they double as the per-candidate progress
// lines, each carrying the K just tried.
//
// FF_PA_CALIBRATE is registered by the [ff_pa] Python extra, so it appears in
// the object list as "ff_pa", never as a gcode_macro: hence detect_object.
//
// The sweep structurally cannot report outside its candidate table, so the
// plausibility band is that table's span (defaults 0.0100..0.0400).
constexpr SaneRange kFfCandidateSpan{0.01f, 0.04f, "direct drive"};

constexpr Provider kFlashforgeEboard{
    "Klipper4FlashForge eBoard PA calibrator",
    Kind::FirmwareCommand,
    /*detect_macro=*/nullptr,
    "ff_pa",
    /*detect=*/nullptr,
    "FF_PA_CALIBRATE TOOL={tool}",
    /*per_tool=*/true,
    R"(pressure_advance\s*=\s*([0-9]*\.?[0-9]+))",
    kKlipperPaEcho,
    /*failure_pattern=*/"",
    /*expected_attempts=*/21, // 3 winning sweeps x 7 candidates, typically
    Kept::Nothing,
    /*firmware_runs_procedure=*/true,
    /*estimate_minutes=*/5, // up to 5 sweeps of about a minute, plus heat-up
    kFfCandidateSpan,
    /*timeout_ms=*/600000, // 10 min: up to 5 sweeps (~1 min each) plus heat-up
};

// --- Stock FlashForge eBoard sweep ------------------------------------------
//
// Stock and Z-Mod Creator 5 firmware: the same eBoard scorer, but the sweep
// lives in the stock touchscreen app, which this app replaces. Klipper only
// forwards [pa_adjust]'s PA_ACTION / PA_GET to the eBoard, so this app runs
// the sweep itself (flashforge_pa_sweep.h) and installs the result for the
// session. [pa_adjust] has no status object and no help text, so its config
// section is the evidence, together with the two driver-latch pins the sweep
// needs.
bool detect_stock_eboard_sweep(const PrinterDiscovery& hw) {
    if (!hw.has_pa_adjust()) {
        return false;
    }
    const auto& objects = hw.printer_objects();
    const auto has = [&objects](std::string_view name) {
        return std::find(objects.begin(), objects.end(), name) != objects.end();
    };
    return has(flashforge::pa_sweep::kPinX) && has(flashforge::pa_sweep::kPinY);
}

constexpr Provider kFlashforgeStockSweep{
    "FlashForge eBoard sweep",
    Kind::FlashforgeSweep,
    /*detect_macro=*/nullptr,
    /*detect_object=*/nullptr,
    detect_stock_eboard_sweep,
    /*gcode=*/"",
    /*per_tool=*/true,
    /*result_pattern=*/"",
    /*attempt_pattern=*/nullptr,
    /*failure_pattern=*/"",
    /*expected_attempts=*/21, // 3 winning sweeps x 7 candidates, typically
    Kept::ThisSession,
    /*firmware_runs_procedure=*/false,
    /*estimate_minutes=*/4, // about 5 s a line, 21 to 35 lines, plus the park
    kFfCandidateSpan,
    /*timeout_ms=*/600000,
};

/// The table. One entry per firmware that can do this.
// Klipper4FlashForge machines carry [pa_adjust] too; their own command is the
// better path there, so it is matched first.
constexpr const Provider* kProviders[] = {&SNAPMAKER_U1, &kFlashforgeEboard,
                                          &kFlashforgeStockSweep};

const Provider* match(const PrinterDiscovery& hw) {
    for (const Provider* p : kProviders) {
        if (p->detect && p->detect(hw)) {
            return p;
        }
        if (p->detect_macro && hw.has_macro(p->detect_macro)) {
            return p;
        }
        if (p->detect_object) {
            const auto& objects = hw.printer_objects();
            if (std::find(objects.begin(), objects.end(), p->detect_object) != objects.end()) {
                return p;
            }
        }
    }
    return nullptr;
}

} // namespace

bool is_supported(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

std::string provider_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : std::string{};
}

bool is_per_tool(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->per_tool;
}

std::optional<Traits> traits(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (!p) {
        return std::nullopt;
    }
    Traits t;
    t.provider = p->name;
    t.per_tool = p->per_tool;
    t.kept = p->kept;
    t.firmware_runs_procedure = p->firmware_runs_procedure;
    t.expected_attempts = p->expected_attempts;
    t.estimate_minutes = p->estimate_minutes;
    t.timeout_ms = p->timeout_ms;
    return t;
}

std::optional<Procedure> procedure_for(const PrinterDiscovery& hw, int tool_index, int temp_c) {
    const Provider* p = match(hw);
    if (!p || p->kind != Kind::FirmwareCommand) {
        return std::nullopt;
    }

    Procedure proc;
    proc.provider = p->name;
    proc.start_gcode =
        fmt::format(fmt::runtime(p->gcode), fmt::arg("tool", tool_index), fmt::arg("temp", temp_c));
    proc.result_pattern = p->result_pattern;
    proc.attempt_pattern = p->attempt_pattern ? p->attempt_pattern : "";
    proc.failure_pattern = p->failure_pattern ? p->failure_pattern : "";
    proc.expected_attempts = p->expected_attempts;
    proc.applies_result = p->kept == Kept::Permanently;
    proc.estimate_minutes = p->estimate_minutes;
    proc.timeout_ms = p->timeout_ms;

    // The command word is the first token of the template, which is also what
    // Klipper quotes back in "Unknown command:".
    proc.command_word = proc.start_gcode.substr(0, proc.start_gcode.find(' '));

    return proc;
}

SaneRange sane_range(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->range : kDirectDrive;
}

bool is_plausible(const PrinterDiscovery& hw, float k) {
    const SaneRange r = sane_range(hw);
    return k >= r.low && k <= r.high;
}

// --- Runs ---------------------------------------------------------------------

namespace {

/// One firmware command: the existing console collector, behind Run. The
/// firmware finishes the command it is in whatever happens here, so stopping
/// only stops listening, and nothing is owed afterwards.
class FirmwareCommandRun final : public Run {
  public:
    void start(IMoonrakerAPI& api, const Procedure& proc, RunCallbacks cb) {
        cancel_ = api.advanced().start_pa_calibrate(
            proc,
            [cb](float k) {
                if (cb.on_result) {
                    cb.on_result(k);
                }
            },
            [cb](const MoonrakerError& err) {
                if (cb.on_error) {
                    cb.on_error(err.message);
                }
            },
            [cb](int attempt, int expected, float k_so_far) {
                if (cb.on_attempt) {
                    cb.on_attempt(Attempt{attempt, expected, k_so_far, Verdict::Unknown});
                }
            });
    }

    void stop() override {
        if (cancel_) {
            cancel_();
            cancel_ = nullptr;
        }
    }

    void before_disconnect() override {
        stop();
    }

  private:
    helix::SharedCallback<void()> cancel_;
};

} // namespace

std::shared_ptr<Run> start_run(IMoonrakerAPI& api, IMoonrakerClient& client,
                               const PrinterDiscovery& hw, const RunRequest& req,
                               RunCallbacks callbacks) {
    const Provider* p = match(hw);
    if (!p) {
        return nullptr;
    }
    switch (p->kind) {
    case Kind::FirmwareCommand: {
        const auto proc = procedure_for(hw, req.tool_index, req.temp_c);
        if (!proc) {
            return nullptr;
        }
        spdlog::info("[pacal] Measuring via {}: {}", proc->provider, proc->start_gcode);
        auto run = std::make_shared<FirmwareCommandRun>();
        run->start(api, *proc, std::move(callbacks));
        return run;
    }
    case Kind::FlashforgeSweep:
        spdlog::info("[pacal] Measuring via {} on T{}", p->name, req.tool_index);
        return flashforge::pa_sweep::start_run(api, client, req, std::move(callbacks));
    }
    return nullptr;
}

std::vector<std::string> recovery_status_objects(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (p && p->kind == Kind::FlashforgeSweep) {
        return {std::string(flashforge::pa_sweep::kPinX), std::string(flashforge::pa_sweep::kPinY)};
    }
    return {};
}

std::string recovery_gcode(const PrinterDiscovery& hw, const nlohmann::json& status) {
    const Provider* p = match(hw);
    if (!p || p->kind != Kind::FlashforgeSweep || !status.is_object()) {
        return {};
    }
    const auto pin = [&status](std::string_view name) {
        const auto it = status.find(std::string(name));
        if (it == status.end() || !it->is_object()) {
            return 0.f;
        }
        return json_util::safe_float(*it, "value", 0.f);
    };
    return flashforge::pa_sweep::latch_recovery_gcode(pin(flashforge::pa_sweep::kPinX),
                                                      pin(flashforge::pa_sweep::kPinY));
}

} // namespace helix::pacal
