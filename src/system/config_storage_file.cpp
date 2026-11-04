// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_error_reporting.h"

#include "config_storage.h"
#include "core/text_io.h"
#include "helix_fs.h"
#include "system/helix_paths.h"

#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
#include "system/telemetry_manager.h"
#define CONFIG_RECORD_ERROR(...) TelemetryManager::instance().record_error(__VA_ARGS__)
#else
#define CONFIG_RECORD_ERROR(...) ((void)0)
#endif

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>

namespace hfs = helix::fs;

namespace helix {

namespace {

std::string errno_reason(int err) {
    switch (err) {
    case ENOSPC:
        return "disk full";
    case EROFS:
        return "read-only filesystem";
    case EACCES:
        return "permission denied";
    default:
        return strerror(err);
    }
}

class FileConfigStorage : public ConfigStorage {
  public:
    FileConfigStorage(std::string path, ConfigFootprint footprint)
        : path_(std::move(path)), footprint_(footprint) {}

    std::optional<std::string> load(std::string& read_error) override {
        struct stat st;
        if (stat(path_.c_str(), &st) != 0) {
            return std::nullopt; // absent — first boot
        }
        std::optional<std::string> text = helix::text_io::read_file(path_);
        if (!text) {
            // Present but unreadable (e.g. permission denied) — distinct
            // from "absent" so Config::init() can route this into
            // corrupt-preserve + backup-restore instead of silently
            // treating a locked-down existing config as first-boot.
            int err = errno;
            read_error = fmt::format("failed to open {} for reading: {}", path_, errno_reason(err));
            return std::nullopt;
        }
        return text;
    }

    bool store(const std::string& bytes) override {
        // Symlink-resolved, and fsynced: without the fsyncs a power cycle can
        // leave settings.json empty on flash-backed filesystems (#943).
        const std::string target_path = helix::paths::write_target(path_);
        if (!helix::text_io::write_file_atomic(target_path, bytes,
                                               helix::text_io::Durability::Fsync)) {
            std::string reason = errno_reason(errno);
            NOTIFY_ERROR("Could not save settings: {}", reason);
            LOG_ERROR_INTERNAL("Failed to save config to {}: {}", target_path, reason);
            CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                                fmt::format("save failed: {}", reason));
            return false;
        }

        // The rolling backup is Config::save()'s job, not the backend's —
        // whether a document is worth preserving is policy, not byte
        // movement.
        return true;
    }

    void preserve_corrupt() override {
        std::string corrupt_path = path_ + ".corrupt";
        std::rename(path_.c_str(), corrupt_path.c_str());
        spdlog::info("[ConfigStorage] Corrupt config saved to {}", corrupt_path);
    }

    bool read_only() override {
        // Write-probe, moved verbatim from Config::init() (lines 1340-1359).
        std::string probe_path = hfs::join_path(hfs::parent_path(path_), ".helix-write-probe");
        helix::text_io::File probe = helix::text_io::open_file(probe_path, "wb");
        if (!probe) {
            int err = errno;
            if (err == EROFS || err == EACCES) {
                spdlog::warn("[ConfigStorage] Read-only filesystem detected ({})", strerror(err));
                return true;
            }
            return false;
        }
        probe.reset();
        std::remove(probe_path.c_str());
        return false;
    }

    std::string describe() const override {
        return path_;
    }

    bool small_footprint() const override {
        return footprint_ == ConfigFootprint::Small;
    }

  private:
    std::string path_;
    ConfigFootprint footprint_;
};

} // namespace

std::unique_ptr<ConfigStorage> make_file_config_storage(const std::string& path,
                                                        ConfigFootprint footprint) {
    return std::make_unique<FileConfigStorage>(path, footprint);
}

} // namespace helix
