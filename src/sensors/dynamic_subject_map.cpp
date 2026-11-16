// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynamic_subject_map.h"

#include "core/subject.h"

#include <algorithm>

namespace helix::sensors {

DynamicSubjectMap::~DynamicSubjectMap() {
    clear();
}

DynamicSubjectMap::Slot* DynamicSubjectMap::slot_for(const std::string& key) const {
    for (const auto& slot : slots_) {
        if (slot->key == key) {
            return slot.get();
        }
    }
    return nullptr;
}

lv_subject_t* DynamicSubjectMap::get_or_create(const std::string& key, int initial,
                                               SubjectLifetime& out_lifetime) {
    Slot* slot = slot_for(key);
    if (slot == nullptr) {
        slots_.push_back(std::make_unique<Slot>());
        slot = slots_.back().get();
        slot->key = key;
        slot->subject.init_int(initial);
        slot->lifetime = std::make_shared<bool>(true);
    }
    out_lifetime = slot->lifetime;
    return &slot->subject;
}

lv_subject_t* DynamicSubjectMap::find(const std::string& key, SubjectLifetime& out_lifetime) const {
    Slot* slot = slot_for(key);
    if (slot == nullptr) {
        out_lifetime.reset();
        return nullptr;
    }
    out_lifetime = slot->lifetime;
    return &slot->subject;
}

void DynamicSubjectMap::remove_missing(const std::vector<std::string>& keep) {
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (std::find(keep.begin(), keep.end(), (*it)->key) == keep.end()) {
            release(**it);
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
}

void DynamicSubjectMap::clear() {
    for (auto& slot : slots_) {
        release(*slot);
    }
    slots_.clear();
}

void DynamicSubjectMap::release(Slot& slot) {
    // Every outstanding copy of the token reads dead before the observers go.
    if (slot.lifetime) {
        *slot.lifetime = false;
    }
    slot.lifetime.reset();
    if (lv_is_initialized()) {
        slot.subject.reset();
    }
}

} // namespace helix::sensors
