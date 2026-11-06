// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_start_params.h"

#include "printer_discovery.h"

#include <algorithm>
#include <cctype>

namespace helix::print_start {

namespace {

/// Z-Mod's own extra, present on every Z-Mod printer.
constexpr const char* kZmodColorObject = "zmod_color";

} // namespace

bool takes_tool_slots(const PrinterDiscovery& hw) {
    const auto& objects = hw.printer_objects();
    return std::find(objects.begin(), objects.end(), kZmodColorObject) != objects.end();
}

std::optional<int> slot_number_of_lane(std::string_view lane) {
    std::size_t start = lane.size();
    while (start > 0 && std::isdigit(static_cast<unsigned char>(lane[start - 1]))) {
        --start;
    }
    // More digits than any slot count; also keeps the sum below from overflowing.
    if (start == lane.size() || lane.size() - start > 6) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t i = start; i < lane.size(); ++i) {
        value = value * 10 + (lane[i] - '0');
    }
    if (value < 1) {
        return std::nullopt;
    }
    return value;
}

PrintStartParams tool_slot_params(const std::map<int, int>& slot_numbers) {
    std::string tools;
    int written = 0; // entries in `tools` so far, one per tool from T0
    for (const auto& [tool, slot] : slot_numbers) {
        if (tool < 0 || slot < 1) {
            continue;
        }
        // An empty entry leaves that tool to the firmware's own default.
        for (; written < tool; ++written) {
            tools += ',';
        }
        tools += std::to_string(slot);
        tools += ',';
        ++written;
    }
    if (tools.empty()) {
        return {};
    }
    tools.pop_back();
    return {{"tools", tools}};
}

} // namespace helix::print_start
