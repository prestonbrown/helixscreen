// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_platform_capabilities.cpp
 * @brief Unit tests for PlatformCapabilities detection
 *
 * Test-first development: These tests are written BEFORE implementation.
 * Tests verify RAM detection from /proc/meminfo, CPU core detection from
 * /proc/cpuinfo, and tier classification logic.
 *
 * Test categories:
 * 1. RAM detection - Parsing /proc/meminfo content
 * 2. CPU core detection - Parsing /proc/cpuinfo content
 * 3. Tier classification - EMBEDDED/BASIC/STANDARD based on hardware
 * 4. Derived capabilities - charts, animations, max_chart_points
 */

#include "../../include/platform_capabilities.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// RAM Detection Tests (/proc/meminfo parsing)
// ============================================================================

TEST_CASE("RAM detection: parse typical meminfo content", "[platform][meminfo]") {
    // Typical /proc/meminfo content from a Raspberry Pi 4 (4GB model)
    const std::string meminfo_content = R"(MemTotal:        3884136 kB
MemFree:          142532 kB
MemAvailable:    2847284 kB
Buffers:          156892 kB
Cached:          2624748 kB
SwapCached:            0 kB
Active:          1892476 kB
Inactive:        1564312 kB
Active(anon):     695148 kB
)";

    size_t ram_mb = parse_meminfo_total_mb(meminfo_content);
    // 3884136 kB = ~3793 MB
    REQUIRE(ram_mb >= 3790);
    REQUIRE(ram_mb <= 3800);
}

TEST_CASE("RAM detection: parse small RAM system (AD5M)", "[platform][meminfo]") {
    // Typical /proc/meminfo from AD5M printer (256MB RAM)
    const std::string meminfo_content = R"(MemTotal:         253440 kB
MemFree:           12456 kB
MemAvailable:      38912 kB
Buffers:            8192 kB
Cached:            45056 kB
)";

    size_t ram_mb = parse_meminfo_total_mb(meminfo_content);
    // 253440 kB = ~247 MB
    REQUIRE(ram_mb >= 245);
    REQUIRE(ram_mb <= 250);
}

TEST_CASE("RAM detection: parse 8GB system", "[platform][meminfo]") {
    const std::string meminfo_content = R"(MemTotal:        8145920 kB
MemFree:         2048000 kB
MemAvailable:    6144000 kB
)";

    size_t ram_mb = parse_meminfo_total_mb(meminfo_content);
    // 8145920 kB = ~7955 MB
    REQUIRE(ram_mb >= 7950);
    REQUIRE(ram_mb <= 7960);
}

TEST_CASE("RAM detection: handle missing MemTotal", "[platform][meminfo][edge]") {
    const std::string meminfo_content = R"(MemFree:          142532 kB
MemAvailable:    2847284 kB
)";

    size_t ram_mb = parse_meminfo_total_mb(meminfo_content);
    REQUIRE(ram_mb == 0); // Should return 0 if MemTotal not found
}

TEST_CASE("RAM detection: handle malformed content", "[platform][meminfo][edge]") {
    const std::string meminfo_content = "garbage data without proper format";
    size_t ram_mb = parse_meminfo_total_mb(meminfo_content);
    REQUIRE(ram_mb == 0);
}

TEST_CASE("RAM detection: handle empty content", "[platform][meminfo][edge]") {
    size_t ram_mb = parse_meminfo_total_mb("");
    REQUIRE(ram_mb == 0);
}

TEST_CASE("parse_meminfo_kb matches a key only at the start of a line",
          "[platform][meminfo][edge]") {
    const std::string meminfo = "SwapCached:        77 kB\nCached:          2624748 kB\n";
    REQUIRE(parse_meminfo_kb(meminfo, "Cached") == 2624748);
    REQUIRE(parse_meminfo_kb(meminfo, "Swap") == 0);
    REQUIRE(parse_meminfo_kb("MemAvailable:\t8192 kB", "MemAvailable") == 8192);
}

// ============================================================================
// CPU Core Detection Tests (/proc/cpuinfo parsing)
// ============================================================================

TEST_CASE("CPU detection: parse quad-core ARM (Pi 4)", "[platform][cpuinfo]") {
    // Typical /proc/cpuinfo from Raspberry Pi 4 (quad-core)
    const std::string cpuinfo_content = R"(processor	: 0
model name	: ARMv7 Processor rev 3 (v7l)
BogoMIPS	: 270.00
Features	: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae evtstrm crc32
CPU implementer	: 0x41
CPU architecture: 7
CPU variant	: 0x0
CPU part	: 0xd08
CPU revision	: 3

processor	: 1
model name	: ARMv7 Processor rev 3 (v7l)
BogoMIPS	: 270.00

processor	: 2
model name	: ARMv7 Processor rev 3 (v7l)
BogoMIPS	: 270.00

processor	: 3
model name	: ARMv7 Processor rev 3 (v7l)
BogoMIPS	: 270.00
)";

    auto cpu_info = parse_cpuinfo(cpuinfo_content);
    REQUIRE(cpu_info.core_count == 4);
    REQUIRE(cpu_info.bogomips >= 250.0f);
}

// One cpuinfo block per core, each naming its implementer and part.
static std::string arm_cpuinfo(std::initializer_list<std::pair<const char*, const char*>> list) {
    std::string s;
    int n = 0;
    for (const auto& [implementer, part] : list) {
        s += "processor\t: " + std::to_string(n++) + "\nCPU implementer\t: " + implementer +
             "\nCPU part\t: " + part + "\n\n";
    }
    return s;
}

TEST_CASE("CPU detection: only an all in-order Arm core set counts as efficiency cores",
          "[platform][cpuinfo]") {
    const auto cores = [](std::initializer_list<std::pair<const char*, const char*>> list) {
        return parse_cpuinfo(arm_cpuinfo(list)).efficiency_cores_only;
    };
    CHECK(cores({{"0x41", "0xd03"}, {"0x41", "0xd03"}, {"0x41", "0xd03"}, {"0x41", "0xd03"}}));
    CHECK(cores({{"0x41", "0xd05"}, {"0x41", "0xd05"}}));
    // Big and little cores together (four A55 and four A76), an A72, another implementer.
    CHECK_FALSE(cores({{"0x41", "0xd05"},
                       {"0x41", "0xd05"},
                       {"0x41", "0xd05"},
                       {"0x41", "0xd05"},
                       {"0x41", "0xd0b"},
                       {"0x41", "0xd0b"},
                       {"0x41", "0xd0b"},
                       {"0x41", "0xd0b"}}));
    CHECK_FALSE(cores({{"0x41", "0xd08"}}));
    CHECK_FALSE(cores({{"0x51", "0xd03"}}));
    CHECK_FALSE(parse_cpuinfo("processor\t: 0\nmodel name\t: x86\nbogomips\t: 4800.00\n")
                    .efficiency_cores_only);
}

TEST_CASE("CPU detection: parse single-core ARM (AD5M)", "[platform][cpuinfo]") {
    // Typical /proc/cpuinfo from AD5M printer (single-core ARM)
    const std::string cpuinfo_content = R"(processor	: 0
model name	: ARM926EJ-S rev 5 (v5l)
BogoMIPS	: 218.00
Features	: swp half thumb fastmult edsp java
CPU implementer	: 0x41
CPU architecture: 5TEJ
CPU variant	: 0x0
CPU part	: 0x926
CPU revision	: 5

Hardware	: Allwinner sun8i Family
Revision	: 0000
Serial		: 165448888811e8c6
)";

    auto cpu_info = parse_cpuinfo(cpuinfo_content);
    REQUIRE(cpu_info.core_count == 1);
    REQUIRE(cpu_info.bogomips >= 200.0f);
}

TEST_CASE("CPU detection: parse x86 Intel processor", "[platform][cpuinfo]") {
    // Typical /proc/cpuinfo from x86 Linux (desktop/dev machine)
    const std::string cpuinfo_content = R"(processor	: 0
vendor_id	: GenuineIntel
cpu family	: 6
model		: 142
model name	: Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz
stepping	: 10
cpu MHz		: 1992.000
cache size	: 8192 KB
bogomips	: 3999.93

processor	: 1
vendor_id	: GenuineIntel
cpu family	: 6
model		: 142
model name	: Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz
cpu MHz		: 1800.000
bogomips	: 3999.93

processor	: 2
vendor_id	: GenuineIntel
model name	: Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz
cpu MHz		: 1800.000
bogomips	: 3999.93

processor	: 3
vendor_id	: GenuineIntel
model name	: Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz
cpu MHz		: 1800.000
bogomips	: 3999.93
)";

    auto cpu_info = parse_cpuinfo(cpuinfo_content);
    REQUIRE(cpu_info.core_count == 4);
    // Note: cpuinfo shows per-core BogoMIPS, so we take the first value
    REQUIRE(cpu_info.bogomips >= 3900.0f);
}

TEST_CASE("CPU detection: parse dual-core system", "[platform][cpuinfo]") {
    const std::string cpuinfo_content = R"(processor	: 0
model name	: ARMv7 Processor
BogoMIPS	: 150.00

processor	: 1
model name	: ARMv7 Processor
BogoMIPS	: 150.00
)";

    auto cpu_info = parse_cpuinfo(cpuinfo_content);
    REQUIRE(cpu_info.core_count == 2);
}

TEST_CASE("CPU detection: handle missing BogoMIPS", "[platform][cpuinfo][edge]") {
    const std::string cpuinfo_content = R"(processor	: 0
model name	: Unknown ARM
Features	: half thumb

processor	: 1
model name	: Unknown ARM
)";

    auto cpu_info = parse_cpuinfo(cpuinfo_content);
    REQUIRE(cpu_info.core_count == 2);
    REQUIRE(cpu_info.bogomips == 0.0f); // Unknown, so 0
}

TEST_CASE("CPU detection: handle empty content", "[platform][cpuinfo][edge]") {
    auto cpu_info = parse_cpuinfo("");
    REQUIRE(cpu_info.core_count == 0);
    REQUIRE(cpu_info.bogomips == 0.0f);
}

TEST_CASE("CPU detection: parse cpu MHz field when no BogoMIPS", "[platform][cpuinfo]") {
    // Some systems report cpu MHz instead of BogoMIPS
    const std::string cpuinfo_content = R"(processor	: 0
model name	: Intel CPU
cpu MHz		: 2400.000

processor	: 1
model name	: Intel CPU
cpu MHz		: 2400.000
)";

    auto cpu_info = parse_cpuinfo(cpuinfo_content);
    REQUIRE(cpu_info.core_count == 2);
    // Should extract MHz as approximate speed indicator
    REQUIRE(cpu_info.cpu_mhz >= 2300);
}

// ============================================================================
// Tier Classification Tests
// ============================================================================

TEST_CASE("Tier classification: EMBEDDED for very low RAM", "[platform][tier]") {
    // Less than 512MB RAM = EMBEDDED, regardless of cores
    auto caps = PlatformCapabilities::from_metrics(256, 4, 1000.0f);
    REQUIRE(caps.tier == PlatformTier::EMBEDDED);
    REQUIRE(caps.supports_charts);
    REQUIRE_FALSE(caps.supports_animations);
    REQUIRE(caps.max_chart_points == 50);
}

TEST_CASE("Tier classification: EMBEDDED for single core", "[platform][tier]") {
    // Single core = EMBEDDED, even with lots of RAM
    auto caps = PlatformCapabilities::from_metrics(4096, 1, 1000.0f);
    REQUIRE(caps.tier == PlatformTier::EMBEDDED);
    REQUIRE(caps.supports_charts);
}

TEST_CASE("Tier classification: EMBEDDED for zero cores (parse failure)",
          "[platform][tier][edge]") {
    // Zero cores indicates cpuinfo parse failure - should default to EMBEDDED
    auto caps = PlatformCapabilities::from_metrics(4096, 0, 0.0f);
    REQUIRE(caps.tier == PlatformTier::EMBEDDED);
    REQUIRE(caps.supports_charts);
}

TEST_CASE("Tier classification: BASIC for mid-range hardware", "[platform][tier]") {
    // 512MB or more RAM with fewer than 4 cores = BASIC
    auto caps = PlatformCapabilities::from_metrics(1024, 2, 500.0f);
    REQUIRE(caps.tier == PlatformTier::BASIC);
    REQUIRE(caps.supports_charts);
    REQUIRE_FALSE(caps.supports_animations);
    REQUIRE(caps.max_chart_points == 50);
}

TEST_CASE("Tier classification: BASIC for dual-core with good RAM", "[platform][tier]") {
    // Plenty of RAM but only 2 cores = BASIC (CPU limited)
    auto caps = PlatformCapabilities::from_metrics(4096, 2, 1000.0f);
    REQUIRE(caps.tier == PlatformTier::BASIC);
}

TEST_CASE("Tier classification: BASIC for 3 cores with good RAM", "[platform][tier]") {
    // 3 cores is still below STANDARD threshold
    auto caps = PlatformCapabilities::from_metrics(4096, 3, 1000.0f);
    REQUIRE(caps.tier == PlatformTier::BASIC);
}

TEST_CASE("Tier classification: STANDARD for high-end hardware", "[platform][tier]") {
    // 768MB+ RAM AND 4+ cores = STANDARD
    auto caps = PlatformCapabilities::from_metrics(4096, 4, 1000.0f);
    REQUIRE(caps.tier == PlatformTier::STANDARD);
    REQUIRE(caps.supports_charts);
    REQUIRE(caps.supports_animations);
    REQUIRE(caps.max_chart_points == 200);
}

TEST_CASE("Tier classification: STANDARD for desktop hardware", "[platform][tier]") {
    // 8GB RAM, 8 cores = definitely STANDARD
    auto caps = PlatformCapabilities::from_metrics(8192, 8, 4000.0f);
    REQUIRE(caps.tier == PlatformTier::STANDARD);
}

TEST_CASE("Tier classification: boundary at exactly 512MB", "[platform][tier][boundary]") {
    // Exactly 512MB = BASIC (not EMBEDDED)
    auto caps = PlatformCapabilities::from_metrics(512, 4, 1000.0f);
    REQUIRE(caps.tier == PlatformTier::BASIC);
}

TEST_CASE("Tier classification: boundary at exactly 768MB", "[platform][tier][boundary]") {
    // Exactly 768MB with 4 cores = STANDARD; one MB less = BASIC
    auto caps = PlatformCapabilities::from_metrics(768, 4, 1000.0f);
    REQUIRE(caps.tier == PlatformTier::STANDARD);
    auto below = PlatformCapabilities::from_metrics(767, 4, 1000.0f);
    REQUIRE(below.tier == PlatformTier::BASIC);
    REQUIRE_FALSE(below.supports_animations);
}

TEST_CASE("Tier classification: a 1GB quad-core Pi 3B is STANDARD", "[platform][tier]") {
    // A Pi 3B reports about 856MB of its 1GB
    auto caps = PlatformCapabilities::from_metrics(856, 4, 1000.0f);
    REQUIRE(caps.tier == PlatformTier::STANDARD);
    REQUIRE(caps.supports_animations);
    REQUIRE(caps.max_chart_points == PlatformCapabilities::kStandardChartPoints);
}

TEST_CASE("Tier classification: boundary at exactly 4 cores", "[platform][tier][boundary]") {
    // Exactly 4 cores with plenty of RAM = STANDARD
    auto caps = PlatformCapabilities::from_metrics(4096, 4, 1000.0f);
    REQUIRE(caps.tier == PlatformTier::STANDARD);

    // 3 cores with same RAM = BASIC
    auto caps2 = PlatformCapabilities::from_metrics(4096, 3, 1000.0f);
    REQUIRE(caps2.tier == PlatformTier::BASIC);
}

TEST_CASE("Tier classification: tiers have different capabilities", "[platform][tier][contrast]") {
    // Contrast test - ensures EMBEDDED and STANDARD actually differ
    // Would catch bugs where all capabilities are set to same value
    auto embedded = PlatformCapabilities::from_metrics(256, 1, 0.0f);
    auto standard = PlatformCapabilities::from_metrics(4096, 4, 1000.0f);

    REQUIRE(embedded.tier != standard.tier);
    REQUIRE(embedded.supports_animations != standard.supports_animations);
    REQUIRE(embedded.max_chart_points != standard.max_chart_points);
}

// ============================================================================
// Derived Capabilities Tests
// ============================================================================

TEST_CASE("Derived capabilities: EMBEDDED tier settings", "[platform][capabilities]") {
    auto caps = PlatformCapabilities::from_metrics(256, 1, 200.0f);

    REQUIRE(caps.tier == PlatformTier::EMBEDDED);
    REQUIRE(caps.supports_charts);
    REQUIRE_FALSE(caps.supports_animations);
    REQUIRE(caps.max_chart_points == 50);
}

TEST_CASE("Derived capabilities: BASIC tier settings", "[platform][capabilities]") {
    auto caps = PlatformCapabilities::from_metrics(1024, 2, 500.0f);

    REQUIRE(caps.tier == PlatformTier::BASIC);
    REQUIRE(caps.supports_charts);
    REQUIRE_FALSE(caps.supports_animations);
    REQUIRE(caps.max_chart_points == 50);
}

TEST_CASE("Derived capabilities: STANDARD tier settings", "[platform][capabilities]") {
    auto caps = PlatformCapabilities::from_metrics(4096, 4, 1000.0f);

    REQUIRE(caps.tier == PlatformTier::STANDARD);
    REQUIRE(caps.supports_charts);
    REQUIRE(caps.supports_animations);
    REQUIRE(caps.max_chart_points == 200);
}

// ============================================================================
// Raw Metrics Storage Tests
// ============================================================================

TEST_CASE("Metrics storage: values are preserved", "[platform][metrics]") {
    auto caps = PlatformCapabilities::from_metrics(2048, 4, 1234.5f);

    REQUIRE(caps.total_ram_mb == 2048);
    REQUIRE(caps.cpu_cores == 4);
    REQUIRE(caps.bogomips == Catch::Approx(1234.5f));
}

// ============================================================================
// Tier String Conversion Tests
// ============================================================================

TEST_CASE("Tier to string: EMBEDDED", "[platform][string]") {
    REQUIRE(platform_tier_to_string(PlatformTier::EMBEDDED) == "embedded");
}

TEST_CASE("Tier to string: BASIC", "[platform][string]") {
    REQUIRE(platform_tier_to_string(PlatformTier::BASIC) == "basic");
}

TEST_CASE("Tier to string: STANDARD", "[platform][string]") {
    REQUIRE(platform_tier_to_string(PlatformTier::STANDARD) == "standard");
}

TEST_CASE("detect_from carries the efficiency-core flag from cpuinfo", "[platform][cpuinfo]") {
    const auto caps = PlatformCapabilities::detect_from(
        "",
        arm_cpuinfo({{"0x41", "0xd03"}, {"0x41", "0xd03"}, {"0x41", "0xd03"}, {"0x41", "0xd03"}}));
    CHECK(caps.efficiency_cores_only);
    CHECK(caps.cpu_cores == 4);
}

// ============================================================================
// Integration Test (on supported platforms: Linux and macOS)
// ============================================================================

TEST_CASE("detect(): returns valid capabilities on supported platforms",
          "[platform][integration][!mayfail]") {
    // This test may fail on unsupported systems (Windows)
    // That's expected and acceptable
    auto caps = PlatformCapabilities::detect();

    // Should have detected some RAM and cores
    REQUIRE(caps.total_ram_mb > 0);
    REQUIRE(caps.cpu_cores > 0);

    // Tier should be one of the valid values
    REQUIRE((caps.tier == PlatformTier::EMBEDDED || caps.tier == PlatformTier::BASIC ||
             caps.tier == PlatformTier::STANDARD));

    // max_chart_points should match tier
    if (caps.tier == PlatformTier::EMBEDDED) {
        REQUIRE(caps.max_chart_points == 50);
    } else if (caps.tier == PlatformTier::BASIC) {
        REQUIRE(caps.max_chart_points == 50);
    } else {
        REQUIRE(caps.max_chart_points == 200);
    }
}
