// SPDX-License-Identifier: GPL-3.0-or-later

#include "zmod_color_status.h"

#include "color_utils.h"

namespace helix::zmod_color {

namespace {

/// The module emits ID as str(i); an int is accepted too, since this is
/// someone else's schema.
std::optional<int> slot_id(const nlohmann::json& entry) {
    auto it = entry.find("ID");
    if (it == entry.end()) {
        return std::nullopt;
    }
    if (it->is_number_integer()) {
        return it->get<int>();
    }
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::vector<std::optional<Slot>>> parse_slots(const nlohmann::json& obj,
                                                            int max_slots) {
    auto it = obj.find("slots");
    if (it == obj.end() || !it->is_array() || max_slots <= 0) {
        return std::nullopt;
    }
    std::vector<std::optional<Slot>> out(static_cast<size_t>(max_slots));
    for (const auto& entry : *it) {
        if (!entry.is_object()) {
            continue;
        }
        auto id = slot_id(entry);
        if (!id || *id < 1 || *id > max_slots) {
            continue;
        }
        Slot slot;
        if (auto mat = entry.find("Material"); mat != entry.end() && mat->is_string()) {
            slot.material = mat->get<std::string>();
            if (slot.material == "?") {
                slot.material.clear();
            }
        }
        if (auto hex = entry.find("HEX"); hex != entry.end() && hex->is_string()) {
            slot.hex = hex->get<std::string>();
        }
        out[static_cast<size_t>(*id - 1)] = std::move(slot);
    }
    return out;
}

std::optional<std::vector<std::pair<int, std::uint32_t>>> parse_palette(const nlohmann::json& obj) {
    auto it = obj.find("palette");
    if (it == obj.end() || !it->is_array()) {
        return std::nullopt;
    }
    std::vector<std::pair<int, std::uint32_t>> out;
    int index = 0;
    for (const auto& entry : *it) {
        if (entry.is_string()) {
            if (auto rgb = parse_hex_color(entry.get<std::string>())) {
                out.emplace_back(index, *rgb);
            }
        }
        ++index;
    }
    return out;
}

std::optional<std::vector<std::string>> parse_valid_types(const nlohmann::json& obj) {
    auto it = obj.find("valid_types");
    if (it == obj.end() || !it->is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    for (const auto& entry : *it) {
        if (entry.is_string() && entry.get<std::string>() != "?") {
            out.push_back(entry.get<std::string>());
        }
    }
    return out;
}

} // namespace helix::zmod_color
