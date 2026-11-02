// SPDX-License-Identifier: GPL-3.0-or-later
#include "error_classify.h"

#include "gcode_error_router.h" // clean_error_text
#include "klipper_error_table.h"
#include "lvgl.h"

namespace helix::error_classify {

std::optional<ErrorEvent> classify(const std::string& raw_line, const ClassifyContext& ctx) {
    const GcodeErrorLine parsed = parse_gcode_error_line(raw_line);
    if (parsed.prefix == GcodeErrorPrefix::None)
        return std::nullopt;

    ErrorEvent e;
    const bool is_bang = parsed.prefix == GcodeErrorPrefix::Bang;
    std::string text = parsed.text;

    // Capture Klipper's wording before clean_error_text() gets a chance to
    // rewrite it — the RPC channel records this exact string, so it is what the
    // cross-source dedup must match on. See ErrorEvent::raw_detail.
    e.raw_detail = text;

    std::string code;
    FirmwareCodeScheme scheme = FirmwareCodeScheme::Opaque;
    GcodeErrorRouter::clean_error_text(text, code, &scheme);
    e.detail = text;
    e.code = code;

    // Only error-table codes carry severity rules, and the table owns them
    // (klipper_error_classify). Any other firmware code is shown to the user
    // but classifies like an uncoded `!!`, so a fault raised mid-print still
    // blocks rather than reading as a transient toast.
    if (!is_bang) { // "Error:" command error
        e.source = ErrorSource::KLIPPER;
        e.severity = ErrorSeverity::WARNING;
    } else if (!code.empty() && scheme == FirmwareCodeScheme::ErrorTable) {
        printer::KlipperErrorClassification table = printer::klipper_error_classify(code);
        e.source = table.source;
        e.severity = table.severity;
        e.title = std::move(table.title);
        e.recovery_actions = std::move(table.recovery_actions);
    } else { // uncoded `!!` — no Klipper error code to key off
        e.source = ErrorSource::GENERIC;
        e.severity =
            (ctx.is_paused || ctx.is_printing) ? ErrorSeverity::CRITICAL : ErrorSeverity::WARNING;

        // A CRITICAL event with no recovery action renders as a text-only modal
        // over a stopped job -- the user reads the error and has nothing to tap
        // (#1152). Offer the one action that is always meaningful on a paused
        // printer. It attributes NOTHING: source stays GENERIC and the title
        // stays empty, so the modal reads "Printer Error". A filament backend
        // that recognizes the fault has already returned its own richer event
        // before the router falls through to this classifier, so this only ever
        // fills the hole nobody claimed.
        //
        // Paused only, deliberately. RESUME needs something to resume; while the
        // print is still running there is no safe generic action to offer, and
        // no filament move belongs on a line whose cause is unknown. is_paused
        // already implies CRITICAL above -- the severity is spelled out because
        // WARNING + an action routes to TOAST_WITH_RECOVER, whose presenter is
        // hard-wired to the key298 recovery service and ignores this vector.
        if (e.severity == ErrorSeverity::CRITICAL && ctx.is_paused) {
            // BOTH neutral: the cause of this line is unknown by definition --
            // this is the arm nobody claimed -- so the UI must not nudge toward
            // resuming a print that may not be safe to resume.
            // Flagged hot: RESUME restarts a paused print, whose very next move
            // extrudes. is_paused is what got us here, and a print can sit paused
            // long enough for idle_timeout to drop the heater, so the presenter
            // reheats before sending rather than handing Klipper a cold extrude.
            e.recovery_actions.push_back({lv_tr("Resume"), "RESUME", "error_classify::resume", "",
                                          /*needs_hot_nozzle=*/true});
            // Without this the modal has exactly one way out and a user who does
            // NOT want to resume is trapped: ActionPromptModal builds its buttons
            // solely from this vector and has no intrinsic close affordance (same
            // trap as #1041). An empty gcode is the dismiss spelling -- the modal
            // closes and sends nothing (#1172).
            e.recovery_actions.push_back({lv_tr("OK"), "", "error_classify::dismiss"});
        }
    }

    // Sticky is uniform across sources: any CRITICAL stays on screen until the
    // user dismisses it; WARNING/INFO auto-dismiss. Computing it once here keeps
    // coded (key8xx) and uncoded CRITICAL paths consistent for the L1 presenter.
    e.sticky = (e.severity == ErrorSeverity::CRITICAL);
    return e;
}

} // namespace helix::error_classify
