// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix/xml/scoped_subject_registry.h"

#include "core/subject.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace helix::xml {

namespace {
// Per-thread: each thread maintains its own active scope. Tests may spin up
// libhv event loops / WebSocket workers; keeping this thread-local prevents
// cross-thread races if a scoped registration ever escapes the test thread.
thread_local lv_xml_component_scope_t* g_active_scope = nullptr;

struct Owner {
    lv_subject_t* subject;
    /// subjects_lvgl_generation() at publish: an owner from an earlier LVGL holds a
    /// freed node and is never handed the name back.
    uint32_t generation;
};

using OwnerKey = std::pair<lv_xml_component_scope_t*, std::string>;
using OwnerMap = std::map<OwnerKey, std::vector<Owner>>;

/// Never destroyed: SubjectManager::deinit_all() reaches this from the atexit chain.
OwnerMap& owners() {
    static auto* map = new OwnerMap();
    return *map;
}

/// Removes `subject` from `name`'s owners; returns the newest live owner left that
/// belongs to the running LVGL, or nullptr.
lv_subject_t* drop_owner(const char* name, const lv_subject_t* subject) {
    auto& map = owners();
    auto it = map.find(OwnerKey{g_active_scope, name});
    if (it == map.end()) {
        return nullptr;
    }
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [subject](const Owner& o) { return o.subject == subject; }),
               list.end());
    const uint32_t generation = helix::subjects_lvgl_generation();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [generation](const Owner& o) { return o.generation != generation; }),
               list.end());
    if (list.empty()) {
        map.erase(it);
        return nullptr;
    }
    // Every owner left is still registered with a live SubjectManager on this LVGL, so
    // its node is allocated; one its owner has reset would only refuse the bindings.
    for (auto o = list.rbegin(); o != list.rend(); ++o) {
        if (helix::subject_is_live(o->subject)) {
            return o->subject;
        }
    }
    return nullptr;
}
} // namespace

ScopedSubjectRegistryOverride::ScopedSubjectRegistryOverride(lv_xml_component_scope_t* scope)
    : previous_(g_active_scope) {
    g_active_scope = scope;
}

ScopedSubjectRegistryOverride::~ScopedSubjectRegistryOverride() {
    g_active_scope = previous_;
}

lv_result_t register_subject_in_current_scope(const char* name, lv_subject_t* subject) {
    if (name == nullptr || subject == nullptr) {
        return LV_RESULT_INVALID;
    }
    return lv_xml_register_subject(g_active_scope, name, subject);
}

lv_result_t unregister_subject_in_current_scope(const char* name) {
    if (name == nullptr) {
        return LV_RESULT_INVALID;
    }
    // Symmetric with register_subject_in_current_scope(), including its use of
    // the *currently active* scope: an owner that registered under a scope
    // override must also deinit under it, or the name is looked up in the wrong
    // scope and survives. Everything registered at global scope (the common
    // case, and every panel today) round-trips correctly.
    return lv_xml_unregister_subject(g_active_scope, name);
}

lv_result_t unregister_subject_in_current_scope(const char* name, const lv_subject_t* subject) {
    if (name == nullptr || lv_xml_find_subject(g_active_scope, name) != subject) {
        return LV_RESULT_INVALID;
    }
    return lv_xml_unregister_subject(g_active_scope, name);
}

void note_subject_owner(const char* name, lv_subject_t* subject) {
    if (name == nullptr || subject == nullptr) {
        return;
    }
    auto& list = owners()[OwnerKey{g_active_scope, name}];
    for (const auto& o : list) {
        if (o.subject == subject) {
            return;
        }
    }
    list.push_back(Owner{subject, helix::subjects_lvgl_generation()});
}

void withdraw_owned_subject(const char* name, const lv_subject_t* subject) {
    if (name == nullptr) {
        return;
    }
    lv_subject_t* successor = drop_owner(name, subject);
    if (lv_xml_find_subject(g_active_scope, name) != subject) {
        return;
    }
    lv_xml_unregister_subject(g_active_scope, name);
    if (successor != nullptr) {
        lv_xml_register_subject(g_active_scope, name, successor);
    }
}

void forget_owned_subject(const char* name, const lv_subject_t* subject) {
    if (name != nullptr) {
        (void)drop_owner(name, subject);
    }
}

lv_xml_component_scope_t* current_scope() {
    return g_active_scope;
}

} // namespace helix::xml
