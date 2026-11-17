// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_discovery_list.h"

#include "lvgl.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace helix {

namespace {

std::function<std::unique_ptr<IMdnsDiscovery>()>& discovery_source() {
    static std::function<std::unique_ptr<IMdnsDiscovery>()> source;
    return source;
}

} // namespace

void set_printer_discovery_source(std::function<std::unique_ptr<IMdnsDiscovery>()> source) {
    discovery_source() = std::move(source);
}

std::unique_ptr<IMdnsDiscovery> make_printer_discovery() {
    return discovery_source() ? discovery_source()() : nullptr;
}

std::vector<DiscoveredPrinter>
merge_discovered_printers(const std::vector<DiscoveredPrinter>& mdns,
                          const std::vector<DiscoveredPrinter>& sweep) {
    std::vector<DiscoveredPrinter> merged;
    merged.reserve(mdns.size() + sweep.size());
    auto add = [&merged](const DiscoveredPrinter& p) {
        if (std::find(merged.begin(), merged.end(), p) == merged.end()) {
            merged.push_back(p);
        }
    };
    for (const auto& p : mdns) {
        add(p);
    }
    for (const auto& p : sweep) {
        add(p);
    }
    return merged;
}

std::string discovered_printer_label(const DiscoveredPrinter& printer) {
    std::string label;
    const bool named = !printer.name.empty() && printer.name != printer.ip_address;
    if (named) {
        label = printer.name + " (";
    }
    label += printer.ip_address;
    char port[8];
    std::snprintf(port, sizeof(port), ":%u", static_cast<unsigned>(printer.port));
    label += port;
    if (named) {
        label += ")";
    }
    return label;
}

std::string discovered_printer_options(const std::vector<DiscoveredPrinter>& printers,
                                       bool searching) {
    std::string options;
    if (printers.empty()) {
        options = searching ? lv_tr("Scanning...") : lv_tr("No printers found");
    } else {
        options = lv_tr("Choose a discovered printer");
    }
    for (const auto& p : printers) {
        options += "\n";
        options += discovered_printer_label(p);
    }
    return options;
}

const DiscoveredPrinter* discovered_printer_at(const std::vector<DiscoveredPrinter>& printers,
                                               uint32_t selected) {
    if (selected == 0 || selected > printers.size()) {
        return nullptr;
    }
    return &printers[selected - 1];
}

} // namespace helix
