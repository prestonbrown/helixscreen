// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform_table.h"

namespace helix::platform {

namespace {

// FlashForge zmod config: IFS slot truth and user-defined filament types.
const std::vector<std::string> kZmodDiagnosticFiles = {
    "/server/files/config/Adventurer5M.json",
    "/server/files/config/mod_data/user.cfg",
};

// K1 series: the launch hook's SSH state log (assets/config/platform/
// hooks-k1.sh), in Moonraker's logs root.
const std::vector<std::string> kK1DiagnosticFiles = {
    "/server/files/logs/helixscreen-ssh.log",
};

// The unified MIPS build runs on both boards, so it asks for both boards'
// files; the one a board lacks is a 404, which the collector skips.
std::vector<std::string> mips_diagnostic_files() {
    std::vector<std::string> files = kZmodDiagnosticFiles;
    files.insert(files.end(), kK1DiagnosticFiles.begin(), kK1DiagnosticFiles.end());
    return files;
}

constexpr uint8_t kElf32 = 1, ELF64 = 2, LE = 1;
constexpr uint16_t EM_ARM_ = 0x28, EM_AARCH64_ = 0xB7, EM_X86_64_ = 0x3E, EM_MIPS_ = 0x08;

// Rows mirror the toolchains in mk/cross.mk. "ad5x" and "k1" name the board
// behind the unified "mips" key.
const std::vector<Info> kPlatforms = {
    {"pi", "Raspberry Pi", false, ELF64, LE, EM_AARCH64_, {}},
    {"pi32", "Raspberry Pi (32-bit)", false, kElf32, LE, EM_ARM_, {}},
    {"x86", "x86 Desktop", false, ELF64, LE, EM_X86_64_, {}},
    {"x86-desktop", "x86 Linux Desktop (windowed)", false, ELF64, LE, EM_X86_64_, {}},
    {"ad5m", "FlashForge Adventurer 5M", true, kElf32, LE, EM_ARM_, kZmodDiagnosticFiles},
    {"ad5x", "FlashForge Adventurer 5X", true, kElf32, LE, EM_MIPS_, kZmodDiagnosticFiles},
    {"mips", "MIPS (K1 series / AD5X)", true, kElf32, LE, EM_MIPS_, mips_diagnostic_files()},
    {"k1", "Creality K1", true, kElf32, LE, EM_MIPS_, kK1DiagnosticFiles},
    // One build serves the K2, K2 Pro and K2 Plus, so the name covers all three.
    {"k2", "Creality K2", true, kElf32, LE, EM_ARM_, {}},
    {"cc1", "Elegoo Centauri Carbon", true, kElf32, LE, EM_ARM_, {}},
    {"snapmaker-u1", "Snapmaker U1", true, ELF64, LE, EM_AARCH64_, {}},
    // The K-Touch ships a firmware image, never an ELF release zip.
    {"esp32", "BTT K-Touch", false, 0, 0, 0, {}},
    // The app arrives as an APK, from Google Play or the release page.
    {"android", "Android", false, 0, 0, 0, {}},
};

} // namespace

const Info* find(const std::string& key) {
    for (const auto& p : kPlatforms) {
        if (key == p.key) {
            return &p;
        }
    }
    return nullptr;
}

bool installs_from_release_asset(const std::string& key) {
    return key != "android";
}

std::vector<std::string> release_asset_keys(const std::string& key, board::Board board) {
    if (!installs_from_release_asset(key)) {
        return {};
    }
    std::vector<std::string> keys{key};
    const char* legacy = board::info(board).legacy_release_asset;
    if (legacy != nullptr && key != legacy) {
        keys.emplace_back(legacy);
    }
    return keys;
}

std::string display_name(const std::string& key) {
    const auto* p = find(key);
    return p ? p->display_name : key;
}

bool elf_header_matches(const Info& platform, const uint8_t (&header)[20]) {
    if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F') {
        return false;
    }
    const uint16_t machine = header[5] == LE
                                 ? static_cast<uint16_t>(header[18] | (header[19] << 8))
                                 : static_cast<uint16_t>((header[18] << 8) | header[19]);
    return header[4] == platform.elf_class && header[5] == platform.elf_data &&
           machine == platform.elf_machine;
}

std::string current_key() {
#ifdef HELIX_PLATFORM_AD5M
    return "ad5m";
#elif defined(HELIX_PLATFORM_CC1)
    return "cc1";
#elif defined(HELIX_PLATFORM_MIPS)
    // One binary serves the K1 series, the AD5X and the Creator 5 line, so one
    // platform key. Which board the binary is ON is a separate runtime question
    // (helix::board::current()); it also picks the legacy release asset a
    // pre-unification release lists this build under.
    return "mips";
#elif defined(HELIX_PLATFORM_K1)
    // k1-dynamic build variant: dev/debug dynamic-linked K1 binary. Not in the
    // release matrix today — map to "k1" so if it ever ships, self-update
    // fetches the static K1 tarball instead of silently falling through to pi.
    return "k1";
#elif defined(HELIX_PLATFORM_K2)
    return "k2";
#elif defined(HELIX_PLATFORM_X86)
    return "x86";
#elif defined(HELIX_PLATFORM_X86_DESKTOP)
    return "x86-desktop";
#elif defined(HELIX_PLATFORM_SNAPMAKER_U1)
    return "snapmaker-u1";
#elif defined(HELIX_PLATFORM_PI32)
    return "pi32";
#elif defined(HELIX_PLATFORM_ESP32)
    return "esp32";
#elif defined(HELIX_PLATFORM_ANDROID)
    return "android";
#else
    return "pi";
#endif
}

} // namespace helix::platform
