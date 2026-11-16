// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/text_io.h"
#include "helix_fs.h"

#include <string>
#include <utility>

namespace helix {

/// A crash-loop guard file that exists for exactly the lifetime of this object.
/// Wrap a call that can fault inside a GPU driver with it: every normal return,
/// a graceful failure included, removes the file, and only a process death inside
/// the scope (a driver SIGSEGV unwinds nothing) leaves it for the next startup to
/// promote into a persistent block (`promote_surviving_gpu_guards()`).
class ScopedGuardFile {
  public:
    explicit ScopedGuardFile(std::string path)
        : path_(std::move(path)), armed_(text_io::write_file(path_, "1")) {}

    ~ScopedGuardFile() {
        if (armed_)
            fs::remove(path_);
    }

    ScopedGuardFile(const ScopedGuardFile&) = delete;
    ScopedGuardFile& operator=(const ScopedGuardFile&) = delete;

    /// False when the file could not be written: the scope runs unguarded.
    bool armed() const {
        return armed_;
    }

    const std::string& path() const {
        return path_;
    }

  private:
    std::string path_;
    bool armed_;
};

} // namespace helix
