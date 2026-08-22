---
description: Continue a multi-phase project. Reads plan doc, shows progress, and resumes work.
argument-hint: "[plan-doc-path or project-name] [--defaults]"
---

# CONTINUE MULTI-PHASE PROJECT

## FLOW (strict order, no skipping)

### 1. LOCATE
- Path provided → use directly
- Else search `docs/devel/plans/` then `~/.claude/plans/`
- **NOT FOUND** → STOP. AskUserQuestion for path.

### 2. READ
Extract from plan: project name, completed phases (checked Progress), next phase, key files, verification steps.

### 3. MATCH HANDOFF
Plan doc = source of truth. Handoffs pollute across sessions.
- Verify: title contains project name, refs match Key Files, phase aligns with Progress
- **CLEAR SINGLE MATCH** → use it
- **MULTIPLE/AMBIGUOUS/ANY DOUBT** → STOP. AskUserQuestion: which handoff?
- **NONE** → create: `HANDOFF: [Project Name] Phase [N]`

### 4. STATUS (must display before proceeding)
```
## [Project Name]
Completed: [x] Phase 0 - `hash` | [x] Phase 1 - `hash`
Next: Phase 2 - [Name] ([goal])
Files: path/file.cpp | Handoff: [hf-XXX]
```

### 5. WORKTREE
If exists: `git status` + `git log --oneline -3`. Report uncommitted/divergence.
- **UNCOMMITTED CHANGES** → STOP. AskUserQuestion: stash/commit/abort?

### 6. OVERRIDES (checkpoint)
**If `--defaults` flag passed:** Skip prompt, use defaults (subagent-driven, TDD per DEFAULTS section).

**Otherwise MUST** AskUserQuestion with two questions:

1. **Execution** (header: "Execution"):
   - "Subagent-driven (Recommended)" - fresh subagent/task, two-stage review
   - "Batched with checkpoints" - human-in-loop between batches of 3

2. **Overrides** (header: "Overrides", multiSelect: true):
   - "None - use defaults"
   - "Skip TDD for this phase"
   - "Extra review rigor"

**DO NOT PROCEED** without response (unless `--defaults`).

### 7. EXECUTE (REQUIRED skill)
- Subagent-driven → **MUST** `superpowers:subagent-driven-development`
- Batched → **MUST** `superpowers:executing-plans`

Only exception: skill not installed → STOP, inform user.
**DO NOT** execute phase yourself.

### 8. VERIFY (REQUIRED skill)
- Run verification from plan
- **MUST** `superpowers:requesting-code-review` for any code changes

Only exception: skill not installed.

### 9. UPDATE
- Commit: `feat(scope): phase N desc`
- Update plan Progress: `- [x] Phase N: [Name] - \`hash\` (date)`
- Mark todo complete

### 10. MORE PHASES?
- **YES** → return to step 6 (preserving `--defaults` flag if originally provided)
- **NO** → step 11

### 11. FINISH (REQUIRED skill)
**MUST** `superpowers:finishing-a-development-branch`

Only exception: skill not installed.

---

## HANDOFF MATCHING RULES

| Situation | Action |
|-----------|--------|
| Explicit path provided | Trust plan, ignore handoff suggestions |
| Single clear match | Resume (verified title+refs+phase) |
| Multiple/ambiguous | **ASK** via AskUserQuestion |
| None exists | Create new handoff |
| Any doubt | **ASK** - never auto-select |

## DEFAULTS
Execution: `subagent-driven-development` (recommended) | Delegation: **MANDATORY** - main coordinates, subagents implement (only skip if skill not installed) | TDD: backend=yes, UI=skip | Reviews: required for code changes | Commits: 1/phase
