// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "subject_debug_registry.h"

#include "helix/xml/scoped_subject_registry.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

#include <atomic>

/// True between the singleton's construction and destruction.
///
/// SubjectManager::deinit_all() unregisters through this registry, and panels in
/// static storage run that during static destruction — potentially after this
/// singleton is gone, where locking a destroyed mutex_ aborts the process with
/// "mutex lock failed: Invalid argument".
///
/// Zero-initialized before any dynamic initialization and never destroyed (it is
/// trivially destructible), so it stays readable for the whole process, including
/// after ~SubjectDebugRegistry. That makes a late unregister a no-op instead of an
/// abort — correct, because nothing can look the entry up after the registry dies
/// either. Preferred over leaking the singleton: the lifetime stays honest.
static std::atomic<bool> g_registry_alive{false};

SubjectDebugRegistry::SubjectDebugRegistry() {
    g_registry_alive.store(true, std::memory_order_release);
}

SubjectDebugRegistry::~SubjectDebugRegistry() {
    g_registry_alive.store(false, std::memory_order_release);
}

SubjectDebugRegistry& SubjectDebugRegistry::instance() {
    static SubjectDebugRegistry instance;
    return instance;
}

void SubjectDebugRegistry::register_subject(lv_subject_t* subject, const std::string& name,
                                            lv_subject_type_t type, const std::string& file,
                                            int line) {
    if (subject == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    subjects_[subject] = SubjectDebugInfo{name, type, file, line};
    name_to_subject_[name] = subject;
}

void SubjectDebugRegistry::unregister_subject(lv_subject_t* subject) {
    if (subject == nullptr) {
        return;
    }
    // See g_registry_alive: a panel destroyed after this singleton would otherwise
    // lock a destroyed mutex_ and abort. Nothing can query the entry at that point
    // either, so dropping the work is the correct outcome, not a papered-over bug.
    if (!g_registry_alive.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subjects_.find(subject);
    if (it == subjects_.end()) {
        return;
    }
    // Drop the reverse mapping only when it still points at THIS subject. A newer
    // instance may already have re-registered the same name at a different address
    // (panels are destroyed and rebuilt); erasing blindly would strip the live
    // entry and leave lookup_by_name() answering for a subject that is going away.
    auto name_it = name_to_subject_.find(it->second.name);
    if (name_it != name_to_subject_.end() && name_it->second == subject) {
        name_to_subject_.erase(name_it);
    }
    subjects_.erase(it);
}

const SubjectDebugInfo* SubjectDebugRegistry::lookup(lv_subject_t* subject) {
    if (subject == nullptr) {
        return nullptr;
    }
    // Mirrors unregister_subject(): a caller running during static destruction
    // (SubjectManager::deinit_all) would otherwise lock a destroyed mutex_.
    if (!g_registry_alive.load(std::memory_order_acquire)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subjects_.find(subject);
    if (it == subjects_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::string SubjectDebugRegistry::type_name(lv_subject_type_t type) {
    switch (type) {
    case LV_SUBJECT_TYPE_INVALID:
        return "INVALID";
    case LV_SUBJECT_TYPE_NONE:
        return "NONE";
    case LV_SUBJECT_TYPE_INT:
        return "INT";
    case LV_SUBJECT_TYPE_FLOAT:
        return "FLOAT";
    case LV_SUBJECT_TYPE_POINTER:
        return "POINTER";
    case LV_SUBJECT_TYPE_COLOR:
        return "COLOR";
    case LV_SUBJECT_TYPE_GROUP:
        return "GROUP";
    case LV_SUBJECT_TYPE_STRING:
        return "STRING";
    default:
        return "UNKNOWN";
    }
}

lv_subject_t* SubjectDebugRegistry::lookup_by_name(const std::string& name) {
    // Same late-caller safety as lookup(): deinit_all() consults this during
    // static destruction and must not lock a destroyed mutex_.
    if (!g_registry_alive.load(std::memory_order_acquire)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = name_to_subject_.find(name);
    if (it == name_to_subject_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<std::pair<std::string, SubjectDebugInfo>> SubjectDebugRegistry::list_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, SubjectDebugInfo>> result;
    result.reserve(subjects_.size());
    for (const auto& [ptr, info] : subjects_) {
        result.emplace_back(info.name, info);
    }
    return result;
}

void SubjectDebugRegistry::dump_all_subjects() {
    std::lock_guard<std::mutex> lock(mutex_);
    spdlog::debug("[SubjectDebugRegistry] Registered subjects: {}", subjects_.size());

    for (const auto& [ptr, info] : subjects_) {
        spdlog::debug("[SubjectDebugRegistry]   {} ({}): {} @ {}:{}", static_cast<void*>(ptr),
                      type_name(info.type), info.name, info.file, info.line);
    }
}

void SubjectDebugRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    subjects_.clear();
    name_to_subject_.clear();
}

namespace helix {

void publish_subject(lv_subject_t* subject, const char* name, lv_subject_type_t type,
                     const char* file, int line) {
    helix::xml::register_subject_in_current_scope(name, subject);
    SubjectDebugRegistry::instance().register_subject(subject, name, type, file, line);
}

void publish_managed_subject(SubjectManager& manager, lv_subject_t* subject, const char* xml_name,
                             lv_subject_type_t type, const char* file, int line) {
    helix::xml::register_subject_in_current_scope(xml_name, subject);
    manager.register_subject(subject, xml_name);
    SubjectDebugRegistry::instance().register_subject(subject, xml_name, type, file, line);
}

} // namespace helix
