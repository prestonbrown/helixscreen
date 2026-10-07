// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_excluded_objects_state.cpp
 * @brief Excluded objects state management extracted from PrinterState
 *
 * Manages the set of objects excluded from printing via Klipper's EXCLUDE_OBJECT
 * feature. Uses version-based notification since LVGL subjects don't support sets.
 *
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_excluded_objects_state.h"

#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <glm/common.hpp>

namespace helix {

void PrinterExcludedObjectsState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterExcludedObjectsState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterExcludedObjectsState] Initializing subjects (register_xml={})",
                  register_xml);

    // Initialize version subject to 0 (no changes yet)
    INIT_SUBJECT_INT(excluded_objects_version, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(defined_objects_version, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(defined_objects_count, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterExcludedObjectsState] Subjects initialized successfully");
}

PrinterExcludedObjectsState::ObjectInfo
PrinterExcludedObjectsState::make_object_info(std::string name, std::optional<glm::vec2> center,
                                              std::vector<glm::vec2> polygon) {
    ObjectInfo info;
    info.name = std::move(name);
    info.has_center = center.has_value();
    if (center) {
        info.center = *center;
    }
    info.has_bbox = !polygon.empty();
    if (info.has_bbox) {
        glm::vec2 lo = polygon.front();
        glm::vec2 hi = polygon.front();
        for (const auto& p : polygon) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        info.bbox_min = lo;
        info.bbox_max = hi;
    }
    info.polygon = std::move(polygon);
    return info;
}

void PrinterExcludedObjectsState::update_from_status(const nlohmann::json& status) {
    // The inner setters (set_excluded_objects / set_defined_objects_with_geometry /
    // set_current_object) log on actual change.
    auto it = status.find("exclude_object");
    if (it == status.end()) {
        return;
    }
    const auto& eo = *it;

    if (eo.contains("excluded_objects") && eo["excluded_objects"].is_array()) {
        std::unordered_set<std::string> excluded;
        for (const auto& obj : eo["excluded_objects"]) {
            if (obj.is_string()) {
                excluded.insert(obj.get<std::string>());
            }
        }
        // set_excluded_objects handles change detection and notification
        set_excluded_objects(excluded);
    }

    // Parse defined objects list with geometry (center + polygon bounding box)
    if (eo.contains("objects") && eo["objects"].is_array()) {
        std::vector<ObjectInfo> objects;
        for (const auto& obj : eo["objects"]) {
            if (!obj.is_object() || !obj.contains("name") || !obj["name"].is_string())
                continue;

            std::optional<glm::vec2> center;
            if (obj.contains("center") && obj["center"].is_array() && obj["center"].size() >= 2 &&
                obj["center"][0].is_number() && obj["center"][1].is_number()) {
                center = glm::vec2(obj["center"][0].get<float>(), obj["center"][1].get<float>());
            }

            std::vector<glm::vec2> polygon;
            if (obj.contains("polygon") && obj["polygon"].is_array()) {
                for (const auto& pt : obj["polygon"]) {
                    if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number()) {
                        polygon.push_back({pt[0].get<float>(), pt[1].get<float>()});
                    }
                }
            }

            objects.push_back(
                make_object_info(obj["name"].get<std::string>(), center, std::move(polygon)));
        }
        set_defined_objects_with_geometry(objects);
    }

    // Parse current object
    if (eo.contains("current_object")) {
        if (eo["current_object"].is_string()) {
            set_current_object(eo["current_object"].get<std::string>());
        } else if (eo["current_object"].is_null()) {
            set_current_object("");
        }
    }
}

void PrinterExcludedObjectsState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[PrinterExcludedObjectsState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterExcludedObjectsState::set_excluded_objects(
    const std::unordered_set<std::string>& objects) {
    // Only update if the set actually changed
    if (excluded_objects_ != objects) {
        excluded_objects_ = objects;

        // Increment version to notify observers
        int version = lv_subject_get_int(&excluded_objects_version_);
        lv_subject_set_int(&excluded_objects_version_, version + 1);

        spdlog::debug("[PrinterExcludedObjectsState] Excluded objects updated: {} objects "
                      "(version {})",
                      excluded_objects_.size(), version + 1);
    }
}

void PrinterExcludedObjectsState::set_defined_objects(const std::vector<std::string>& objects) {
    // Only update if the list actually changed
    if (defined_objects_ != objects) {
        defined_objects_ = objects;

        // Increment version to notify observers
        int version = lv_subject_get_int(&defined_objects_version_);
        lv_subject_set_int(&defined_objects_version_, version + 1);
        lv_subject_set_int(&defined_objects_count_, static_cast<int>(defined_objects_.size()));

        spdlog::debug("[PrinterExcludedObjectsState] Defined objects updated: {} objects "
                      "(version {})",
                      defined_objects_.size(), version + 1);
    }
}

void PrinterExcludedObjectsState::set_defined_objects_with_geometry(
    const std::vector<ObjectInfo>& objects) {
    std::vector<std::string> names;
    names.reserve(objects.size());
    object_geometry_.clear();
    for (const auto& obj : objects) {
        names.push_back(obj.name);
        object_geometry_[obj.name] = obj;
    }
    set_defined_objects(names);
}

std::optional<PrinterExcludedObjectsState::ObjectInfo>
PrinterExcludedObjectsState::get_object_geometry(const std::string& name) const {
    auto it = object_geometry_.find(name);
    if (it != object_geometry_.end())
        return it->second;
    return std::nullopt;
}

void PrinterExcludedObjectsState::set_current_object(const std::string& name) {
    if (current_object_ != name) {
        current_object_ = name;

        // Bump excluded version to notify overlay observers of current object change
        int version = lv_subject_get_int(&excluded_objects_version_);
        lv_subject_set_int(&excluded_objects_version_, version + 1);

        spdlog::debug("[PrinterExcludedObjectsState] Current object: '{}' (version {})",
                      current_object_, version + 1);
    }
}

void PrinterExcludedObjectsState::clear_objects() {
    // Route through the setters so change detection, version bumps and logging behave
    // exactly as they do for a normal status update.
    set_excluded_objects({});
    set_defined_objects_with_geometry({});
    set_current_object("");
}

} // namespace helix
