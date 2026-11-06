// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_start_params.h"

#include "filament_mapper.h"
#include "printer_discovery.h"

namespace helix::print_start {

bool takes_tool_slots(const PrinterDiscovery& hw) {
    return hw.has_zmod_afc();
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

int mapped_backend(const std::vector<ToolMapping>& mappings) {
    for (const auto& m : mappings) {
        if (!m.is_auto && m.mapped_slot >= 0 && m.tool_index >= 0) {
            return m.mapped_backend < 0 ? 0 : m.mapped_backend;
        }
    }
    return 0;
}

std::map<int, int> full_tool_slots(const std::vector<ToolMapping>& mappings, int backend_idx) {
    std::map<int, int> slots;
    for (const auto& m : mappings) {
        // A slot number names a lane only within its own backend.
        if (m.is_auto || m.mapped_slot < 0 || m.tool_index < 0 ||
            (m.mapped_backend < 0 ? 0 : m.mapped_backend) != backend_idx) {
            continue;
        }
        slots[m.tool_index] = m.mapped_slot;
    }
    return slots;
}

} // namespace helix::print_start
