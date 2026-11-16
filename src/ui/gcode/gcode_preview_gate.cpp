// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_preview_gate.h"

#include "app_globals.h"
#include "board_fingerprint.h"
#include "config.h"
#include "gcode_preview_cost.h"
#include "gcode_preview_setup.h"
#include "memory_utils.h"
#include "printer_state.h"

#if HELIX_HAS_GCODE_VIEWER
#include "helix_version.h"
#include "platform_capabilities.h"
#endif

#include <spdlog/spdlog.h>

namespace helix::ui {

std::optional<PreviewRateEntry> parse_preview_rate(const nlohmann::json* node) {
    if (node == nullptr || !node->is_object()) {
        return std::nullopt;
    }
    const auto rate = node->find("rate");
    const auto key = parse_measurement_key(*node);
    if (rate == node->end() || !rate->is_number() || !key) {
        return std::nullopt;
    }
    const double value = rate->get<double>();
    if (!(value > 0.0)) {
        return std::nullopt;
    }
    return PreviewRateEntry{value, key->board, key->version};
}

nlohmann::json preview_rate_json(const PreviewRateEntry& entry) {
    nlohmann::json node{{"rate", entry.mb_per_cpu_s}};
    write_measurement_key(node, {entry.board, entry.version});
    return node;
}

double learned_preview_rate(const helix::Config& config, const std::string& board,
                            const std::string& version) {
    const auto entry = parse_preview_rate(config.try_get_json(kPreviewRatePath));
    if (!entry || entry->board != board || entry->version != version) {
        return 0.0;
    }
    return entry->mb_per_cpu_s;
}

void save_preview_rate(helix::Config& config, const PreviewRateEntry& entry) {
    config.set<nlohmann::json>(kPreviewRatePath, preview_rate_json(entry));
    config.save();
}

PreviewGate decide_preview_gate(uint64_t bytes, const PreviewGateInputs& in) {
    if (!in.memory_safe) {
        return PreviewGate::TooLarge;
    }
    if (in.auto_mode && helix::gcode::judge_preview(bytes, in.learned_rate, in.printing) ==
                            helix::gcode::PreviewVerdict::TooExpensive) {
        return PreviewGate::TooExpensive;
    }
    return PreviewGate::Build;
}

// A build without the viewer never streams a preview, so it has no rate to learn
// or key: the board and version are empty, the gate never reads a rate and
// record_preview_rate() is inert.
const std::string& preview_rate_board() {
    static const std::string board = [] {
#if HELIX_HAS_GCODE_VIEWER
        // The display backend is left out: it does not change what the CPU
        // scans in a second.
        return board_fingerprint(current_board_facts(helix::PlatformCapabilities::detect(), ""));
#else
        return std::string();
#endif
    }();
    return board;
}

const std::string& preview_rate_version() {
#if HELIX_HAS_GCODE_VIEWER
    static const std::string version = helix_version_full();
#else
    static const std::string version;
#endif
    return version;
}

namespace {

bool print_holds_machine() {
    return job_holds_machine(get_printer_state().print_state().get_print_lifecycle());
}

#if HELIX_HAS_GCODE_VIEWER
/// The last rate kept this run (main thread), for when none is stored.
double& session_rate() {
    static double rate = 0.0;
    return rate;
}
#endif

} // namespace

PreviewGateInputs current_preview_gate_inputs(uint64_t bytes) {
    PreviewGateInputs in;
    in.memory_safe = helix::is_gcode_2d_streaming_safe(static_cast<size_t>(bytes));
    in.auto_mode = preview_mode_is_auto();
    in.printing = print_holds_machine();
#if HELIX_HAS_GCODE_VIEWER
    const helix::Config* config = helix::Config::get_instance();
    if (config && !preview_rate_board().empty()) {
        in.learned_rate =
            learned_preview_rate(*config, preview_rate_board(), preview_rate_version());
    }
    if (!(in.learned_rate > 0.0)) {
        in.learned_rate = session_rate();
    }
#endif
    return in;
}

PreviewGate preview_gate(uint64_t bytes, const char* log_tag) {
    const PreviewGateInputs in = current_preview_gate_inputs(bytes);
    const PreviewGate gate = decide_preview_gate(bytes, in);
    switch (gate) {
    case PreviewGate::TooLarge: {
        const auto mem = helix::get_system_memory_info();
        spdlog::warn("[{}] G-code too large for 2D streaming: file={} bytes, available RAM={}MB - "
                     "using thumbnail only",
                     log_tag, bytes, mem.available_mb());
        break;
    }
    case PreviewGate::TooExpensive:
        spdlog::info(
            "[{}] G-code preview would cost ~{:.1f} CPU-s on this board (budget {:.0f}s{}) "
            "- using thumbnail only",
            log_tag, helix::gcode::projected_preview_cpu_s(bytes, in.learned_rate),
            helix::gcode::preview_budget_s(in.printing), in.printing ? " while printing" : "");
        break;
    case PreviewGate::Build:
        break;
    }
    return gate;
}

RateRecord decide_rate_record(double mb_per_cpu_s, bool board_known, bool printing,
                              double stored_rate) {
    if (!(mb_per_cpu_s > 0.0) || !board_known) {
        return RateRecord::Ignore;
    }
    if (printing) {
        return RateRecord::SessionOnly;
    }
    if (stored_rate > 0.0) {
        const double ratio = mb_per_cpu_s / stored_rate;
        if (ratio > 0.9 && ratio < 1.1) {
            return RateRecord::SessionOnly;
        }
    }
    return RateRecord::Persist;
}

void record_preview_rate(double mb_per_cpu_s) {
#if !HELIX_HAS_GCODE_VIEWER
    // Nothing scans an index here, so there is never a rate to keep; compiling
    // the store out lets the linker drop it from the firmware image.
    (void)mb_per_cpu_s;
#else
    helix::Config* config = helix::Config::get_instance();
    const bool board_known = config && !preview_rate_board().empty();
    const double stored =
        board_known ? learned_preview_rate(*config, preview_rate_board(), preview_rate_version())
                    : 0.0;
    switch (decide_rate_record(mb_per_cpu_s, board_known, print_holds_machine(), stored)) {
    case RateRecord::Ignore:
        return;
    case RateRecord::SessionOnly:
        session_rate() = mb_per_cpu_s;
        return;
    case RateRecord::Persist:
        session_rate() = mb_per_cpu_s;
        spdlog::info("[PreviewGate] Learned index rate {:.1f} MB/cpu-s on {}", mb_per_cpu_s,
                     preview_rate_board());
        save_preview_rate(*config, {mb_per_cpu_s, preview_rate_board(), preview_rate_version()});
        return;
    }
#endif
}

} // namespace helix::ui
