// SPDX-License-Identifier: GPL-3.0-or-later
#include "error_classify.h"

#include "gcode_error_router.h" // clean_error_text
#include "lvgl/src/others/translation/lv_translation.h"

#include <cctype>

namespace helix::error_classify {

namespace {

ErrorSource source_for_code(const std::string& code) {
    if (code.rfind("key", 0) == 0)
        return ErrorSource::CFS; // Creality CFS codes
    return ErrorSource::GENERIC;
}

bool is_error_prefix(const std::string& line) {
    if (line.size() >= 2 && line[0] == '!' && line[1] == '!')
        return true;
    if (line.size() >= 6) {
        std::string p = line.substr(0, 5);
        for (auto& c : p)
            c = static_cast<char>(std::tolower(c));
        if (p == "error" && line[5] == ':')
            return true;
    }
    return false;
}

} // namespace

std::optional<ErrorEvent> classify(const std::string& raw_line, const ClassifyContext& ctx) {
    if (!is_error_prefix(raw_line))
        return std::nullopt;

    ErrorEvent e;
    const bool is_bang = raw_line.size() >= 2 && raw_line[0] == '!' && raw_line[1] == '!';

    std::string text;
    if (is_bang) {
        text =
            (raw_line.size() >= 3 && raw_line[2] == ' ') ? raw_line.substr(3) : raw_line.substr(2);
    } else { // "Error:"
        text =
            (raw_line.size() >= 7 && raw_line[6] == ' ') ? raw_line.substr(7) : raw_line.substr(6);
    }

    // Capture Klipper's wording before clean_error_text() gets a chance to
    // rewrite it — the RPC channel records this exact string, so it is what the
    // cross-source dedup must match on. See ErrorEvent::raw_detail.
    e.raw_detail = text;

    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);
    e.detail = text;
    e.code = code;

    if (!is_bang) { // "Error:" command error
        e.source = ErrorSource::KLIPPER;
        e.severity = ErrorSeverity::WARNING;
    } else if (!code.empty()) {
        e.source = source_for_code(code);
        if (code.rfind("key8", 0) == 0) {
            e.severity = ErrorSeverity::CRITICAL;
            if (code == "key840") {
                e.recovery_actions.push_back(
                    {lv_tr("Reset CFS"), "BOX_ERROR_CLEAR", "error_classify::key840_reset"});
            } else if (code == "key843") {
                // key843 is the RFID-read failure: most often the box answering
                // busy while mid-operation rather than a broken tag (#1387). It
                // self-resolves when the box frees (the deferred insert probe
                // lands) or the user re-seats a genuinely bad tag, so the key8xx
                // blanket CRITICAL promotion is wrong here: CRITICAL without
                // actions routes to a blocking modal whose advice would assert
                // one cause for all of them. A toast needs no action.
                e.severity = ErrorSeverity::WARNING;
            }
        } else if (code == "key298") {
            e.severity = ErrorSeverity::WARNING;
            e.recovery_actions.push_back({lv_tr("Recover"), "", "error_classify::key298_recover"});
        } else {
            e.severity = ErrorSeverity::WARNING;
        }
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
