---
description: Create a release — bump version, changelog, tag, push. Automates the full release workflow.
argument-hint: "[patch | minor | major | X.Y.Z | X.Y.Z-beta | X.Y.Z-rc.N]"
allowed-tools: ["Bash", "Read", "Glob", "Grep", "Edit", "Write", "AskUserQuestion", "WebFetch"]
---

# RELEASE WORKFLOW

You are automating the HelixScreen release process. Follow each step in order. Hard stop on failures — do not proceed with warnings or partial state.

**A release is cut from the newest commit whose CI is fully green, never from a frozen
tip.** The branch keeps moving while you release: the version bump and changelog are
committed on a short-lived branch made from that green commit (`RELEASE_SHA`), the tag goes
on that commit, and the branch is merged back. Nobody has to stop landing work, and a red
tip cannot delay the release by hours.

## CONTEXT (gather first)

Collect all of these before starting:

```bash
git rev-parse --abbrev-ref HEAD          # "main", or the release/X.Y line this version belongs to
git fetch origin --tags                   # origin/$BRANCH and every tag, as GitHub has them
RELEASE_SHA=$(scripts/latest-green-sha.sh --branch "$BRANCH")   # newest fully green commit (STEP 0)
git show "$RELEASE_SHA:VERSION.txt"       # Current version, as the release commit sees it
git describe --tags --abbrev=0 "$RELEASE_SHA"  # Last tag reachable from it (e.g., v0.9.3)
git log --oneline "$(git describe --tags --abbrev=0 "$RELEASE_SHA")..$RELEASE_SHA"  # Commits this release ships
git log --oneline "$RELEASE_SHA..origin/$BRANCH"  # Commits newer than it: they ship next time
```

Every `HEAD` and `..HEAD` further down means `$RELEASE_SHA` until STEP 6 makes the
release commit, and every command from STEP 2 on runs in the release worktree (STEP 0).

Today's date: use `date +%Y-%m-%d` output.

---

## STEP 0: PRE-FLIGHT CHECKS

All of these must pass. If ANY fails, STOP and tell the user why.

| Check | How | Fail action |
|-------|-----|-------------|
| Branch is releasable | `git rev-parse --abbrev-ref HEAD` is `main` or `release/X.Y`. Store it as `BRANCH`. | STOP: "{branch} is neither main nor a release/X.Y line" |
| Tags fetched | `git fetch origin --tags` | Just do it silently |
| A green commit exists | `RELEASE_SHA=$(scripts/latest-green-sha.sh --branch "$BRANCH")` exits 0 | STOP: show its stderr, which names each recent push and why it is not green |
| Release worktree | `scripts/setup-worktree.sh --base "$RELEASE_SHA" release-prep/v{NEW_VERSION}` once STEP 1 has the version, then work in `.worktrees/v{NEW_VERSION}` holding `worktree:v{NEW_VERSION}` | STOP on any setup failure |

**Why a green SHA, and what counts as green.** `scripts/latest-green-sha.sh` takes the
commits CI ran on for the last 30 pushes to `$BRANCH`, newest first, and prints the first
one still on `origin/$BRANCH` whose push-triggered runs have all completed as `success` or
`skipped`, with `Build` and `Code Quality` (the two that run on every push) among them as
`success`. A run still in progress, failed or cancelled disqualifies the commit. When the
newest push has only cancelled runs, it exits 3 and prints `gh run rerun <id>` for each:
rerun them and resolve again once they finish (`--walk-past-cancelled` takes an older
commit instead); path-filtered workflows such as `XML Lint` count when they ran. `--require` changes
the required set, `--depth` the window. The release then never waits on the tip: it ships
what CI has already proven, and whatever landed since rides the next release.

Show the user `RELEASE_SHA`, its subject, and the `RELEASE_SHA..origin/$BRANCH` list, so
it is clear what this release does NOT contain. If that list holds something the user
expects in the release, STOP and say which commit it is waiting on: releasing a newer
commit means waiting for its CI, never skipping the check.

The release worktree is a fresh checkout of `RELEASE_SHA`, so the main tree's state
(peers' uncommitted work, a red tip, a merge in progress) is irrelevant to the release,
and nothing in this workflow edits the main tree until STEP 7's merge back.

**Which branch may release which version.** A patch on a shipped line lives on that
line's branch, never on `main`, because `main` carries the next minor's development.
Tagging `1.0.1` from a `main` reading `1.1.0-beta.1` would publish 1.1 work as a 1.0
patch, and tags only move forward, so there is no taking it back. `main` may release any
version; a `release/X.Y` branch may release only `X.Y.*`. STEP 1 enforces the pairing
once the version is resolved.

Every `main` hardcoded below is really `$BRANCH`: the green-SHA query, the merge back,
and the push. A release-line branch also lags `main`'s own tooling, so check before
reaching for it - `scripts/version-compare.sh`, `scripts/latest-green-sha.sh` and
`make land-gate` may not exist there. The equivalents are a careful manual semver
comparison, `latest-green-sha.sh --branch release/X.Y` run from a `main` checkout (it only
reads git and gh), and `make test-all` plus CI's UI slice (STEP 2).

---

## STEP 1: DETERMINE VERSION

**Argument:** `$ARGUMENTS` (the user's argument to `/release`)

### If argument is a bump type (`patch`, `minor`, `major`):
- Parse last tag (strip `v` prefix) into MAJOR.MINOR.PATCH **plus any prerelease suffix**
- **A prerelease on the last tag means that triple has not shipped.** `patch` finishes
  the prerelease line and resolves to the triple itself, not to the next patch. Treating
  the suffix as noise ships the patch of a release that never existed and strands the
  triple unpublished forever, because tags only move forward.
- Otherwise bump the named component and reset the lower components to 0
- Examples:
  - last tag `v0.9.3` + `patch` = `0.9.4`
  - last tag `v1.1.0-beta.1` + `patch` = `1.1.0`  (NOT `1.1.1`)
  - last tag `v1.1.0-beta.1` + `minor` = `1.2.0`
  - last tag `v1.1.0-beta.1` + `major` = `2.0.0`

### If argument is an explicit version (e.g., `1.0.0`, `1.0.0-beta`, `1.0.0-rc.1`):
- Strip leading `v` if present
- Validate: must match pattern `MAJOR.MINOR.PATCH[-PRERELEASE]`
- PRERELEASE can be: `alpha`, `beta`, `rc.N`

### If no argument:
- Compare `VERSION.txt` content at `RELEASE_SHA` with last tag (strip `v` prefix)
- If VERSION.txt is ALREADY different (higher) than last tag → use VERSION.txt value
- If VERSION.txt matches last tag → use AskUserQuestion:
  - Question: "No version specified and VERSION.txt matches the last tag (vX.Y.Z). What version should this release be?"
  - Header: "Version"
  - Options: "Patch (X.Y.Z+1)", "Minor (X.Y+1.0)", "Major (X+1.0.0)"
  - (user can also type a custom version via Other)

### Validation (all cases):
- Must be valid semver: `MAJOR.MINOR.PATCH` with optional `-PRERELEASE`
- Must rank strictly above the last tag. Do not eyeball this, and do not reach for
  `sort -V`: GNU version sort ranks `1.1.0` BELOW `1.1.0-beta.1`, the opposite of
  semver. Ask the comparator:
  ```bash
  scripts/version-compare.sh "$NEW_VERSION" "$(git describe --tags --abbrev=0 | sed 's/^v//')"
  ```
  `1` is the only answer that may proceed. `0` or `-1` is a STOP. A non-zero exit
  means it refused to rank the pair rather than guessing — also a STOP, never read
  that as agreement.
- The version must belong to `$BRANCH`. On a `release/X.Y` branch, `NEW_VERSION` must
  start with `X.Y.`; on `main` anything valid passes:
  ```bash
  case "$BRANCH" in
    release/*) case "$NEW_VERSION" in
                 "${BRANCH#release/}".*) ;;
                 *) echo "STOP: $NEW_VERSION does not belong to $BRANCH"; exit 1 ;;
               esac ;;
  esac
  ```
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

CI being green on `RELEASE_SHA` is already established: STEP 0 resolved it that way, so
there is no separate "is CI red" check here and no reason to look at the branch tip.

### The landing gate

```bash
HELIX_GATE_FULL=1 make land-gate
```

Run it in the release worktree. `HELIX_GATE_FULL=1` turns off the gate's test selection
(a landing runs only what its change can reach), so a release gate runs every step and
every test, as it does on a `release/*` or `release-prep/*` branch without it. It is the local mirror of CI: the unit sweep, the sweep
again in CI's 8-shard layout, the bats suite, CI's `tests/ui` pytest slice (headless) and
the moonraker plugin tests, and the generated-artefact checks: the theme token table
(`check-theme-tokens`), the XML linter schema against a regeneration (`check-xml-schema`),
`lint-xml`, and the crash worker's LVGL event-code table. It prints a PASS/FAIL table with
durations; logs are in `build/land-gate/`.

- All rows PASS → continue
- A `xml-schema` FAIL alone means the committed snapshot drifted from its inputs, which is
  release housekeeping, not a code fault: run `make regen-xml-schema`, then `make lint-xml`.
  If the lint passes, the regenerated `tools/xml-linter/schema/schema.json` goes into the
  release commit (STEP 6). If it fails, those are real errors (a `#const` reference with no
  definition anywhere): STOP and fix on `$BRANCH` first.
- Any other FAIL → STOP: "Land gate failed — fix before releasing." Show the table and the
  failing step's log tail. A failure here on a commit CI passed is a local-environment or
  flake question worth settling before tagging, not one to wave through. A fix lands on
  `$BRANCH` the normal way; then re-resolve `RELEASE_SHA` once its CI is green.
- A missing `.venv` is `make venv-setup`, not a reason to skip the UI step.

On a release line without `make land-gate`, run `make test-all` and
`.venv/bin/python -m pytest tests/ui/ -v --ignore=tests/ui/test_screens.py -m "not slow"`
instead, and `make regen-xml-schema && make lint-xml` for the schema.

### Release-only builds

Per-push CI links splash and watchdog for x86 only; the Release workflow cross-compiles
every platform with `HELIX_PACKAGING=1`. A break that only those builds see (a MIPS32
link needing libatomic, a packaged binary still carrying mocks or the ctl server) shows
up first on the tag otherwise.

```bash
make release-gate TIERS=heavy                              # unified MIPS, the default
make release-gate TIERS=heavy RELEASE_GATE_TARGETS="mips pi32 k2"   # more targets
```

It runs the packaged docker cross build, `make release-<target>`, and the splash,
watchdog and mock-symbol checks, under a `heavy:release-gate` claim, and prints a
PASS/FAIL table. Any FAIL row → STOP: "Release gate failed for {target}." Show the
table and the tail of the failing step's log (`build/release-gate/<step>.log`).

---

## STEP 3: CHANGELOG (CHECKPOINT 1)

### Gather commits
```bash
git log {last_tag}..$RELEASE_SHA --format="%h %s" --no-merges
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
- If this is a significant release (minor or major bump), use the milestone shape from `docs/devel/CHANGELOG_STYLE.md`: intro paragraph naming the release's role, bold sub-group headers inside categories. Never a `<!-- whatsnew -->` block: the Play Store text lives in `android/whatsnew/` (STEP 5)

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

### Write the Play Store "What's new" (stable releases only)
For a version with no `-` suffix, write `android/whatsnew/{NEW_VERSION}.txt`: plain text,
at most 500 characters, what changed for a Play Store user, distilled from the entry
(`android/whatsnew/README.md`). Show it to Preston with the changelog draft. Check it:
`scripts/generate-whatsnew.sh /tmp/whatsnew-preview.txt` must exit 0. The release
workflow fails a stable tag without it. Prereleases skip this.

### Apply any approved doc changes from Step 4

### Show summary
Run `git diff` and show a brief summary of what changed.

### Release gate, fast tier

```bash
make release-gate TIERS=fast
```

Checks that `VERSION.txt` has a `## [X.Y.Z] - date` heading and a `[X.Y.Z]:` compare
link in `CHANGELOG.md`, that the tag does not exist yet, that the Play Store text
generates (a stable version needs `android/whatsnew/{NEW_VERSION}.txt`), and that the
installer bundles build. Any FAIL row → STOP and fix it before committing.

---

## STEP 6: COMMIT + TAG

### Commit
In the release worktree, on `release-prep/v{NEW_VERSION}` (its parent is `RELEASE_SHA`):

```bash
git commit -m "chore(release): v{NEW_VERSION}" -- VERSION.txt CHANGELOG.md
# A stable release also commits android/whatsnew/{NEW_VERSION}.txt (new file:
# `git add -N` it first). Append tools/xml-linter/schema/schema.json if Step 2's
# regeneration changed it, and any other file modified in Step 5.
git rev-parse HEAD^    # must print RELEASE_SHA: the release is exactly what CI proved, plus this commit
```

The tag goes on this commit. Everything it contains beyond `RELEASE_SHA` is the version,
the changelog, the Play Store text and generated snapshots, so the green CI verdict on `RELEASE_SHA` covers it.

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
First merge the release commit back into `$BRANCH` and publish it, then push the tag.
`$BRANCH` has usually moved past `RELEASE_SHA`, so this is a real merge; the only files it
can conflict on are `VERSION.txt` and `CHANGELOG.md`, and a conflict there is a peer who
edited them, which means STOP and ask them, not resolve it for them.

`$BRANCH` is updated by pushing the release branch from the release worktree straight to
`refs/heads/$BRANCH`, so the same steps serve `main` and a `release/X.Y` line whatever the
main tree has checked out. The tag is pushed only after `origin/$BRANCH` is verified to
hold the release commit, so a rejected push can never leave a tag on an unpublished commit.

Run the whole block as ONE script, from the release worktree (`bash -c '...'` or a file in
the scratchpad): the claim is owned by that script's pid for exactly as long as it runs,
and every step stops the script on failure.

```bash
set -u
BRANCH=...; VER=v{NEW_VERSION}; PREP=release-prep/$VER; MAIN_TREE=<main tree path>
stop() { echo "STOP: $*" >&2; exit 1; }

# 1. Local $BRANCH must hold nothing origin lacks. Peers' unpushed commits there would be
#    stranded behind the published release, and are not ours to push: STOP and ask whose.
git fetch origin || stop "fetch failed"
if git -C "$MAIN_TREE" rev-parse -q --verify "refs/heads/$BRANCH" >/dev/null; then
    ahead=$(git -C "$MAIN_TREE" log --oneline "origin/$BRANCH..$BRANCH")
    [ -z "$ahead" ] || stop "local $BRANCH has unpushed commits; ask their owners first:
$ahead"
fi

# 2. Claim $BRANCH for this script's life; never pipe take, test its own status.
scripts/helix-claim take "worktree:$BRANCH" "release $VER: merge back" --pid $$ ||
    stop "worktree:$BRANCH is held; ask its holder"
trap 'scripts/helix-claim release-if-owned-by $$ "worktree:$BRANCH"' EXIT

# 3. Merge origin/$BRANCH in and push, until the push is accepted. A rejection means
#    $BRANCH moved again: fetch, merge, push once more.
for attempt in 1 2 3 4 5; do
    git -c merge.autoStash=false merge --no-edit "origin/$BRANCH" ||
        stop "merge of origin/$BRANCH conflicts: a peer edited VERSION.txt or CHANGELOG.md"
    git push origin "$PREP:refs/heads/$BRANCH" && break
    [ "$attempt" -lt 5 ] || stop "push to $BRANCH rejected 5 times; $BRANCH is too busy, retry later"
    git fetch origin || stop "fetch failed"
done

# 4. Verify, then tag. Only now is the release commit public.
git fetch origin || stop "fetch failed"
[ "$(git rev-parse "origin/$BRANCH")" = "$(git rev-parse "$PREP")" ] ||
    stop "origin/$BRANCH is not the release branch tip; do NOT push the tag"
git merge-base --is-ancestor "$VER" "origin/$BRANCH" || stop "$VER is not on origin/$BRANCH"
git push origin "$VER" || stop "tag push failed; origin/$BRANCH is published, push $VER by hand"

# 5. Bring a checked-out local $BRANCH up to date (a plain fast-forward: step 1 proved it
#    holds nothing of its own), then tear down.
if [ "$(git -C "$MAIN_TREE" rev-parse --abbrev-ref HEAD)" = "$BRANCH" ]; then
    git -C "$MAIN_TREE" -c merge.autoStash=false merge --ff-only "origin/$BRANCH" ||
        echo "note: local $BRANCH did not fast-forward (a peer committed meanwhile); it merges on their next pull" >&2
fi
```

If the merge in step 3 brought in commits whose CI is not yet green, run `HELIX_GATE_FULL=1 make land-gate`
in the release worktree before pushing (step 3 then becomes merge, gate, push); skip it
when every one of them already passed CI on `$BRANCH`. After the script succeeds:
`scripts/teardown-worktree.sh "v{NEW_VERSION}"`.

The tag push is what starts the Release workflow, and the tagged commit is the release
commit, now reachable from `$BRANCH` through the merge. Then show: "Pushed! Watch the
build: https://github.com/prestonbrown/helixscreen/actions"

For stable releases (tag without `-`), the `notify-website` job will fire a
`repository_dispatch` to helixscreen-website once the release job finishes,
auto-rebuilding helixscreen.org from the new tag's `docs/user/`. No manual
website deploy needed. Prereleases skip this.

**Stable releases upload to Google Play on their own.** `publish-android` sends the AAB to
the production track as a full rollout, pending Google's review. When the Release run is
green, check that job's log says `Finished uploading to the Play Store` and tell Preston it
is in review. Only if that job failed: rerun it (`gh run rerun <run> --job <job>`; the AAB
artifact is kept), and fall back to `docs/devel/ANDROID_PLAY_STORE.md` "Uploading a stable
release by hand" only when the rerun cannot succeed. Prereleases skip Play: betas never go
there.

### If "Keep local":
Show: "Release commit and tag created locally on release-prep/v{NEW_VERSION}. When ready,
run STEP 7's merge-back script from the release worktree: it merges origin/$BRANCH in,
pushes the release branch to $BRANCH, verifies it, and only then pushes the tag."

### If "Undo":
**Ask for explicit confirmation** before running destructive commands:
```bash
git tag -d "v{NEW_VERSION}"
scripts/teardown-worktree.sh "v{NEW_VERSION}" --force --force-branch
```
Nothing outside the release worktree changed before STEP 7's merge, so removing the
worktree and its branch is the whole undo. Show: "Undone. Tag deleted, release branch and
worktree removed; $BRANCH was never touched."

---

## ERROR HANDLING

| Error | Action |
|-------|--------|
| Branch neither `main` nor `release/X.Y` | STOP with message, do not offer to switch |
| Version does not match the release line | STOP: "v{NEW_VERSION} does not belong to {BRANCH}" |
| No green commit in the window | STOP with `latest-green-sha.sh`'s stderr: each recent push and why it is not green |
| Quality checks fail | STOP with the `❌` output — do not release on a red branch |
| Land gate fails | STOP with its PASS/FAIL table and the failing step's log tail |
| Merge back conflicts | STOP: a peer edited VERSION.txt or CHANGELOG.md; ask them, do not resolve it for them |
| Push to `$BRANCH` rejected | `$BRANCH` moved again: the merge-back loop fetches, merges and pushes again; after 5 tries, STOP and retry later. The tag is never pushed before `origin/$BRANCH` holds the release commit |
| Local `$BRANCH` has unpushed commits | STOP before claiming: they are a peer's, and pushing past them strands them. Ask whose they are |
| Release gate fails | STOP with the PASS/FAIL table and the failing step's log tail |
| Version not greater | STOP: "v{NEW_VERSION} is not greater than v{LAST_TAG}" |
| User aborts at any checkpoint | STOP cleanly, no partial state |
| Push fails | Show error, commit+tag remain local for manual retry |
| Tag already exists | STOP: "Tag v{NEW_VERSION} already exists" |
