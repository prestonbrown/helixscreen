---
description: Continue a multi-phase project. Reads plan doc, shows progress, and resumes work.
argument-hint: "[plan-doc-path or project-name] [--defaults]"
---

# Continue a Multi-Phase Project

The plan doc is the source of truth. Handoffs from other sessions can describe a
different project with a similar name, so trust one only when it matches the plan.

## 1. Locate the plan
Use the path if one was given. Otherwise search `docs/devel/plans/`, then
`~/.claude/plans/`. If nothing matches, ask for the path with AskUserQuestion.

## 2. Read it
Extract the project name, completed phases (checked Progress items), the next phase,
key files and the verification steps.

## 3. Match a handoff
A handoff matches when its title names the project, its refs match Key Files, and its
phase agrees with Progress.

| Situation | Action |
|-----------|--------|
| Explicit plan path given | Trust the plan; ignore handoff suggestions |
| One clear match | Resume it |
| Several, or any doubt | Ask which one with AskUserQuestion; never auto-select |
| None | Create `HANDOFF: [Project Name] Phase [N]` |

## 4. Show status before doing anything
```
## [Project Name]
Completed: [x] Phase 0 - `hash` | [x] Phase 1 - `hash`
Next: Phase 2 - [Name] ([goal])
Files: path/file.cpp | Handoff: [hf-XXX]
```

## 5. Check the worktree
If one exists, run `git status` and `git log --oneline -3` and report uncommitted work or
divergence. Uncommitted changes are someone's work: ask whether to commit or abort
before going further.

## 6. Choose execution (checkpoint)
With `--defaults`, use the defaults below without asking. Otherwise ask with
AskUserQuestion and wait for the answer:

1. **Execution** (header "Execution"): "Subagent-driven (Recommended)" - a fresh
   subagent per task, each running its own tags, with one review of the phase at the end; or "Batched with checkpoints" - human review
   between batches of 3.
2. **Overrides** (header "Overrides", multiSelect): "None - use defaults",
   "Skip TDD for this phase", "Extra review rigor".

## 7. Execute
Subagent-driven: dispatch one implementer agent per task with the plan's task text,
interfaces and tags in its brief, each in its own worktree, and verify each by its commit
and diff, not its report. Batched: implement 3 tasks, stop for the user's review, repeat.
This session coordinates and delegates; it does not implement the phase itself.

## 8. Verify
Once per phase, at its end, per root `CLAUDE.md` § Verification Cadence: one review of the
phase, `mutate-diff` scoped to the phase's tags, and one `land-gate` before it lands. While
the phase is in progress, tasks run only their own tags.

## 9. Record
- Commit: `feat(scope): phase N desc`
- Check off Progress in the plan: `- [x] Phase N: [Name] - \`hash\` (date)`

## 10. Next
More phases: back to step 6, keeping `--defaults` if it was given. Otherwise ask the
user whether to land the branch, open a PR, or keep it.

## Defaults
Subagent-driven execution; TDD for backend work, not for UI; one review per phase at
its end; one commit per phase.
