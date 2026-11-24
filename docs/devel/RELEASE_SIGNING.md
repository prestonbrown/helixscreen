# Release Manifest Signing

Every channel's release list, `releases.helixscreen.org/<channel>/manifest.json`, is published
with a minisign signature beside it, `<channel>/manifest.json.minisig` (#1834). The manifest
carries the SHA-256 of every release archive, so once it is signed with a key held offline,
someone who can write to the R2 bucket or edit a GitHub release can no longer swap a binary and
its hash together: the swapped manifest fails its signature on every client that checks.

Signatures are **prehashed** (`minisign -S -H` semantics, algorithm `ED`: Ed25519 over the
BLAKE2b-512 of the file). Every checker refuses a legacy signature over the raw file. The
trusted comment (`file:manifest.json channel:<channel>`) is covered by the signature's global
signature, and the update checker requires it to name the channel it fetched, so a manifest
signed for one channel is refused on another.

## Two keys

| Key | Signs | Trusted by clients on | Environment | Reviewer |
|-----|-------|-----------------------|-------------|----------|
| **Release key** | Every release that publishes to stable or beta, a beta's dev manifest included | stable, beta, dev | `release-signing` (`v*` tags) | Preston approves each run |
| **Dev key** | A release that publishes to dev alone: an alpha tag (`v*-alpha.*`) from a branch declaring `RELEASE_CHANNEL=dev` | dev only | `release-signing-dev` (`v*-alpha.*` tags) | none |

Alphas and, later, nightly builds publish unattended because the key that signs them can do
nothing outside the dev channel: a stable or beta manifest signed by the dev key is refused as
"signed with a key this channel does not trust". A beta, which publishes to beta and dev, still
signs both manifests with the release key, so the dev channel accepts either key.

**Which key a release uses** is decided once, by the release job's channel step
(`scripts/release-channel.sh`, its `signing` output), before any job that holds a key runs:
`dev` when the channels are exactly `dev`, the tag is an alpha, and
`.github/release-minisign-dev.pub` holds a key; `main` otherwise. `upload-r2` takes its
environment from that output and hands the signing step only that environment's secret.
`recover-r2.yml` resolves it the same way in its guard job.

**The `.pub` file is the switch.** While `.github/release-minisign-dev.pub` is the placeholder,
every release signs with the release key and needs approval, as before the dev key existed.
Clients must trust the dev key before anything is signed with it, so the key is committed in
two steps (§ "Setting it up", step 5).

## Key files

| File | Role |
|------|------|
| `.github/release-minisign.pub` | The release public key. The release workflow verifies every release-key signature against it before upload |
| `.github/release-minisign-dev.pub` | The dev public key, or a placeholder while there is none. Release-key signing is unaffected by it; a real key here switches alpha releases to the dev key |
| `include/system/release_signature.h` | `kReleasePublicKey` and `kDevPublicKey`, the keys compiled into the app; tests pin each equal to its `.pub` once that holds a key |
| `src/system/release_signature.cpp` | The app's minisign verifier (Monocypher's BLAKE2b and Ed25519) and `verify_channel_manifest()`, the per-channel key and trusted-comment rule |
| `lib/monocypher/` | Monocypher 4.0.2, vendored unmodified (CC0 / BSD-2) |
| `scripts/release-channel.sh` | Resolves a release's channels and its signing key (`signing=main\|dev`) |
| `scripts/sign-release-manifest.sh` | Signs and checks each channel manifest in CI; `--key dev` signs only the dev channel's manifest |
| `.github/workflows/release.yml` (`upload-r2`) | Generates, signs, verifies and uploads each manifest, behind `release-signing` or `release-signing-dev` |
| `.github/workflows/recover-r2.yml` | Republishes a tag's R2 upload, signing its regenerated manifests the same way, in the same environment; dispatched from the tag (`--ref vX.Y.Z`), since both environments admit only tags. A dispatch runs the tag's own copy of the workflow, so a tag whose recover-r2 predates signing would republish an unsigned manifest beside the old signature. If that tag's own `release.yml` already signs (v1.1.0-rc.3 and later on 1.1), re-run its Release `upload-r2` job instead. A tag whose `release.yml` predates signing too (v1.0.9 and earlier, 1.1 betas before rc.3) can't be republished signed by any workflow: recover by cutting a new tag |
| helix-installer `crates/core/src/keys.rs` (`RELEASE_PUBKEY`) | The release key compiled into the easy installer. It reads the dev channel too (an installed prerelease, a pinned channel), so it needs the dev key, trusted on dev only, before alphas sign with it |

The release key is `RWRXImmr5YYfqd5aaV48WN+50k5e83NgKuEtxECC6agLUBkXG7t7AH9S` (key id
`A91F86E5AB692257`). It is permanent once clients compile it in: replacing it means shipping
every client again. The secret half is held offline and reaches CI only as an environment secret.
The dev key is equally permanent once compiled in; its secret half reaches CI only as a
`release-signing-dev` secret.

## Who checks what

| Checker | Refuses | Where |
|---------|---------|-------|
| Release workflow | A signature that does not verify, prehashed, against the chosen key's `.pub`; a missing secret; a placeholder `.pub`; the dev key on any manifest but dev's | `scripts/sign-release-manifest.sh` |
| Easy installer | A release list with no signature, a wrong-key, legacy or failing one, before any hash in it is used for `helix_install` or the K-Touch zip | `fetch_signed_release_list()` in helix-installer's release step |
| HelixScreen's update checker | The same, before the manifest is parsed, plus the dev key on stable or beta and a trusted comment naming another channel; a release that did not come from a verified manifest is never downloaded | `src/system/update_checker.cpp#fetch_verified_manifest`, `#start_download` |
| `install.sh` | Nothing (out of scope) | see below |

### The update checker

`UpdateChecker::fetch_verified_manifest()` fetches `manifest.json`, then `manifest.json.minisig`,
and hands both, with the channel it fetched, to `helix::parse_signed_release_manifest()`, which
parses the manifest only after `release_signature::verify_channel_manifest()` accepts the
signature: by the release key on any channel or the dev key on dev, with a trusted comment
carrying `channel:<that channel>`. Only then is `ReleaseInfo::signature_verified`
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
  the same rule that already lets it redirect where updates come from. A key named there
  replaces both compiled-in keys on every channel, and the signature's trusted comment must
  still name the channel (`-t "file:manifest.json channel:dev"`).

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

The private keys never go through an agent or CI logs. Run these yourself, from the machine
that holds `~/.helixscreen-release-key/`.

### 1. The release environment (done)

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
`verify-release-assets`, `publish-android` and `notify-website` run. An alpha signed with the
dev key does not wait: it runs in `release-signing-dev` straight through.

### 5. The dev key

**a. Generate it**, offline, on the machine that holds the release key, with a password:

```bash
minisign -G -p ~/.helixscreen-release-key/helixscreen-dev.pub \
            -s ~/.helixscreen-release-key/helixscreen-dev.key \
            -c "HelixScreen dev-channel key"
chmod 600 ~/.helixscreen-release-key/helixscreen-dev.key
```

Keep the secret beside the release key and back it up the same way. Never reuse the release key's
password.

**b. The environment.** No reviewers, and a deployment policy admitting only alpha tags:

```bash
R=prestonbrown/helixscreen
gh api -X PUT repos/$R/environments/release-signing-dev \
  -F "deployment_branch_policy[protected_branches]=false" \
  -F "deployment_branch_policy[custom_branch_policies]=true"
gh api -X POST repos/$R/environments/release-signing-dev/deployment-branch-policies \
  -f name='v*-alpha.*' -f type=tag
gh api repos/$R/environments/release-signing-dev/deployment-branch-policies   # shows one: v*-alpha.*
```

**c. The secrets**, on that environment only (never repository level, never `release-signing`):

```bash
gh secret set HELIX_DEV_MINISIGN_KEY --env release-signing-dev -R $R \
  < ~/.helixscreen-release-key/helixscreen-dev.key
gh secret set HELIX_DEV_MINISIGN_PASSWORD --env release-signing-dev -R $R   # prompts
```

**d. Teach the clients the key, signing with the release key still.** Put the key line
(`sed -n 2p ~/.helixscreen-release-key/helixscreen-dev.pub`) into `kDevPublicKey` in
`include/system/release_signature.h`, and into helix-installer's dev-channel key, and leave
`.github/release-minisign-dev.pub` as the placeholder. Ship an alpha from that commit: it is
signed with the release key and approved as usual, and every dev-channel client that installs
it trusts the dev key from then on.

**e. Switch alphas to the dev key.** Once that alpha is out (and the installer release carrying
the key), replace `.github/release-minisign-dev.pub` with the `.pub` file. From the next alpha
on, CI signs dev-only releases with the dev key in `release-signing-dev`. A dev client older
than step d refuses those manifests ("signed with a key this channel does not trust") until
reinstalled; that is why d comes first.

**Revoking the dev key** (leaked, or no longer wanted): restore the placeholder
`.github/release-minisign-dev.pub` and delete the environment's secrets, so alphas sign with the
release key again; then ship a release with `kDevPublicKey` emptied. Clients that have not
updated keep trusting the old key on the dev channel only.

## What a leaked dev key can and cannot do

The dev key is held where no reviewer stands between it and a published manifest, so it is
assumed easier to steal than the release key.

| An attacker holding the dev key | Can | Cannot |
|----|----|----|
| and nothing else | Sign any manifest the dev channel will accept | Publish it: R2 write access is a separate secret |
| and R2 write access | Make dev-channel installs (and dev-channel `dev_url` servers) install any archive whose hash the manifest names, i.e. run code on every dev-channel printer that updates | Get a stable or beta client to accept anything: those verify against the release key only, and a dev-key signature there is refused whatever its trusted comment says. Replay a release-signed beta or dev manifest onto stable: the trusted comment names its own channel |
| through the workflow (push an alpha tag) | Run `upload-r2` without review, publishing a dev manifest signed with the dev key | Sign with the release key: its secrets live only in `release-signing`, which still needs approval. Publish a stable or beta manifest that verifies: the dev key signs only the dev channel's manifest, and clients would refuse it elsewhere anyway |

So the blast radius of the dev key is the dev channel, which is already opted into unfinished
builds. Two things narrow who can push an alpha tag at all: a tag ruleset restricting creation of
`v*` tags to maintainers, and keeping the environment's policy at `v*-alpha.*`. The workflow's
key choice is not itself the security boundary (anyone who can push a tag controls the workflow
file at that tag); the boundaries are the environments' secret scoping and reviewer, and the
clients' per-channel key rule.

## Nightly builds (proposed, not built)

A nightly is a dev-only release, so it needs no new key or environment if its tag is an alpha
build: `vX.Y.Z-alpha.N.YYYYMMDD`, where `N` is the last alpha. That matches `v*-alpha.*`, resolves
to `signing=dev`, and under semver precedence (which `helix::version` follows) orders after
`alpha.N`, after the previous night's, and before `alpha.N+1`, so the dev channel only moves
forward. A nightly workflow would push that tag from a scheduled job on main and let
`release.yml` do the rest.

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
| `tests/unit/test_release_signature.cpp` (`[release_signature]`) | Good prehashed signature, tampered manifest, wrong key, legacy signature, edited trusted comment, malformed input; the key matrix (release and dev key, by stable, beta and dev, valid and tampered, with keys made in the test); a manifest signed for one channel refused on another; no dev key; compiled-in keys equal their `.pub` files |
| `tests/unit/test_update_checker_signature.cpp` (`[update_checker][signature]`) | Unsigned, badly signed and other-channel manifests refused, a good one parsed as verified, GitHub releases never installable, and a full check against a loopback server |
| `tests/shell/test_release_manifest_signing.bats` | The release and recovery workflows' signing step, upload order, secret gating, and the environment and key for each run mode and signing key; the script's dev-key rules |
| `tests/shell/test_release_channel.bats` | Which releases resolve to the dev key |

Signing a test manifest by hand:

```bash
minisign -G -W -p test.pub -s test.key
minisign -S -s test.key -m manifest.json -t "file:manifest.json channel:dev"
minisign -V -H -p test.pub -m manifest.json     # -H refuses a legacy signature
```
