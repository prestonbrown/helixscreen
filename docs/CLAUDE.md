# docs/CLAUDE.md — Documentation Hub

## Quick Routing

| I need to... | Go to |
|--------------|-------|
| Look up how a system works | `devel/CLAUDE.md` — find the feature doc |
| Understand a shared convention spec | `specs/CLAUDE.md` — public, vendor-neutral specs |
| Write/update user-facing docs | `user/CLAUDE.md` — style rules for end users |
| Find XML widget docs | `devel/LVGL9_XML_GUIDE.md` |
| Understand modal patterns | `devel/MODAL_SYSTEM.md` |
| Check a feature's architecture | `devel/ARCHITECTURE.md` → the right chapter in `devel/architecture/` |
| See what's planned/complete | [GitHub issues](https://github.com/prestonbrown/helixscreen/issues) |
| Find in-flight plans/specs | `devel/plans/` — point-in-time working docs, deleted when the work ships |
| Update the doc index | `README.md` + this file + relevant `CLAUDE.md` |

## Writing Documentation

- Developer docs go in `devel/`, user docs go in `user/`
- Follow the style of existing docs (see `devel/SOUND_SYSTEM.md` or `devel/MODAL_SYSTEM.md`)
- Developer docs: include overview, key files table, architecture, real code examples, developer extension guide
- User docs: step-by-step, no source code references, copy-pasteable commands
- No SPDX headers on docs (only on source code)
- Keep code examples real — pull from actual files, don't invent
- When adding new docs, update: this file, `README.md`, and `devel/CLAUDE.md` or `user/CLAUDE.md`

## Plans and Specs

Plans and specs live in `docs/devel/plans/` while work is in flight (`YYYY-MM-DD-<topic>-design.md` for designs, `YYYY-MM-DD-<topic>.md` for plans). They are scaffolding, not documentation: when work ships, durable knowledge lands where it belongs - code, the feature's devel doc, an architecture chapter - and the plan file is deleted in the same change. Abandoned and superseded plans are deleted too. Git history is the archive. `docs/superpowers/` and `.superpowers/` are local working space, never committed, never force-added.
