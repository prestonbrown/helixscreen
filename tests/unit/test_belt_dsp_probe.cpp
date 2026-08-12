// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_dsp_probe.h"

#include "../catch_amalgamated.hpp"

using helix::calibration::cached_dsp_probe;
using helix::calibration::dsp_ms_is_capable;
using helix::calibration::MAX_PSD_MS;
using helix::calibration::probe_dsp_throughput;

TEST_CASE("dsp_ms_is_capable brackets the threshold", "[belt][dsp_probe]") {
    CHECK(dsp_ms_is_capable(1.0));
    CHECK(dsp_ms_is_capable(MAX_PSD_MS - 0.001));
    CHECK_FALSE(dsp_ms_is_capable(MAX_PSD_MS + 0.001));
    CHECK_FALSE(dsp_ms_is_capable(10000.0));
}

TEST_CASE("dsp_ms_is_capable rejects a nonsense measurement", "[belt][dsp_probe]") {
    // A zero or negative elapsed time means the clock or the timing code is
    // broken. Reading that as "infinitely fast hardware" would enable the
    // feature on exactly the machines whose timing cannot be trusted.
    CHECK_FALSE(dsp_ms_is_capable(0.0));
    CHECK_FALSE(dsp_ms_is_capable(-1.0));
}

TEST_CASE("probe measures a positive elapsed time", "[belt][dsp_probe]") {
    const auto r = probe_dsp_throughput();
    CHECK(r.psd_ms > 0.0);
    CHECK(r.capable == dsp_ms_is_capable(r.psd_ms));
}

TEST_CASE("the build machine passes the capability gate", "[belt][dsp_probe]") {
    // Not a property of the code so much as a guard on the threshold: if a
    // developer laptop or CI runner cannot clear a bar the reference CB1 clears
    // with 2.6x margin, MAX_PSD_MS has been set wrong.
    const auto r = probe_dsp_throughput();
    INFO("measured psd_ms = " << r.psd_ms << ", threshold = " << MAX_PSD_MS);
    CHECK(r.capable);
}

TEST_CASE("cached probe returns a stable result", "[belt][dsp_probe]") {
    const auto& a = cached_dsp_probe();
    const auto& b = cached_dsp_probe();
    CHECK(&a == &b);
    CHECK(a.psd_ms == b.psd_ms);
}
