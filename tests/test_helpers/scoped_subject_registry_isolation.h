// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/static_subject_registry.h"

#include <utility>
#include <vector>

/// Runs a test against an empty StaticSubjectRegistry and puts the process-wide
/// entries back when it ends, slots and callbacks unchanged.
///
/// clear() drops entries no fixture re-creates: a source behind a process-once
/// guard (SwatchGridSubjects) never registers again, so a later test that expects
/// it fails or passes depending on which tests shared its process. Entries the test
/// registers are discarded, since they may close over its stack.
class ScopedSubjectRegistryIsolation {
  public:
    ScopedSubjectRegistryIsolation()
        : saved_(std::exchange(StaticSubjectRegistry::instance().deinitializers_, {})) {}
    ~ScopedSubjectRegistryIsolation() {
        StaticSubjectRegistry::instance().deinitializers_ = std::move(saved_);
    }
    ScopedSubjectRegistryIsolation(const ScopedSubjectRegistryIsolation&) = delete;
    ScopedSubjectRegistryIsolation& operator=(const ScopedSubjectRegistryIsolation&) = delete;

  private:
    std::vector<StaticSubjectRegistry::DeinitEntry> saved_;
};
