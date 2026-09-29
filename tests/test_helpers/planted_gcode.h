// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mock_planted_gcodes.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace helix {

/// A .gcode planted in the mock printer's virtual gcodes root for one test.
///
/// It lands in this process's planted overlay ($TMPDIR/helix_planted_<pid>),
/// which the mocks list, read and delete from like assets/test_gcodes, so what
/// is on disk is what the next get_directory reports: a delete driven through
/// the mock removes the real file, and remove_from_disk() stands in for an
/// operation that already happened on the printer's storage. The shared
/// assets/test_gcodes stays untouched, so parallel test processes never see
/// each other's files (the mock's history totals scan it).
class PlantedGcode {
  public:
    /// @param name File name; @param subdir Optional directory relative to
    /// the gcodes root, created when absent and removed on destruction.
    explicit PlantedGcode(const std::string& name, const std::string& subdir = "",
                          const std::string& content = "; planted for a test\nG28\n") {
        root_ = std::filesystem::temp_directory_path().string() + "/helix_planted_" +
                std::to_string(::getpid());
        helix::mock::set_planted_gcode_dir(root_);
        subdir_ = subdir;
        const std::string dir = subdir.empty() ? root_ : root_ + "/" + subdir;
        std::filesystem::create_directories(dir);
        path_ = dir + "/" + name;
        std::ofstream out(path_, std::ios::trunc | std::ios::binary);
        out << content;
        REQUIRE(out.good());
    }

    ~PlantedGcode() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        if (!subdir_.empty()) {
            std::filesystem::remove(root_ + "/" + subdir_, ec); // empty now; leave parents alone
        }
        std::filesystem::remove(root_, ec); // only once the last planted file is gone
    }

    PlantedGcode(const PlantedGcode&) = delete;
    PlantedGcode& operator=(const PlantedGcode&) = delete;

    bool on_disk() const {
        return std::filesystem::exists(path_);
    }

    /// Take the file off disk behind the panel's back, so only a notification
    /// can tell it the listing is stale. The dtor's remove() tolerates the file
    /// already being gone.
    bool remove_from_disk() {
        return std::remove(path_.c_str()) == 0;
    }

    /// The basename, the way a listing of the file's directory reports it.
    std::string name() const {
        return std::filesystem::path(path_).filename().string();
    }

    /// The path relative to the gcodes root — the form Moonraker's queue
    /// addresses the file by.
    std::string relative() const {
        return subdir_.empty() ? name() : subdir_ + "/" + name();
    }

  private:
    std::string path_;
    std::string root_;
    std::string subdir_;
};

} // namespace helix
