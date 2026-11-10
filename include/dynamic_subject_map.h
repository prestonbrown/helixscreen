// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/observer_guard.h" // SubjectLifetime
#include "lvgl.h"

#include <memory>
#include <string>
#include <vector>

namespace helix::sensors {

/**
 * @brief Integer subjects created at runtime, one per key, each with its own
 *        death signal.
 *
 * For subjects whose set follows discovery (one per temperature sensor, one per
 * sensor reading) and so cannot live in a SubjectManager. They are not
 * registered with the XML system: consumers get the pointer and its lifetime
 * from the owner and observe with that lifetime.
 *
 * Removing a subject flips its lifetime to false, then deinits it, so an
 * ObserverGuard holding a copy of the token sees it dead before
 * lv_subject_deinit() frees the observers it would otherwise remove.
 *
 * Main thread only. Subject addresses are stable until the key is removed:
 * each slot is its own allocation, found by a linear scan (a few dozen keys).
 */
class DynamicSubjectMap {
  public:
    DynamicSubjectMap() = default;
    ~DynamicSubjectMap();
    DynamicSubjectMap(const DynamicSubjectMap&) = delete;
    DynamicSubjectMap& operator=(const DynamicSubjectMap&) = delete;

    /// The subject for `key`, created with `initial` if absent. `out_lifetime`
    /// receives its death signal.
    lv_subject_t* get_or_create(const std::string& key, int initial, SubjectLifetime& out_lifetime);

    /// The subject for `key`, or nullptr (and an empty `out_lifetime`) if absent.
    [[nodiscard]] lv_subject_t* find(const std::string& key, SubjectLifetime& out_lifetime) const;

    /// Tear down every subject whose key is not in `keep`.
    void remove_missing(const std::vector<std::string>& keep);

    /// Tear down every subject.
    void clear();

    [[nodiscard]] size_t size() const {
        return slots_.size();
    }

  private:
    struct Slot {
        std::string key;
        lv_subject_t subject{};
        SubjectLifetime lifetime;
    };
    static void release(Slot& slot);
    [[nodiscard]] Slot* slot_for(const std::string& key) const;

    std::vector<std::unique_ptr<Slot>> slots_;
};

} // namespace helix::sensors
