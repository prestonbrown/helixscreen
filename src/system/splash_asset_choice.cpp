// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "splash_asset_choice.h"

#include "prerender_size_class.h"

namespace helix {

namespace {

std::string canvas_path(const std::string& mode, const std::string& cls) {
    return "assets/images/prerendered/splash-3d-" + mode + "-" + cls + ".bin";
}

std::string logo_bin_path(const std::string& cls) {
    return "assets/images/prerendered/splash-logo-" + cls + ".bin";
}

} // namespace

SplashChoice choose_splash_asset(int width, int height, bool dark_mode,
                                 const SplashAssetResolver& resolve) {
    SplashChoice choice;
    const std::string mode = dark_mode ? "dark" : "light";
    choice.size_class = get_splash_3d_size_name(width, height);

    std::string found = resolve(canvas_path(mode, choice.size_class));

    // A 480x400 panel takes the tiny canvas when its own is absent. The class
    // moves with it, so the height check below and the logo lookup further down
    // both ask about the canvas actually in hand.
    if (found.empty() && choice.size_class == "small") {
        const std::string fallback = resolve(canvas_path(mode, "tiny"));
        if (!fallback.empty()) {
            choice.size_class = "tiny";
            found = fallback;
        }
    }

    // A canvas is drawn at its own size, so one taller than the panel would be
    // clipped. Discard it and take a scaled fallback instead.
    if (!found.empty()) {
        const int canvas_h = get_splash_3d_target_height(choice.size_class.c_str());
        if (canvas_h > 0 && canvas_h > height) {
            choice.canvas_too_tall = true;
            found.clear();
        }
    }

    if (!found.empty()) {
        choice.kind = SplashAssetKind::FullScreen3DBin;
        choice.path = found;
        return choice;
    }

    // The 3D source scales to any panel, so it is the better fallback than the
    // flat logo whenever it is present - including when a canvas existed but did
    // not fit.
    found = resolve("assets/images/helixscreen-logo-3d-" + mode + ".png");
    if (!found.empty()) {
        choice.kind = SplashAssetKind::Source3DPng;
        choice.path = found;
        return choice;
    }

    // The class here is the selected one in practice: step 2 only moves it when
    // the tiny canvas is taken, and that path has already returned. Asking about
    // choice.size_class keeps the two in step regardless.
    found = resolve(logo_bin_path(choice.size_class));
    if (!found.empty()) {
        choice.kind = SplashAssetKind::LogoBin;
        choice.path = found;
        return choice;
    }

    found = resolve("assets/images/helixscreen-logo.png");
    if (!found.empty()) {
        choice.kind = SplashAssetKind::LogoPng;
        choice.path = found;
    }
    return choice;
}

} // namespace helix
