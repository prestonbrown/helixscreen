# HelixScreen Architecture Guide

One subsystem per chapter, one hour per chapter. Start with the chapter for
the area you came to work on; read [the router](../ARCHITECTURE.md) first if
you want the 15-minute whole-app mental model.

## I want to work on...

| I want to... | Read |
|--------------|------|
| change a screen's layout/controls | [01 — Declarative UI](01-declarative-ui.md), then [UI_CONTRIBUTOR_GUIDE](../UI_CONTRIBUTOR_GUIDE.md) |
| add/observe printer data | [02 — Subjects & data flow](02-subjects-dataflow.md) |
| write background-thread code | [03 — Threading & lifetime](03-threading-lifetime.md), then [THREADING](../THREADING.md) |
| talk to Moonraker | [04 — Moonraker integration](04-moonraker.md), then [MOONRAKER_ARCHITECTURE](../MOONRAKER_ARCHITECTURE.md) |
| find which singleton owns a piece of printer state | [05 — Printer state & singletons](05-printer-state.md) |
| support a new printer model or gate UI on a capability | [06 — Discovery & capabilities](06-discovery-capabilities.md) |
| support a new filament system | [07 — Filament & AMS](07-filament-ams.md), then [FILAMENT_MANAGEMENT](../FILAMENT_MANAGEMENT.md) |
| add a screen, overlay, or modal | [08 — Panels, overlays & modals](08-panels-navigation.md), then [MODAL_SYSTEM](../MODAL_SYSTEM.md) |
| add a home-screen widget | [09 — Home panel widgets](09-home-widgets.md) |
| change colors, spacing, or theming | [10 — Theme, tokens & layout](10-theme-tokens-layout.md), then [THEME_SYSTEM](../THEME_SYSTEM.md) |
| change boot or shutdown ordering | [11 — Startup & shutdown](11-startup-shutdown.md) |
| work on updates, sound, LED, telemetry, or plugins | [12 — System services](12-system-services.md) |
| add a peripheral or remote-control the UI | [13 — Peripherals & remote](13-peripherals.md) |
| make HelixScreen run on a new board | [14 — Build & platforms](14-build-platforms.md), then [BUILD_SYSTEM](../BUILD_SYSTEM.md) |
| pay down tech debt | [15 — Known debt](15-known-debt.md) |
| work on the G-code viewer, parsing, or object picking | [16 — G-code pipeline](16-gcode-pipeline.md), then [EXCLUDE_OBJECTS](../EXCLUDE_OBJECTS.md) |

## The series

Part I — The reactive core: 01, 02, 03
Part II — Talking to the printer: 04, 05, 06, 07
Part III — The UI layer: 08, 09, 10
Part IV — Platform & services: 11, 12, 13, 14
Appendix: 15 — Known debt
Added after the series was numbered (no renumbering): 16 — G-code pipeline

## Editing a chapter

Cite files the way the chapters already do — a backticked path, optionally with a
line: `` `src/printer/printer_state.cpp:622` ``. Do not write the markdown link
yourself. `make regen-doc-links` wraps every citation in a link to the file (and
line) it names, re-deriving the URL from the citation text on each run, so a
renamed file is fixed in one place and every link follows. [`quality-checks.sh`](../../../scripts/quality-checks.sh)
fails the commit if a chapter is out of date with the generator.

The citation is also what keeps the chapters honest: [`check_doc_refs.py`](../../../scripts/check_doc_refs.py) proves
the path resolves, the line is inside the file, and the symbol named beside a
cite still sits within five lines of it. Write the citation, run the generator.
