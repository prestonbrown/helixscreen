// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_error_reporting.h"

#include "config_storage.h"

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
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

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
    explicit FileConfigStorage(std::string path) : path_(std::move(path)) {}

    std::optional<std::string> load() override {
        struct stat st;
        if (stat(path_.c_str(), &st) != 0) {
            return std::nullopt; // absent — first boot
        }
        std::ifstream in(path_);
        if (!in.is_open()) {
            // Present but unreadable (e.g. permission denied) — distinct
            // from "absent" so Config::init() can route this into
            // corrupt-preserve + backup-restore instead of silently
            // treating a locked-down existing config as first-boot.
            int err = errno;
            throw std::runtime_error(
                fmt::format("failed to open {} for reading: {}", path_, errno_reason(err)));
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool store(const std::string& bytes) override {
        // Atomic save: symlink-resolve, write .tmp, fsync, rename, fsync
        // parent dir. Moved verbatim from Config::save() (see #943: without
        // the fsyncs a power cycle can leave settings.json empty on
        // flash-backed filesystems).
        try {
            std::string target_path = path_;
            {
                std::error_code ec;
                if (fs::is_symlink(path_, ec)) {
                    auto real = fs::canonical(path_, ec);
                    if (!ec) {
                        spdlog::debug("[ConfigStorage] Resolved symlink {} -> {}", path_,
                                      real.string());
                        target_path = real.string();
                    }
                }
            }

            std::string tmp_path = target_path + ".tmp";
            {
                std::ofstream o(tmp_path);
                if (!o.is_open()) {
                    std::string reason = errno_reason(errno);
                    NOTIFY_ERROR("Could not save settings: {}", reason);
                    LOG_ERROR_INTERNAL("Failed to open temp file for writing: {} ({})", tmp_path,
                                       reason);
                    CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                                        fmt::format("open failed: {}", reason));
                    return false;
                }

                o << bytes;
                o.flush();

                if (!o.good()) {
                    std::string reason = errno_reason(errno);
                    NOTIFY_ERROR("Failed to save settings: {}", reason);
                    LOG_ERROR_INTERNAL("Failed to write config to {}: {}", tmp_path, reason);
                    CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                                        fmt::format("write error: {}", reason));
                    std::remove(tmp_path.c_str());
                    return false;
                }
            }

            {
                int fd = ::open(tmp_path.c_str(), O_RDONLY);
                if (fd >= 0) {
                    (void)::fsync(fd);
                    ::close(fd);
                }
            }

            if (std::rename(tmp_path.c_str(), target_path.c_str()) != 0) {
                NOTIFY_ERROR("Failed to save configuration file");
                LOG_ERROR_INTERNAL("Failed to rename temp file '{}' to '{}': {}", tmp_path,
                                   target_path, strerror(errno));
                CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                                    fmt::format("rename failed: {}", strerror(errno)));
                std::remove(tmp_path.c_str());
                return false;
            }

            {
                std::string dir = fs::path(target_path).parent_path().string();
                if (!dir.empty()) {
                    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
                    if (dfd >= 0) {
                        (void)::fsync(dfd);
                        ::close(dfd);
                    }
                }
            }

            // The rolling backup is Config::save()'s job, not the backend's —
            // whether a document is worth preserving is policy, not byte
            // movement.
            return true;
        } catch (const std::exception& e) {
            NOTIFY_ERROR("Failed to save configuration: {}", e.what());
            LOG_ERROR_INTERNAL("Exception while saving config to {}: {}", path_, e.what());
            CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                                fmt::format("exception: {}", e.what()));
            return false;
        }
    }

    void preserve_corrupt() override {
        std::string corrupt_path = path_ + ".corrupt";
        std::rename(path_.c_str(), corrupt_path.c_str());
        spdlog::info("[ConfigStorage] Corrupt config saved to {}", corrupt_path);
    }

    bool read_only() override {
        // Write-probe, moved verbatim from Config::init() (lines 1340-1359).
        fs::path config_dir = fs::path(path_).parent_path();
        std::string probe_path = (config_dir / ".helix-write-probe").string();
        std::ofstream probe(probe_path);
        if (!probe.is_open()) {
            int err = errno;
            if (err == EROFS || err == EACCES) {
                spdlog::warn("[ConfigStorage] Read-only filesystem detected ({})", strerror(err));
                return true;
            }
            return false;
        }
        probe.close();
        std::remove(probe_path.c_str());
        return false;
    }

    std::string describe() const override {
        return path_;
    }

  private:
    std::string path_;
};

} // namespace

std::unique_ptr<ConfigStorage> make_file_config_storage(const std::string& path) {
    return std::make_unique<FileConfigStorage>(path);
}

} // namespace helix
