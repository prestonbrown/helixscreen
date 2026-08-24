// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_temp_reclaim.h"

#include "../catch_amalgamated.hpp"

using helix::ui::is_reclaimable_download;

namespace {
constexpr const char* TEMP = "/var/cache/helix/gcode_temp";
} // namespace

TEST_CASE("reclaim: files we downloaded are ours to delete", "[gcode][reclaim]") {
    REQUIRE(is_reclaimable_download("/var/cache/helix/gcode_temp/detail_123.gcode", TEMP));
    REQUIRE(is_reclaimable_download("/var/cache/helix/gcode_temp/sub/detail_123.gcode", TEMP));
    // A trailing slash on the configured dir must not change the answer.
    REQUIRE(is_reclaimable_download("/var/cache/helix/gcode_temp/detail_123.gcode",
                                    "/var/cache/helix/gcode_temp/"));
}

TEST_CASE("reclaim: Moonraker's own print file is never deletable", "[gcode][reclaim]") {
    // The whole reason this rule exists: a same-host open hands the view the
    // user's actual print file, and every teardown path would have removed it.
    REQUIRE_FALSE(is_reclaimable_download("/mnt/UDISK/printer_data/gcodes/part.gcode", TEMP));
    REQUIRE_FALSE(is_reclaimable_download("/home/pi/printer_data/gcodes/part.gcode", TEMP));
}

TEST_CASE("reclaim: a shared prefix is not containment", "[gcode][reclaim]") {
    // "/…/gcode_temp" must not claim a sibling directory that starts with it.
    REQUIRE_FALSE(is_reclaimable_download("/var/cache/helix/gcode_temp_old/detail.gcode", TEMP));
    REQUIRE_FALSE(is_reclaimable_download("/var/cache/helix/gcode_tempest.gcode", TEMP));
    // The directory itself is not a file to reclaim.
    REQUIRE_FALSE(is_reclaimable_download(TEMP, TEMP));
    REQUIRE_FALSE(is_reclaimable_download("/var/cache/helix/gcode_temp/", TEMP));
}

TEST_CASE("reclaim: traversal cannot escape the cache dir", "[gcode][reclaim]") {
    // Starts inside the cache dir and resolves to the gcodes root.
    REQUIRE_FALSE(is_reclaimable_download(
        "/var/cache/helix/gcode_temp/../../../mnt/printer_data/gcodes/part.gcode", TEMP));
    REQUIRE_FALSE(is_reclaimable_download("/var/cache/helix/gcode_temp/../part.gcode", TEMP));
    // A leading-dots filename is a normal name, not traversal.
    REQUIRE(is_reclaimable_download("/var/cache/helix/gcode_temp/..thumb.gcode", TEMP));
    REQUIRE(is_reclaimable_download("/var/cache/helix/gcode_temp/a..b.gcode", TEMP));
}

TEST_CASE("reclaim: an unset cache dir is not licence to delete", "[gcode][reclaim]") {
    // get_helix_cache_dir() returns "" when no cache is configured. Treating
    // that as a match would make every path reclaimable.
    REQUIRE_FALSE(is_reclaimable_download("/var/cache/helix/gcode_temp/detail.gcode", ""));
    REQUIRE_FALSE(is_reclaimable_download("", TEMP));
    REQUIRE_FALSE(is_reclaimable_download("", ""));
}
