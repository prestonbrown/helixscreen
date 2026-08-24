---
description: Start a new multi-phase project. Researches codebase, proposes phases, creates plan doc, and sets up worktree.
argument-hint: "[rough description of what you want to build]"
---

# NEW MULTI-PHASE PROJECT

## FLOW (strict order, no skipping)

### 1. CHECK SUPERPOWERS
Test if `superpowers:brainstorming` available.
- **AVAILABLE** → follow SUPERPOWERS PATH (step 2a)
- **NOT AVAILABLE** → follow FALLBACK PATH (step 2b)

---

## SUPERPOWERS PATH (preferred)

### 2a. BRAINSTORM (REQUIRED skill)
**MUST** invoke `superpowers:brainstorming`
- Explores user intent, requirements, design before implementation
- Produces approved design

### 3a. WRITE PLAN (REQUIRED skill)
**MUST** invoke `superpowers:writing-plans`
- Creates structured plan doc from approved design

### 4a. SETUP WORKTREE (REQUIRED skill)
**MUST** invoke `superpowers:using-git-worktrees`
- Creates isolated worktree for feature work

### 5a. HANDOFF
- Output `HANDOFF: [Project Name]`
- Inform user: plan location, worktree path, use `/continue` to resume

**DONE**

---

## FALLBACK PATH (only if superpowers unavailable)

### 2b. REQUIREMENTS
- If user hasn't described feature → AskUserQuestion: "What feature would you like to build?"
- Clarify scope, constraints, priorities

### 3b. RESEARCH
Launch Explore agents (parallel) for:
- Similar patterns in codebase
- Architecture/conventions
- Testing patterns
- Existing infrastructure to leverage

### 4b. PROPOSE PHASES
AskUserQuestion with phase breakdown:
- Each phase ≈ 1 commit
- Phase 0 = foundation/setup
- Typically 3-6 phases
- Each phase has: goal, deliverables, verification

**DO NOT PROCEED** without user approval.

### 5b. REFINE
Based on feedback:
- **NEEDS CHANGES** → return to step 4b
- **APPROVED** → step 6b

### 6b. CREATE PLAN DOC
Write to `docs/devel/plans/[project-name]-plan.md`:
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

### 7b. WORKTREE? (checkpoint)
AskUserQuestion: "Create worktree for isolated development?"
- **YES** → step 8b
- **NO** → step 9b

### 8b. SETUP WORKTREE
Check CLAUDE.md for conventions. Run:
```bash
./scripts/setup-worktree.sh feature/[feature-name]
# Creates worktree at .worktrees/[feature-name] and builds automatically
```

### 9b. HANDOFF
- Output `HANDOFF: [Project Name]`
- Inform user: plan location, worktree path (if created), use `/continue` to resume

**DONE**

---

## PLAN LOCATION
Prefer `docs/devel/plans/` (in project, versioned). Use `~/.claude/plans/` only if project disallows.

## DEFAULTS
Delegation: main coordinates, agents research | TDD: backend=yes, UI=skip | Commits: 1/phase
