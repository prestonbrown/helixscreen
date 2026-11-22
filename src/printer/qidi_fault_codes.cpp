// SPDX-License-Identifier: GPL-3.0-or-later
#include "qidi_fault_codes.h"

#include "core/text_io.h"
#include "display_numbering.h"
#include "exception_policy.h"
#include "helix_regex.h"
#include "lvgl.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>

namespace helix::qidi {
namespace {

/// Lanes in one QIDI Box: QIDI's `2C` is the second Box's third lane.
constexpr int kSlotsPerBox = 4;

/// How a code's print effect reads on the console. FollowPrint blocks while a
/// print runs or is paused (the pause codes, the load faults); Warning never
/// does (a fan or dryer warning, an RFID scan, a drying setting). Runout is a
/// warning that names its slot as the runout's lane: the Box raises no runout
/// fault of its own, so the stock runout guidance (Load, Resume) owns the
/// paused print and this toast stands down where that dialog explains the slot.
enum class Effect { FollowPrint, Warning, Runout };

/// One code's wording. "{}" in any of the three is the slot the message
/// names, as the app labels it ("Slot 3"), or the fan it names.
struct Row {
    int module;
    int index;
    Effect effect;
    const char* summary;
    const char* cause;
    const char* fix;
};

// The QIDI Box (004), fans (002) and heater checks (005). 004_008 and
// 004_025 are raised from compiled code whose context is unknown, so they
// keep QIDI's own message; 004_012 and 004_024 are not raised anywhere.
constexpr Row kRows[] = {
    {4, 1, Effect::FollowPrint, TR_NOOP("{} could not load"),
     TR_NOOP("Filament fed into {} never reached the Box's feed sensor, or the sensor "
             "triggered unreliably."),
     TR_NOOP("Push the filament into {} until the feeder grabs it, cut its end clean and "
             "check that the slot's sensor is not stuck, then try again.")},
    {4, 2, Effect::FollowPrint, TR_NOOP("The toolhead is already loaded"),
     TR_NOOP("Another filament is already in the toolhead, so the Box will not feed {}."),
     TR_NOOP("Unload the current filament first, then load {}.")},
    {4, 3, Effect::FollowPrint, TR_NOOP("{} could not unload"),
     TR_NOOP("Pulling the filament back into the Box did not clear the slot's sensor: a tangled "
             "spool, or stuck or snapped filament."),
     TR_NOOP("Free the spool and pull the filament out of {} by hand, then try again.")},
    {4, 4, Effect::FollowPrint, TR_NOOP("Unload the toolhead first"),
     TR_NOOP("The slot asked to eject still has its filament loaded in the toolhead."),
     TR_NOOP("Unload the toolhead first, then eject the slot.")},
    {4, 5, Effect::FollowPrint, TR_NOOP("{} has no filament"),
     TR_NOOP("The slot chosen for this tool change has no filament at its inlet."),
     TR_NOOP("Load filament into {}, then resume.")},
    {4, 6, Effect::FollowPrint, TR_NOOP("Filament did not reach the toolhead"),
     TR_NOOP("Filament left the Box but never reached the toolhead's filament sensor: a gap in "
             "the PTFE tube, a clog at the toolhead entry, or a cut tip left behind."),
     TR_NOOP("Check the PTFE tube from the Box to the toolhead and the toolhead entry, and clear "
             "any cut tip, then retry.")},
    {4, 7, Effect::FollowPrint, TR_NOOP("The toolhead is not loaded"),
     TR_NOOP("An unload was asked for, but the Box's buffer sensor sees no filament in the "
             "path."),
     TR_NOOP("Check that the toolhead is really empty, then resume.")},
    {4, 9, Effect::FollowPrint, TR_NOOP("Filament did not unload from the toolhead"),
     TR_NOOP("Pulling the filament back from the toolhead never cleared the Box's buffer sensor: "
             "the filament was not cut through, its tip swelled, or it tangled."),
     TR_NOOP("Check that the cutter cut fully and pull the filament back by hand if needed, then "
             "retry.")},
    {4, 10, Effect::FollowPrint, TR_NOOP("The Box's filament state is wrong"),
     TR_NOOP("After a restart, the slot saved as loaded no longer exists: the Box was removed or "
             "the number of Boxes changed."),
     TR_NOOP("Unload the filament from the toolhead by hand, then reconnect the Box or "
             "restart.")},
    {4, 11, Effect::Warning, TR_NOOP("Unload before reading the tag"),
     TR_NOOP("Reading a slot's RFID tag moves filament, and the toolhead is still loaded."),
     TR_NOOP("Unload, then read the tag again.")},
    {4, 13, Effect::FollowPrint, TR_NOOP("Filament is tangled"),
     TR_NOOP("The Box's motion sensor saw no movement while feeding: the spool is tangled or "
             "wound over itself."),
     TR_NOOP("Untangle or rewind the spool, then resume.")},
    {4, 14, Effect::Warning, TR_NOOP("Drying settings are missing"),
     TR_NOOP("Drying was started with no time or no temperature."),
     TR_NOOP("Set a drying temperature and a duration, then start again.")},
    {4, 15, Effect::FollowPrint, TR_NOOP("The Box's self-check failed"),
     TR_NOOP("At startup the toolhead holds filament, but the Box could not tell which slot it "
             "came from."),
     TR_NOOP("Pull the filament out of the toolhead by hand, then load again.")},
    {4, 16, Effect::Runout, TR_NOOP("{} ran out"),
     TR_NOOP("{} ran out of filament during the print, and auto refill is off."),
     TR_NOOP("Load a new spool into {}, or turn on auto refill with a matching spool, then "
             "resume.")},
    {4, 17, Effect::FollowPrint, TR_NOOP("The old filament would not purge"),
     TR_NOOP("During auto refill, the old filament would not purge out of the toolhead."),
     TR_NOOP("Clear the nozzle and the purge chute, load {}, then resume.")},
    {4, 18, Effect::FollowPrint, TR_NOOP("{} can't refill automatically"),
     TR_NOOP("Auto refill needs the vendor, color and type set for the empty slot, and they are "
             "not."),
     TR_NOOP("Set the slot's filament information, or load the slot by hand.")},
    {4, 19, Effect::Warning, TR_NOOP("Check the PTFE tube"),
     TR_NOOP("While reading a tag, the filament stalled before the sensor while still at the "
             "inlet, usually from a bent or kinked tube."),
     TR_NOOP("Straighten the PTFE tube for that slot and read the tag again.")},
    {4, 20, Effect::Warning, TR_NOOP("The filament left the slot"),
     TR_NOOP("While reading a tag, the filament left the inlet sensor."),
     TR_NOOP("Insert the filament again and read the tag again.")},
    {4, 21, Effect::FollowPrint, TR_NOOP("Loaded filament not recognized"),
     TR_NOOP("A load was asked for while the toolhead holds filament the Box has no record of."),
     TR_NOOP("Unload the toolhead by hand, then load again.")},
    {4, 22, Effect::FollowPrint, TR_NOOP("No matching slot for auto refill"),
     TR_NOOP("Auto refill found no other slot with the same vendor, color and type."),
     TR_NOOP("Load a matching spool, or load any slot by hand and resume.")},
    {4, 23, Effect::FollowPrint, TR_NOOP("Auto refill failed"),
     TR_NOOP("Auto refill could not start: the buffer sensor is empty or there is no next "
             "slot."),
     TR_NOOP("Load filament by hand and resume.")},
    {2, 1, Effect::Warning, TR_NOOP("Fan {} is not spinning"),
     TR_NOOP("The fan was turned on, but its speed sensor reads 0 RPM."),
     TR_NOOP("Check that fan for a loose plug, debris or a failed motor.")},
    {2, 2, Effect::FollowPrint, TR_NOOP("Fan {} stopped"),
     TR_NOOP("A cooling fan stopped while its heater was on, so the firmware turned the heater "
             "off."),
     TR_NOOP("Check that fan and its connector. If the printer shut down, restart the "
             "firmware.")},
    {5, 1, Effect::Warning, TR_NOOP("The Box dryer is not heating"),
     TR_NOOP("The QIDI Box dryer could not reach its target: an open lid, a failed heater or a "
             "cold room. Only the dryer stopped; the print goes on."),
     TR_NOOP("Close the Box lid, check the Box power and start drying again. If it keeps "
             "failing, the heater or its sensor needs checking.")},
};

const Row* row_for_code(const FaultCode& c) {
    if (c.kind != 'E') {
        return nullptr;
    }
    for (const Row& r : kRows) {
        if (r.module == c.module && r.index == c.index) {
            return &r;
        }
    }
    return nullptr;
}

/// The firmware's own sentence: after `Message:` when the line labels it,
/// else after the code.
std::string firmware_message(const std::string& text) {
    constexpr std::string_view kLabel = "Message:";
    if (const auto at = text.find(kLabel); at != std::string::npos) {
        return std::string(text_io::trim(std::string_view(text).substr(at + kLabel.size())));
    }
    static const Regex code_re("QD[A-Z]_\\d{3}_\\d{3}");
    RegexMatch m;
    if (regex_search(text, m, code_re)) {
        return std::string(text_io::trim(std::string_view(text).substr(m.end())));
    }
    return text;
}

/// What "{}" names in a row: the slot QIDI's label (`1A`..`4D`, Box then
/// lane) points at, as every other screen names it, or the fan the fan codes
/// quote.
std::string subject_of(const std::string& message) {
    static const Regex fan_re("\"fan_name\"\\s*:\\s*\"([^\"]+)\"");
    RegexMatch m;
    if (regex_search(message, m, fan_re)) {
        return m.str(1);
    }
    if (const int slot = named_slot(message); slot >= 0) {
        return ui::lane_label(ui::LaneNoun::Slot, slot);
    }
    return "?";
}

std::string fill(const char* source, const std::string& subject) {
    std::string out;
    if (!contain_exceptions("[QidiFault] format",
                            [&] { out = fmt::format(fmt::runtime(lv_tr(source)), subject); })) {
        contain_exceptions("[QidiFault] format source",
                           [&] { out = fmt::format(fmt::runtime(source), subject); });
    }
    return out;
}

} // namespace

std::optional<FaultCode> decode_fault_code(std::string_view text) {
    static const Regex re("QD([A-Z])_(\\d{3})_(\\d{3})");
    RegexMatch m;
    if (!regex_search(text, m, re)) {
        return std::nullopt;
    }
    FaultCode c;
    c.kind = m.str(1)[0];
    c.module = text_io::parse_leading<int>(m.str(2)).value_or(0);
    c.index = text_io::parse_leading<int>(m.str(3)).value_or(0);
    return c;
}

int named_slot(const std::string& text) {
    static const Regex slot_re("\\b([1-4])([A-D])\\b");
    RegexMatch m;
    if (!regex_search(text, m, slot_re)) {
        return -1;
    }
    const int box = m.str(1)[0] - '1';
    const int lane = m.str(2)[0] - 'A';
    return box * kSlotsPerBox + lane;
}

std::string format_fault_code(const FaultCode& c) {
    return fmt::format("QD{}_{:03d}_{:03d}", c.kind, c.module, c.index);
}

std::optional<FaultCode> decode_retry_step(std::string_view retry_step, int* slot) {
    static const Regex re("^QD([A-Z])_(\\d{3})_(\\d{3})(?:_(\\d+))?$");
    RegexMatch m;
    const std::string_view v = text_io::trim(retry_step);
    if (!regex_search(v, m, re)) {
        return std::nullopt;
    }
    FaultCode c;
    c.kind = m.str(1)[0];
    c.module = text_io::parse_leading<int>(m.str(2)).value_or(0);
    c.index = text_io::parse_leading<int>(m.str(3)).value_or(0);
    if (slot) {
        *slot = m[4].matched ? text_io::parse_leading<int>(m.str(4)).value_or(0) : 0;
    }
    return c;
}

bool try_again_retries(const FaultCode& c) {
    return c.kind == 'E' && c.module == 4 && ((c.index >= 1 && c.index <= 7) || c.index == 9);
}

std::optional<ErrorEvent> classify_fault_line(const std::string& line, const ClassifyContext& ctx) {
    // QIDI raises on `!!`, `//` and unprefixed lines; none of the prefixes is
    // part of the fault.
    std::string text(text_io::trim(line));
    for (const std::string_view prefix : {std::string_view("!!"), std::string_view("//")}) {
        if (text.rfind(prefix, 0) == 0) {
            text = std::string(text_io::trim(std::string_view(text).substr(prefix.size())));
            break;
        }
    }
    const auto code = decode_fault_code(text);
    if (!code) {
        return std::nullopt;
    }
    const Row* row = row_for_code(*code);
    const std::string message = firmware_message(text);

    ErrorEvent e;
    e.source = ErrorSource::QIDI;
    e.code = format_fault_code(*code);
    e.raw_detail = text;
    const bool warning = row && row->effect != Effect::FollowPrint;
    if (row && row->effect == Effect::Runout) {
        e.runout_lane = named_slot(message);
    }
    e.severity = (!warning && (ctx.is_printing || ctx.is_paused)) ? ErrorSeverity::CRITICAL
                                                                  : ErrorSeverity::WARNING;
    if (row) {
        const std::string subject = subject_of(message);
        e.title = fill(row->summary, subject);
        e.detail = fill(row->cause, subject) + "\n\n" + fill(row->fix, subject);
    } else {
        e.detail = message;
    }
    e.sticky = e.severity == ErrorSeverity::CRITICAL;
    return e;
}

} // namespace helix::qidi
