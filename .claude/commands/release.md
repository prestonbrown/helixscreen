---
description: Create a release — bump version, changelog, tag, push. Automates the full release workflow.
argument-hint: "[patch | minor | major | X.Y.Z | X.Y.Z-beta | X.Y.Z-rc.N]"
allowed-tools: ["Bash", "Read", "Glob", "Grep", "Edit", "Write", "AskUserQuestion", "WebFetch"]
---

# RELEASE WORKFLOW

You are automating the HelixScreen release process. Follow each step in order. Hard stop on failures — do not proceed with warnings or partial state.

## CONTEXT (gather first)

Collect all of these before starting:

```bash
git rev-parse --abbrev-ref HEAD          # Must be "main"
git status --porcelain | grep -v -E '\.claude-recall/(LESSONS\.md|LESSONS\.md\.lock|CLAUDE\.md|injection-stats\.json|stats\.json)'  # Must be empty (ignoring recall files)
git fetch origin && git status -sb        # Must be up-to-date with origin/main
cat VERSION.txt                           # Current version
git describe --tags --abbrev=0 2>/dev/null  # Last tag (e.g., v0.9.3)
git log $(git describe --tags --abbrev=0 2>/dev/null)..HEAD --oneline  # Commits since last tag
```

Today's date: use `date +%Y-%m-%d` output.

---

## STEP 0: PRE-FLIGHT CHECKS

All of these must pass. If ANY fails, STOP and tell the user why.

| Check | How | Fail action |
|-------|-----|-------------|
| On `main` branch | `git rev-parse --abbrev-ref HEAD` = "main" | STOP: "Switch to main first" |
| Clean working tree | `git status --porcelain` is empty after ignoring `LESSONS.md`, `LESSONS.md.lock`, `CLAUDE.md`, `injection-stats.json`, and `stats.json` in `.claude-recall/`. All five are recall's own state, rewritten by any session; since worktrees route their writes here (`PROJECT_DIR`, see `setup-worktree.sh`), they are dirty most of the time and are not a reason to hold a release. Commit them with it. | STOP: "Uncommitted changes — commit or stash first" |
| Up to date with origin | `git fetch origin` then check `git status -sb` for "behind" | STOP: "Branch is behind origin/main — pull first" |
| Tags fetched | `git fetch --tags origin` | Just do it silently |

---

## STEP 1: DETERMINE VERSION

**Argument:** `$ARGUMENTS` (the user's argument to `/release`)

### If argument is a bump type (`patch`, `minor`, `major`):
- Parse last tag (strip `v` prefix) into MAJOR.MINOR.PATCH (ignore any prerelease suffix)
- Bump the appropriate component, reset lower components to 0
- Example: last tag `v0.9.3` + `patch` = `0.9.4`

### If argument is an explicit version (e.g., `1.0.0`, `1.0.0-beta`, `1.0.0-rc.1`):
- Strip leading `v` if present
- Validate: must match pattern `MAJOR.MINOR.PATCH[-PRERELEASE]`
- PRERELEASE can be: `alpha`, `beta`, `rc.N`

### If no argument:
- Compare `VERSION.txt` content with last tag (strip `v` prefix)
- If VERSION.txt is ALREADY different (higher) than last tag → use VERSION.txt value
- If VERSION.txt matches last tag → use AskUserQuestion:
  - Question: "No version specified and VERSION.txt matches the last tag (vX.Y.Z). What version should this release be?"
  - Header: "Version"
  - Options: "Patch (X.Y.Z+1)", "Minor (X.Y+1.0)", "Major (X+1.0.0)"
  - (user can also type a custom version via Other)

### Validation (all cases):
- Must be valid semver: `MAJOR.MINOR.PATCH` with optional `-PRERELEASE`
- Must be strictly greater than last tag's version (compare without `v` prefix)
- If validation fails → STOP with clear error

Store the resolved version as `NEW_VERSION` (without `v` prefix) for all subsequent steps.

---

## STEP 2: TEST GATE

Run tests. This is mandatory and cannot be skipped.

### Quality checks

Same script CI's "Code Quality" workflow runs — single source of truth. Run it first so
lint/validation failures surface before the long test build.

```bash
./scripts/quality-checks.sh
```

- If it exits 0 → continue
- If it exits non-zero → STOP: "Quality checks failed — fix before releasing." Show the
  failing check. Look for the `❌` lines; `⚠️` lines (clang-format / XML formatting) are
  advisory and do NOT fail the script.

Do not release on a red `main`. Before starting, also confirm CI is green for the commit
being released:

```bash
# EVERY workflow, not just Code Quality. Build compiles with clang on Ubuntu while
# local builds here use g++, so a clang-only diagnostic under -Werror is red on CI
# and green on the release machine - checking one workflow reads as all-clear.
gh run list --limit 20 --json conclusion,status,workflowName,headSha \
  --jq '.[] | select(.conclusion == "failure") | "\(.workflowName)\t\(.headSha[0:9])"'
```

If ANY workflow's latest run is a `failure`, treat it as a STOP — fix it first.
(v0.99.92 shipped with Code Quality red for ~10 hours because this gate did not exist.
v0.99.118 nearly shipped with the Ubuntu `Build` job red for an hour, because the gate
only looked at Code Quality: `Build Status` even prints "tolerated because this is an
integration branch", so main stays red without anything stopping a release.)

### C++ tests
```bash
make test-run
```

- If tests pass → continue
- If tests fail → STOP: "Tests failed — fix before releasing." Show the failure output.

### Shell tests
```bash
make test-shell
```

- If tests pass → continue
- If bats is not installed → STOP: "bats not found — install it before releasing." Show the installation instructions from the make output.
- If tests fail → STOP: "Shell tests failed — fix before releasing." Show the failure output.

### Regenerate bundled installers
```bash
bash scripts/bundle-installer.sh -o scripts/install.sh
bash scripts/bundle-uninstaller.sh -o scripts/uninstall.sh
```

- Run `git diff --name-only` to check if `scripts/install.sh` or `scripts/uninstall.sh` changed
- If either changed, they will be staged and included in the release commit (Step 6)
- This ensures bundled installers are always in sync with their source modules

### Regenerate the XML linter schema

`tools/xml-linter/schema/schema.json` is a committed snapshot of every constant and
widget attribute the runtime can resolve. The pre-commit hook only regenerates it when a
lint actually fails, so a snapshot can drift ahead of or behind the tree without any gate
noticing — it ships stale. A release is the one point where it is worth forcing.

```bash
make regen-xml-schema
make lint-xml
```

- If `make lint-xml` fails → STOP: "XML lint failed against a freshly regenerated schema."
  These are real errors (a `#const` reference with no definition anywhere), not staleness.
  Show the failing lines and fix before releasing.
- If it passes and `git diff --name-only` shows `tools/xml-linter/schema/schema.json`
  changed, stage it with the release commit (Step 6). A regenerated-but-unchanged snapshot
  is the normal, expected outcome.

---

## STEP 3: CHANGELOG (CHECKPOINT 1)

### Gather commits
```bash
git log {last_tag}..HEAD --format="%h %s" --no-merges
```

### Draft changelog entry

Follow `docs/devel/CHANGELOG_STYLE.md` for separator, issue links, voice, and entry
shapes (daily vs milestone). The skeleton is Keep a Changelog:

```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added
- Feature descriptions in user-facing language

### Fixed
- Bug fix descriptions

### Changed
- Changes to existing functionality
```

Rules for writing the changelog:
- **Rewrite** commit messages into user-facing language (not developer shorthand)
- **Merge** related commits into single entries
- **Omit** noise: refactors with no user impact, CI-only changes, typo fixes, test-only changes
- **Categorize** using conventional commit types: `feat:` → Added, `fix:` → Fixed, `refactor:/perf:/build:` → Changed, `docs:` → Changed (only if user-visible)
- Only include categories that have entries (don't add empty sections)
- If this is a significant release (minor or major bump), use the milestone shape from `docs/devel/CHANGELOG_STYLE.md`: intro paragraph naming the release's role, bold sub-group headers inside categories, and a `<!-- whatsnew -->` block

### Show draft to user

Use AskUserQuestion to present the drafted changelog entry:
- Question: "Here's the draft changelog for v{NEW_VERSION}:\n\n{formatted entry}\n\nHow does this look?"
- Header: "Changelog"
- Options: "Looks good", "I'll edit it after commit", "Abort release"

If "Abort release" → STOP.

---

## STEP 4: README & DOCS SCAN

Quick scan for things that might need updating:

1. **Check README.md** for version strings, beta notices, or outdated references
2. **If major version bump** (MAJOR changed): flag that an upgrade guide may be needed
3. **If going from prerelease to stable** (e.g., `1.0.0-beta` → `1.0.0`): flag beta language to remove
4. **If going to `1.0.0` or higher**: check for "beta" or "pre-release" language in README

If there are findings, use AskUserQuestion:
- Question: "Found some docs that might need updating:\n\n{findings}\n\nWant me to update these?"
- Header: "Docs"
- Options: "Update them", "Skip for now", "Abort release"

If nothing to flag, skip this step silently.

---

## STEP 5: APPLY CHANGES

### Update VERSION.txt
Write `NEW_VERSION` (just the version string, with trailing newline) to `VERSION.txt`.

### Update CHANGELOG.md
- Insert the new entry after the header block (after the "adheres to Semantic Versioning" line and blank line)
- Add a comparison link at the bottom of the file following the existing pattern:
  ```
  [X.Y.Z]: https://github.com/prestonbrown/helixscreen/compare/v{PREVIOUS}...v{NEW_VERSION}
  ```
- Update the previous version's link if needed (the `[PREVIOUS]` entry should compare against its predecessor, not HEAD)

### Apply any approved doc changes from Step 4

### Show summary
Run `git diff` and show a brief summary of what changed.

---

## STEP 6: COMMIT + TAG

### Stage files explicitly
Per project conventions — NEVER `git add -A` or `git add .`. Stage only the files actually modified:

```bash
git add VERSION.txt CHANGELOG.md
# Plus, if Step 2's regeneration changed them:
#   scripts/install.sh scripts/uninstall.sh tools/xml-linter/schema/schema.json
# Plus any other files modified in Step 5
```

### Commit
```bash
git commit -m "chore(release): v{NEW_VERSION}"
```

### Create annotated tag
The tag annotation becomes the GitHub release body (CI extracts it). Use the changelog entry as the tag message.

**CRITICAL — preserve the `###` markdown headers.** `git tag`'s default cleanup mode is `strip`, which deletes every line beginning with `#` — that silently removes the `### Added` / `### Fixed` / `### Changed` section headers from the release body (passing `-m "..."` does NOT avoid this; `strip` applies to `-m` and `-F` alike). You MUST write the message to a file and pass `--cleanup=verbatim`:

```bash
# Write the changelog entry (with its ### section headers) to a temp file:
cat > /tmp/helix-tag-msg-v{NEW_VERSION}.txt <<'TAGMSG'
v{NEW_VERSION}

### Added
- ...

### Fixed
- ...

### Changed
- ...
TAGMSG

git tag -a "v{NEW_VERSION}" -F /tmp/helix-tag-msg-v{NEW_VERSION}.txt --cleanup=verbatim
```

(If the shell mangles the heredoc, use the Write tool to create the temp file instead — same `--cleanup=verbatim` flag.)

The tag message should be the changelog entry content (the Added/Fixed/Changed sections, optionally with the prose summary). Format it cleanly — this is what users see on the GitHub release page.

**Verify before continuing** that the headers survived:

```bash
git cat-file tag "v{NEW_VERSION}" | grep -E '^### (Added|Fixed|Changed)'
```

If that prints nothing, the headers were stripped — delete the tag (`git tag -d v{NEW_VERSION}`, safe while unpushed) and recreate it with `--cleanup=verbatim`.

---

## STEP 7: PUSH (CHECKPOINT 2)

### Show release summary

Display a summary box:
```
## Release Summary: v{NEW_VERSION}

Previous: v{LAST_TAG}
Commits:  {count} commits
Files:    VERSION.txt, CHANGELOG.md{, other files}

Changelog preview:
{first 5 lines of changelog entry}

Pre-release: {yes/no} (tags with `-` are marked prerelease on GitHub)
```

### Ask for confirmation

Use AskUserQuestion:
- Question: "Ready to push v{NEW_VERSION} to origin? This will trigger the GitHub Actions release pipeline."
- Header: "Push"
- Options:
  - "Push it" — push commit and tag
  - "Keep local" — don't push, user will push manually later
  - "Undo" — reset the commit and delete the tag

### If "Push it":
```bash
git push origin main
git push origin "v{NEW_VERSION}"
```
Then show: "Pushed! Watch the build: https://github.com/prestonbrown/helixscreen/actions"

For stable releases (tag without `-`), the `notify-website` job will fire a
`repository_dispatch` to helixscreen-website once the release job finishes,
auto-rebuilding helixscreen.org from the new tag's `docs/user/`. No manual
website deploy needed. Prereleases skip this.

### If "Keep local":
Show: "Release commit and tag created locally. Push when ready:\n`git push origin main && git push origin v{NEW_VERSION}`"

### If "Undo":
**Ask for explicit confirmation** before running destructive commands:
```bash
git tag -d "v{NEW_VERSION}"
git reset --soft HEAD~1
```
Show: "Undone. Commit removed (changes preserved as staged), tag deleted."

---

## ERROR HANDLING

| Error | Action |
|-------|--------|
| Not on main | STOP with message, do not offer to switch |
| Dirty working tree | STOP with message, do not offer to stash |
| Behind origin | STOP with message, do not offer to pull |
| Quality checks fail | STOP with the `❌` output — do not release on a red `main` |
| Tests fail | STOP with failure output |
| Version not greater | STOP: "v{NEW_VERSION} is not greater than v{LAST_TAG}" |
| User aborts at any checkpoint | STOP cleanly, no partial state |
| Push fails | Show error, commit+tag remain local for manual retry |
| Tag already exists | STOP: "Tag v{NEW_VERSION} already exists" |
