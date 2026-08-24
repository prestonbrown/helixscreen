// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_root_resolver.h"

#include "../catch_amalgamated.hpp"

namespace {

// Restores the process-global asset_root to its prior value on scope exit,
// including when a REQUIRE in between throws — without this, a failing
// assertion mid-test leaks the non-default root into every later test case.
struct AssetRootGuard {
    std::string saved = helix::asset_root();
    ~AssetRootGuard() {
        helix::set_asset_root(saved);
    }
};

} // namespace

TEST_CASE("asset_path is identity under the default root", "[paths][asset_root]") {
    AssetRootGuard guard;
    helix::set_asset_root(""); // reset to default
    REQUIRE(helix::asset_root() == ".");
    REQUIRE(helix::asset_path("ui_xml") == "ui_xml");
    REQUIRE(helix::asset_path("assets/filaments.json") == "assets/filaments.json");
}

TEST_CASE("asset_path joins under an explicit root", "[paths][asset_root]") {
    AssetRootGuard guard;
    helix::set_asset_root("/littlefs/"); // trailing slash must be stripped
    REQUIRE(helix::asset_root() == "/littlefs");
    REQUIRE(helix::asset_path("ui_xml") == "/littlefs/ui_xml");
    helix::set_asset_root(""); // restore for other tests (guard is the backstop)
    REQUIRE(helix::asset_path("ui_xml") == "ui_xml");
}
