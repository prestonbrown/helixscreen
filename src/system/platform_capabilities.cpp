// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file platform_capabilities.cpp
 * @brief Implementation of hardware capability detection
 *
 * Parses /proc/meminfo and /proc/cpuinfo on Linux systems to detect
 * hardware metrics and classify the platform tier.
 * On macOS, uses sysctl for hardware detection.
 */

#include "platform_capabilities.h"

#include "core/text_io.h"
#include "helix_regex.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <optional>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace helix {

// ============================================================================
// Helper functions
// ============================================================================

namespace {

/**
 * @brief Read entire file content as string
 * @param path File path to read
 * @return File content, or empty string on failure
 */
std::string read_file_content(const std::string& path) {
    return helix::text_io::read_file(path).value_or("");
}

/**
 * @brief Classify hardware metrics into a platform tier
 */
PlatformTier classify_tier(size_t ram_mb, int cores) {
    // EMBEDDED: RAM < 512MB OR single core
    // Note: cores <= 1 includes cores=0 (parse failure) and cores=1 (single core)
    // Both cases should be treated as EMBEDDED for safety
    if (ram_mb < PlatformCapabilities::kEmbeddedRamThresholdMb || cores <= 1) {
        return PlatformTier::EMBEDDED;
    }

    // STANDARD: RAM >= 768MB AND 4+ cores
    if (ram_mb >= PlatformCapabilities::kStandardRamThresholdMb &&
        cores >= PlatformCapabilities::kStandardCpuCoresMin) {
        return PlatformTier::STANDARD;
    }

    // Everything else is BASIC
    return PlatformTier::BASIC;
}

/**
 * @brief Set derived capabilities based on tier
 */
void set_derived_capabilities(PlatformCapabilities& caps) {
    switch (caps.tier) {
    case PlatformTier::EMBEDDED:
        // Temp graphs already run 1200 live points on AD5M (EMBEDDED),
        // so a static 132-point frequency response chart is lighter
        caps.supports_charts = true;
        caps.supports_animations = false;
        caps.max_chart_points = 50;
        break;

    case PlatformTier::BASIC:
        caps.supports_charts = true;
        caps.supports_animations = false;
        caps.max_chart_points = PlatformCapabilities::kBasicChartPoints;
        break;

    case PlatformTier::STANDARD:
        caps.supports_charts = true;
        caps.supports_animations = true;
        caps.max_chart_points = PlatformCapabilities::kStandardChartPoints;
        break;
    }
}

// ============================================================================
// macOS-specific detection
// ============================================================================

#ifdef __APPLE__
/**
 * @brief Get total RAM in MB on macOS using sysctl
 * @return Total RAM in MB, or 0 on failure
 */
size_t get_macos_ram_mb() {
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) {
        return static_cast<size_t>(memsize / (1024 * 1024)); // bytes to MB
    }
    return 0;
}

/**
 * @brief Get CPU core count on macOS using sysctl
 * @return Number of CPU cores, or 0 on failure
 */
int get_macos_cpu_cores() {
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.ncpu", &ncpu, &len, nullptr, 0) == 0) {
        return ncpu;
    }
    return 0;
}
#endif

} // namespace

// ============================================================================
// /proc/meminfo parsing
// ============================================================================

uint64_t parse_meminfo_kb(const std::string& content, const std::string& key) {
    if (key.empty()) {
        return 0;
    }
    // Format: "MemTotal:        3884136 kB"
    const std::string prefix = key + ":";
    for (size_t pos = 0; pos < content.size();) {
        const size_t eol = std::min(content.find('\n', pos), content.size());
        if (content.compare(pos, prefix.size(), prefix) == 0) {
            return helix::text_io::parse_leading<unsigned long long>(
                       std::string_view(content).substr(pos + prefix.size(),
                                                        eol - pos - prefix.size()))
                .value_or(0);
        }
        pos = eol + 1;
    }
    return 0;
}

size_t parse_meminfo_total_mb(const std::string& content) {
    return static_cast<size_t>(parse_meminfo_kb(content, "MemTotal") / 1024);
}

// ============================================================================
// /proc/cpuinfo parsing
// ============================================================================

CpuInfo parse_cpuinfo(const std::string& content) {
    CpuInfo info;

    if (content.empty()) {
        return info;
    }

    // Count processor entries
    // Each CPU core has a "processor : N" line
    helix::Regex processor_regex(R"(processor\s*:\s*\d+)");
    auto proc_begin = helix::RegexIterator(content, processor_regex);
    auto proc_end = helix::RegexIterator();
    info.core_count = static_cast<int>(std::distance(proc_begin, proc_end));

    // Extract BogoMIPS (first occurrence)
    // Format: "BogoMIPS : 270.00" or "bogomips : 3999.93"
    helix::Regex bogomips_regex(R"([Bb]ogo[Mm][Ii][Pp][Ss]\s*:\s*([0-9.]+))");
    helix::RegexMatch match;
    if (helix::regex_search(content, match, bogomips_regex) && match.size() > 1) {
        // Ignore parse failures
        info.bogomips = helix::text_io::parse_leading<float>(match[1].str()).value_or(0.0f);
    }

    // Extract CPU MHz if BogoMIPS not found or as supplement
    // Format: "cpu MHz : 2400.000"
    helix::Regex mhz_regex(R"(cpu MHz\s*:\s*([0-9.]+))");
    if (helix::regex_search(content, match, mhz_regex) && match.size() > 1) {
        // Ignore parse failures
        info.cpu_mhz =
            static_cast<int>(helix::text_io::parse_leading<float>(match[1].str()).value_or(0.0f));
    }

    // Each core's design. A block per core carries its implementer and part; only
    // Arm's own (implementer 0x41) part numbers are known here.
    static constexpr int kEfficiencyParts[] = {0xc05, 0xc07, 0xd04, 0xd03, 0xd05, 0xd46, 0xd80};
    int named_cores = 0;
    bool all_efficiency = true;
    int implementer = -1;
    auto field = [](std::string_view l, std::string_view key) -> std::optional<int> {
        if (l.substr(0, key.size()) != key) {
            return std::nullopt;
        }
        const auto colon = l.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string value(l.substr(colon + 1));
        return static_cast<int>(std::strtol(value.c_str(), nullptr, 0));
    };
    for (std::string_view line : helix::text_io::lines(content)) {
        if (auto v = field(line, "CPU implementer")) {
            implementer = *v;
        } else if (auto part = field(line, "CPU part")) {
            named_cores++;
            all_efficiency = all_efficiency && implementer == 0x41 &&
                             std::find(std::begin(kEfficiencyParts), std::end(kEfficiencyParts),
                                       *part) != std::end(kEfficiencyParts);
            implementer = -1;
        }
    }
    info.efficiency_cores_only = named_cores > 0 && all_efficiency;

    // CPU name. No architecture publishes it under the same key: x86 has
    // "model name", ARM has "Hardware", MIPS has "cpu model", and some boards
    // only carry "Processor". First hit wins; a kernel with none of them leaves
    // the field empty for the caller to label.
    for (const char* key : {"model name", "Hardware", "cpu model", "Processor", "machine"}) {
        helix::Regex model_regex(std::string(key) + R"(\s*:\s*([^\n]+))");
        if (!helix::regex_search(content, match, model_regex) || match.size() <= 1) {
            continue;
        }
        std::string value = match[1].str();
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        if (!value.empty()) {
            info.model = value;
            break;
        }
    }

    return info;
}

// ============================================================================
// PlatformCapabilities implementation
// ============================================================================

PlatformCapabilities PlatformCapabilities::detect() {
#ifdef __APPLE__
    // macOS: sysctl, no bogomips
    auto caps = from_metrics(get_macos_ram_mb(), get_macos_cpu_cores(), 0.0f);
    spdlog::debug("Platform detected: RAM={}MB, cores={}, tier={}", caps.total_ram_mb,
                  caps.cpu_cores, platform_tier_to_string(caps.tier));
    return caps;
#else
#ifdef __ANDROID__
    spdlog::debug("Android platform: using /proc for hardware detection");
#endif
    return detect_from(read_file_content("/proc/meminfo"), read_file_content("/proc/cpuinfo"));
#endif
}

PlatformCapabilities PlatformCapabilities::detect_from(const std::string& meminfo,
                                                       const std::string& cpuinfo) {
    const CpuInfo cpu_info = parse_cpuinfo(cpuinfo);
    auto caps =
        from_metrics(parse_meminfo_total_mb(meminfo), cpu_info.core_count, cpu_info.bogomips);
    caps.efficiency_cores_only = cpu_info.efficiency_cores_only;

    spdlog::debug("Platform detected: RAM={}MB, cores={}, tier={}", caps.total_ram_mb,
                  caps.cpu_cores, platform_tier_to_string(caps.tier));
    return caps;
}

PlatformCapabilities PlatformCapabilities::from_metrics(size_t ram_mb, int cores,
                                                        float bogomips_val) {
    PlatformCapabilities caps;

    caps.total_ram_mb = ram_mb;
    caps.cpu_cores = cores;
    caps.bogomips = bogomips_val;

    // Classify tier and set derived capabilities
    caps.tier = classify_tier(ram_mb, cores);
    set_derived_capabilities(caps);

    return caps;
}

// ============================================================================
// Utility functions
// ============================================================================

bool full_style_effects_allowed(PlatformTier tier) {
    return tier == PlatformTier::STANDARD;
}

std::string platform_tier_to_string(PlatformTier tier) {
    switch (tier) {
    case PlatformTier::EMBEDDED:
        return "embedded";
    case PlatformTier::BASIC:
        return "basic";
    case PlatformTier::STANDARD:
        return "standard";
    }
    return "unknown";
}

} // namespace helix
