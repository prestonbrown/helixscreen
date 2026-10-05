// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix/xml/scoped_subject_registry.h"

namespace helix::xml {

namespace {
// Per-thread: each thread maintains its own active scope. Tests may spin up
// libhv event loops / WebSocket workers; keeping this thread-local prevents
// cross-thread races if a scoped registration ever escapes the test thread.
thread_local lv_xml_component_scope_t* g_active_scope = nullptr;
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

lv_xml_component_scope_t* current_scope() {
    return g_active_scope;
}

} // namespace helix::xml
