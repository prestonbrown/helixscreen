---
name: autonomous-session
description: Use when given autonomous control of HelixScreen — the user says to work independently, autonomously, with minimal interruption, unsupervised, or "keep going without asking". Defines the scratchpad workspace, what counts as good autonomous work, and which decisions must still come back to the user.
---

# Autonomous Sessions

**Scratchpad**: `.claude/scratchpad/` - Claude's workspace for:
- Ideas and feature concepts
- Research notes and findings
- Work-in-progress designs
- Lessons learned and deep dives (see `.claude/scratchpad/` for past examples)

Ask the user only for the items under **Ask first**, a UX preference call, or when truly blocked;
record findings in the scratchpad for future sessions.

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
