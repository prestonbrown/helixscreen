# Android Play Store — Publishing & Automation

How the CI pipeline ships `helix-screen` to Google Play, and the one-time manual setup required before the automated upload can work.

## Where to pick up — 2026-08-15

**Package-name registration cleared.** `org.helixscreen.app` is verified against the upload
key's fingerprint `41:B4:26:42:44:FF:90:FA:11:BD:37:56:21:10:C2:E2:66:F4:AB:42:FB:49:87:62:75:49:BF:AF:DE:65:A8:AC`.
The ownership-challenge stub at `~/.android-keystore/adi-registration/` has done its job and can
be deleted once Play Console shows the package with `Keys: 1`.

**Deadline that now governs the sequence: 2026-08-31.** New app submissions and app updates must
target **API 36** (Android 16) from that date; extensions to 2026-11-01 can be requested. We are
on `targetSdkVersion 35`, which is accepted only up to 2026-08-30. See "Target API level" below —
it decides whether the first upload goes in as-is or waits for an SDK bump.

**Pre-flight on the artifact — done 2026-08-15, all green.** `helixscreen-android-v0.99.113.aab`
is staged at `~/Downloads/` and was checked directly rather than assumed:

| Check | Result |
|-------|--------|
| Signer identity | `CN=HelixScreen, OU=356C LLC, O=356C LLC` — the real upload key, not the debug key |
| SHA-256 fingerprint | matches the registered fingerprint exactly |
| 16 KB page alignment | all 8 native libs (`libmain`, `libSDL2`, `libc++_shared`, `libturbojpeg` × arm64-v8a/x86_64) report ELF `LOAD align 2**14`. Required for native code since 2025-11-01; NDK r29 gives it by default |
| versionCode | `99113`. Next tag is `0.99.114` → `99114` |

**Then, in order:**

1. **Create the app record** in Play Console: name "HelixScreen", default language English (US), free, type = App.
2. **Complete the declarations** — answers are pre-derived in "Play Console declarations" below so this is transcription, not decision-making.
3. **Upload the listing assets by hand** from `android/fastlane/metadata/android/en-US/` — the workflow does not sync them.
4. **First manual AAB upload** to the internal track. Google requires this one upload by hand before the Publishing API will accept anything.
5. **Accept the Google-generated app signing key** at the Play App Signing prompt. This is a decided question — see the first-manual-upload prerequisite below for the reasoning and for the two-signature consequence it carries.
6. **Register Google's app signing key** as a second key on the same package (fingerprint from Play Console → Test and release → App integrity → App signing). Check whether Google auto-registered it first. Do not try to complete an ownership challenge for it by hand — we do not hold that private key.
7. **Service account** in Google Cloud: enable the Google Play Android Developer API, grant "Release manager" scoped to this app only, download the JSON key, paste it verbatim as the `PLAY_SERVICE_ACCOUNT_JSON` GitHub secret. The next release tag then publishes to the internal track automatically.

Steps 1-3 are unaffected by the target-API question and can be done now.

**Not blocking any of this:** regular releases keep working. Every tag produces signed APKs and an AAB on the GitHub release, and `publish-android` skips cleanly while `PLAY_SERVICE_ACCOUNT_JSON` is unset.

### Target API level

`android/app/build.gradle` sets `compileSdkVersion 35` / `targetSdkVersion 35`. Google's annual
requirement moves to **API 36 on 2026-08-31**, for new submissions *and* for updates to existing
apps. Two consequences:

- Uploading v0.99.113 on or before 2026-08-30 is accepted as-is. After that date the same file is rejected.
- Either way, **every update published after 2026-08-31 needs API 36**, so the bump is required soon regardless of when the first upload happens.

The lower-risk sequence is to get the first manual upload in on 35 — its only job is to enroll
Play App Signing and unblock steps 5-7 — and treat the SDK bump as its own change, so a
first-submission milestone is not coupled to an untested SDK jump. Android 16 enforces
edge-to-edge display for apps targeting API 36, which a fullscreen SDL surface needs testing
against on a real device before it ships.

### Review risk: the app needs hardware a reviewer does not have

HelixScreen is a companion app for a Klipper printer on the same LAN. A Play reviewer opening it
cold sees the first-run wizard and no printer, and there is no login for the "App access" section
to explain that with. `--test` (mock printer) is a CLI flag with no path to it from the Android
launcher. Put the explanation in **App content → App access → Instructions for reviewers**: state
that the app requires a Klipper/Moonraker printer reachable on the local network, that no account
or credential exists, and that the setup wizard appearing with no printers found is correct
behavior rather than a broken build.

### Play Console declarations

Derived from `docs/user/PRIVACY_POLICY.md`. Section references below point back at it.

**Privacy policy URL:** `https://helixscreen.org/legal/privacy/`

**Data safety — top-level answers:**

| Question | Answer | Basis |
|----------|--------|-------|
| Does your app collect or share any of the required user data types? | **Yes** | Telemetry, opt-in (§4) |
| Is all user data encrypted in transit? | **Yes** | HTTPS/TLS to `telemetry.helixscreen.org` (§7.2) |
| Do you provide a way for users to request data deletion? | **Yes** | In-app Settings → Telemetry → Clear All Events, plus privacy@helixscreen.org (§9.2, §9.3) |
| Is any data sold or shared with third parties? | **No** | §12 |

**Data safety — data types.** Every row is *collected, not shared*, and **optional** ("users can
choose whether this data is collected") because telemetry is off by default (§3).

| Category → Type | Purposes | What it is |
|-----------------|----------|------------|
| App info and performance → Crash logs | Analytics | Signal, uptime, backtrace addresses (§4.3) |
| App info and performance → Diagnostics | Analytics | Memory snapshots, frame timing, connection stability (§4.4) |
| App activity → Other actions | Analytics, App functionality | Panel usage, feature adoption, settings changes, print outcomes (§4.2, §4.4) |
| Device or other IDs → Device or other IDs | Analytics | The `device_id`. See the note below |

The `device_id` row is a judgment call worth recording rather than re-deriving. It is an
app-generated random UUID, double-hashed with a local salt that is never transmitted (§6), so it
identifies nothing off-device — but it *is* a persistent per-install identifier, and Play's
definition of this category explicitly names the analogous Firebase installation ID. Declare it.
Over-declaring costs nothing; under-declaring is a policy strike.

Note that §5 rules out the categories that usually cause trouble: no IP addresses, MAC addresses,
hostnames, SSIDs, serial numbers, location, contacts, files, or credentials.

**Remaining questionnaires:**

| Section | Answer |
|---------|--------|
| Target audience | 18 and over. Do **not** claim child appeal — it pulls the app into Families policy for no benefit. §11 states we do not knowingly collect from under-16s |
| Content rating (IARC) | Category "Utility, Productivity, Communication, or Other"; every content question No. Expect Everyone / PEGI 3 |
| Ads | No ads |
| Government app | No |
| Financial features | None |
| Health | No |
| COVID-19 contact tracing / status | No |
| News app | No |
| Data deletion URL | Not needed — deletion is in-app (§9.2); use privacy@helixscreen.org if a URL is demanded |

**No sensitive-permission declaration is required.** `android/app/src/main/AndroidManifest.xml`
declares only `INTERNET`, `ACCESS_NETWORK_STATE`, and `WAKE_LOCK`. None is in Play's
restricted set, so none of the extra permission-declaration forms (all-files access, package
visibility, location, SMS/call log, camera/mic, accessibility, exact alarms) applies. Re-check
this if a permission is ever added — those forms are a review delay, not a checkbox.

The activity is `android:screenOrientation="sensorLandscape"`, so the app is landscape-only and
the store screenshots are landscape to match.

## Status snapshot — 2026-04-23

**Done:**
- In-repo automation (CI builds signed AAB, generates whatsnew from CHANGELOG, publishes to internal track when service-account secret is set).
- Upload keystore generated and backed up at `~/.android-keystore/helixscreen-upload.jks`; four keystore-related GitHub secrets set (`ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`).
- Store assets committed: 8 phone screenshots, 1024×500 feature graphic, 512×512 store icon, title / short / full descriptions. Fastlane metadata at `android/fastlane/metadata/android/en-US/`.
- Privacy policy published at `https://helixscreen.org/legal/privacy/` (platform-neutral; applies to Android as-is).
- Google Play Developer account org verification **cleared** (356C LLC).

**Warning — v0.99.43 AAB is debug-signed.** Secrets were added at 16:39 UTC on 2026-04-23; v0.99.43 built at 23:15 UTC on 2026-04-22, before the secrets were present, so its keystore step fell back to the Android Debug keystore. Play Store will reject it. **First manual upload must use v0.99.44 or later** — those are the first releases signed with the real upload keystore.

**Blocked on first manual upload (user):**
- Create the HelixScreen app record in Play Console.
- Upload listing text + store icon + feature graphic + 8 screenshots via Play Console UI.
- First manual AAB upload to the internal track (Google requires this before the Publishing API accepts uploads). Grab the AAB from whichever GitHub release is most recent at that point — CI already signs it with the upload keystore.
- Enroll in Play App Signing (prompted during the first manual upload).
- Create the Google Cloud service account, enable the Google Play Android Developer API, grant Release Manager scoped to the HelixScreen app, download the JSON key, paste it as the `PLAY_SERVICE_ACCOUNT_JSON` GitHub secret.

**Regular releases are unblocked.** On every release tag:
- `build-android` produces a properly-signed AAB (attached to the GitHub release for download).
- `publish-android` runs but skips cleanly when `PLAY_SERVICE_ACCOUNT_JSON` is unset — it logs a notice and every subsequent step is gated on the secret being present, so the job exits green.
- Nightly CI does not build Android at all, so nothing to worry about there.
- Once the service-account secret is added, the next release tag automatically starts pushing to the Play Store internal track. No workflow change needed.

## Overview

On every release tag (`v*`), `.github/workflows/release.yml` runs:

1. **`build-android`** — builds APKs (`assembleRelease`) and an AAB (`bundleRelease`), signed with the upload keystore held in GitHub secrets. Verifies the signature on the built bytes before anything is uploaded (see "What happens when the keystore is missing"). Generates a Play Store "What's new" text from `CHANGELOG.md` and uploads three artifacts: `release-android` (APKs), `release-android-aab` (AAB), `release-android-whatsnew`.
2. **`publish-android`** — uploads the AAB to the Play Store's **internal** track with status **draft**, using the `r0adkll/upload-google-play@v1` action. Inert when `PLAY_SERVICE_ACCOUNT_JSON` is unset.
3. **`release`** — attaches all artifacts to the GitHub release. It `needs` **both** `build-platforms` and `build-android`, and asserts the three APKs and the AAB are present by name before creating the release. Without the `build-android` dependency the release could be created while Android was still building (90 min budget against the matrix's 82) and, because the artifact globs run with `fail_on_unmatched_files: false`, it would publish with no Android downloads and still report success.

Once the upload lands on the internal track, you promote it to open testing or production from the Play Console UI. The automation stops at `draft` on purpose so every release gets a final human review before it goes live.

## Source of truth

| Path | Holds |
|------|-------|
| `android/fastlane/metadata/android/en-US/title.txt` | App title (≤30 chars) |
| `android/fastlane/metadata/android/en-US/short_description.txt` | Short description (≤80 chars) |
| `android/fastlane/metadata/android/en-US/full_description.txt` | Full description (≤4000 chars) |
| `android/fastlane/metadata/android/en-US/images/icon.png` | Store icon (512×512, symlinked to `docs/store/android/icon-512.png`) |
| `android/fastlane/metadata/android/en-US/images/featureGraphic.png` | Feature graphic (1024×500, symlinked to `docs/store/android/`) |
| `android/fastlane/metadata/android/en-US/images/phoneScreenshots/*.png` | Phone screenshots (symlinked to `docs/store/android/`) |
| `android/fastlane/metadata/android/en-US/changelogs/<versionCode>.txt` | "What's new" per release. **Generated, not maintained** — `scripts/generate-whatsnew.sh` writes it from `CHANGELOG.md` at release time and `release.yml` reads it straight back. Nothing is committed here; the source of truth is the `CHANGELOG.md` section for the version. (One stale file, 9943.txt, was committed under the retired `major*10000` packing and has been removed — it would have read as the naming convention.) |

The listing text (title / descriptions / screenshots) is **not** synced by the workflow — `r0adkll/upload-google-play` only handles the AAB + whatsnew. Updates to those files are version-controlled here so the Play Console listing can be kept in sync by hand, with this tree as the canonical record. If we ever need full metadata sync, switch to `fastlane supply` (see "Future work").

## One-time manual prerequisites

Before the first run of `publish-android` can succeed, the following must be done by hand. Tick each item as it completes.

- [x] **Google Play Developer account** — registered 2026-04-23 as `356C LLC`; org verification cleared same day.
- [x] **Android developer verification / package-name registration** — `org.helixscreen.app` verified 2026-08-15 against the upload key's fingerprint. See "Android developer verification" below for the challenge process that got it there.
- [ ] **Create the app in Play Console** — name "HelixScreen", default language English (US), free, app type = App.
- [ ] **Complete Play Console declarations** — target audience, data safety, content rating (expected: Everyone), ads (none), government/COVID/financial questionnaires. Answers are pre-derived in "Play Console declarations" above.
- [ ] **Reviewer instructions** — App content → App access. The app is useless without a Klipper printer on the LAN and a cold reviewer sees only the setup wizard; say so explicitly.
- [ ] **Upload listing assets manually** — title, short/full description, store icon, feature graphic, and 8 screenshots from `android/fastlane/metadata/android/en-US/`. Use the text in the `.txt` files verbatim so Play Console matches the repo.
- [x] **Privacy policy URL** — `https://helixscreen.org/legal/privacy/` is live. Paste that URL into Play Console → App content → Privacy policy. The Play Console "Data Safety" questionnaire is separate from the privacy policy URL — fill it out based on what Section 4 of the policy describes (opt-in telemetry; no PII; encrypted in transit; users can request deletion of local queue).
- [ ] **First manual AAB upload** — Google requires one manual AAB upload before the Publishing API will accept uploads. **Use v0.99.44 or later** — v0.99.43 and earlier are debug-signed and will be rejected. Download `helixscreen-android-v<VERSION>.aab` from the GitHub release and upload it to the internal track via the Play Console UI. This also triggers the **Play App Signing** enrollment prompt.

  **Decision: accept the Google-generated app signing key.** Google holds the app signing key, our upload keystore stays an upload key only, and a lost or compromised upload key can be reset by Google instead of orphaning the app. The consequence is worth stating plainly because nothing else in this doc did:

  - **`org.helixscreen.app` then has two signatures.** Play-served installs carry Google's app signing key; the APKs we attach to GitHub releases carry our upload key. Android identifies an app by package name *plus* signing certificate, so the two channels are mutually exclusive on a device: a user who installed from GitHub cannot take a Play update, and vice versa, without uninstalling first — which wipes local config (printer list, wizard state, themes).
  - **Both keys must be registered against the package name** for Android developer verification. Multiple signing keys per package are supported, so this is a registration detail, not a conflict — but registering only one of them leaves the other channel's installs unverifiable.
  - **Escape hatch, if the split becomes a real support burden:** the Play Developer API's `generatedapks.download` returns the Play-signed universal APK for a given versionCode. Publishing *that* to GitHub releases instead of our own `assembleRelease` output would converge both channels on Google's key and make installs interchangeable. Not worth the extra API plumbing until someone actually hits the problem.
- [ ] **Generate a service account for the API** — in Google Cloud Console (linked to the Play Developer account):
  1. Create a new service account named e.g. `helixscreen-ci`.
  2. Enable the "Google Play Android Developer API" for the project.
  3. Create a JSON key for the service account and download it.
- [ ] **Grant the service account access to the app** — Play Console → Users and permissions → Invite new users → add the service account email → grant **"Release manager"** role scoped to the HelixScreen app (production + testing tracks). Do **not** grant account-level admin.
- [x] **Generate the upload keystore** — done 2026-04-23. Backed up at `~/.android-keystore/helixscreen-upload.jks`. Passwords stored in password manager.
- [x] **Add the four keystore GitHub secrets** — `ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD` all set 2026-04-23 (verified via `gh secret list`).
- [ ] **Add the `PLAY_SERVICE_ACCOUNT_JSON` secret** — paste the full JSON contents (plain text, not base64) once the service account is created. Blocked on the Play Console app existing.

After all prerequisites are ticked off, the next release tag will trigger an automated upload to the internal track. Until then, **regular releases continue normally** — the `build-android` job produces properly-signed AABs (thanks to the keystore secrets being in place), and the `publish-android` job detects the missing service-account secret and skips every step via its guarded `if:` conditions. No build fails; the AAB is just not pushed to Google until we're ready.

### What happens when the keystore is missing

This used to say the missing-keystore case "fails noisily in `publish-android` instead of silently shipping a broken release." That was never true. `publish-android` runs *after* `release`, so the GitHub artifacts are already published by the time it could object; it is also inert without `PLAY_SERVICE_ACCOUNT_JSON`, so it objected to nothing. What actually happened is that v0.99.43 shipped a debug-signed AAB and nobody found out until someone tried to upload it.

A release build now refuses to produce an artifact it cannot sign properly, at three points:

1. **`release.yml` → "Materialize upload keystore"** exits 1 when `ANDROID_KEYSTORE_BASE64` is unset. A release tag has no business producing unsigned artifacts, and failing here names the missing secret.
2. **Gradle** fails any release-signing task (`assembleRelease`, `bundleRelease`, `packageRelease`, …) when no keystore is configured, listing the four env vars it needs. The check hangs off `gradle.taskGraph.whenReady` rather than firing at configuration time — configuration is shared with the debug variant, and `.github/workflows/build.yml` runs `assembleDebug` on every PR, which must keep working. `assembleDebug` and `lintRelease` are deliberately unaffected.
3. **`release.yml` → "Verify release artifacts are signed with the upload key"** runs after `bundleRelease` and before anything is renamed or uploaded. It reads the signature actually on the bytes — `apksigner verify --print-certs` for the APKs, `keytool -printcert -jarfile` for the AAB, since apksigner cannot read a bundle — and fails on the debug keystore's `CN=Android Debug`.

   **The two tools are not interchangeable, in either direction.** `minSdk` is 28, so apksigner signs the APKs with v2/v3 only and skips the v1 JAR signature entirely; `keytool -printcert -jarfile` on one of our APKs answers `Not a signed jar file`. The AAB goes the other way — AGP jar-signs bundles, so keytool reads it fine and apksigner cannot. Worse, keytool prints that refusal and **exits 0**, so a check that merely looks for the absence of `CN=Android Debug` would pass on an artifact it never actually read. The step therefore requires *positive* evidence: a SHA-256 fingerprint with hex after it must be present, or the artifact fails. Absence of the debug DN is not proof of a good signature. It also prints each artifact's SHA-256 signer fingerprint, which is what Play Console and Android developer verification want for package-name registration; read it off any release run rather than recomputing it from the keystore.

**Local escape hatch:** `./gradlew assembleRelease -PallowDebugSigning` builds a debug-signed release variant for a local smoke test. It exists so a developer without the keystore can still build; anything produced with it must never leave the machine.

## Android developer verification

Google is phasing in a requirement that the developer behind a package name be a verified identity. Where we stand:

- **Status: cleared 2026-08-15.** The upload key is registered against `org.helixscreen.app`. Google's app signing key still is not — it does not exist until Play App Signing enrollment completes on the first manual upload. The rest of this section is the record of how the challenge worked, kept because the same flow applies when registering that second key turns out not to be automatic.
- **Registration requires an ownership challenge, even though the app has never been on Play.** The docs describe a lighter path for "new" package names, but `org.helixscreen.app` has real installs from sideloaded GitHub-release APKs, so Play Console treats it as existing: adding the fingerprint leaves the package in **Draft** with `Keys: 0` until an APK signed by that key is uploaded. Draft is not registered.

  The challenge does **not** want our real app — Google's instruction is to use an empty project matching the package name, so the upload is a ~2 KB stub, not the 117 MB release APK:

  1. Play Console → Android developer verification → the package → **Verify** on the fingerprint row, and copy the snippet it shows (a bare ~26-character token, no `key=` prefix).
  2. Build a throwaway Gradle project with `applicationId "org.helixscreen.app"`, no code, and the snippet in `app/src/main/assets/adi-registration.properties`.
  3. Build `assembleRelease` unsigned, then sign by hand so the keystore password never enters a file or env var:
     `apksigner sign --ks ~/.android-keystore/helixscreen-upload.jks --ks-key-alias helixscreen-upload --out signed.apk app-release-unsigned.apk`
  4. Confirm before uploading: `apksigner verify --print-certs signed.apk` must report the registered fingerprint. Add `--min-sdk-version 21` if you want to see the v1/v2 rows — at the APK's own `minSdk 28` apksigner reports them `false` because it does not evaluate them in that range, which looks like a signing failure and is not one.
  5. Upload it in the Verify flow.

  Keep the stub out of the repo. It is account-specific, single-use, and never distributed — Google only reads its signature.
- **Register both signing keys.** With Play App Signing accepted (see above), Play-served installs carry Google's app signing key and GitHub-release APKs carry our upload key. Multiple keys per package name are supported — register both, or one channel's installs are unverifiable. The two fingerprints come from different places, and only one of them is ours to print:

  | Key | SHA-256 fingerprint from |
  |-----|--------------------------|
  | Our upload key (GitHub-release APKs) | The "Verify release artifacts are signed with the upload key" step on any release run |
  | Google's app signing key (Play-served installs) | Play Console → Test and release → App integrity → App signing. Does not exist until Play App Signing enrollment completes, so it cannot be registered before the first manual upload |
- **Enforcement starts 2026-09-30**, and only for **Brazil, Indonesia, Singapore, and Thailand**, and only on *participating* app stores. Our sideload channel (direct APK download from a GitHub release) is not affected by that wave. Global enforcement is **2027**.
- **Register early anyway.** It costs nothing now, and it parks the package name — `org.helixscreen.app` is not reserved by anything today.

## Promoting a release

The automation stops at **internal / draft**. Promote manually:

1. Play Console → Release → Testing → **Internal testing** — click the draft release, review, and roll it out to testers (or discard and let the next tag upload a fresh one).
2. Once smoke-tested: **Promote release** → **Open testing** → submit for review (~1–3 days for review on a new app; faster on updates).
3. Once open testing is stable: **Promote release** → **Production**.

The internal track only needs an email list (add yourself + any trusted testers) and does not require review. Use it as the first sanity check after every release.

## Changing automation behavior

To change the target track or status, edit the `publish-android` job in `.github/workflows/release.yml`:

| Setting | Value now | Alternatives |
|---------|-----------|--------------|
| `track` | `internal` | `alpha`, `beta`, `production` |
| `status` | `draft` | `inProgress`, `halted`, `completed` |
| `changesNotSentForReview` | `true` | `false` — sends the release for review automatically; can't be used on `production` without supplying release notes |

## Local dry runs

Regenerate the whatsnew file locally to inspect what CI will upload:

```bash
scripts/generate-whatsnew.sh /tmp/whatsnew-preview.txt
cat /tmp/whatsnew-preview.txt
wc -m /tmp/whatsnew-preview.txt   # must be ≤500 chars for Play Store
```

The script reads the section for `VERSION.txt`'s current version from `CHANGELOG.md`, strips markdown, and truncates on a sentence boundary with an ellipsis if it would exceed 500 characters.

To test the full upload path without touching production, point the action at a sandbox app: create a second Play Console app (e.g. `org.helixscreen.app.dev`), invite the same service account, and override `packageName` in a temporary workflow branch. Not currently set up — only add this if we need to debug the uploader itself.

## Gotchas

- **First upload must be manual.** The Publishing API refuses the very first upload for any new app. This is a Google constraint, not a workflow limitation.
- **versionCode must strictly increase**, and there is exactly one definition of it: **`scripts/android-version-code.sh`**. It packs `VERSION.txt` as `major*1000000 + minor*1000 + patch`, so `0.99.113` → `99113` and `1.0.0` → `1000000`. Re-releasing the same `VERSION.txt` to the Play Store will be rejected. The lanes are 1000 wide because the field below must never overflow into the one above: under the earlier `major*10000 + minor*100 + patch` packing, `0.99.113` produced `10013` and `1.0.0` produced `10000`, so 1.0 would have been rejected as a downgrade both by the Play Store and by every sideloaded install.

  Three consumers call that script — `android/app/build.gradle` (the versionCode itself), `scripts/generate-whatsnew.sh` (names `changelogs/<code>.txt`), `.github/workflows/release.yml` (reads that same file back). They used to each write the arithmetic out, and they diverged the moment the lanes were widened: release.yml kept the old packing, so it looked for changelogs/10014.txt while the script had written changelogs/99114.txt. The `if [ -f "$SRC" ]` fell through to a `::warning::`, `if-no-files-found: ignore` skipped the upload, and the whatsnew artifact silently did not exist — invisible only because `publish-android` is inert without the service-account secret. `tests/shell/test_android_version_code.bats` gates against a second copy reappearing. Query the current value with `cd android && ./gradlew -q printVersionCode` or `scripts/android-version-code.sh`.
- **Pre-release tags (`v1.0.0-beta`) still upload.** The `publish-android` job does not check for pre-release tags. If we later want to gate pre-releases out of the Play Store, add an `if: !contains(github.ref_name, '-')` at the job level.
- **Store icon vs launcher icon.** `android/app/src/main/res/mipmap-*/ic_launcher.png` (max 192×192) is what users see on their home screen. Play Console separately requires a **512×512** store icon for the listing, generated from `assets/images/helix-icon.png` onto the launcher's #2D2D2B background and committed at `docs/store/android/icon-512.png`. Upload it manually the first time; after that it persists on the listing until you change it.
- **Screenshot dimensions.** Current screenshots in `docs/store/android/` are 960×540. Play Store accepts anything ≥320 on each side, so these pass — but re-shooting at 1920×1080 would look crisper on large-screen previews. Not blocking.
- **Secret hygiene.** `PLAY_SERVICE_ACCOUNT_JSON` is the key to pushing arbitrary code to production. Rotate if exposed. The service account should hold only "Release manager" scoped to this app — not account-level admin.

## Future work

- **Full metadata sync via `fastlane supply`** — would let the workflow also update the store listing text/screenshots on every release. Adds a fastlane install step in CI. Skipped for now since listing copy changes rarely.
- **Pre-release gating** — skip `publish-android` on tags containing `-` (e.g., `v1.0.0-beta`).
- **Per-platform release notes** — currently the same whatsnew is used for Android and all other platforms. Splitting is low-value until we have Android-specific changes that don't apply elsewhere.
- **Automatic promotion internal → open testing** — possible via a scheduled workflow that checks internal crash rates. Premature until we have tester volume.

## Related docs

- `docs/user/PRIVACY_POLICY.md` — privacy policy text (to be published at helixscreen.org/privacy)
- `scripts/generate-upload-keystore.sh` — one-time upload keystore generation
- `scripts/generate-whatsnew.sh` — per-release whatsnew extraction from CHANGELOG.md
