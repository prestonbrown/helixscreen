// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_thumbnail_printer_key.cpp
 * @brief A thumbnail is cached per printer: a same-named file on another printer is a
 *        different file.
 */

#include "../lvgl_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "thumbnail_cache.h"

#include <string>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "Thumbnail cache keys depend on the printer's address",
                 "[thumbnail][multi-printer]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    const std::string thumb = ".thumbs/benchy-300x300.png";

    api.set_http_base_url("http://10.0.0.1:7125");
    const std::string on_a = ThumbnailCache::compute_hash(thumb);

    api.set_http_base_url("http://10.0.0.2:7125");
    CHECK(ThumbnailCache::compute_hash(thumb) != on_a);

    api.set_http_base_url("http://10.0.0.1:7125");
    CHECK(ThumbnailCache::compute_hash(thumb) == on_a);
}
