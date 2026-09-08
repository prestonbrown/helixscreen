// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme_token_table.h"

#include <cstdlib>
#include <cstring>

namespace helix::theme_tokens {

bool enabled() {
    static const bool on = [] {
        if (const char* env = std::getenv("HELIX_TOKEN_TABLE")) {
            return env[0] == '1';
        }
        // Installed builds read the table; dev builds scan.
        //
        // Aggregating tokens live reopens every top-level ui_xml file once per
        // aggregation call, ~28 times a boot, and that scan is most of what
        // theme_manager_init spends: 7.2s of a 16.8s splash on a 480x272 QIDI Q2
        // reading from eMMC. The table answers the same queries with no I/O.
        //
        // ESP32 ships ui_xml as a read-only frogfs image and cross-built release
        // targets install files nobody edits in place, which is already why hot
        // reload defaults off there (RuntimeConfig::hot_reload_enabled). A native
        // dev build keeps scanning so edit-XML-and-relaunch still moves tokens,
        // and HELIX_TOKEN_TABLE=0 restores scanning on a device that needs it.
#if defined(HELIX_PLATFORM_ESP32) || defined(HELIX_RELEASE_BUILD) ||                               \
    defined(HELIX_TOKEN_TABLE_DEFAULT_ON)
        return true;
#else
        return false;
#endif
    }();
    return on;
}

bool covers(const char* element_type) {
    for (size_t i = 0; i < k_token_table_count; ++i) {
        if (std::strcmp(k_token_table[i].type, element_type) == 0) {
            return true;
        }
    }
    return false;
}

bool answers_from_table(bool table_enabled, const char* element_type, const char* directory,
                        const char* canonical_dir) {
    if (!table_enabled || element_type == nullptr || directory == nullptr ||
        canonical_dir == nullptr) {
        return false;
    }
    return covers(element_type) && std::strcmp(directory, canonical_dir) == 0;
}

std::unordered_map<std::string, std::string> for_element(const char* element_type) {
    std::unordered_map<std::string, std::string> out;
    for (size_t i = 0; i < k_token_table_count; ++i) {
        const TokenEntry& e = k_token_table[i];
        if (std::strcmp(e.type, element_type) == 0) {
            out[e.name] = e.value;
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> for_suffix(const char* element_type,
                                                        const char* suffix) {
    std::unordered_map<std::string, std::string> out;
    const size_t slen = std::strlen(suffix);
    for (size_t i = 0; i < k_token_table_count; ++i) {
        const TokenEntry& e = k_token_table[i];
        if (std::strcmp(e.type, element_type) != 0) {
            continue;
        }
        const size_t nlen = std::strlen(e.name);
        // Same predicate as the runtime scanner (ends_with_suffix): name must
        // be at least as long as the suffix and end with it; key is the
        // stripped base (empty when the name IS the suffix, matching the
        // scanner).
        if (nlen >= slen && std::strcmp(e.name + nlen - slen, suffix) == 0) {
            out[std::string(e.name, nlen - slen)] = e.value;
        }
    }
    return out;
}

} // namespace helix::theme_tokens
