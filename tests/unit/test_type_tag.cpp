// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// helix::type_tag<T>() is the RTTI-free replacement for std::type_index as a
// map key (PanelWidgetManager's shared-resource store). These tests pin the two
// properties that make the substitution sound: one tag per type for the whole
// process (including across TUs), and no two types sharing a tag.

#include "helix_type_tag.h"
#include "panel_widget_manager.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

// Defined in test_type_tag_other_tu.cpp.
namespace type_tag_other_tu {
std::size_t tag_int();
std::size_t tag_string();
std::size_t tag_manager();
} // namespace type_tag_other_tu

namespace {

struct TagA {};
struct TagB {};

/// Base/derived pair for the interface-typed registration case: production
/// registers IMoonrakerAPI, not the concrete API type.
struct ITagService {
    virtual ~ITagService() = default;
    virtual int value() const = 0;
};

struct TagServiceImpl : ITagService {
    int value() const override {
        return 7;
    }
};

/// Flips a flag on destruction so ownership (or the lack of it) is observable.
struct DeathWatch {
    bool* flag;
    explicit DeathWatch(bool* f) : flag(f) {}
    ~DeathWatch() {
        *flag = true;
    }
};

} // namespace

TEST_CASE("type_tag: same type yields the same tag across translation units", "[type_tag]") {
    REQUIRE(helix::type_tag<int>() == type_tag_other_tu::tag_int());
    REQUIRE(helix::type_tag<std::string>() == type_tag_other_tu::tag_string());
    REQUIRE(helix::type_tag<helix::PanelWidgetManager>() == type_tag_other_tu::tag_manager());
}

TEST_CASE("type_tag: repeated calls are stable", "[type_tag]") {
    const std::size_t first = helix::type_tag<TagA>();
    REQUIRE(helix::type_tag<TagA>() == first);
    REQUIRE(helix::type_tag<TagA>() == first);
}

TEST_CASE("type_tag: distinct types get distinct tags", "[type_tag]") {
    const std::set<std::size_t> tags{
        helix::type_tag<TagA>(),          helix::type_tag<TagB>(),
        helix::type_tag<int>(),           helix::type_tag<double>(),
        helix::type_tag<std::string>(),   helix::type_tag<ITagService>(),
        helix::type_tag<TagServiceImpl>()};
    REQUIRE(tags.size() == 7);
}

TEST_CASE("type_tag: pointer and value types are distinct keys", "[type_tag]") {
    REQUIRE(helix::type_tag<TagA>() != helix::type_tag<TagA*>());
}

TEST_CASE("PanelWidgetManager: type-tag keyed shared resources round-trip", "[type_tag]") {
    auto& mgr = helix::PanelWidgetManager::instance();
    mgr.clear_shared_resources();

    SECTION("shared_ptr registration retrieves the same object") {
        auto owned = std::make_shared<std::string>("shared");
        mgr.register_shared_resource<std::string>(owned);
        REQUIRE(mgr.shared_resource<std::string>() == owned.get());
        REQUIRE(*mgr.shared_resource<std::string>() == "shared");
    }

    SECTION("registration under a base interface retrieves through that interface") {
        std::shared_ptr<ITagService> svc = std::make_shared<TagServiceImpl>();
        mgr.register_shared_resource<ITagService>(svc);

        ITagService* got = mgr.shared_resource<ITagService>();
        REQUIRE(got != nullptr);
        REQUIRE(got->value() == 7);
        REQUIRE(got == svc.get());

        // The tag is the key, so the concrete type is a different slot entirely -
        // same exact-match semantics std::type_index had.
        REQUIRE(mgr.shared_resource<TagServiceImpl>() == nullptr);
    }

    SECTION("shared_ptr registration keeps ownership and destroys as T") {
        bool destroyed = false;
        mgr.register_shared_resource<DeathWatch>(std::make_shared<DeathWatch>(&destroyed));
        REQUIRE(destroyed == false);
        mgr.clear_shared_resources();
        REQUIRE(destroyed == true);
    }

    SECTION("raw pointer registration is non-owning") {
        bool destroyed = false;
        auto watched = std::make_unique<DeathWatch>(&destroyed);
        mgr.register_shared_resource<DeathWatch>(watched.get());
        REQUIRE(mgr.shared_resource<DeathWatch>() == watched.get());
        mgr.clear_shared_resources();
        REQUIRE(destroyed == false);
    }

    SECTION("unregistered types return nullptr and re-registration replaces") {
        REQUIRE(mgr.shared_resource<TagA>() == nullptr);

        auto first = std::make_shared<std::string>("first");
        auto second = std::make_shared<std::string>("second");
        mgr.register_shared_resource<std::string>(first);
        mgr.register_shared_resource<std::string>(second);
        REQUIRE(*mgr.shared_resource<std::string>() == "second");
    }

    mgr.clear_shared_resources();
}
