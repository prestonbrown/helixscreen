// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ethernet_manager_test_access.h
 * @brief The friend accessor for reaching EthernetManager's backend from tests.
 *
 * Mirrors WiFiManagerTestAccess::backend() — the backend is picked
 * automatically in the constructor, so a test that needs to drive the mock's
 * connected state (EthernetBackendMock::set_connected_state) has to reach it
 * through the manager rather than construct its own.
 */

#include "ethernet_manager.h"

// EthernetManager is a global-scope class (no namespace), so this accessor
// is too -- a namespace-qualified friend wouldn't match.
class EthernetManagerTestAccess {
  public:
    static EthernetBackend* backend(const EthernetManager& mgr) {
        return mgr.backend_.get();
    }
};
