// SPDX-License-Identifier: GPL-3.0-or-later

// The client.cfg layer-pause macros (SET_PAUSE_NEXT_LAYER, SET_PAUSE_AT_LAYER
// and the SET_PRINT_STATS_INFO wrapper that keeps their state), so the Pause at
// Layer modal can be driven under --test. On by default;
// HELIX_MOCK_LAYER_PAUSE=0 removes them to show the printer without them.

#include "core/text_io.h"
#include "env_knobs.h"
#include "moonraker_client_mock.h"

#include <spdlog/spdlog.h>

#include <sstream>

using namespace helix;

namespace {

constexpr const char* kStateObject = "gcode_macro SET_PRINT_STATS_INFO";

/// The value of `KEY=` among a line's words, case-insensitive on the key.
std::optional<std::string> param(const std::string& line, const std::string& key) {
    std::istringstream words(line);
    std::string word;
    const std::string prefix = text_io::to_upper(key) + "=";
    while (words >> word) {
        if (text_io::to_upper(word).rfind(prefix, 0) == 0) {
            return word.substr(prefix.size());
        }
    }
    return std::nullopt;
}

int param_int(const std::string& value, int fallback) {
    return text_io::parse_leading<int>(value).value_or(fallback);
}

} // namespace

bool MoonrakerClientMock::layer_pause_macros_loaded() {
    const char* value = std::getenv("HELIX_MOCK_LAYER_PAUSE");
    return value == nullptr || env_truthy(value);
}

json MoonrakerClientMock::layer_pause_status() const {
    json status = json::object();
    if (!layer_pause_macros_loaded()) {
        return status;
    }
    status[kStateObject] = {
        {"pause_next_layer", {{"enable", layer_pause_next_.load()}, {"call", "PAUSE"}}},
        {"pause_at_layer",
         {{"enable", layer_pause_at_enable_.load()},
          {"layer", layer_pause_at_layer_.load()},
          {"call", "PAUSE"}}}};
    return status;
}

void MoonrakerClientMock::append_layer_pause_objects(json& objects) {
    if (!layer_pause_macros_loaded()) {
        return;
    }
    objects.push_back("gcode_macro SET_PAUSE_NEXT_LAYER");
    objects.push_back("gcode_macro SET_PAUSE_AT_LAYER");
    objects.push_back(kStateObject);
}

// Mirrors the client.cfg macro bodies: ENABLE defaults to 1 for NEXT_LAYER;
// for AT_LAYER it defaults to whether LAYER was given, and LAYER keeps its
// previous value when omitted.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_layer_pause(const std::string& g) {
    if (!layer_pause_macros_loaded()) {
        return std::nullopt;
    }
    bool changed = false;
    std::istringstream lines(g);
    std::string raw;
    while (std::getline(lines, raw)) {
        const std::string line(text_io::trim(raw));
        const std::string upper = text_io::to_upper(line);
        if (upper.rfind("SET_PAUSE_NEXT_LAYER", 0) == 0) {
            const auto enable = param(line, "ENABLE");
            layer_pause_next_ = enable ? param_int(*enable, 1) != 0 : true;
            layer_pause_next_armed_at_ = get_current_layer();
            changed = true;
        } else if (upper.rfind("SET_PAUSE_AT_LAYER", 0) == 0) {
            const auto enable = param(line, "ENABLE");
            const auto layer = param(line, "LAYER");
            layer_pause_at_enable_ = enable ? param_int(*enable, 0) != 0 : layer.has_value();
            if (layer) {
                layer_pause_at_layer_ = param_int(*layer, layer_pause_at_layer_.load());
            }
            changed = true;
        }
    }
    if (changed) {
        spdlog::debug("[MoonrakerClientMock] layer pause now {}", layer_pause_status().dump());
        dispatch_status_update(layer_pause_status());
    }
    return std::nullopt;
}

void MoonrakerClientMock::layer_pause_on_layer(int previous_layer, int current_layer) {
    if (!layer_pause_macros_loaded() || current_layer <= previous_layer) {
        return;
    }
    bool fire = false;
    if (layer_pause_next_.load() && current_layer > layer_pause_next_armed_at_.load()) {
        layer_pause_next_ = false;
        fire = true;
    } else if (layer_pause_at_enable_.load() && current_layer == layer_pause_at_layer_.load()) {
        // client.cfg compares CURRENT_LAYER for equality: a layer the print
        // skips past never fires and stays armed, as on a real printer.
        layer_pause_at_enable_ = false;
        fire = true;
    }
    if (!fire) {
        return;
    }
    spdlog::info("[MoonrakerClientMock] Layer pause fired at layer {}", current_layer);
    dispatch_gcode_response("// PAUSE, forced by layer pause");
    dispatch_status_update(layer_pause_status());
    pause_print_internal();
}

void MoonrakerClientMock::reset_layer_pause() {
    layer_pause_next_ = false;
    layer_pause_at_enable_ = false;
    layer_pause_at_layer_ = 0;
    if (layer_pause_macros_loaded()) {
        dispatch_status_update(layer_pause_status());
    }
}
