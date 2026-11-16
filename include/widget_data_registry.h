// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "flat_map.h"
#include "lvgl/lvgl.h"

namespace helix {

/// Widget -> per-instance data lookup for a custom widget type. Every registry
/// stores into one FlatMap<lv_obj_t*, void*> type, so a new widget type adds no
/// map code to the firmware image. Lookup is a linear scan over the live
/// widgets of one type. The registry does not own the data.
template <typename T> class WidgetDataRegistry {
  public:
    /// The data registered for `obj`, or nullptr.
    T* get(lv_obj_t* obj) const {
        auto it = map_.find(obj);
        return it != map_.end() ? static_cast<T*>(it->second) : nullptr;
    }

    /// Registers `data` for `obj`, replacing any earlier entry.
    void set(lv_obj_t* obj, T* data) {
        map_.insert_or_assign(obj, static_cast<void*>(data));
    }

    /// Removes `obj` and returns what was registered for it, or nullptr.
    T* take(lv_obj_t* obj) {
        auto it = map_.find(obj);
        if (it == map_.end()) {
            return nullptr;
        }
        T* data = static_cast<T*>(it->second);
        map_.erase(it);
        return data;
    }

    /// Calls fn(obj, data) for every entry. fn must not add or remove entries.
    template <typename Fn> void for_each(Fn&& fn) const {
        for (const auto& kv : map_) {
            fn(kv.first, static_cast<T*>(kv.second));
        }
    }

    void clear() {
        map_.clear();
    }
    size_t size() const {
        return map_.size();
    }
    bool empty() const {
        return map_.empty();
    }

  private:
    FlatMap<lv_obj_t*, void*> map_;
};

} // namespace helix
