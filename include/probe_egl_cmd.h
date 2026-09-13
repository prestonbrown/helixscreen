// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix {
namespace probe {

/// Bring EGL up on this board's scanout node, report what answered, and tear it
/// down again. Implements the `--probe-egl` one-shot.
///
/// This is the gate the launcher uses to decide whether to run the EGL build of
/// helix-screen or step down to the DRM dumb-buffer build. It is a separate
/// process from the UI on purpose: a GPU bring-up that aborts inside the driver
/// takes down only the probe, and the launcher reads that as a refusal.
///
/// Runs before logging, config, the instance lock and any chdir, so it answers
/// on a board that already has HelixScreen running and needs no config dir.
/// Output is one line on stdout; the exit status is the verdict.
///
/// @return 0 when EGL comes up on real GPU hardware, non-zero otherwise —
///         including a build with no EGL compiled in, so a launcher probing a
///         binary that cannot answer gets a refusal rather than a booted UI.
int run_probe_egl();

} // namespace probe
} // namespace helix
