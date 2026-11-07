// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file display_metrics.h
 * @brief Physical display metrics and the UI scale factor derived from them.
 *
 * Every size constant in the UI is authored in pixels against a 160 DPI
 * reference, and the breakpoint ladder is selected from raw pixel dimensions.
 * That makes resolution a proxy for physical screen size, and the proxy breaks
 * on high-DPI panels: proportions stay correct while everything shrinks
 * physically in lockstep. A 1080x2400 phone at ~405 DPI draws a 192px grid cell
 * that measures 12mm, against the ~30mm the ladder intends.
 *
 * The fix is a multiplicative UI scale factor applied on top of the existing
 * ladder. Breakpoint selection stays on raw pixels, so no existing device can
 * change tier — regression is structurally impossible rather than merely
 * unlikely.
 *
 * @note This scale is deliberately NOT fed to lv_display_set_dpi(). LVGL
 *       derives padding from its DPI via LV_DPX_CALC, so raising it would
 *       resize padding on every shipping device. Application keeps forcing
 *       LV_DPI_DEF there, which is also what makes the UI immune to lying
 *       kernel drivers (see application.cpp). The two concepts stay separate.
 */

#pragma once

#include "board_identity.h"

#include <cstdint>
#include <optional>
#include <string>

namespace helix {

/// Physical description of a panel that is permanently attached to a board.
///
/// Only boards whose display is integrated get an entry. A Raspberry Pi, a
/// CB1, or an x86 desktop drives a user-chosen panel, so panel size is not a
/// property of those (Board::Generic) and they are deliberately absent.
struct PanelSpec {
    int32_t width_px;
    int32_t height_px;
    double diagonal_mm;
    /// Rotation in degrees that shows the UI upright in the printer's own mount,
    /// or -1 where that is left to the first-boot rotation probe.
    int upright_rotation = -1;
};

/// Where a resolved DPI value came from. Ordered by descending trust.
enum class DpiSource {
    UserOverride,     ///< --dpi or the UI scale setting. Always wins.
    PlatformMeasured, ///< A platform we trust to measure honestly (Android).
    KnownPanel,       ///< Static fact about an integrated panel.
    Fallback,         ///< Nothing usable; the 160 DPI authoring reference.
};

struct ResolvedDpi {
    double dpi;
    DpiSource source;
};

/// Everything the UI's size follows from, decided in one place for startup and
/// for every later change of the drawing surface.
struct ScaleDecision {
    ResolvedDpi resolved; ///< Per logical pixel
    double auto_scale;    ///< From the DPI alone
    double ui_scale;      ///< After the stored setting
    int32_t lvgl_dpi;     ///< kReferenceDpi scaled by ui_scale, for lv_display_set_dpi
};

/// Human-readable source name, for the one startup log line that records how
/// the UI decided its scale. Worth logging: when a user reports the UI is the
/// wrong size, which source won is the first thing to know.
const char* dpi_source_name(DpiSource source);

/// Physical-size reasoning for the UI. All pure functions — no global state,
/// no LVGL dependency — so the whole policy is unit-testable in isolation.
class DisplayMetrics {
  public:
    /// DPI the UI's pixel constants were authored against.
    static constexpr double kReferenceDpi = 160.0;

    /// Plausibility band for any platform-derived measurement. Anything outside
    /// is discarded rather than clamped: the CB1/HDMI5 computes to 23 DPI and
    /// the Snapmaker U1 to 57, both from physically impossible panel sizes (the
    /// U1 claims a 217x136mm panel on a 480x320 display).
    ///
    /// This band is a backstop, not the primary defense. The primary defense is
    /// that no Linux target consults the kernel at all — resolve_dpi() only ever
    /// sees a measurement from a platform explicitly trusted to be honest, which
    /// today means Android alone. 6 of the 8 devices surveyed report a wrong or
    /// absent physical size, so the fleet's own numbers never enter this path.
    static constexpr double kMinPlausibleDpi = 60.0;
    static constexpr double kMaxPlausibleDpi = 700.0;

    /// simpledrm synthesizes exactly 96 DPI when the bootloader hands it no
    /// panel size. Observed on the CC1, whose 480x272 panel reports 127x71mm —
    /// 127mm is exactly 5.000in, and 480/5.000 is exactly 96.00. A reported 96
    /// is therefore a "no data" tell, not a measurement.
    static constexpr double kSynthesizedDpi = 96.0;
    static constexpr double kSynthesizedDpiEpsilon = 0.6;

    /// Current sizing is accepted through here, so the scale stays 1.0 up to
    /// it. Anchored just above the AD5M's measured 221.5 DPI — the highest-DPI
    /// panel in the fleet whose present sizing is known-good. Every shipping
    /// printer measured (CC1 131, SonicPad 170, CB1 187, Pi 206, K1C/K2+ 218,
    /// AD5M 221) sits at or below this, so none of them move.
    static constexpr double kScaleDeadbandDpi = 225.0;

    /// Growth per DPI above the deadband. Calibrated so 405 DPI yields 1.5781,
    /// which turns the XXLarge 192px grid cell into 303px and the 40px body
    /// font into ~63px — the intended physical size for a phone-class panel.
    static constexpr double kScaleSlopePerDpi = 0.0032118;

    /// Ceiling, so an absurd measurement that still passes the plausibility
    /// band cannot produce a UI that cannot lay out.
    static constexpr double kMaxScale = 2.0;

    /// The scale curve: flat across the verified-good band, then a gentle
    /// linear ramp. Never returns less than 1.0, so a low-DPI panel can only
    /// keep today's sizing, never shrink below it.
    static double ui_scale_for_dpi(double dpi);

    /// DPI of the pixels the UI draws in, when the platform presents a logical
    /// surface smaller than the panel and upscales it (Android renders at an
    /// integer fraction of the native resolution). @p physical_dpi is per native
    /// pixel; the result is per logical pixel, which is what the scale curve
    /// must see, or a surface already enlarged by the upscale is enlarged again.
    /// Sizes are compared by diagonal so the panel's orientation does not matter.
    /// Unknown or degenerate sizes return @p physical_dpi unchanged.
    static double logical_dpi(double physical_dpi, int native_w, int native_h, int logical_w,
                              int logical_h);

    /// Whether a measurement is inside the plausibility band.
    static bool dpi_is_plausible(double dpi);

    /// Whether a measurement looks synthesized rather than measured.
    static bool dpi_is_synthesized(double dpi);

    /// Whether a measurement should be believed at all.
    static bool dpi_is_trustworthy(double dpi);

    /// The integrated panel for a board, or nullopt when the board drives an
    /// arbitrary user-chosen display or its panel is unmeasured. A diagonal_mm of
    /// 0 means the geometry and mount are known but the physical size is not.
    static std::optional<PanelSpec> known_panel(board::Board board);

    /// The board's known panel mount (upright_rotation), or -1 when the board has
    /// none or the live framebuffer (@p native_w x @p native_h, before rotation)
    /// is not the panel the mount was measured on. Board::K1 is also the mips
    /// fall-through for unrecognised boxes, so the geometry is what confirms it.
    static int board_mount_rotation(board::Board board, int native_w, int native_h);

    /// DPI implied by a panel's pixel dimensions and physical diagonal.
    static double dpi_of(const PanelSpec& panel);

    /// Resolve an effective DPI from every available source, most trusted
    /// first. @p user_override_dpi is the --dpi flag / stored setting (<=0 for
    /// unset); @p platform_measured is a measurement from a platform we trust
    /// (Android's SDL_GetDisplayDPI), which is still plausibility-checked.
    static ResolvedDpi resolve_dpi(int user_override_dpi, std::optional<double> platform_measured,
                                   board::Board board);

    /// The whole chain from measurements to scale: @p platform_native_dpi is
    /// converted to the logical surface (logical_dpi), resolved against
    /// @p user_override_dpi and the board (resolve_dpi), turned into the auto
    /// scale, and then the stored @p scale_setting has its say. A surface that
    /// changes size at runtime (a foldable folding) reruns this with its new
    /// native and logical sizes.
    static ScaleDecision decide_scale(int user_override_dpi,
                                      std::optional<double> platform_native_dpi, int native_w,
                                      int native_h, int logical_w, int logical_h,
                                      board::Board board, int scale_setting);

    /// Scale an authored pixel constant. The single choke point every future
    /// physical-units caller goes through, so widening the refactor past
    /// Android does not mean rewriting call sites again.
    static int32_t scaled_px(int32_t authored_px, double scale);

    /// Remap a font asset name to the face nearest the scaled size, e.g.
    /// "noto_sans_32" at 1.578 becomes "noto_sans_48". Snaps to the NEAREST
    /// available size rather than rounding down: a scaled 40 wants 63, and 64
    /// is off by 1 where the next rung down is off by 15.
    ///
    /// Returns @p font_name unchanged when the scale is 1.0, when the name has
    /// no parseable trailing size, or when the family is unknown. Faces above
    /// the XXLarge tier are not linked on printer builds, but those never ask:
    /// their scale is pinned to 1.0 by the deadband.
    static std::string scaled_font_name(const std::string& font_name, double scale);

    /// Stored UI-scale setting meaning "follow the panel's DPI".
    static constexpr int kScaleSettingAutomatic = 0;

    /// Smallest stored setting that is not Automatic. The scale only ever
    /// grows, so there is nothing below the authored size to offer.
    static constexpr int kMinScaleSettingPercent = 100;
    static constexpr int kMaxScaleSettingPercent = static_cast<int>(kMaxScale * 100);

    /// The scale a stored UI-scale setting selects.
    ///
    /// @p user_scale_percent is the persisted setting: kScaleSettingAutomatic
    /// to follow @p auto_scale, or an explicit percentage. Anything outside
    /// [kMinScaleSettingPercent, kMaxScaleSettingPercent] falls back to
    /// @p auto_scale rather than clamping — a setting that far out is a
    /// corrupt or hand-edited value, and honouring a clamped version of it
    /// would silently pin the UI to a size nobody chose.
    static double scale_for_setting(int user_scale_percent, double auto_scale);

    /// The DPI-derived scale, before the stored setting had its say. The
    /// settings UI shows it as the "Automatic (150%)" hint, so it has to
    /// survive being overridden. Set once during display init alongside
    /// set_active_scale(); defaults to 1.0.
    static void set_auto_scale(double scale);
    static double auto_scale();

    /// The process-wide scale, resolved during display init and again when
    /// the drawing surface changes size (decide_scale). It defaults to 1.0 so
    /// any code path that runs before display init (or in a unit test)
    /// behaves exactly as it does today.
    static void set_active_scale(double scale);
    static double active_scale();
};

} // namespace helix
