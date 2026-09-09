# HelixScreen Release Process

This document describes how to create and publish releases of HelixScreen.

---

## Table of Contents

- [Version Scheme](#version-scheme)
- [Automated Release Pipeline](#automated-release-pipeline)
- [Creating a Release](#creating-a-release)
- [Release Checklist](#release-checklist)
- [Hotfix Releases](#hotfix-releases)
- [Pre-release Versions](#pre-release-versions)

---

## Version Scheme

HelixScreen uses [Semantic Versioning](https://semver.org/):

```
MAJOR.MINOR.PATCH[-PRERELEASE]
```

| Component | When to Increment |
|-----------|-------------------|
| **MAJOR** | Breaking changes (config format, API, incompatible UI changes) |
| **MINOR** | New features, backwards-compatible |
| **PATCH** | Bug fixes, documentation, minor improvements |
| **PRERELEASE** | Trunk builds heading for the next MINOR: `-beta.N` |

> ### A prerelease sorts below its release, and the whole scheme rests on that
>
> `helix::version::Version` implements Semantic Versioning 2.0.0 precedence
> (`src/util/version.cpp`), so `1.1.0-beta.1 < 1.1.0-beta.2 < 1.1.0-rc.1 < 1.1.0`.
> That ordering is what lets the trunk ship `1.1.0-beta.N` while `release/1.0`
> still ships `1.0.x`: a beta outranks every 1.0 hotfix and is still superseded by
> the real `1.1.0` when it publishes.
>
> The same question gets asked in three languages, so three implementations exist:
> the app (`src/util/version.cpp`), CI's pre-upload channel guard
> (`scripts/version-compare.sh`), and the Android versionCode
> (`scripts/android-version-code.sh`). They share one set of cases in
> `tests/fixtures/version_precedence.txt`, and adding a version there fails every
> suite that disagrees with it. Change one comparator and you change all three.
>
> `sort -V` is not one of them and must not be used: GNU version sort orders
> `1.1.0` **before** `1.1.0-beta.1`, the opposite of semver.

### Suffixes the tooling accepts

`-alpha.N`, `-beta.N`, `-rc.N`, with N from 1 to 29. The Android versionCode packs
the suffix into a 100-wide lane and errors on anything it does not recognise rather
than guess an ordinal, so a novel suffix fails the release build instead of
shipping an unpublishable APK. Build metadata (`+sha`) does not affect precedence
and is not used.

### Examples

- `v1.0.0` - First stable release
- `v1.1.0-beta.1` - First beta of the 1.1 line, from `main`
- `v1.1.0-rc.1` - Release candidate, same line, higher precedence than any beta
- `v1.1.0` - The 1.1 stable release, from `release/1.1`
- `v1.1.1` - Bug fix on the 1.1 line
- `v2.0.0` - Breaking changes

### The trunk's beta line

`main` carries `RELEASE_CHANNEL=beta`, so its tags reach the beta and dev channels
while `release/1.0` keeps stable:

| Branch | Versions | Channel |
|--------|----------|---------|
| `release/1.0` | `1.0.0`, `1.0.1`, `1.0.2` ... | stable |
| `main` | `1.1.0-beta.1`, `1.1.0-beta.2` ... | beta + dev |
| `release/1.1` (future) | `1.1.0`, then `1.1.1` ... | stable |

The trunk names the release it is heading for, so a beta install says which minor
it belongs to and `1.1.0` stays free for the stable cut. A beta at `1.1.0-beta.7`
is offered `1.1.0` the moment it publishes, and a `1.0.2` hotfix on the stable line
never looks like an upgrade to it.

When `release/1.1` is cut, `main` moves to `1.2.0-beta.1` and the pattern repeats.

---

## Automated Release Pipeline

The release process is fully automated via GitHub Actions (`.github/workflows/release.yml`).

### Trigger

Pushing a tag matching `v*` triggers the release workflow:

```bash
git tag v1.2.0
git push origin v1.2.0
```

### Pipeline Stages

```text
┌─────────────────────────────────────────────────────────────┐
│                     Tag Push (v1.2.0)                       │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                         Build Matrix                        │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
  validate-shell gate → build-platforms matrix (9):
    pi · pi32 · ad5m · cc1 · k1 · ad5x · k2 · x86 · snapmaker-u1
  plus build-android → publish-android
    `release` needs build-android too, not just build-platforms — otherwise it
    could publish before Android finished and ship with no APKs, silently.
    build-android FAILS the whole release if ANDROID_KEYSTORE_BASE64 is unset;
    it no longer falls back to the debug keystore. See ANDROID_PLAY_STORE.md.

  Each platform job: Docker cross-toolchain → target arch → package .zip
                               │
                               ▼
                     ┌──────────────────┐
                     │     release      │
                     │                  │
                     │ • Download all   │
                     │   artifacts      │
                     │ • Extract version│
                     │ • Generate notes │
                     │ • Create release │
                     └──────────────────┘
```

### Build Artifacts

| Platform | Artifact Name | Contents |
|----------|---------------|----------|
| Raspberry Pi (64-bit) | `helixscreen-pi.zip` | aarch64 binary, assets, configs |
| Raspberry Pi (32-bit) | `helixscreen-pi32.zip` | armhf binary, assets, configs |
| AD5M | `helixscreen-ad5m.zip` | armv7l binary (static), assets, configs |
| CC1 | `helixscreen-cc1.zip` | ARM binary, assets, configs |
| K1 | `helixscreen-k1.zip` | MIPS32 binary (static, musl), assets, configs |
| AD5X | `helixscreen-ad5x.zip` | MIPS binary (ZMOD), assets, configs |
| K2 | `helixscreen-k2.zip` | ARM binary (static, musl), assets, configs |
| x86 | `helixscreen-x86.zip` | x86 binary, assets, configs |
| Snapmaker U1 | `helixscreen-snapmaker-u1.zip` | ARM binary, assets, configs |
| Android | Play Store bundle | via `build-android` / `publish-android` |

> **Bridge release note:** Starting with v1.0.0 (the version you're currently preparing), the primary release asset is `helixscreen-{platform}.zip` (unversioned filename). The legacy `helixscreen-{platform}-v{version}.tar.gz` is still published during this bridge release for backwards compatibility with older installed versions; it will be removed in the following release.

---

## Creating a Release

### Step 1: Prepare the Release

1. **Ensure main branch is stable:**
   ```bash
   git checkout main
   git pull origin main
   make test-run  # Run tests
   ```

2. **Bump `VERSION.txt` and add the CHANGELOG entry, in one `chore(release):` commit.**
   `VERSION.txt` is the source of truth for the built binary (`Makefile:195`,
   `mk/cross.mk`) - the version does **not** come from the git tag. Tagging
   without bumping the file ships a binary that reports the previous version.
   - `echo "X.Y.Z" > VERSION.txt`
   - Add the release section to `CHANGELOG.md` above the previous one
   - `git commit -m "chore(release): vX.Y.Z" CHANGELOG.md VERSION.txt`
   - Also check `CLAUDE.md` / `README.md` for any hardcoded version strings

3. **Test on actual hardware:**
   - MainsailOS / Raspberry Pi
   - AD5M with ForgeX (if applicable)
   - Run through verification checklist in `docs/user/TESTING_INSTALLATION.md`

### Step 2: Create the Tag

**With release notes (recommended):**

```bash
# Create annotated tag with release notes
git tag -a v1.2.0 -m "$(cat <<'EOF'
## What's New

### Features
- Added input shaper visualization
- Improved AMS panel responsiveness

### Bug Fixes
- Fixed crash when Moonraker disconnects during print
- Fixed touch calibration on rotated displays

### Other
- Updated documentation
- Performance improvements
EOF
)"

# Push the tag
git push origin v1.2.0
```

**Without release notes:**

```bash
git tag v1.2.0
git push origin v1.2.0
```

The workflow auto-generates basic release notes if no annotation is provided.

### Step 3: Monitor the Build

1. Go to **Actions** tab on GitHub
2. Watch the "Release" workflow
3. Build takes ~15-20 minutes typically

### Step 4: Verify the Release

1. Go to **Releases** on GitHub
2. Check both platform archives are attached
3. Verify checksums in release notes
4. Test installation:
   ```bash
   curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
   ```

---

## Release Checklist

### Before Tagging

- [ ] All tests pass (`make test-run`)
- [ ] No critical bugs in issue tracker
- [ ] Documentation updated for new features
- [ ] Tested on real hardware (Pi and/or AD5M)

### After Release

- [ ] Release workflow completed successfully
- [ ] Both platform artifacts attached
- [ ] Release notes accurate
- [ ] Installation tested via curl|sh
- [ ] Update any external references (Discord, documentation sites)

---

## Hotfix Releases

For urgent bug fixes:

1. **Create hotfix branch** from the release tag:
   ```bash
   git checkout -b hotfix/v1.2.1 v1.2.0
   ```

2. **Apply minimal fix** - only the necessary changes

3. **Test thoroughly** - verify the fix, check for regressions

4. **Merge to main** (via PR if time permits):
   ```bash
   git checkout main
   git merge hotfix/v1.2.1
   ```

5. **Tag and release:**
   ```bash
   git tag -a v1.2.1 -m "Fix: [description of fix]"
   git push origin v1.2.1
   ```

---

## Pre-release Versions

For testing new features before stable release:

### Creating a Pre-release

Tag the branch whose `RELEASE_CHANNEL` already routes where you want it to go. The
suffix expresses precedence; the branch chooses the audience:

```bash
git tag -a v1.1.0-beta.2 -m "Beta: new AMS features"
git push origin v1.1.0-beta.2
```

### Pre-release Behavior

The GitHub prerelease flag and the R2 upload channels both come from the
**`RELEASE_CHANNEL` file at the repo root**, on the branch being tagged — not from
the tag string. See `docs/devel/UPDATE_SYSTEM.md` § "How CI Determines Upload
Channels" for the full table and the reason.

- `RELEASE_CHANNEL=stable` -> full GitHub release, shown as "latest"
- `RELEASE_CHANNEL=beta` or `dev` -> marked **prerelease**, not shown as "latest"
- Users must explicitly choose to install a prerelease:
  ```bash
  curl -sSL .../install.sh | sh -s -- --version v1.1.0-beta.2
  ```

**The suffix is not the routing.** A `-beta.N` suffix says where the version sits
in the ordering, not who receives it: the dev channel cannot be spelled in a
version string at all, and a plain `v1.1.0` tag pushed from the trunk would reach
the stable fleet if the tag decided. `RELEASE_CHANNEL` keeps that decision a
property of the branch, which is why `scripts/release-channel.sh` rejects a
suffixed tag on a `stable` branch: a prerelease has no business on the stable
channel regardless of which branch it was cut from.

### Graduating the beta line

A stable release is not a re-tag of the last beta. Cut the maintenance branch,
edit its `RELEASE_CHANNEL` to `stable`, drop the suffix from `VERSION.txt`, and tag
there:

```bash
git checkout -b release/1.1 main
$EDITOR RELEASE_CHANNEL          # beta -> stable
printf '1.1.0\n' > VERSION.txt   # was 1.1.0-beta.N
git commit -m "chore(release): v1.1.0" RELEASE_CHANNEL VERSION.txt CHANGELOG.md
git tag -a v1.1.0 -m "1.1.0"
git push origin release/1.1 v1.1.0
```

Dropping the suffix is itself the promotion: `1.1.0` outranks every
`1.1.0-beta.N`, so the beta fleet is offered it as an ordinary forward step.
`main` then moves to `1.2.0-beta.1` and keeps publishing to beta.

---

## Manual Release (Emergency)

If GitHub Actions fails, you can build and release manually:

### Build Locally

```bash
# Build for Pi
make PLATFORM_TARGET=pi clean release-pi

# Build for AD5M
make PLATFORM_TARGET=ad5m clean release-ad5m
```

### Create Release Manually

1. Go to GitHub **Releases** → **Draft a new release**
2. Choose the tag
3. Write release notes
4. Upload the `.zip` files from `releases/` (plus the legacy `.tar.gz` files if the bridge release still ships them)
5. Publish

---

## Changelog Generation

`CHANGELOG.md` is the changelog. It is written by hand in the release commit (Step 1);
nothing generates it. The annotated tag's message becomes the GitHub release body
(Step 2), and `scripts/generate-whatsnew.sh` distills the finished CHANGELOG section
into the Play Store "What's New" - both consume what was already written.

Prose style - user-facing voice, separator, issue-link form, and the daily vs milestone
entry shapes - is specified in [CHANGELOG_STYLE.md](CHANGELOG_STYLE.md). Read it before
drafting the release's section.

---

## Troubleshooting

### Build Fails

1. Check Actions logs for the specific error
2. Common issues:
   - Submodule not checked out
   - Docker cache issues (try re-running)
   - Toolchain image problems

### Wrong Version Released

1. **Delete the release** on GitHub
2. **Delete the tag:**
   ```bash
   git tag -d v1.2.0
   git push origin :refs/tags/v1.2.0
   ```
3. Fix the issue
4. Re-tag and push

### Missing Artifact

If one platform's build fails:

1. Fix the issue
2. Delete the partial release
3. Delete and re-push the tag to trigger fresh build

---

*Related: [CI/CD Guide](CI_CD_GUIDE.md) | [Testing Installation](../user/TESTING_INSTALLATION.md)*
