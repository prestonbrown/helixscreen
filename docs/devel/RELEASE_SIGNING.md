# Release Manifest Signing

Every channel's release list, `releases.helixscreen.org/<channel>/manifest.json`, is published
with a minisign signature beside it, `<channel>/manifest.json.minisig` (#1834). The manifest
carries the SHA-256 of every release archive, so once it is signed with a key held offline,
someone who can write to the R2 bucket or edit a GitHub release can no longer swap a binary and
its hash together: the swapped manifest fails its signature on every client that checks.

Signatures are **prehashed** (`minisign -S -H` semantics, algorithm `ED`: Ed25519 over the
BLAKE2b-512 of the file). Every checker refuses a legacy signature over the raw file. The
trusted comment (`file:manifest.json channel:<channel>`) is covered by the signature's global
signature, so it cannot be moved onto another channel's signature.

## Key files

| File | Role |
|------|------|
| `.github/release-minisign.pub` | The public key. The release workflow verifies every signature against it before upload |
| `include/system/release_signature.h` | `kReleasePublicKey`, the same key compiled into the app; a test pins the two equal |
| `src/system/release_signature.cpp` | The app's minisign verifier (Monocypher's BLAKE2b and Ed25519) |
| `lib/monocypher/` | Monocypher 4.0.2, vendored unmodified (CC0 / BSD-2) |
| `scripts/sign-release-manifest.sh` | Signs and checks each channel manifest in CI |
| `.github/workflows/release.yml` (`upload-r2`) | Generates, signs, verifies and uploads each manifest, behind the `release-signing` environment |
| `.github/workflows/recover-r2.yml` | Republishes a tag's R2 upload, signing its regenerated manifests the same way; dispatched from the tag (`--ref vX.Y.Z`), since the environment admits only tags. A dispatch runs the tag's own copy of the workflow, so a tag whose recover-r2 predates signing would republish an unsigned manifest beside the old signature. If that tag's own `release.yml` already signs (v1.1.0-rc.3 and later on 1.1), re-run its Release `upload-r2` job instead. A tag whose `release.yml` predates signing too (v1.0.9 and earlier, 1.1 betas before rc.3) can't be republished signed by any workflow: recover by cutting a new tag |
| helix-installer `crates/core/src/keys.rs` (`RELEASE_PUBKEY`) | The same key compiled into the easy installer |

The release key is `RWRXImmr5YYfqd5aaV48WN+50k5e83NgKuEtxECC6agLUBkXG7t7AH9S` (key id
`A91F86E5AB692257`). It is permanent once clients compile it in: replacing it means shipping
every client again. The secret half is held offline and reaches CI only as an environment secret.

## Who checks what

| Checker | Refuses | Where |
|---------|---------|-------|
| Release workflow | A signature that does not verify, prehashed, against `.github/release-minisign.pub`; a missing secret; a placeholder `.pub` | `scripts/sign-release-manifest.sh` |
| Easy installer | A release list with no signature, a wrong-key, legacy or failing one, before any hash in it is used for `helix_install` or the K-Touch zip | `fetch_signed_release_list()` in helix-installer's release step |
| HelixScreen's update checker | The same, before the manifest is parsed; a release that did not come from a verified manifest is never downloaded | `src/system/update_checker.cpp#fetch_verified_manifest`, `#start_download` |
| `install.sh` | Nothing (out of scope) | see below |

### The update checker

`UpdateChecker::fetch_verified_manifest()` fetches `manifest.json`, then `manifest.json.minisig`,
and hands both to `helix::parse_signed_release_manifest()`, which parses the manifest only after
`release_signature::verify()` accepts the signature. Only then is `ReleaseInfo::signature_verified`
set. `start_download()` refuses any release for which `helix::release_is_verified()` is false
(unsigned, or no SHA-256 to check the download against), and `do_download()` refuses a download
with no hash rather than skipping the check.

- **A refused signature is an error, not a fallback.** Stable and beta fall back to the GitHub
  API only when R2 is unreachable. A manifest that is served but fails its signature is reported
  ("manifest signature refused: ..."), because the GitHub answer carries no signature and would
  only hide the failure.
- **A GitHub-fallback release announces but never installs.** It has no signature and no hash;
  Install reports "Error: Update can't be verified".
- **The dev channel and `dev_url`** verify exactly the same way. A test server signing with its
  own key names that key in the root-owned update_urls.json
  (`docs/devel/UPDATE_SYSTEM.md` § "R2 Base URL Override"):

  ```json
  { "dev_url": "http://192.168.1.100:8080/dev/", "release_pubkey": "RW..." }
  ```

  The file is trusted only when root or the app's user owns it and nothing else can write it,
  the same rule that already lets it redirect where updates come from.

### The ESP32 firmware

`release_signature.cpp` and Monocypher are in the firmware's source list
(`firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`) and compile under its
`-fno-exceptions` flags, but the firmware has no on-device updater yet (updates arrive over USB or
the web flasher), so nothing references the verifier and `--gc-sections` drops it from the image.
An OTA path calls `helix::release_signature::verify()` on the manifest it fetched, as the update
checker does.

### `install.sh`

Out of scope. BusyBox printers have no tool that can check Ed25519, and a release-bundled installer
run by hand checks only GitHub's published digest. The in-app update path is covered because the
app verifies the manifest before it downloads the archive and hands it to `install.sh --local`.
Options for later: a tiny static verifier per platform shipped beside `install.sh`.

## Setting it up (Preston, once)

The private key never goes through an agent or CI logs. Run these yourself, from the machine that
holds `~/.helixscreen-release-key/helixscreen-release.key`.

### 1. The environment (done)

The `release-signing` environment exists with you as required reviewer and a deployment policy
admitting only `v*` tags (tag pushes are how releases run):

```bash
R=prestonbrown/helixscreen
gh api -X PUT repos/$R/environments/release-signing \
  -F "reviewers[][type]=User" -F "reviewers[][id]=$(gh api user --jq .id)" \
  -F "deployment_branch_policy[protected_branches]=false" \
  -F "deployment_branch_policy[custom_branch_policies]=true"
gh api -X POST repos/$R/environments/release-signing/deployment-branch-policies \
  -f name='v*' -f type=tag
```

`gh api repos/$R/environments/release-signing` shows it. A dry run (`workflow_dispatch`) runs
`upload-r2` with no environment and never waits for approval.

### 2. The secrets

Environment secrets are readable only by a job a reviewer has approved:

```bash
R=prestonbrown/helixscreen
gh secret set HELIX_RELEASE_MINISIGN_KEY --env release-signing -R $R \
  < ~/.helixscreen-release-key/helixscreen-release.key
gh secret set HELIX_RELEASE_MINISIGN_PASSWORD --env release-signing -R $R   # prompts; skip if the key has no password
```

If an earlier setup put either secret at repository level, delete it so only the environment
holds the key: `gh secret delete HELIX_RELEASE_MINISIGN_KEY -R $R` (and the password).

Until the key secret is set, every release fails at "Generate and sign channel manifests" with
nothing uploaded: `sign-release-manifest.sh --mode publish` refuses to run without it.

### 3. Every branch that tags

`.github/release-minisign.pub` must hold the real key on every branch a release is tagged from
(`main`, `release/1.1`, `release/1.0`), or that branch's releases fail closed at the placeholder
check. Each branch also needs `sign-release-manifest.sh` and the `upload-r2` signing step.

### 4. Approving a release

A tag push runs the build, and "Create Release" leaves the GitHub release as a draft.
`upload-r2` then waits with "Waiting for review: release-signing needs approval". Approve it in
the run's page (or `gh run view <id> --web`). Nothing is public until you do: after `upload-r2`
succeeds, `publish-release` publishes the draft and posts to Discord, and only then do
`verify-release-assets`, `publish-android` and `notify-website` run.

## Rollout order

A client that requires a signature cannot update from a channel that serves none, so:

1. Signed manifests published on **every** channel (`stable`, `beta`, `dev`): check with
   `curl -fsS https://releases.helixscreen.org/<channel>/manifest.json.minisig`. This needs a
   signed release from each branch that publishes to that channel.
2. Then the easy installer's `RELEASE_PUBKEY` lands ([helix-installer SIGNING.md](https://github.com/prestonbrown/helix-installer/blob/main/docs/SIGNING.md),
   "HelixScreen release lists").
3. HelixScreen builds that verify (1.2 on) can ship once their own channel and any channel their
   users may switch to are signed. A user switching a verifying build to a channel that serves no
   signature sees "manifest is not signed" until that channel publishes a signed release.

## Testing

Tests use throwaway keys only (`tests/fixtures/release_signing/README.md`), never the release key.

| Test | Covers |
|------|--------|
| `tests/unit/test_release_signature.cpp` (`[release_signature]`) | Good prehashed signature, tampered manifest, wrong key, legacy signature, edited trusted comment, malformed input, compiled-in key equals `.github/release-minisign.pub` |
| `tests/unit/test_update_checker_signature.cpp` (`[update_checker][signature]`) | Unsigned and badly signed manifests refused, a good one parsed as verified, GitHub releases never installable, and a full check against a loopback server |
| `tests/shell/test_release_manifest_signing.bats` | The release and recovery workflows' signing step, upload order, secret gating and environment |

Signing a test manifest by hand:

```bash
minisign -G -W -p test.pub -s test.key
minisign -S -s test.key -m manifest.json -t "file:manifest.json channel:dev"
minisign -V -H -p test.pub -m manifest.json     # -H refuses a legacy signature
```
