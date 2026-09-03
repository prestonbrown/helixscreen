# Branching

Two long-lived branches. Everything else is a short feature branch off `main`.

| Branch | Purpose | `RELEASE_CHANNEL` | A tag here publishes to |
|--------|---------|-------------------|-------------------------|
| `main` | The trunk. All development lands here. | `beta` | beta + dev |
| `release/1.0` | The 1.0 maintenance line. | `stable` | stable |

**There is no development branch.** Work goes to `main` through a feature branch
and a merge, and `main` is what ships to the beta and dev channels.

## Which way fixes flow

**Fix on `main`, cherry-pick down to `release/1.0`** when the 1.0 fleet needs it.
Never the reverse, and never a merge in either direction: the two lines have
diverged by a release and a merge would drag the whole delta across.

`release/1.0` receives only what the 1.0 fleet needs — a crash, a data-loss bug,
a printer that stops being detected. It does not receive features, refactors, or
dependency bumps. A fix that will not cherry-pick cleanly is a fix that needs
writing twice; that is the cost of a maintenance line and it is expected.

## The channel constraint

`RELEASE_CHANNEL` is a file at the repo root, read by `scripts/release-channel.sh`
and consumed by `.github/workflows/release.yml`. It is declared per branch rather
than derived from the tag string, because `helix::version::Version` discards
prerelease suffixes — so `v1.1.0-dev1` and `v1.1.0-dev2` compare EQUAL and the
in-app updater stops offering builds after the first install.

**`main` must never declare `stable`.** It carries versions ahead of the released
line, so a tag there would publish over the stable manifest for every user, and
`release.yml`'s pre-upload downgrade guard cannot refuse it — a higher version is
a *forward* move. Cutting a new maintenance line and flipping the trunk's channel
belong to one change, never two. See `RELEASE_1_0_CHECKLIST.md` § "The atomic
branch cut".

Nothing about moving a branch publishes anything: `release.yml` triggers only on
`push: tags: v*`, and the R2 workflows are `workflow_dispatch` only. A release is
always a deliberate tag. See `RELEASE_PROCESS.md`.

## What CI covers, and what it therefore does not

`build.yml`, `quality.yml` and `lint-xml.yml` run on `main` and on pull requests.
`esp32-build.yml` runs on `main` and on pull requests.

The consequence is worth stating plainly, because it is not obvious and it has
cost real time: **a long-lived branch that is neither `main` nor a PR gets no CI
at all.** Code merged there compiles only on whatever a developer happens to
build, and defects that need a different toolchain — a firmware target, a
32-bit `long`, a platform without `gethostname()` — accumulate invisibly until
the branch reaches `main`. Keep feature branches short and open a PR, or accept
that the first honest build is the merge.

The same principle is why nothing in the tree is compiled by a source list that
no job builds: an unbuilt tree rots silently and needs a bespoke gate to notice
what a build would have caught for free.
