# 1.1 documentation sweep: accurate, illustrated, concise

Status: design, answers from Preston 2026-11-10. Lands on `release/1.1`, merged forward to `main`.

## Why

A lot has changed since 1.0 (2026-09-09): 18 betas and about 1,250 feature commits, led by
filament systems, the home panel, plugins, belt tuning, screensavers, motion and power-loss
recovery. The docs were edited as features landed, but nobody has read them end to end
against the shipping app. Preston wants 1.1 to ship with documentation that is accurate,
better illustrated, and clear and thorough without padding. This is an **uplift**, not just
a correction pass.

Success: a new 1.1 user can install, set up and run their printer from the manual alone, and
every screen they see looks like the screenshot. Every factual claim in the manual (menu
path, setting name, default, behavior) has been checked against the code by an agent that did
not write it. Every doc image can be regenerated with one command.

## Starting point (inventory 2026-11-10)

- `docs/user/`: 62 pages, ~19k lines. Four very large pages: `TROUBLESHOOTING.md` (2,571),
  `CONFIGURATION.md` (2,369), `guide/home-panel.md` (1,310), `guide/filament.md` (908).
  Read on GitHub and published as versioned pages at helixscreen.org (`/1.1/guide/...`),
  built by `prestonbrown/helixscreen-website` when `release.yml` (`notify-website`) pings it.
  The app hard-codes `guide/`, `guide/getting-started/` and `upgrading/` URLs
  (`include/helix_version.h#helix::docs_url`, Help and Updates settings), some as QR codes.
- `docs/devel/`: 174 files, ~84k lines (architecture chapters, feature docs, printers,
  printer research, plans). `docs/audits/`: 6 point-in-time reports.
- Images: 140 under `docs/images/`, all references resolve, 5 unreferenced. About 60 of the
  105 user images have a recipe in `scripts/screenshot-all.sh`; about 45 were captured by hand
  (home widgets, tool offsets, screen sharing, touch calibration, wizard, others). There are
  two capture flows: `make screenshots` (`scripts/generate-screenshots.sh`, top-level
  `screenshot-*.png`) and `scripts/screenshot-all.sh` with `scripts/screenshot-recipes.sh`.
- `scripts/doc_anchors.py --check` reports 52 broken or ambiguous code anchors, all in
  `docs/devel/` (clusters: `TELEMETRY_ADMIN.md`, `architecture/07-filament-ams.md`). It is
  not in the gates. `check_doc_refs.py` (gated) checks paths, links, make targets and count
  claims, and passes.
- Nothing checks prose against code, screenshot freshness, or external links.
- `docs/devel/CHANGELOG_1_1_DRAFT.md` (~1,700 lines) is the master list of what changed.

## Goals

### User manual (priority; gates 1.1 final)

1. **Accuracy.** Every page claim-checked against the 1.1 code and a live `--test` mock run.
   Install and printer-family pages the mock cannot exercise: claims checked against the
   installer scripts and code, then each install path run remotely on a fleet printer where
   one exists (ssh via zeus, `device:` claim, idle printer, Preston confirms each device
   before anything is installed on it). Steps that need hands are marked and listed for
   Preston after 2026-11-15; they do not block 1.1. (Preston.)
2. **Coverage.** Every user-facing feature in `CHANGELOG_1_1_DRAFT.md` (Added and Changed) is
   documented somewhere a user would look. A coverage table maps changelog entries to pages.
3. **Restructure the giants.** `TROUBLESHOOTING.md` becomes symptom-first topic pages
   (connection, display/touch, printing, filament systems, updates/install, performance),
   with a short index page. `CONFIGURATION.md` splits into one page per config area.
   `home-panel.md` and `filament.md` are split if a claim-checked rewrite still leaves them
   hard to navigate (writer's judgment, reviewer agrees). Add a short **getting started per
   printer family** where install pages exist (U1, K1, K2, AD5M, AD5X, QIDI, CC1, generic).
   Every other page keeps its place and is fixed in place. The sweep may edit the website
   repo: moved pages get nav entries and redirects there. `guide/getting-started.md`,
   `UPGRADING.md` and the guide index are never renamed (QR codes on shipped devices).
   (Preston.)
4. **Concise.** Step-by-step where the user acts, short reference tables where they look
   things up. No source references (`docs/user/CLAUDE.md` style rules apply).

### Screenshots

5. **More and better pictures.** Full-screen shots where orientation matters; **callouts**
   everywhere else: a crop of the card or widget by its `ctl geom` box with a small margin,
   with **numbered markers** drawn on it and a matching numbered list under the image. No
   text drawn into images (translation and search).
6. **Variants where the screen differs.** Most shots come from one generic mock printer.
   Filament, tool changer and printer-specific pages get shots from each backend's mock
   persona (AFC, Happy Hare, toolchanger, U1, CFS, ACE, AD5X IFS and the rest) where the
   screen actually differs.
7. **Reproducible.** App shots are 800x480 unless the page is about another layout (small,
   portrait, ultrawide), which the manifest's size field states. Images that cannot come
   from `ctl` (a browser view, ESP32 photos, first-run states) get a `manual` manifest entry
   naming the source and how to recapture. (Preston.) One recipe manifest covers every doc image: output path, mock
   persona/env, `ctl` navigation steps, crop target (widget name or full screen), and marker
   positions (relative to named widgets, so they survive layout tweaks). The two current
   capture flows fold into it. `make doc-screenshots` regenerates all of them headless
   (`HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`). A fast lint fails a commit that adds or
   references a doc image with no recipe. No CI pixel diffing.

### Dev docs and agent docs (accuracy + prune; may land after 1.1 final)

8. Fix the 52 broken code anchors and add `doc_anchors.py --check` to the fast gates.
9. Claim-check each feature doc and architecture chapter against the code.
10. Delete or fold docs describing dead code or superseded investigations (point-in-time
    audits in `docs/audits/`, stale printer research), per the plans lifecycle in
    `docs/CLAUDE.md`. Every deletion listed in the wave's report.
11. `CLAUDE.md` files and `.claude/rules/` checked for stale commands, paths and claims.
    These are edited conservatively: fix what is wrong, never reword rules.
12. `docs/devel/GALLERY.md` regenerated from the new manifest.

## Non-goals

- No docs site generator, theme or hosting change; website edits are limited to nav and
  redirects for moved pages.
- No translation of the manual.
- No new features or UI changes to make a doc easier to write; a UI bug found while
  documenting is filed as an issue (or fixed separately) and the doc describes 1.1 as shipped.
- `docs/superpowers/` and `.worktrees/` are out of scope (local scratch).
- Main-only (1.2) features are not documented in this sweep.

## Anti-goals

- Rewritten prose that is still wrong. A page is done only after an independent claim check.
- Longer docs for their own sake. Uplift means clearer and better pictured; a page that grows,
  grows because a feature was missing.
- Hand-made images with no recipe.
- A broken link from the outside world to the install or troubleshooting pages.

## Constraints

- Branch: written against `release/1.1` (what 1.1 ships), landed there, merged forward to
  `main`. Before starting, diff `docs/` between `main` and `release/1.1` and record which
  side is ahead per file; the sweep works on the 1.1 copy and the forward merge resolves.
- Wave A's tooling (scripts, `make doc-screenshots`, the lint) lands on `release/1.1` and
  merges forward. The lint covers images referenced from `docs/user/`, `README.md`,
  `docs/README.md` and `GALLERY.md`; `main` adds recipes for its own 1.2 references when the
  forward merge lands. (My call.)
- Wave G checks dev docs, `CLAUDE.md` files and `.claude/rules/` against **`main`**, since
  that is the code contributors and agents work on; `release/1.1` takes only the anchor
  fixes and deletions that apply there. The anchor gate lands on `main` first. (My call.)
- Execution: a multi-agent workflow in waves, one wave per area, each page by a writer agent
  and claim-checked by a separate checker agent. Preston opted in; state the agent count per
  wave before launching it. Waves: (A) screenshot tooling and manifest, (B) setup and install,
  (C) home, printing, monitoring, (D) filament systems and tool changers, (E) settings and
  configuration, (F) troubleshooting restructure, (G) dev docs and agent docs. A and B first;
  C-F can overlap once A lands.
- Subagents Opus or lower. Screenshot runs headless, socket and config dir pinned per tree
  (root `CLAUDE.md`), never on a real printer.
- Each wave lands as one batch: one review, one `make land-gate` (docs-only waves skip a
  second gate per the verification cadence). The user-manual waves must be on `release/1.1`
  before 1.1 final is tagged; tag timing stays Preston's call.

## Decided by Preston (2026-11-10)

User manual first; scripted, reproducible screenshots with numbered-marker callouts; claim
checks by an agent that did not write the page; user docs gate 1.1 final, dev docs may follow;
restructure the four giant pages and keep the rest in place; land on `release/1.1` and port
to `main`; recipe lint plus one regen command, no CI diffing; dev docs get accuracy plus
pruning; per-backend screenshots where screens differ; run as a workflow in waves; the
sweep may edit the website repo but never renames the QR-code pages; install pages verified
by code plus remote device runs, hands-on steps deferred past 1.1; 800x480 shots plus
manual records for non-app images.

My calls (check these): wave A tooling lands on `release/1.1`; wave G checks against `main`.

## Left to the builder

- Manifest format (YAML/JSON/shell) and how markers are drawn (ImageMagick or a small
  script), as long as one command regenerates everything and markers anchor to widgets.
- Exact split of `CONFIGURATION.md` and `TROUBLESHOOTING.md` into pages.
- Claim-check output format; each wave reports claims checked, mismatches fixed, issues filed.
- Which existing hand-made images to drop rather than recreate.
