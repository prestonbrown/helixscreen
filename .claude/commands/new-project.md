---
description: Start a new multi-phase project. Researches codebase, proposes phases, creates plan doc, and sets up worktree.
argument-hint: "[rough description of what you want to build]"
---

# New Multi-Phase Project

Two paths: the superpowers skills when `superpowers:brainstorming` is installed, the
fallback below when it is not. Both end in a plan doc, an optional worktree, and a
handoff that `/continue-project` resumes.

## With superpowers

1. **Brainstorm** with `superpowers:brainstorming` until the user approves a design.
2. **Plan** with `superpowers:writing-plans`, written to the location below.
3. **Worktree:** `scripts/setup-worktree.sh feature/<name>`, per the `helix-ops` skill § Worktrees.
   A harness-made worktree has no lib/ symlinks or submodules, so prefer the script.
4. **Handoff** (below).

## Fallback

1. **Requirements.** If the user hasn't described the feature, ask what they want to
   build. Clarify scope, constraints and priorities.
2. **Research** with parallel Explore agents: similar patterns in the codebase,
   architecture and conventions, testing patterns, infrastructure to reuse.
3. **Propose phases** with AskUserQuestion and wait for approval; iterate on feedback.
   Each phase is about one commit with a goal, deliverables and verification. Phase 0
   is foundation; 3-6 phases is typical.
4. **Write the plan doc** in this shape:
   ```markdown
   # [Feature Name]

   ## Overview
   [1-2 sentences]

   ## Phases

   ### Phase 0: [Name]
   **Goal**: [What]
   **Deliverables**: [List]
   **Verification**: [How to verify]

   ### Phase 1: [Name]
   ...

   ## Progress
   - [ ] Phase 0: [Name]
   - [ ] Phase 1: [Name]

   ## Key Files
   - `path/file.cpp` - [role]
   ```
5. **Worktree?** Ask whether to create one; if yes,
   `scripts/setup-worktree.sh feature/<name>` (creates `.worktrees/<name>` and builds).
6. **Handoff** (below).

## Plan location
`docs/devel/plans/YYYY-MM-DD-<topic>.md` (naming convention: `docs/CLAUDE.md`). Use
`~/.claude/plans/` only if the project disallows in-tree plans.

## Handoff
Output `HANDOFF: [Project Name]`, then tell the user the plan location, the worktree
path if one was created, and that `/continue-project` resumes it.

## Defaults
The session coordinates and agents research; TDD for backend work, not for UI; one
commit per phase.
