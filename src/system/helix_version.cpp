// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file helix_version.cpp
 * @brief The only translation unit that sees the build's git hash.
 *
 * helix_git_hash.h is generated per build and holds nothing but the short
 * commit hash, so a new commit invalidates this object alone. A -D on the
 * global flags would put a value that changes every commit on every TU's command
 * line, which is the one input ccache's direct mode cannot look past.
 */

#include "helix_version.h"

#include "helix_git_hash.h" // generated: build/generated/helix_git_hash.h

#include <cstdio>

const char* helix_git_hash() {
    return HELIX_GIT_HASH;
}

const char* helix_version_full() {
    static char buf[64];
    static bool initialized = false;
    if (!initialized) {
        snprintf(buf, sizeof(buf), "%s (%s)", HELIX_VERSION, HELIX_GIT_HASH);
        initialized = true;
    }
    return buf;
}
