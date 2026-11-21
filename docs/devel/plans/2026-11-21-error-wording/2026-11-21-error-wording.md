# Error wording: Klipper, QIDI and FlashForge

Plain-language summary, cause and fix for the errors printers actually raise, in the shape the
Snapmaker U1 table already uses (`include/snapmaker_exceptions.h`). The research is in this folder:
`errmap-klipper.md`, `errmap-qidi.md` and `errmap-flashforge.md`. Each ends with its recommended rows,
its matching strategy and its open questions; read all three before starting.

## Intent

A user who sees `MCU 'mcu' shutdown: Timer too close` or `!!Code:QDE_004_016` today learns nothing
about what broke or what to do. Each row turns one firmware message into a summary (a title or stop
reason), a cause sentence and a fix sentence, with the captured names (heater, MCU, stepper, lane)
filled in. Firmware text we do not recognize is shown unchanged, as today.

## Decisions

1. **One classification, many presenters.** A matcher returns a classification: row id, captures,
   severity, recovery (FIRMWARE_RESTART, RESTART after a config fix, retry, home). The toast, the
   modal, the recovery dialog, the stop reason and the journal each render it. They never match
   text themselves.
2. **Vendor knowledge behind provider tables** (`.claude/rules/vendor-abstraction.md`). Generic
   Klipper/Kalico reason rows apply to every printer. QIDI and FlashForge rows live in their own
   modules, plugged into `src/printer/firmware_fault_codes.cpp` (coded identities: QIDI `QDE_xxx_yyy`,
   FlashForge three-part `coded` values and `E0NNN`) or into the generic table's per-firmware row set
   (prose keyed by an anchored prefix), each gated by a detection predicate. Adding a firmware's
   wording touches one module.
3. **Match on stable identities first, then anchored prefixes.** A code is the identity when one
   exists. Prose is keyed on the anchored, case-sensitive prefix the maps list, after normalizing
   (`!! `, the `MCU '...' shutdown: ` prefix, Klipper's canned trailers). Captures use small
   `helix::Regex` patterns, never `std::regex`.
4. **Shutdowns and config errors: our wording above Klipper's text**, never instead of it. The
   config-error dialog already does "preface + raw"; extend that model. Support threads keep the
   exact firmware line.
5. **Unknown keeps the firmware's words.**
6. **Strings are `TR_NOOP`**, captures inserted with `fmt::runtime`; run `make translations` and
   commit the packs with each phase that adds strings.
7. **QIDI `//` lines carrying a QD code classify.** Thirteen Box codes, every pausing one among
   them, arrive as `//` lines that are dropped today. Only lines carrying a QD code are accepted
   from that channel; ordinary comments still never classify.

## Phases (each its own commit or few; tags only while building)

1. **Generic Klipper table.** The top 30 rows in `errmap-klipper.md` § 3 plus MCU-10/MCU-11 (M112,
   webhooks e-stop). Normalizer, prefix keys, captures, severity and recovery per row. Wire it into
   the generic `!!` classifier (`src/application/error_classify.cpp`) and the shutdown/error
   presenter (state_message, `print_stats.message`). Kalico's `pid_calibrate` `!!` warning
   (§ 5 item 7) classifies as a warning, not an error.
2. **Retire the three hand matchers** onto the table: `src/application/gcode_error_text.cpp`
   rewrites, `printer_event_journal.cpp` `kClassMarkers`, `printer_network_state.cpp`
   `is_klippy_config_error` / `is_klippy_mcu_connect_error`. Fix the misses the map lists (§ 5
   items 3 to 6) as rows, not special cases.
3. **QIDI.** `errmap-qidi.md` § 4 and § 5: a `qidi_fault_codes` provider; `//` QD lines; the pause
   codes first; `TRY_MOVE_AGAIN` as Retry where `retry_step` names the code; standing faults from
   `retry_step` only when live state agrees; dedup with `AmsBackendQidi::current_error()`.
4. **FlashForge.** `errmap-flashforge.md` § 5 and § 6: three-part `coded` rows, then prose rows
   gated per firmware (ZMOD IFS, Forge-X, OpenCreator), English and Russian for the IFS strings;
   I1/I6/I9 become the AD5X IFS backend's fault text; dedup against the firmware's own prompt.

## Out of scope

Beacon, Cartographer, Eddy, Happy Hare and AFC wording; ZMOD languages beyond English and Russian;
QIDI Max 4 and Plus 5 (unpublished sources). Items each map marks **unverified** stay as the map
says; do not guess a wording for a message the map could not source.

## Done means

Each phase's rows have tests that build the classification from the exact firmware line in the
map (one case per row family, a table-driven case for the rest), each seen red first. Every
surface that showed raw text for a worded row shows the wording. This folder is deleted in the
change that ships the last phase.
