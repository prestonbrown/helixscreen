// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "subject_managed_panel.h"

#include <string>
#include <vector>

namespace helix {

/**
 * Subjects one instance of a surface publishes under names no other instance
 * shares, handed to its component as props.
 *
 * For a surface that can exist twice (two modals of one class kept alive, two
 * maps on screen): each instance's XML binds its own subjects, whichever
 * instance published last. publish() names a subject "<prefix>_<suffix>" and
 * records the prop that carries the name; attrs() is the prop list for
 * lv_xml_create() or Modal::show(). Destroying this object (or reset())
 * withdraws the names and deinits the subjects, so declare it AFTER the
 * lv_subject_t members it publishes: members die in reverse order.
 */
class InstanceSubjects {
  public:
    /// @param base Readable stem for this instance's names, e.g. "exclude_map".
    explicit InstanceSubjects(const char* base)
        : base_(base), prefix_(next_instance_prefix(base)) {}
    ~InstanceSubjects() {
        subjects_.deinit_all();
    }

    InstanceSubjects(const InstanceSubjects&) = delete;
    InstanceSubjects& operator=(const InstanceSubjects&) = delete;

    /// Publish an initialised @p subject as "<prefix>_<suffix>" and pass that
    /// name to the component as prop @p prop.
    void publish(lv_subject_t* subject, const char* prop, const char* suffix) {
        names_.push_back(prefix_ + "_" + suffix);
        subjects_.publish(names_.back().c_str(), subject);
        props_.emplace_back(prop);
    }

    /// Withdraw every name and deinit the subjects, then take a fresh prefix:
    /// for an owner that rebuilds its view and publishes again.
    void reset() {
        subjects_.deinit_all();
        names_.clear();
        props_.clear();
        prefix_ = next_instance_prefix(base_.c_str());
    }

    /// prop/name pairs, nullptr-terminated. Valid until the next publish() or reset().
    const char** attrs() {
        attrs_.clear();
        for (size_t i = 0; i < names_.size(); ++i) {
            attrs_.push_back(props_[i].c_str());
            attrs_.push_back(names_[i].c_str());
        }
        attrs_.push_back(nullptr);
        return attrs_.data();
    }

    [[nodiscard]] bool empty() const {
        return names_.empty();
    }

  private:
    std::string base_;
    std::string prefix_;
    SubjectManager subjects_;
    // attrs() rebuilds its pointers on each call, so names_ may reallocate freely.
    std::vector<std::string> names_;
    std::vector<std::string> props_;
    std::vector<const char*> attrs_;
};

} // namespace helix
