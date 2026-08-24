// SPDX-License-Identifier: GPL-3.0-or-later
#include "layout_manager.h"

#include "data_root_resolver.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unistd.h>

namespace {
// Every directory name under ui_xml/ that can hold a layout override. Content
// subdirectories (components/, translations/) are deliberately absent.
constexpr const char* VARIANT_DIRS[] = {"micro_portrait", "tiny_portrait", "portrait",
                                        "ultrawide",      "micro",         "tiny"};
} // namespace

namespace helix {

bool LayoutManager::is_variant_dir(const std::string& dir) {
    return std::find(std::begin(VARIANT_DIRS), std::end(VARIANT_DIRS), dir) !=
           std::end(VARIANT_DIRS);
}

LayoutManager& LayoutManager::instance() {
    static LayoutManager instance;
    return instance;
}

void LayoutManager::init(int width, int height) {
    if (!override_name_.empty()) {
        type_ = name_to_type(override_name_);
    } else {
        type_ = detect(width, height);
    }
    name_ = type_to_name(type_);
    initialized_ = true;
    width_ = width;
    height_ = height;
    spdlog::info("LayoutManager: {}x{} -> layout '{}'{}", width, height, name_,
                 override_name_.empty() ? "" : " (override)");
}

void LayoutManager::set_override(const std::string& name) {
    if (name.empty()) {
        override_name_.clear();
        return;
    }
    // Normalize hyphens to underscores (CLI uses tiny-portrait, internal uses tiny_portrait)
    std::string normalized = name;
    std::replace(normalized.begin(), normalized.end(), '-', '_');
    // Validate the name by converting (logs warning if unknown)
    name_to_type(normalized);
    override_name_ = normalized;
}

LayoutType LayoutManager::type() const {
    return type_;
}

const std::string& LayoutManager::name() const {
    return name_;
}

bool LayoutManager::is_standard() const {
    return type_ == LayoutType::STANDARD;
}

std::vector<std::string> LayoutManager::variant_chain() const {
    switch (type_) {
    // Portrait sub-classes fall back to the shared portrait/ layer before base,
    // so they only need to carry files that actually differ from portrait/.
    case LayoutType::MICRO_PORTRAIT:
        return {"micro_portrait", "portrait"};
    case LayoutType::TINY_PORTRAIT:
        return {"tiny_portrait", "portrait"};
    case LayoutType::PORTRAIT:
        return {"portrait"};
    case LayoutType::ULTRAWIDE:
        return {"ultrawide"};
    case LayoutType::MICRO:
        return {"micro"};
    case LayoutType::TINY:
        return {"tiny"};
    case LayoutType::STANDARD:
    default:
        return {};
    }
}

std::string LayoutManager::active_variant_dir(const std::string& filename) const {
    for (const auto& dir : variant_chain()) {
        std::string variant_path = helix::asset_path("ui_xml/" + dir + "/" + filename);
        if (access(variant_path.c_str(), F_OK) == 0) {
            return dir;
        }
    }
    return {};
}

std::string LayoutManager::resolve_xml_path(const std::string& filename) const {
    std::string variant = active_variant_dir(filename);
    return variant.empty() ? helix::asset_path("ui_xml/" + filename)
                           : helix::asset_path("ui_xml/" + variant + "/" + filename);
}

bool LayoutManager::has_override(const std::string& filename) const {
    return !active_variant_dir(filename).empty();
}

LayoutType detect_layout_type(int width, int height) {
    float ratio = static_cast<float>(width) / static_cast<float>(height);
    int max_dim = std::max(width, height);
    int min_dim = std::min(width, height);

    if (max_dim <= 480) {
        // 480x272 class gets a dedicated layout target (e.g., Ender 3 V3 KE panel).
        if (min_dim <= 272) {
            return (width >= height) ? LayoutType::MICRO : LayoutType::MICRO_PORTRAIT;
        }
        return (width >= height) ? LayoutType::TINY : LayoutType::TINY_PORTRAIT;
    }
    if (ratio > 2.5f) {
        return LayoutType::ULTRAWIDE;
    }
    if (ratio < 0.8f) {
        return LayoutType::PORTRAIT;
    }
    return LayoutType::STANDARD;
}

bool is_portrait_layout(LayoutType type) {
    return type == LayoutType::PORTRAIT || type == LayoutType::TINY_PORTRAIT ||
           type == LayoutType::MICRO_PORTRAIT;
}

LayoutType LayoutManager::detect(int width, int height) const {
    return detect_layout_type(width, height);
}

const char* LayoutManager::type_to_name(LayoutType type) {
    switch (type) {
    case LayoutType::STANDARD:
        return "standard";
    case LayoutType::ULTRAWIDE:
        return "ultrawide";
    case LayoutType::PORTRAIT:
        return "portrait";
    case LayoutType::MICRO:
        return "micro";
    case LayoutType::MICRO_PORTRAIT:
        return "micro_portrait";
    case LayoutType::TINY:
        return "tiny";
    case LayoutType::TINY_PORTRAIT:
        return "tiny_portrait";
    }
    return "standard";
}

LayoutType LayoutManager::name_to_type(const std::string& name) {
    if (name == "standard")
        return LayoutType::STANDARD;
    if (name == "ultrawide")
        return LayoutType::ULTRAWIDE;
    if (name == "portrait")
        return LayoutType::PORTRAIT;
    if (name == "micro")
        return LayoutType::MICRO;
    if (name == "micro_portrait")
        return LayoutType::MICRO_PORTRAIT;
    if (name == "tiny")
        return LayoutType::TINY;
    if (name == "tiny_portrait")
        return LayoutType::TINY_PORTRAIT;

    spdlog::warn("LayoutManager: unknown layout name '{}', defaulting to standard", name);
    return LayoutType::STANDARD;
}

} // namespace helix
