// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_menu_context.h"

#include "color_utils.h"
#include "core/gcode_filename.h"
#include "plugin_manifest.h"

#include <algorithm>

namespace helix::plugin_menu {

namespace {

/// `hex` in the "#RRGGBB" form every context colour uses; empty when it is not a colour.
std::string normalized_hex(const std::string& hex) {
    auto rgb = parse_hex_color(hex);
    return rgb ? color_to_hex_string(*rgb) : std::string();
}

void put_positive(json& j, const char* key, double v) {
    if (v > 0)
        j[key] = v;
}

void put_nonempty(json& j, const char* key, const std::string& v) {
    if (!v.empty())
        j[key] = v;
}

} // namespace

json print_file_context(const PrintFileFacts& f) {
    json j = json::object();
    const bool usb = !f.local_path.empty();
    j["source"] = usb ? "usb" : "printer";
    j["name"] = f.name;
    if (usb)
        j["local_path"] = f.local_path;
    else
        j["path"] = gcode::join_gcode_path(f.dir, f.name);
    if (f.meta) {
        put_positive(j, "estimated_time_s", f.meta->estimated_time);
        put_positive(j, "layer_height", f.meta->layer_height);
        put_positive(j, "object_height", f.meta->object_height);
    }

    const size_t tools = std::max({f.materials.size(), f.colors.size(), f.tool_grams.size()});
    if (tools > 0) {
        json filament = json::array();
        for (size_t i = 0; i < tools; ++i) {
            json t{{"tool", static_cast<int>(i)}};
            if (i < f.materials.size())
                put_nonempty(t, "material", f.materials[i]);
            if (i < f.colors.size())
                put_nonempty(t, "color", normalized_hex(f.colors[i]));
            if (i < f.tool_grams.size() && f.tool_grams[i] >= 0)
                t["grams"] = f.tool_grams[i];
            filament.push_back(std::move(t));
        }
        j["filament"] = std::move(filament);
    }
    if (!f.tools_used.empty())
        j["tools_used"] = json(std::vector<int>(f.tools_used.begin(), f.tools_used.end()));
    return j;
}

json slot_context(const SlotInfo& slot, int global_index, const AmsUnit* unit,
                  const std::string& label) {
    json j = json::object();
    if (unit)
        j["unit"] = unit->unit_index;
    // Counted from the unit's first slot: backends disagree on whether
    // SlotInfo::slot_index is local to the unit or global.
    j["slot"] = unit ? global_index - unit->first_slot_global_index : global_index;
    j["label"] = label;
    if (slot.mapped_tool >= 0)
        j["tool"] = slot.mapped_tool;
    // An empty slot carries whatever it last held; none of that is loaded now.
    if (slot.status == SlotStatus::EMPTY)
        return j;
    put_nonempty(j, "material", slot.material);
    put_nonempty(j, "brand", slot.brand);
    if (slot.color_rgb != kAmsDefaultSlotColor)
        j["color"] = color_to_hex_string(slot.color_rgb);
    if (slot.spoolman_id > 0)
        j["spool_id"] = slot.spoolman_id;
    return j;
}

json spool_context(const SpoolInfo& spool) {
    json j = json::object();
    if (spool.id > 0)
        j["spool_id"] = spool.id;
    put_nonempty(j, "material", spool.material);
    put_nonempty(j, "color", normalized_hex(spool.color_hex));
    put_nonempty(j, "brand", spool.vendor);
    put_positive(j, "remaining_g", spool.remaining_weight_g);
    return j;
}

json tile_context(const std::string& widget_id) {
    const std::string_view local = plugin::local_part_of(widget_id);
    return json{{"widget", local.empty() ? widget_id : std::string(local)}};
}

} // namespace helix::plugin_menu
