# CHANGELOG Style

The reader of `CHANGELOG.md` is a printer owner deciding whether to update. That reader
does not know what a subject, a backend or an L-number is, and never will. This doc pins
the prose rules for new entries: voice, mechanics, and the two entry shapes.

Why pin it now: 203 releases over 197 days (median gap one day, max seven) means the
changelog is written constantly; the prose drifted across three eras (bold-lead bullets
4% to 65% to 89% of entries, issue refs 61% to 64% to 86%), and every contested variant
(separator, link form, intro presence) was decided differently in different months.

## The two shapes

### Daily shape (the default)

Most releases land one to five changes. Write them as plain bold-lead bullets under the
usual categories - no intro paragraph, no extra structure:

```markdown
## [0.99.116] - 2026-09-02

### Fixed

- **Bed mesh rendering on K2** - mesh heights are read in the units the printer
  actually sends, so the preview no longer shows a flat plate.
```

### Milestone shape

For a release that needs a paragraph to explain itself (a minor bump, a release
candidate, a theme with several chapters), expand:

1. An intro paragraph naming the release's role ("This is the second 1.0 release
   candidate... the bulk of the work is in two areas: ..."). One paragraph, no more.
2. Bold sub-group headers inside categories when a category spans unrelated areas
   (`**Printer identification**`, `**Crashes**`) - so a reader can scan past what they
   do not care about.

## Voice: symptom and consequence, never machinery

Every bullet says what the user saw (or would have seen) and what changed for them.
No subjects, classes, function names, L-numbers, or internal identifiers - those name
the fix, not the experience. If a bullet's lead only makes sense to someone reading the
source, rewrite the lead.

### Translating recurring concepts

| Internal concept | Write instead |
|-----------------|---------------|
| subject | the value shown on screen |
| panel / overlay | screen |
| backend | filament system |
| class or function name | describe the behavior it produces |
| internal identifiers, L-numbers | omit |

A machinery name may appear when it is itself the user-facing thing being talked about
(a setting the user can see, a G-code the user writes, a file the user edits). It may
not appear as an explanation of something else.

## Mechanics

- **Separator: hyphen.** Bold lead, then ` - `, then the body. The census found em-dash
  685 / hyphen 155 / period 24; hyphen wins because it survives every font, terminal and
  copy-paste path the changelog travels. No em-dashes in new entries.
- **Issue links: bare `(#N)`**, placed after the bold lead. Census: bare `#N` 440 vs
  autolink `owner/repo#N` 150, markdown `[#N]` 12, `(fixes #N)` 9 - the repo renders
  bare refs, and one form keeps greppability. Never the full `prestonbrown/helixscreen#N`
  spelling inside this repo's own changelog.
- **Printer name in the bold lead whenever a note is not fleet-wide.** "Bed mesh
  rendering on K2", not a buried "on Creality printers" in the last sentence - printers
  are the scoping fact readers scan for (AFC 196 / AD5X 146 / K2 97 mentions in the
  existing file say so). If it applies to every printer, name none.
- **Migration notes: a bold `**Upgrading from X?**` callout at the top of the section**
  whenever behavior changed in a way an updating user must act on or will notice
  (settings moved, defaults flipped, config format bumped). One or two sentences, then
  on with the entry. Not for ordinary fixes.

## Play Store "What's New" lives outside this file

The Play Store "What's New" field is a different surface with a 500-character budget. Its
text is not in `CHANGELOG.md`: a stable release writes it by hand in
`android/whatsnew/<version>.txt` (`android/whatsnew/README.md`), and
`scripts/generate-whatsnew.sh` uses that file verbatim. New entries carry no
`<!-- whatsnew -->` block; the blocks in older entries stay as they were written, and
every tool that publishes a section strips HTML comments from it.

## Never change these

Three parts of the file are load-bearing for things that are not human readers:

- The `## [X.Y.Z] - YYYY-MM-DD` heading grammar. `src/system/update_checker.cpp` parses
  these headings out of the raw file at runtime to show each version's notes in the
  update dialog. Do not reformat, reflow or "improve" the heading line.
- The `### Added` / `### Fixed` / `### Changed` categories (Keep a Changelog).
- The compare-link footer at the bottom of the file
  (`[X.Y.Z]: https://github.com/prestonbrown/helixscreen/compare/vA...vB`).

The 500-char What's New budget in `scripts/generate-whatsnew.sh` likewise stays.

## Past entries stay as they are

No retro-editing. Entries written before these rulings keep their era's spelling,
em-dashes and machinery names - the changelog is a historical record, and rewriting it
would falsify what each release actually said. These rules govern new entries only.

## Worked examples

Both "before" lines are verbatim from real past entries (kept as-is per the rule above);
each "after" is the same fix under these rules.

Machinery voice (v0.99.68) - the lead names the fix's internals, the body names the
technique, and nowhere does it say what a user would have seen:

> `- **L081 cleanup in bedmesh background callbacks** — replaced bare `tok.expired()`
> checks with `lifetime_.bg_cb` to close another Mechanism C UAF surface.`

After:

```markdown
- **Rare crash when closing the bed mesh screen** - a background mesh update could
  still reach the screen after it was gone, showing up as a random app restart; the
  update now waits for the screen instead of touching it directly.
```

Separator and link form (v0.99.95) - em-dash separator, full autolink form, and
"reactively from its own per-slot subject" is machinery explaining machinery:

> `- **AMS slot material label** (prestonbrown/helixscreen#1065) — the slot material
> label updates reactively from its own per-slot subject.`

After:

```markdown
- **AMS slot labels now update after a filament change** (#1065) - changing only the
  material on a slot used to leave its label stale until you left that screen and
  came back; the
  label now follows what the printer reports.
```
