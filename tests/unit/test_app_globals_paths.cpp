// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_globals.h"
#include "platform_info.h"

#include "../catch_amalgamated.hpp"

// NOTE ON WHAT IS ACTUALLY LINKED HERE.
// app_globals.o is excluded from the test link; tests/ui_test_utils.cpp supplies
// the three accessors below. So these cases pin the *interface contract* that
// both the stub and the production body have to satisfy - never a relative path,
// never a trailing slash, and the same answer every call (all three production
// bodies memoize into a function-local static, so a second call that differs is
// a real bug). The production derivation itself is covered by
// tests/unit/application/test_data_root_resolver.cpp.

namespace {

// Empty is a legal answer (nothing resolvable); anything non-empty must be an
// absolute path. A relative path here silently mis-resolves every file the app
// opens, depending on the cwd it happened to be launched from.
void require_absolute_or_empty(const std::string& p) {
    if (!p.empty()) {
        INFO("path was: " << p);
        REQUIRE(p.front() == '/');
    }
}

// Callers concatenate "/" + name onto these, so a trailing slash yields "//".
void require_no_trailing_slash(const std::string& p) {
    if (p.size() > 1) {
        INFO("path was: " << p);
        REQUIRE(p.back() != '/');
    }
}

} // namespace

TEST_CASE("app_get_install_root returns an absolute, slash-free, stable path",
          "[app_globals][paths]") {
    const std::string root = app_get_install_root();

    require_absolute_or_empty(root);
    require_no_trailing_slash(root);

    // Derived once and cached - a second call must not re-derive to something
    // else (or leak a per-call temporary).
    REQUIRE(app_get_install_root() == root);
}

TEST_CASE("app_get_cache_dir returns an absolute, stable path", "[app_globals][paths]") {
    const std::string cache = app_get_cache_dir();

    require_absolute_or_empty(cache);
    require_no_trailing_slash(cache);
    REQUIRE(app_get_cache_dir() == cache);
}

TEST_CASE("app_get_config_dir returns a slash-free, stable path", "[app_globals][paths]") {
    const std::string cfg = app_get_config_dir();

    // Deliberately NOT require_absolute_or_empty(): app_get_config_dir()
    // documents a best-effort *relative* fallback ("config") for the case where
    // the install root could not be derived, so absoluteness is not part of its
    // contract the way it is for the other two.
    require_no_trailing_slash(cfg);
    REQUIRE(app_get_config_dir() == cfg);
}

TEST_CASE("writable_install_root is empty when the firmware owns the install",
          "[app_globals][paths][install_root]") {
    struct ResetOverride {
        ~ResetOverride() {
            helix::set_external_updates_default_override(-1);
        }
    } reset;
    const std::string root = "/oem/apps/helixscreen/latest";

    helix::set_external_updates_default_override(1);
    CHECK(helix::writable_install_root(root).empty());

    helix::set_external_updates_default_override(0);
    CHECK(helix::writable_install_root(root) == root);
}
