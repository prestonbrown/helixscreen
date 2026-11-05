// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.h"
#include "config_dir_guard.h"

#include <fstream>
#include <string>

#include "hv/json.hpp"

namespace helix::test {

/// Base for tests of the versioned config migrations. The sandboxed HELIX_CONFIG_DIR keeps
/// Config's backup-restore search paths inside a per-process directory, so concurrent shards
/// never clobber each other's settings.json mid-migration.
class ConfigMigrationFixture {
  public:
    ConfigMigrationFixture() : config_path(guard_.dir.string() + "/settings.json") {}
    ~ConfigMigrationFixture() {
        config.clear_path();
    }

  protected:
    /// Write a settings.json, then init Config from it so the real versioned migrations run.
    void write_and_init(const nlohmann::json& contents) {
        std::ofstream(config_path) << contents.dump(2);
        config.init(config_path);
    }

  private:
    helix::ConfigDirGuard guard_{"config_migration"};

  protected:
    std::string config_path;
    Config config;
};

} // namespace helix::test
