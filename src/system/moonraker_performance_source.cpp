// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "moonraker_performance_source.h"

#include "i_moonraker_api.h"
#include "moonraker_types.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "hv/json.hpp"

namespace helix {
namespace perf {

using nlohmann::json;

// Handler name used when registering/unregistering persistent method callbacks.
static constexpr char HANDLER_NAME[] = "MoonrakerPerformanceSource";

MoonrakerPerformanceSource::MoonrakerPerformanceSource(IMoonrakerAPI* api) : api_(api) {}

MoonrakerPerformanceSource::~MoonrakerPerformanceSource() {
    stop();
}

// ----------------------------------------------------------------------------
// start / stop
// ----------------------------------------------------------------------------

void MoonrakerPerformanceSource::start() {
    if (running_)
        return;
    running_ = true;

    // Subscribe to live proc_stat push notifications. These persist across
    // reconnects, so register them once up front (no WS needed).
    // register_method_callback delivers the full WS message to the callback;
    // params are at j["params"][0].
    api_->register_method_callback(
        "notify_proc_stat_update", HANDLER_NAME,
        lifetime_.bg_cb("MoonrakerPerformanceSource::notify_proc_stat", [this](const json& j) {
            // Runs on main thread via bg_cb defer.
            if (!j.contains("params") || !j["params"].is_array() || j["params"].empty())
                return;
            const json& body = j["params"][0];
            if (body.is_object())
                on_proc_stat_payload(body);
        }));

    // Re-fetch proc_stats on Klippy ready (firmware restart).
    api_->register_method_callback(
        "notify_klippy_ready", HANDLER_NAME,
        lifetime_.bg_cb("MoonrakerPerformanceSource::notify_klippy_ready", [this](const json&) {
            // Runs on main thread via bg_cb defer.
            run_initial_handshake();
        }));

    // Hook notify_status_update for MCU live frames. MCU objects are
    // subscribed by MoonrakerDiscoverySequence as part of the single union
    // subscription — issuing our own printer.objects.subscribe here would
    // replace the main subscription and wipe heater/print_stats/fan updates
    // (Moonraker docs: "A new request will override a previous request").
    //
    // Use subscribe_notifications (notify_callbacks_) instead of
    // register_method_callback (method_callbacks_): only notify_callbacks_
    // is fanned out by MoonrakerClient::dispatch_status_update, which is
    // how the discovery-sequence subscribe response delivers the initial
    // MCU snapshot. method_callbacks_ would only receive subsequent live
    // WS frames, leaving MCU rows blank until Klipper's first incremental
    // push.
    status_sub_id_ = api_->subscribe_notifications(
        lifetime_.bg_cb("MoonrakerPerformanceSource::notify_status", [this](const json& j) {
            // Runs on main thread via bg_cb defer.
            // notify_callbacks_ also receives notify_filelist_changed
            // and other methods — filter by the key shape since the
            // params[0] payload differs per method.
            if (!j.contains("params") || !j["params"].is_array() || j["params"].empty())
                return;
            const json& obj = j["params"][0];
            if (!obj.is_object())
                return;
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                // Route MCU keys (matches "mcu" and "mcu <name>").
                const std::string& key = it.key();
                if (key == "mcu" || key.rfind("mcu ", 0) == 0) {
                    on_mcu_status_update(key, it.value());
                }
            }
        }));

    // Hook the WS connect event for the proc_stats REST fetch. Requires the
    // HTTP base URL to be set, which Application configures AFTER
    // PerformanceState::set_source(). add_connected_observer fires immediately
    // if already connected, or on the next WS open / Klippy ready transition.
    api_->get_client().add_connected_observer(
        HANDLER_NAME, lifetime_.bg_cb("MoonrakerPerformanceSource::on_connected", [this]() {
            // Runs on main thread via bg_cb defer.
            run_initial_handshake();
        }));
}

void MoonrakerPerformanceSource::run_initial_handshake() {
    // Initial host-stats snapshot via REST GET /machine/proc_stats.
    // RestResponse.data contains the full JSON-RPC-like body; the actual
    // payload is under resp.data["result"].
    api_->rest().call_rest_get(
        "/machine/proc_stats",
        lifetime_.bg_cb("MoonrakerPerformanceSource::initial_proc_stats",
                        [this](const RestResponse& resp) {
                            // Runs on main thread via bg_cb defer.
                            if (!resp.success) {
                                spdlog::debug("[perf] initial proc_stats failed: {}", resp.error);
                                return;
                            }
                            if (resp.data.contains("result") && resp.data["result"].is_object()) {
                                on_proc_stat_payload(resp.data["result"]);
                            }
                        }));

    // MCU live updates ride the main union subscription set up by
    // MoonrakerDiscoverySequence (see "Hook notify_status_update" in start()).
}

void MoonrakerPerformanceSource::stop() {
    if (!running_)
        return;
    running_ = false;

    // Invalidate the lifetime guard — all in-flight bg_cb wrappers become no-ops.
    lifetime_.invalidate();

    // Unregister persistent method callbacks + the on-connected observer.
    api_->unregister_method_callback("notify_proc_stat_update", HANDLER_NAME);
    api_->unregister_method_callback("notify_klippy_ready", HANDLER_NAME);
    api_->get_client().remove_connected_observer(HANDLER_NAME);

    // Drop the notify_status_update subscription registered for MCU live updates.
    if (status_sub_id_ != 0) {
        api_->unsubscribe_notifications(status_sub_id_);
        status_sub_id_ = 0;
    }

    mcu_state_.clear();
    latest_ = {};
}

// ----------------------------------------------------------------------------
// on_proc_stat_payload  (main thread)
// ----------------------------------------------------------------------------

void MoonrakerPerformanceSource::on_proc_stat_payload(const json& body) {
    if (!body.is_object())
        return;

    // system_cpu_usage: { "cpu": <aggregate 0-100>, "cpu0": ..., ... }
    if (body.contains("system_cpu_usage") && body["system_cpu_usage"].is_object()) {
        const auto& cu = body["system_cpu_usage"];
        if (cu.contains("cpu") && cu["cpu"].is_number())
            latest_.host_cpu_pct = cu["cpu"].get<float>();
    }

    // cpu_temp: scalar °C
    if (body.contains("cpu_temp") && body["cpu_temp"].is_number())
        latest_.host_cpu_temp_c = body["cpu_temp"].get<float>();

    // system_memory: { "total": <kB>, "available": <kB>, ... }
    if (body.contains("system_memory") && body["system_memory"].is_object()) {
        const auto& m = body["system_memory"];
        if (m.contains("total") && m["total"].is_number() && m.contains("available") &&
            m["available"].is_number()) {
            const auto total_kb = m["total"].get<uint64_t>();
            const auto avail_kb = m["available"].get<uint64_t>();
            if (total_kb > 0) {
                latest_.host_mem_free_mb = static_cast<uint32_t>(avail_kb / 1024);
                latest_.host_mem_pct_used =
                    100.0f * (1.0f - static_cast<float>(avail_kb) / static_cast<float>(total_kb));
            }
        }
    }

    // throttled: { "bits": <uint32>, "flags": [<string>, ...] }
    if (body.contains("throttled") && body["throttled"].is_object()) {
        const auto& th = body["throttled"];
        uint32_t bits = 0;
        std::vector<std::string> flags;
        if (th.contains("bits") && th["bits"].is_number())
            bits = th["bits"].get<uint32_t>();
        if (th.contains("flags") && th["flags"].is_array()) {
            for (const auto& f : th["flags"])
                if (f.is_string())
                    flags.push_back(f.get<std::string>());
        }
        latest_.host_throttle_bits = bits;
        latest_.host_throttle_text = format_throttle_text(bits, flags);
    }

    if (cb_)
        cb_(latest_);
}

// ----------------------------------------------------------------------------
// on_mcu_status_update  (main thread)
// ----------------------------------------------------------------------------

void MoonrakerPerformanceSource::on_mcu_status_update(const std::string& name,
                                                      const json& payload) {
    if (!payload.is_object())
        return;
    auto& st = mcu_state_[name];

    bool updated = false;

    // mcu_awake: cumulative seconds the MCU spent executing work.
    // Compute load = Δawake / Δwall; gate on minimum dt to avoid noise on
    // the very first sample (where last_t is zero-initialised).
    if (payload.contains("last_stats") && payload["last_stats"].is_object()) {
        const auto& ls = payload["last_stats"];
        if (ls.contains("mcu_awake") && ls["mcu_awake"].is_number()) {
            const double awake = ls["mcu_awake"].get<double>();
            const auto now = std::chrono::steady_clock::now();
            // Leave load absent on the first sample and on degenerate deltas;
            // downstream subjects honour `present = 0` for std::nullopt so the
            // UI bar hides instead of snapping to 0%.
            std::optional<float> load;
            if (st.initialized) {
                const double d_awake = awake - st.last_awake;
                const double d_t = std::chrono::duration<double>(now - st.last_t).count();
                if (d_t >= 0.1 && d_awake >= 0.0)
                    load = static_cast<float>(d_awake / d_t);
            }
            st.last_awake = awake;
            st.last_t = now;
            st.initialized = true;

            // Upsert into latest_.mcus.
            bool found = false;
            for (auto& m : latest_.mcus) {
                if (m.name == name) {
                    m.load = load;
                    found = true;
                    break;
                }
            }
            if (!found) {
                McuStat m;
                m.name = name;
                m.load = load;
                m.retransmits = st.retrans;
                latest_.mcus.push_back(std::move(m));
                // Keep sorted for stable UI ordering.
                std::sort(latest_.mcus.begin(), latest_.mcus.end(),
                          [](const McuStat& a, const McuStat& b) { return a.name < b.name; });
            }
            updated = true;
        }
    }

    // bytes_retransmit: cumulative retransmit byte counter. Klipper publishes
    // this INSIDE last_stats (see Status_Reference.html#mcu: "last_stats.<name>")
    // — reading it at the top level (the old path) silently never fired.
    if (payload.contains("last_stats") && payload["last_stats"].is_object()) {
        const auto& ls = payload["last_stats"];
        if (ls.contains("bytes_retransmit") && ls["bytes_retransmit"].is_number()) {
            st.retrans = ls["bytes_retransmit"].get<uint64_t>();
            for (auto& m : latest_.mcus) {
                if (m.name == name) {
                    m.retransmits = st.retrans;
                    break;
                }
            }
            updated = true;
        }
    }

    if (updated && cb_)
        cb_(latest_);
}

// ----------------------------------------------------------------------------
// format_throttle_text (static)
// ----------------------------------------------------------------------------

std::string
MoonrakerPerformanceSource::format_throttle_text(uint32_t bits,
                                                 const std::vector<std::string>& flags) {
    if (bits == 0)
        return "";

    // Use Moonraker-provided flag strings when available.
    if (!flags.empty()) {
        std::string out;
        for (size_t i = 0; i < flags.size(); ++i) {
            if (i)
                out += ", ";
            out += flags[i];
        }
        return out;
    }

    // Fallback bit-decode table (Raspberry Pi throttle register layout).
    std::string out;
    if (bits & 0x00001)
        out += "Under-voltage now; ";
    if (bits & 0x00002)
        out += "Freq capped now; ";
    if (bits & 0x00004)
        out += "Throttled now; ";
    if (bits & 0x10000)
        out += "Under-voltage previously; ";
    if (bits & 0x20000)
        out += "Freq capped previously; ";
    if (bits & 0x40000)
        out += "Throttled previously; ";
    if (!out.empty())
        out.erase(out.size() - 2); // strip trailing "; "
    return out;
}

} // namespace perf
} // namespace helix
