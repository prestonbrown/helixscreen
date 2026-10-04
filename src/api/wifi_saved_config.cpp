// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_saved_config.h"

#include "data_root_resolver.h"
#include "json_utils.h"
#include "log_redact.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "hv/json.hpp"

// Filesystem-type probing is not portable. Linux reports a numeric magic in
// statfs::f_type and declares statfs in <sys/vfs.h>; the BSDs (macOS included,
// which is where this builds during development) have no such magic and
// instead name the type in statfs::f_fstypename, declared in <sys/mount.h>.
#if defined(__linux__)
#include <sys/vfs.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6
#endif
#else
#include <sys/mount.h>
#include <sys/param.h>
#endif

namespace helix::wifi {

namespace detail {

bool is_volatile_path(const std::string& path) {
    struct statfs sfs {};
    if (::statfs(path.c_str(), &sfs) != 0)
        return false; // unknown — assume persistent rather than churn files

#if defined(__linux__)
    const auto type = static_cast<unsigned long>(sfs.f_type);
    return type == static_cast<unsigned long>(TMPFS_MAGIC) ||
           type == static_cast<unsigned long>(RAMFS_MAGIC);
#else
    // BSD/macOS name the type instead of numbering it. Matching by name keeps
    // the predicate meaningful on a dev machine rather than hardcoding false,
    // which would make the tmpfs branch untestable anywhere but a printer.
    const std::string type = sfs.f_fstypename;
    return type == "tmpfs" || type == "ramfs" || type == "devtmpfs";
#endif
}

} // namespace detail

namespace {

std::mutex g_target_mutex;
std::string g_persistent_target;

/// Mirror the target's mode and owner: an existing file keeps its own, a new
/// one inherits from its parent directory. We are writing the platform's file,
/// not ours.
struct FileIdentity {
    mode_t mode = 0644;
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
};

FileIdentity identity_to_mirror(const std::string& path) {
    FileIdentity id;
    struct stat st {};

    if (::stat(path.c_str(), &st) == 0) {
        id.mode = st.st_mode & 07777;
        id.uid = st.st_uid;
        id.gid = st.st_gid;
        return id;
    }

    const size_t slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
    if (::stat(dir.c_str(), &st) == 0) {
        id.mode = st.st_mode & 0666; // directory execute bits are not for files
        id.uid = st.st_uid;
        id.gid = st.st_gid;
    }
    return id;
}

bool write_mirrored(const std::string& path, const std::string& contents, const FileIdentity& id) {
    const std::string tmp = path + ".helix.tmp";

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, id.mode);
    if (fd < 0) {
        spdlog::error("[WifiSavedConfig] Cannot create {}: {}", tmp, std::strerror(errno));
        return false;
    }

    bool ok = true;
    const char* data = contents.data();
    size_t remaining = contents.size();
    while (remaining > 0) {
        const ssize_t n = ::write(fd, data, remaining);
        if (n <= 0) {
            spdlog::error("[WifiSavedConfig] Write failed on {}: {}", tmp, std::strerror(errno));
            ok = false;
            break;
        }
        data += n;
        remaining -= static_cast<size_t>(n);
    }

    if (ok) {
        ::fchmod(fd, id.mode);
        if (id.uid != static_cast<uid_t>(-1) && ::fchown(fd, id.uid, id.gid) != 0) {
            // Non-fatal: probably not root. Say so rather than fail the connect.
            spdlog::debug("[WifiSavedConfig] Could not mirror ownership on {}: {}", tmp,
                          std::strerror(errno));
        }
    }
    ::close(fd);

    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        spdlog::error("[WifiSavedConfig] Cannot replace {}: {}", path, std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace

void remember_persistent_target(const std::string& conf_path) {
    std::lock_guard<std::mutex> lock(g_target_mutex);
    g_persistent_target.clear();

    if (conf_path.empty())
        return;

    // Only a symlink tells us the platform intended somewhere else. A plain
    // file is either already persistent (nothing to do) or a link a previous
    // save already ate (nothing we can learn from).
    struct stat lst {};
    if (::lstat(conf_path.c_str(), &lst) != 0 || !S_ISLNK(lst.st_mode))
        return;

    char resolved[PATH_MAX];
    if (::realpath(conf_path.c_str(), resolved) == nullptr)
        return;

    const std::string target(resolved);
    if (target == conf_path)
        return;

    // If the link points at volatile storage too, mirroring buys nothing.
    if (detail::is_volatile_path(target)) {
        spdlog::debug("[WifiSavedConfig] {} resolves to {}, also volatile — nothing to mirror",
                      conf_path, target);
        return;
    }

    g_persistent_target = target;
    spdlog::info("[WifiSavedConfig] wpa_supplicant config {} is a link to persistent {} — will "
                 "restore it there if a save replaces the link",
                 conf_path, target);
}

std::string persistent_target() {
    std::lock_guard<std::mutex> lock(g_target_mutex);
    return g_persistent_target;
}

bool mirror_to_persistent(const std::string& conf_path) {
    const std::string target = persistent_target();
    if (target.empty() || conf_path.empty())
        return false;

    std::string contents;
    {
        std::ifstream in(conf_path, std::ios::binary);
        if (!in.is_open()) {
            spdlog::error("[WifiSavedConfig] Cannot read {} to mirror it", conf_path);
            return false;
        }
        contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (contents.empty())
        return false;

    if (!write_mirrored(target, contents, identity_to_mirror(target)))
        return false;

    // No SSID or PSK in the log: the file we just wrote holds the secret, and
    // the log must not become the second copy.
    spdlog::info("[WifiSavedConfig] wpa_supplicant saved to volatile {} — mirrored to {} so the "
                 "network survives a reboot",
                 conf_path, target);
    return true;
}

namespace store {

namespace {

/// Encode an arbitrary byte string into a representation guaranteed to be
/// valid UTF-8: printable ASCII passes through untouched, every other byte
/// becomes a literal `\xNN` escape (and a literal backslash becomes `\x5c`,
/// so decoding is unambiguous). Round-trips losslessly through unescape_bytes().
///
/// An SSID is a raw 802.11 octet string with no guarantee of being valid
/// UTF-8 at all — real routers broadcast arbitrary bytes, and
/// wpa_string_is_valid()'s `c < 32` control-character check only rejects them
/// when `char` is signed (GCC on ARM, i.e. every real target, defaults it
/// unsigned), so such SSIDs reach here unfiltered. Without this encoding,
/// write_store()'s dump() would substitute U+FFFD for the invalid bytes, and
/// that mangled text would never again string-equal the real SSID — dedup in
/// save() would silently stop matching and append a fresh duplicate
/// cleartext-PSK record on every single connect to that network, forever.
std::string escape_bytes(const std::string& raw) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c == '\\') {
            out += "\\x5c";
        } else if (c >= 0x20 && c < 0x7f) {
            out += static_cast<char>(c);
        } else {
            out += '\\';
            out += 'x';
            out += hex[(c >> 4) & 0xf];
            out += hex[c & 0xf];
        }
    }
    return out;
}

/// Inverse of escape_bytes(). Text with no `\xNN` escapes (every entry
/// written before this fix existed, plus any hand-edited/legacy file) passes
/// through unchanged, so this is safe to apply unconditionally to whatever
/// parse_store() reads back.
std::string unescape_bytes(const std::string& escaped) {
    std::string out;
    out.reserve(escaped.size());
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < escaped.size();) {
        if (escaped[i] == '\\' && i + 3 < escaped.size() && escaped[i + 1] == 'x') {
            const int hi = hex_val(escaped[i + 2]);
            const int lo = hex_val(escaped[i + 3]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 4;
                continue;
            }
        }
        out += escaped[i];
        ++i;
    }
    return out;
}

/// Parse the store's on-disk JSON. Any failure — missing file, empty file,
/// malformed JSON, wrong top-level shape — yields an empty vector rather than
/// throwing or propagating an exception to the caller. Fields are read and
/// written manually (rather than via nlohmann's to_json/from_json ADL hooks)
/// so field names stay an explicit, greppable on-disk format.
std::vector<SavedNetwork> parse_store(const std::string& contents) {
    if (contents.empty())
        return {};

    try {
        const auto j = nlohmann::json::parse(contents);
        if (!j.is_array())
            return {};

        std::vector<SavedNetwork> nets;
        nets.reserve(j.size());
        for (const auto& entry : j) {
            if (!entry.is_object())
                continue;
            SavedNetwork net;
            net.ssid = unescape_bytes(entry.value("ssid", std::string()));
            net.psk = unescape_bytes(entry.value("psk", std::string()));
            if (!net.ssid.empty())
                nets.push_back(std::move(net));
        }
        return nets;
    } catch (const nlohmann::json::exception&) {
        // Deliberately not logging e.what(): nlohmann embeds a "last read:
        // '<token>'" fragment of the buffered lexer state in parse_error
        // messages. If the file was corrupted or truncated partway through a
        // psk string value, that token is a chunk of a real cleartext
        // password — exactly the secret this store exists to protect. A
        // fixed message plus the path is all any caller needs; the content
        // of a corrupt store is not diagnosable from a log line anyway.
        spdlog::warn("[WifiSavedConfig] Store at {} is corrupt, treating as empty", store_path());
        return {};
    }
}

/// Write @p nets to the store atomically at mode 0600. This file is ours
/// alone — unlike write_mirrored()'s usual caller (mirror_to_persistent(),
/// which must preserve a vendor file's existing mode/ownership), there is no
/// inherited identity to respect here, so the mode is forced rather than
/// derived from identity_to_mirror().
bool write_store(const std::vector<SavedNetwork>& nets) {
    const std::string path = store_path();

    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::error_code ec;
        std::filesystem::create_directories(path.substr(0, slash), ec);
        if (ec) {
            spdlog::error("[WifiSavedConfig] Cannot create directory for {}: {}", path,
                          ec.message());
            return false;
        }
    }

    nlohmann::json j = nlohmann::json::array();
    for (const auto& net : nets)
        j.push_back(
            nlohmann::json{{"ssid", escape_bytes(net.ssid)}, {"psk", escape_bytes(net.psk)}});

    FileIdentity id;
    id.mode = 0600; // forced — see write_store() doc comment above

    // escape_bytes() above already guarantees every field is pure ASCII, so
    // serialization should never meet invalid UTF-8 here. safe_dump() is the
    // backstop: a throw out of this call would unwind out of connect_network()
    // past the point where connecting_in_progress_ gets reset, permanently
    // hanging the connect UI. The escaping is what keeps dedup working —
    // U+FFFD substitution would leave the stored text unable to string-equal
    // the real SSID, so every connect would append a fresh duplicate record.
    return write_mirrored(path, helix::json_util::safe_dump(j, 2), id);
}

} // namespace

std::string store_path() {
    return helix::writable_path("wifi_networks.json");
}

bool save(const SavedNetwork& net) {
    if (net.ssid.empty())
        return false;

    auto nets = load();
    auto it = std::find_if(nets.begin(), nets.end(),
                           [&](const SavedNetwork& n) { return n.ssid == net.ssid; });
    if (it != nets.end())
        *it = net;
    else
        nets.push_back(net);

    const bool ok = write_store(nets);
    // Counts and a redacted SSID only — net.psk must never reach a log line.
    if (ok)
        spdlog::info("[WifiSavedConfig] Saved '{}' to {} ({} network(s) stored)",
                     helix::redact::ssid(net.ssid), store_path(), nets.size());
    else
        spdlog::error("[WifiSavedConfig] Failed to save '{}' to {}", helix::redact::ssid(net.ssid),
                      store_path());
    return ok;
}

bool remove(const std::string& ssid) {
    auto nets = load();
    const size_t before = nets.size();
    nets.erase(std::remove_if(nets.begin(), nets.end(),
                              [&](const SavedNetwork& n) { return n.ssid == ssid; }),
               nets.end());
    if (nets.size() == before)
        return true; // nothing to remove; store already reflects the desired state

    const bool ok = write_store(nets);
    if (ok)
        spdlog::info("[WifiSavedConfig] Removed '{}' from {} ({} network(s) remain)",
                     helix::redact::ssid(ssid), store_path(), nets.size());
    else
        spdlog::error("[WifiSavedConfig] Failed to remove '{}' from {}", helix::redact::ssid(ssid),
                      store_path());
    return ok;
}

std::vector<SavedNetwork> load() {
    std::ifstream in(store_path(), std::ios::binary);
    if (!in.is_open())
        return {};

    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_store(contents);
}

} // namespace store

} // namespace helix::wifi
