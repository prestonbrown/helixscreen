// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>

/**
 * @file splash_asset_choice.h
 * @brief Which splash artwork a screen gets, decided once
 *
 * Two binaries draw a splash: `helix-splash` at boot and the app's own
 * `show_splash_screen()` once it is up. They hand off to each other on the same
 * display within a second or so, so any disagreement about which artwork to use
 * is visible as the picture changing under the user. This is the one decision
 * both of them make.
 *
 * It is pure. The caller supplies a resolver, because the two of them look in
 * different places - the boot binary stats paths relative to its working
 * directory, the app also searches the build tree - and takes back a path it can
 * hand straight to LVGL. Everything else, the widget tree and the animation, is
 * each caller's own business and deliberately not shared.
 *
 * The ladder, in order:
 *
 * 1. the full-screen canvas for the panel's size class
 * 2. the `tiny` canvas, when a `small` panel has none of its own
 * 3. neither, if the canvas found is taller than the panel
 * 4. the 3D logo PNG, centred and scaled
 * 5. the pre-rendered centred logo for the class settled on at step 2
 * 6. the logo PNG, centred and scaled
 */

namespace helix {

enum class SplashAssetKind {
    Nothing,         ///< Nothing resolved; the caller draws whatever it can.
    FullScreen3DBin, ///< Pre-rendered canvas at exactly the screen resolution.
    Source3DPng,     ///< 3D logo source, centred and scaled at runtime.
    LogoBin,         ///< Pre-rendered centred logo.
    LogoPng,         ///< Logo source, centred and scaled at runtime.
};

struct SplashChoice {
    SplashAssetKind kind = SplashAssetKind::Nothing;
    /// Whatever the resolver returned, ready for the caller to use as-is.
    std::string path;
    /// The class actually settled on, which is not always the one the panel
    /// selects: step 2 can move it. Callers that go on to ask
    /// get_splash_3d_target_height() must ask about THIS one.
    std::string size_class;
    /// True when a canvas existed but was taller than the panel. The caller has
    /// a scaled fallback either way; this is worth a log line because it means
    /// the panel has no canvas of its own.
    bool canvas_too_tall = false;
};

/// Return a usable path for `relative`, or "" when it is not there.
using SplashAssetResolver = std::function<std::string(const std::string& relative)>;

/**
 * @brief Pick the splash artwork for a screen
 *
 * @param width Effective display width in pixels, after any rotation
 * @param height Effective display height in pixels, after any rotation
 * @param dark_mode Selects the dark or light variant
 * @param resolve Maps a relative asset path to a usable one, or ""
 */
[[nodiscard]] SplashChoice choose_splash_asset(int width, int height, bool dark_mode,
                                               const SplashAssetResolver& resolve);

} // namespace helix
