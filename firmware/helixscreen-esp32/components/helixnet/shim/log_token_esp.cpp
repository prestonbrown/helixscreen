// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one out-of-line emitter every tokenized log call reaches: encode the
// record, base64 it and write it to the console, with the build-id header line
// first and every kHeaderEvery records after. See log_token.h.

#include "esp_app_desc.h"
#include "esp_log.h"
#include "log_token.h"
#include "platform_stubs.h"

#include <atomic>

namespace {

constexpr const char* kTag = "helixnet";

esp_log_level_t esp_level(int level) {
    switch (level) {
    case 0:
        return ESP_LOG_VERBOSE;
    case 1:
        return ESP_LOG_DEBUG;
    case 2:
        return ESP_LOG_INFO;
    case 3:
        return ESP_LOG_WARN;
    default:
        return ESP_LOG_ERROR;
    }
}

std::atomic<unsigned> g_records{0};

void write_header() {
    // The first CONFIG_APP_RETRIEVE_LEN_ELF_SHA (9) hex digits of the ELF's SHA-256,
    // as the boot banner's "ELF file SHA256" prints them; the database records the full hash.
    char sha[65];
    esp_app_get_elf_sha256(sha, sizeof sha);
    esp_log_write(ESP_LOG_ERROR, kTag, "%s%s\n", helix::logtok::kHeaderPrefix, sha);
}

} // namespace

void helix_logtok_emit(int level, std::uint32_t token, fmt::format_args args) {
    if (g_records.fetch_add(1, std::memory_order_relaxed) % helix::logtok::kHeaderEvery == 0) {
        write_header();
    }
    std::uint8_t record[helix::logtok::kMaxRecordBytes];
    const std::size_t n = helix::logtok::encode_record(level, token, args, record, sizeof record);
    char line[helix::logtok::kMaxLineChars];
    line[0] = '$';
    helix::logtok::base64_encode(record, n, line + 1, sizeof line - 1);
    esp_log_write(esp_level(level), kTag, "%s\n", line);
}
