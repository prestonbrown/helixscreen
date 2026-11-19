// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Manages font and image registration with LVGL XML system
 *
 * Provides static methods for registering fonts and images that can be
 * referenced by name in XML layout files.
 *
 * All methods are static since assets are registered globally with LVGL.
 * Registration is idempotent - calling multiple times is safe.
 *
 * Font registration is breakpoint-aware: fonts only used at larger breakpoints
 * are skipped on smaller screens, saving ~500-800KB of .rodata pages. The skip
 * is a deferral rather than a latch — register_fonts_for_tier() adds the missing
 * tiers when a runtime resize raises the breakpoint (#1210).
 *
 * @code
 * // Register all assets at startup
 * AssetManager::register_all();
 *
 * // Or register separately
 * AssetManager::register_fonts();
 * AssetManager::register_images();
 * @endcode
 */
class AssetManager {
  public:
    /**
     * @brief Register fonts with LVGL XML system, skipping unused sizes
     *
     * Uses the current LVGL display's constrained axis to determine the active
     * breakpoint and defer fonts that are only used at larger breakpoints.
     * Thin wrapper over register_fonts_for_tier().
     *
     * Registers:
     * - MDI icon fonts (16, 24, 32, 48, 64px) — all breakpoints
     * - Noto Sans regular fonts (10-28px) — subset by breakpoint
     * - Noto Sans bold fonts (14-28px) — all breakpoints (used in watchdog/modals)
     * - Noto Sans light fonts (10-18px) — subset by breakpoint
     * - Montserrat aliases (for XML compatibility) — subset by breakpoint
     */
    static void register_fonts();

    /**
     * @brief Register the font tiers needed by an explicit breakpoint tier
     *
     * Re-entrant counterpart of register_fonts(). Remembers the highest tier
     * registered so far and, on a call for a HIGHER tier, registers only the
     * faces the additional tiers unlock. A call for the same or a lower tier is
     * a no-op: fonts are never unregistered, because the lv_font_t objects are
     * static .rodata and live widgets hold pointers into them.
     *
     * @param tier UiBreakpoint index (0 = Micro … 6 = XXLarge), clamped
     * @return Number of faces registered by this call (0 = nothing to do)
     */
    static int register_fonts_for_tier(int tier);

    /**
     * @brief Highest breakpoint tier whose fonts are registered
     * @return UiBreakpoint index, or -1 if register_fonts() has never run
     */
    static int registered_font_tier();

    /**
     * @brief Every text face name registered once the breakpoint reaches @p tier
     *
     * The registration table itself, in a build of this binary's font tier.
     * A scaled-up font token resolves only to a registered name, so the CJK
     * filename bake and prune size their bins from this
     * (scripts/translations/registered_text_fonts.txt, pinned by [font_tier]).
     */
    static std::vector<std::string> text_font_names_for_breakpoint(int tier);

    /**
     * @brief Drop registration bookkeeping so a test can replay startup
     *
     * Does NOT unregister anything from LVGL — nothing can. Only the
     * high-water mark and the "already done" flags are cleared.
     */
    static void reset_for_test();

    /**
     * @brief Register all images with LVGL XML system
     *
     * Registers common images used in XML layouts:
     * - Printer placeholder images
     * - Filament spool graphics
     * - Thumbnail placeholders
     * - SVG icons
     */
    static void register_images();

    /**
     * @brief Register all assets (fonts and images)
     *
     * Convenience method that calls register_fonts() and register_images().
     */
    static void register_all();

    /**
     * @brief Check if fonts have been registered
     * @return true if register_fonts() has been called
     */
    static bool fonts_registered();

    /**
     * @brief Check if images have been registered
     * @return true if register_images() has been called
     */
    static bool images_registered();

  private:
    static bool s_fonts_registered;
    static bool s_images_registered;
    static int s_registered_font_tier;
};
