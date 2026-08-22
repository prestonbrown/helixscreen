# Contributing to HelixScreen

Thanks for being here. HelixScreen is a touchscreen UI for Klipper 3D printers, built by and for people who want more from their hardware than a browser tab can offer. Contributions are welcome at every scale — a translation fix, a theme tweak, a whole new feature subsystem.

This file is the **front door**. It tells you where to go next based on what you want to do.

**New contributor?** There is a marked path:
[ONBOARDING.md](docs/devel/ONBOARDING.md) (environment + build + the 15-minute
mental model) → [YOUR_FIRST_CONTRIBUTION.md](docs/devel/YOUR_FIRST_CONTRIBUTION.md)
(walkthrough of a real contribution) → [the architecture guide](docs/devel/architecture/README.md)
(pick your subsystem). Each doc links to the next.

---

## Before you start

Make sure you can build and run HelixScreen locally:

```bash
git clone https://github.com/prestonbrown/helixscreen.git
cd helixscreen
git submodule update --init --recursive
make setup              # Installs pre-commit hook + commit template
make -j                 # Builds the binary (not tests — see Makefile)
./build/bin/helix-screen --test -vv
```

If the mock printer UI comes up, you're ready.

Full environment setup (dependencies per OS), the build/test workflow, and the
whole-app mental model live in the next stop on the path:
→ **[docs/devel/ONBOARDING.md](docs/devel/ONBOARDING.md)**

---

## What kind of contribution is this?

Pick the row that matches what you want to do. Each links to the doc that will actually help you.

| You want to... | Start here |
|---|---|
| **Fix a bug** you hit | Open an issue if one doesn't exist. Then → [DEVELOPMENT.md § Contributing](docs/devel/DEVELOPMENT.md#contributing) for the code workflow. |
| **Fix a layout** at some breakpoint (clipping, wrapping, portrait issues) | → [UI Contributor Guide](docs/devel/UI_CONTRIBUTOR_GUIDE.md) |
| **Create a theme** (a new color palette — no code) | → [Theme Contributor Guide](docs/devel/THEME_CONTRIBUTOR_GUIDE.md) |
| **Add or improve a translation** (or add a new language) | → [Translation Contributor Guide](docs/devel/TRANSLATION_CONTRIBUTOR_GUIDE.md) |
| **Add a settings overlay or feature overlay** (the most common "real" contribution) | → [Your First Contribution](docs/devel/YOUR_FIRST_CONTRIBUTION.md) |
| **Add a modal dialog** | → [Modal System](docs/devel/MODAL_SYSTEM.md) |
| **Add a new widget** or semantic component | → [LVGL 9 XML Guide](docs/devel/LVGL9_XML_GUIDE.md) + [UI Contributor Guide](docs/devel/UI_CONTRIBUTOR_GUIDE.md) |
| **Add a printer to the database** | Edit `assets/config/printer_database.json`. Follow the existing entries. No C++ needed. |
| **Add a new filament backend** (AMS / IFS / CFS / etc.) | → [Filament Management](docs/devel/FILAMENT_MANAGEMENT.md) |
| **Add support for a new platform** (a new SBC, a new stock firmware) | Open a GitHub Discussion first — these contributions span build system, cross-compile, and deployment. → [Build System](docs/devel/BUILD_SYSTEM.md) |
| **Write or improve documentation** | → [docs/CLAUDE.md](docs/CLAUDE.md) for the doc structure. User docs in `docs/user/`, developer docs in `docs/devel/`. |
| **Write a plugin** | → [Plugin Development](docs/devel/PLUGIN_DEVELOPMENT.md) |
| **Propose something bigger** (architectural change, new subsystem) | Open a GitHub Discussion. Align on scope before writing code — it saves rework for both of us. |

If you're unsure where your contribution fits, open a [Discussion](https://github.com/prestonbrown/helixscreen/discussions) or ask on [Discord](https://discord.gg/RZCT2StKhr) before starting.

---

## When you're stuck

Two debugging references worth knowing:

- **[Contributor Gotchas](docs/devel/CONTRIBUTOR_GOTCHAS.md)** — Symptom-indexed lookup for common silent failures ("my component doesn't render", "my binding doesn't update", "my click does nothing"). Flip here first.
- **[Developer Quick Reference](docs/devel/DEVELOPER_QUICK_REFERENCE.md)** — Code patterns for specific scenarios.

And the single best debugging move: find the closest-shaped sibling in `src/ui/` and diff your code against it.

---

## The workflow, briefly

1. **Branch** from `main`: `git switch -c feature/short-name` (or `fix/short-name` for bugs).
2. **Worktree** if the change spans many files: `scripts/setup-worktree.sh feature/short-name`.
3. **Build and test** locally. `make test-run` for the full suite. Test at multiple breakpoints for UI changes (see [UI Contributor Guide § Screen Breakpoints](docs/devel/UI_CONTRIBUTOR_GUIDE.md)).
4. **Commit** using the `type(scope): summary` format — `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `style`. The pre-commit hook will auto-format via clang-format.
5. **Open a PR** against `main`. Describe what changed and why. Include screenshots for UI work at multiple breakpoints. Link any related issues.

Full details on code standards, commit style, and PR expectations:
→ **[DEVELOPMENT.md § Contributing](docs/devel/DEVELOPMENT.md#contributing)**

---

## Plans and specs

In-flight design docs live in [docs/devel/plans/](docs/devel/plans/) — working scaffolding, deleted in the same change that ships the work. Don't commit plans anywhere else, and don't resurrect shipped ones; git history is the archive (full convention: [docs/CLAUDE.md](docs/CLAUDE.md)).

---

## Things that will get your PR bounced

These are non-negotiable. They're in place because violations have caused real crashes, real user-visible regressions, or real contributor onboarding pain.

1. **No `lv_obj_add_event_cb()` in C++.** Use XML `<event_cb>` + `lv_xml_register_event_cb()`.
2. **No hardcoded colors or spacing.** Use design tokens (`#card_bg`, `#space_md`).
3. **No direct `lv_subject_set_*()` calls from background threads.** Use `ui_queue_update()` or the `AsyncLifetimeGuard` pattern.
4. **No `printf`, `cout`, or `LV_LOG_*`.** Use `spdlog`.
5. **No `--no-verify` on commits.** If a hook fails, fix the underlying issue.
6. **No translations on product names, material codes, or URLs.** Wrap sentences that *contain* them, not the names themselves. See [CONTRIBUTOR_GOTCHAS.md](docs/devel/CONTRIBUTOR_GOTCHAS.md).

The full set of rules lives in [CLAUDE.md](CLAUDE.md) at the repo root. It's the authoritative reference for what the codebase expects.

---

## Communication

- **[Discord](https://discord.gg/RZCT2StKhr)** — Get help, share your setup, follow development. Fastest way to reach the project.
- **[GitHub Discussions](https://github.com/prestonbrown/helixscreen/discussions)** — Longer-form questions, proposals, architectural conversations worth preserving.
- **[GitHub Issues](https://github.com/prestonbrown/helixscreen/issues)** — Bug reports, feature requests with a clear scope.

HelixScreen is also discussed in the [FuriousForging Discord](https://discord.gg/Cg4yas4V) (#mods-and-projects) and [VORONDesign Discord](https://discord.gg/voron) (#voc_works).

---

## Code of conduct

Be a decent human. Assume good faith. Disagree on technical substance, not on people. If an interaction goes sideways, reach out to the maintainer privately rather than escalating in public.

---

## License

HelixScreen is licensed under [GPL v3.0 or later](LICENSE). By contributing, you agree your contributions will be licensed under the same terms. Add the SPDX header to any new source file:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
```

(XML files use `<!-- ... -->` comments for the header. Full format reference: [COPYRIGHT_HEADERS.md](docs/devel/COPYRIGHT_HEADERS.md).)

---

## Your next step

- **First time here?** Continue the path: → **[ONBOARDING.md](docs/devel/ONBOARDING.md)**
- **Already know the codebase?** Pick your subsystem: → **[architecture/README.md](docs/devel/architecture/README.md)**

Thanks for helping make HelixScreen better.
