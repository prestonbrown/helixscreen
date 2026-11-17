# Release Manifest Signing

Every channel's release list, `releases.helixscreen.org/<channel>/manifest.json`, is published
with a minisign signature beside it, `<channel>/manifest.json.minisig` (#1834). The manifest
carries the SHA-256 of every release archive, so once it is signed with a key held offline,
someone who can write to the R2 bucket or edit a GitHub release can no longer swap a binary and
its hash together: the swapped manifest fails its signature on every client that checks.

This branch carries the signing side only. HelixScreen 1.1 does not verify the signature itself;
the app's verifier starts in 1.2, so 1.1 builds keep reading `manifest.json` as before and ignore
the `.minisig` beside it.

Signatures are **prehashed** (`minisign -S -H` semantics, algorithm `ED`: Ed25519 over the
BLAKE2b-512 of the file). Every checker refuses a legacy signature over the raw file. The
trusted comment (`file:manifest.json channel:<channel>`) is covered by the signature's global
signature, so it cannot be moved onto another channel's signature.

## Key files

| File | Role |
|------|------|
| `.github/release-minisign.pub` | The public key. The release workflow verifies every signature against it before upload |
| `scripts/sign-release-manifest.sh` | Signs and checks each channel manifest in CI |
| `.github/workflows/release.yml` (`upload-r2`) | Generates, signs, verifies and uploads each manifest, behind the `release-signing` environment |
| helix-installer `crates/core/src/keys.rs` (`RELEASE_PUBKEY`) | The same key compiled into the easy installer |

The release key is `RWRXImmr5YYfqd5aaV48WN+50k5e83NgKuEtxECC6agLUBkXG7t7AH9S` (key id
`A91F86E5AB692257`). It is permanent once clients compile it in: replacing it means shipping
every client again. The secret half is held offline and reaches CI only as an environment secret.

## Who checks what

| Checker | Refuses | Where |
|---------|---------|-------|
| Release workflow | A signature that does not verify, prehashed, against `.github/release-minisign.pub`; a missing secret; a placeholder `.pub` | `scripts/sign-release-manifest.sh` |
| Easy installer | A release list with no signature, a wrong-key, legacy or failing one, before any hash in it is used | `fetch_signed_release_list()` in helix-installer's release step |
| HelixScreen 1.1's update checker | Nothing: it does not read the signature | |
| `install.sh` | Nothing (out of scope): BusyBox printers have no tool that can check Ed25519 | |

## Setting it up (Preston, once)

The private key never goes through an agent or CI logs. Run these yourself, from the machine that
holds `~/.helixscreen-release-key/helixscreen-release.key`.

### 1. The environment

The `release-signing` environment has Preston as required reviewer and a deployment policy
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

A tag push runs the build as before; `upload-r2` then waits with "Waiting for review:
release-signing needs approval". Approve it in the run's page (or
`gh run view <id> --web`). Nothing reaches R2 until you do.

## Rollout order

A client that requires a signature cannot update from a channel that serves none, so:

1. Signed manifests published on **every** channel (`stable`, `beta`, `dev`): check with
   `curl -fsS https://releases.helixscreen.org/<channel>/manifest.json.minisig`. This needs a
   signed release from each branch that publishes to that channel.
2. Then the easy installer's `RELEASE_PUBKEY` lands ([helix-installer SIGNING.md](https://github.com/prestonbrown/helix-installer/blob/main/docs/SIGNING.md),
   "HelixScreen release lists").
3. HelixScreen builds that verify (1.2 on) can ship once their own channel and any channel their
   users may switch to are signed.

## Testing

Tests use throwaway keys only, never the release key.
`tests/shell/test_release_manifest_signing.bats` covers the signing script, the workflow's signing
step, upload order, secret gating and environment; `tests/shell/test_release_dry_run.bats` covers
the dry-run check that each signature reached R2.

Signing a test manifest by hand:

```bash
minisign -G -W -p test.pub -s test.key
minisign -S -s test.key -m manifest.json -t "file:manifest.json channel:dev"
minisign -V -H -p test.pub -m manifest.json     # -H refuses a legacy signature
```
