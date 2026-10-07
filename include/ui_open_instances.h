// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <lvgl.h>
#include <vector>

namespace helix::ui {

/// Whether @p obj is @p ancestor or lies inside it.
inline bool descends_from(lv_obj_t* obj, lv_obj_t* ancestor) {
    for (lv_obj_t* o = obj; o; o = lv_obj_get_parent(o)) {
        if (o == ancestor) {
            return true;
        }
    }
    return false;
}

/// The live instances of a component whose XML event callbacks are registered
/// once for the whole app. A callback finds the instance it belongs to by the
/// widget that fired, so any number can be open at once. T needs root().
template <typename T> class OpenInstances {
  public:
    void add(T* instance) {
        instances_.push_back(instance);
    }
    void remove(T* instance) {
        instances_.erase(std::remove(instances_.begin(), instances_.end(), instance),
                         instances_.end());
    }
    /// The open instance whose root contains @p obj, or nullptr.
    T* owner_of(lv_obj_t* obj) const {
        for (T* instance : instances_) {
            if (instance->root() && descends_from(obj, instance->root())) {
                return instance;
            }
        }
        return nullptr;
    }

  private:
    std::vector<T*> instances_;
};

} // namespace helix::ui
