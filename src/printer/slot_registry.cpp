// SPDX-License-Identifier: GPL-3.0-or-later
#include "slot_registry.h"

#include <algorithm>

namespace helix::printer {

// Static sentinel for invalid unit access
static const RegistryUnit INVALID_UNIT{"", -1, 0};

void SlotRegistry::initialize(const std::string& unit_name,
                              const std::vector<std::string>& slot_names) {
    clear();

    RegistryUnit unit;
    unit.name = unit_name;
    unit.first_slot = 0;
    unit.slot_count = static_cast<int>(slot_names.size());
    units_.push_back(unit);

    for (int i = 0; i < static_cast<int>(slot_names.size()); ++i) {
        SlotEntry entry;
        entry.global_index = i;
        entry.unit_index = 0;
        entry.backend_name = slot_names[i];
        entry.info.global_index = i;
        entry.info.slot_index = i;
        slots_.push_back(std::move(entry));
    }

    rebuild_reverse_maps();
    initialized_ = true;
}

void SlotRegistry::initialize_units(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& units) {
    clear();

    int global_offset = 0;
    for (int u = 0; u < static_cast<int>(units.size()); ++u) {
        const auto& [unit_name, slot_names] = units[u];

        RegistryUnit reg_unit;
        reg_unit.name = unit_name;
        reg_unit.first_slot = global_offset;
        reg_unit.slot_count = static_cast<int>(slot_names.size());
        units_.push_back(reg_unit);

        for (int s = 0; s < static_cast<int>(slot_names.size()); ++s) {
            SlotEntry entry;
            entry.global_index = global_offset + s;
            entry.unit_index = u;
            entry.backend_name = slot_names[s];
            entry.info.global_index = global_offset + s;
            entry.info.slot_index = s;
            slots_.push_back(std::move(entry));
        }

        global_offset += static_cast<int>(slot_names.size());
    }

    rebuild_reverse_maps();
    initialized_ = true;
}

void SlotRegistry::reorganize(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& unit_slot_map) {
    // Stash existing slot data by backend_name
    std::unordered_map<std::string, SlotEntry> stash;
    for (auto& slot : slots_) {
        stash[slot.backend_name] = std::move(slot);
    }

    slots_.clear();
    units_.clear();

    int global_offset = 0;
    int unit_idx = 0;
    for (const auto& [unit_name, slot_names] : unit_slot_map) {
        RegistryUnit reg_unit;
        reg_unit.name = unit_name;
        reg_unit.first_slot = global_offset;
        reg_unit.slot_count = static_cast<int>(slot_names.size());
        units_.push_back(reg_unit);

        for (int s = 0; s < static_cast<int>(slot_names.size()); ++s) {
            const auto& name = slot_names[s];
            auto it = stash.find(name);
            if (it != stash.end()) {
                // Preserve existing data, fix up indices
                SlotEntry entry = std::move(it->second);
                entry.global_index = global_offset + s;
                entry.unit_index = unit_idx;
                entry.info.global_index = global_offset + s;
                entry.info.slot_index = s;
                slots_.push_back(std::move(entry));
            } else {
                // New slot with defaults
                SlotEntry entry;
                entry.global_index = global_offset + s;
                entry.unit_index = unit_idx;
                entry.backend_name = name;
                entry.info.global_index = global_offset + s;
                entry.info.slot_index = s;
                slots_.push_back(std::move(entry));
            }
        }

        global_offset += static_cast<int>(slot_names.size());
        ++unit_idx;
    }

    rebuild_reverse_maps();
    initialized_ = true;
}

int SlotRegistry::slot_count() const {
    return static_cast<int>(slots_.size());
}

bool SlotRegistry::is_valid_index(int global_index) const {
    return global_index >= 0 && global_index < static_cast<int>(slots_.size());
}

const SlotEntry* SlotRegistry::get(int global_index) const {
    if (!is_valid_index(global_index))
        return nullptr;
    return &slots_[global_index];
}

SlotEntry* SlotRegistry::get_mut(int global_index) {
    if (!is_valid_index(global_index))
        return nullptr;
    return &slots_[global_index];
}

const SlotEntry* SlotRegistry::find_by_name(const std::string& backend_name) const {
    auto it = name_to_index_.find(backend_name);
    if (it == name_to_index_.end())
        return nullptr;
    return &slots_[it->second];
}

SlotEntry* SlotRegistry::find_by_name_mut(const std::string& backend_name) {
    auto it = name_to_index_.find(backend_name);
    if (it == name_to_index_.end())
        return nullptr;
    return &slots_[it->second];
}

int SlotRegistry::index_of(const std::string& backend_name) const {
    auto it = name_to_index_.find(backend_name);
    if (it == name_to_index_.end())
        return -1;
    return it->second;
}

std::string SlotRegistry::name_of(int global_index) const {
    if (!is_valid_index(global_index))
        return "";
    return slots_[global_index].backend_name;
}

int SlotRegistry::unit_count() const {
    return static_cast<int>(units_.size());
}

const RegistryUnit& SlotRegistry::unit(int unit_index) const {
    if (unit_index < 0 || unit_index >= static_cast<int>(units_.size())) {
        return INVALID_UNIT;
    }
    return units_[unit_index];
}

std::pair<int, int> SlotRegistry::unit_slot_range(int unit_index) const {
    if (unit_index < 0 || unit_index >= static_cast<int>(units_.size())) {
        return {0, 0};
    }
    const auto& u = units_[unit_index];
    return {u.first_slot, u.first_slot + u.slot_count};
}

int SlotRegistry::unit_for_slot(int global_index) const {
    if (!is_valid_index(global_index))
        return -1;
    return slots_[global_index].unit_index;
}

int SlotRegistry::tool_for_slot(int global_index) const {
    if (!is_valid_index(global_index))
        return -1;
    return slots_[global_index].info.mapped_tool;
}

int SlotRegistry::slot_for_tool(int tool_number) const {
    if (tool_number < 0 || tool_number >= static_cast<int>(tool_to_slot_.size()))
        return -1;
    return tool_to_slot_[tool_number];
}

void SlotRegistry::set_tool_mapping(int global_index, int tool_number, MappingSource source) {
    if (!is_valid_index(global_index) || tool_number < 0)
        return;

    // Bump before the early-outs below can't happen (there are none past this
    // point) but after validation, so a rejected write never counts as
    // confirmation. Counted per accepted firmware write rather than per
    // changed value: a firmware report that MATCHES what we optimistically
    // wrote is still proof the printer applied it, and is the common case for
    // a restore that worked.
    if (source == MappingSource::Firmware) {
        ++firmware_mapping_generation_;
    }

    // Clear any previous tool on this slot, but only if the reverse map
    // still points to this slot (another slot may have already claimed it)
    int old_tool = slots_[global_index].info.mapped_tool;
    if (old_tool >= 0 && old_tool < static_cast<int>(tool_to_slot_.size()) &&
        tool_to_slot_[old_tool] == global_index) {
        tool_to_slot_[old_tool] = -1;
    }

    // If another slot previously held this tool number, clear that slot's mapping
    // so each tool maps to exactly one slot
    if (tool_number < static_cast<int>(tool_to_slot_.size())) {
        int old_slot = tool_to_slot_[tool_number];
        if (old_slot >= 0 && old_slot != global_index && is_valid_index(old_slot) &&
            slots_[old_slot].info.mapped_tool == tool_number) {
            slots_[old_slot].info.mapped_tool = -1;
        }
    }

    // Set the mapping
    slots_[global_index].info.mapped_tool = tool_number;

    // Grow tool_to_slot_ if needed
    if (tool_number >= static_cast<int>(tool_to_slot_.size())) {
        tool_to_slot_.resize(tool_number + 1, -1);
    }
    tool_to_slot_[tool_number] = global_index;
}

void SlotRegistry::clear_tool_mapping(int global_index) {
    if (!is_valid_index(global_index))
        return;

    // Drop the reverse-map entry only if it still points at this slot; another
    // slot may have already claimed the tool number.
    int old_tool = slots_[global_index].info.mapped_tool;
    if (old_tool >= 0 && old_tool < static_cast<int>(tool_to_slot_.size()) &&
        tool_to_slot_[old_tool] == global_index) {
        tool_to_slot_[old_tool] = -1;
    }

    slots_[global_index].info.mapped_tool = -1;
}

void SlotRegistry::set_tool_map(const std::vector<int>& tool_to_slot, MappingSource source) {
    // One bump per accepted bulk write: Happy Hare reports the ENTIRE ttg_map in
    // every status update, so a single call is the whole firmware truth rather
    // than one lane's worth of it.
    if (source == MappingSource::Firmware) {
        ++firmware_mapping_generation_;
    }

    // Clear all existing mappings
    for (auto& slot : slots_) {
        slot.info.mapped_tool = -1;
    }
    tool_to_slot_.clear();

    tool_to_slot_.resize(tool_to_slot.size(), -1);
    for (int t = 0; t < static_cast<int>(tool_to_slot.size()); ++t) {
        int slot_idx = tool_to_slot[t];
        if (is_valid_index(slot_idx)) {
            tool_to_slot_[t] = slot_idx;
            slots_[slot_idx].info.mapped_tool = t;
        }
    }
}

const std::vector<int>& SlotRegistry::tool_map() const {
    return tool_to_slot_;
}

uint64_t SlotRegistry::firmware_mapping_generation() const {
    return firmware_mapping_generation_;
}

int SlotRegistry::backup_for_slot(int global_index) const {
    if (!is_valid_index(global_index))
        return -1;
    return slots_[global_index].endless_spool_backup;
}

void SlotRegistry::set_backup(int global_index, int backup_slot) {
    if (!is_valid_index(global_index))
        return;
    slots_[global_index].endless_spool_backup = backup_slot;
}

std::vector<int> SlotRegistry::backup_edges() const {
    std::vector<int> edges;
    edges.reserve(slots_.size());
    for (const auto& slot : slots_) {
        edges.push_back(slot.endless_spool_backup);
    }
    return edges;
}

AmsSystemInfo SlotRegistry::build_system_info() const {
    AmsSystemInfo info;
    info.total_slots = slot_count();

    for (int u = 0; u < static_cast<int>(units_.size()); ++u) {
        const auto& reg_unit = units_[u];

        AmsUnit unit;
        unit.unit_index = u;
        unit.name = reg_unit.name;
        unit.slot_count = reg_unit.slot_count;
        unit.first_slot_global_index = reg_unit.first_slot;

        for (int s = 0; s < reg_unit.slot_count; ++s) {
            int gi = reg_unit.first_slot + s;
            if (is_valid_index(gi)) {
                unit.slots.push_back(slots_[gi].info);
            }
        }

        info.units.push_back(std::move(unit));
    }

    info.tool_to_slot_map = tool_to_slot_;

    return info;
}

bool SlotRegistry::is_initialized() const {
    return initialized_;
}

void SlotRegistry::clear() {
    slots_.clear();
    name_to_index_.clear();
    tool_to_slot_.clear();
    units_.clear();
    initialized_ = false;
}

void SlotRegistry::rebuild_reverse_maps() {
    name_to_index_.clear();
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        name_to_index_[slots_[i].backend_name] = i;
    }

    // Rebuild tool_to_slot_ from slots
    tool_to_slot_.clear();
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        int tool = slots_[i].info.mapped_tool;
        if (tool >= 0) {
            if (tool >= static_cast<int>(tool_to_slot_.size())) {
                tool_to_slot_.resize(tool + 1, -1);
            }
            tool_to_slot_[tool] = i;
        }
    }
}

} // namespace helix::printer
