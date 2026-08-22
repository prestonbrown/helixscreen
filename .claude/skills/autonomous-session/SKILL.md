---
name: autonomous-session
description: Use when given autonomous control of HelixScreen — the user says to work independently, autonomously, with minimal interruption, unsupervised, or "keep going without asking". Defines the scratchpad workspace, what counts as good autonomous work, and which decisions must still come back to the user.
---

# Autonomous Sessions

When given autonomous control, Claude works independently to improve HelixScreen with minimal interruption.

**Scratchpad**: `.claude/scratchpad/` - Claude's workspace for:
- Ideas and feature concepts
- Research notes and findings
- Work-in-progress designs
- Lessons learned and deep dives (see `.claude/scratchpad/` for past examples)

**Mission**: Make HelixScreen the best damn touchscreen UI for Klipper printers.

**Autonomy Guidelines**:
- Work independently on improvements that align with existing patterns
- Commit working code with tests (don't leave broken state)
- Ask user ONLY for: major architectural decisions, UX preference calls, or when truly blocked
- Document findings in scratchpad for future sessions
- Small failures are fine - learn and move on

**Good autonomous work**:
- Polish and micro-improvements
- Code cleanup and consistency
- Adding missing tests
- Fixing obvious bugs
- Implementing features from labeled GitHub issues

**Ask first**:
- New architectural patterns
- Removing/deprecating features
- Changes to critical paths — PrinterState, WebSocket/threading, shutdown, DisplayManager, XML processing (see `CLAUDE.md` § "Critical Paths")
- Anything that changes user-facing behavior significantly
