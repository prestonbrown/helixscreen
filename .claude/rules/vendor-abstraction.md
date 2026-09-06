---
paths:
  - "src/**/*"
  - "include/**/*"
---
# Vendor Knowledge Stays Behind an Abstraction

**A vendor, firmware, or mod name may appear in ONE module per capability. Generic code
asks that module a capability question and never names the vendor.**

Generic code is anything whose job is not "support vendor X": `PrinterState` and its
sub-states, the discovery/subscription builder, `Application` startup, panels, widgets,
formatters. When one of those grows an `if (zmod) … else if (creality) …`, the vendor
matrix is spread across every layer and the next firmware means editing all of them.

| ❌ WRONG | ✅ CORRECT |
|----------|-----------|
| `zmod::parse_persisted_z_offset(status)` in `PrinterMotionState::update_from_status` | `zoffset::read_persisted_offset_microns(status)` - the module owns which firmwares and which schema |
| `if (hw.has_macro("SAVE_ZMOD_DATA")) subs["save_variables"] = nullptr;` in the subscription builder | `for (auto& o : zoffset::required_status_objects(hw)) subs[o] = nullptr;` |
| `api->execute_gcode("SAVE_ZMOD_DATA LOAD_ZOFFSET=1")` in `Application` | `zoffset::persistence_enable_gcode(hw)` - empty string when the printer needs none |

**The test: adding a second firmware with the same capability must touch exactly one
file.** If it would touch the status parser *and* the subscription builder *and*
startup, the abstraction is missing. Model it as a provider table keyed on a detection
predicate, with the capability questions as free functions over it:
`include/z_offset_persistence.h` + `src/printer/z_offset_persistence.cpp` is the
reference shape (~40 lines of table, three questions: what to subscribe, how to read it,
how to enable it).

**Naming follows the same rule.** Subjects, accessors and headers name the *capability*
(`persisted_z_offset`, `firmware_persists_z_offset`), never the vendor
(`zmod_z_offset`). A vendor-named symbol reachable from generic code is the smell even
when the call site looks clean.

Existing vendor dispatch that already lives behind an interface is the pattern working:
`AmsBackend*` (one class per filament system, `AmsState` never names one),
`ZOffsetCalibrationStrategy`, `PrinterDetector` capability lookups. Follow those.

Genuinely unavoidable vendor branch in generic code? Annotate it: `// VENDOR_OK: <reason>`.
