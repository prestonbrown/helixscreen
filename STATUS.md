# Project Status & Documentation Guide

**Last Updated:** 2025-10-26

This project uses **git history** and **focused documentation** instead of maintaining a development journal.

## Where to Find Information

### Current Work & Next Steps
👉 **[HANDOFF.md](HANDOFF.md)** - Active work status, priorities, critical patterns

### Project Overview & Patterns
👉 **[CLAUDE.md](CLAUDE.md)** - Build system, architecture, logging policy, agents

### Technical Reference
👉 **[docs/LVGL9_XML_GUIDE.md](docs/LVGL9_XML_GUIDE.md)** - Complete LVGL 9 XML reference
👉 **[docs/QUICK_REFERENCE.md](docs/QUICK_REFERENCE.md)** - Common code patterns
👉 **[docs/COPYRIGHT_HEADERS.md](docs/COPYRIGHT_HEADERS.md)** - GPL v3 header templates
👉 **[docs/UI_TESTING.md](docs/UI_TESTING.md)** - UI test infrastructure & patterns
👉 **[docs/MEMORY_ANALYSIS.md](docs/MEMORY_ANALYSIS.md)** - Memory profiling & lifecycle patterns

### Development History
👉 **Git commits** - Use descriptive commit messages
```bash
git log --oneline --since="2 weeks ago"
git log --grep="wizard" --oneline
git log --all --full-history -- path/to/file
```

### Release History
👉 **[CHANGELOG.md](CHANGELOG.md)** - User-facing changes by version

### Archived Documentation
👉 **[docs/archive/](docs/archive/)** - Historical development journals

## Documentation Philosophy

**Keep it lean:**
- HANDOFF.md ≤ 150 lines (prune completed work aggressively)
- Git commits are the source of truth for "what happened"
- Patterns go in CLAUDE.md once established
- Delete documentation when it's no longer relevant

**Trust the tools:**
- Git history > development journals
- Code > comments
- Working examples > abstract explanations

## Major Architectural Decisions

This section captures **non-obvious design choices** that aren't clear from code:

### Reactive Subject-Observer Pattern (2025-10-25)

**Decision:** Use LVGL 9's subject/observer system for all UI state updates instead of direct widget manipulation.

**Rationale:**
- Separates business logic from UI rendering
- Enables unit testing without LVGL dependencies
- Prevents spaghetti code from direct widget access
- Matches modern reactive UI patterns (React, Vue, SwiftUI)

**Pattern:** See CLAUDE.md Pattern #2 and wizard validation implementation.

### XML-First UI Development (2025-10-24)

**Decision:** Define all UI layouts in XML, never create LVGL widgets directly in C++.

**Rationale:**
- No recompilation for layout changes
- Declarative syntax is easier to read/maintain
- Complete separation of presentation from logic
- Designer-friendly (future LVGL SquareLine Studio integration)

**Trade-offs:** Steeper learning curve, requires understanding LVGL 9 XML property system.

### Create-Once Panel Lifecycle (2025-10-27)

**Decision:** Pre-create all panels at startup, toggle visibility for navigation instead of dynamic create/delete.

**Rationale:**
- Memory profiling shows negligible cost (<2 MB for all panels vs framework's 55 MB)
- Instant panel switching (0ms flag toggle vs 50-100ms creation)
- State preservation automatic (no serialization needed)
- Predictable memory usage (no runtime allocation failures)
- See docs/MEMORY_ANALYSIS.md for detailed profiling data

**When to reconsider:** If running on <256 MB RAM hardware or supporting 50+ panels.

---

**Note:** Once patterns are documented in CLAUDE.md, remove them from this section to keep it lean.
