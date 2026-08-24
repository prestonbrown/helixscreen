# Google Play Store Submission Checklist

**Prerequisites:** All code fixes and store assets are committed on main.

> **Correction (2026-08-15).** Section 3 below names a GitHub secret
> `ANDROID_KEYSTORE_PATH` holding the base64-encoded keystore. No such secret
> exists and none should be created. The real secret is
> **`ANDROID_KEYSTORE_BASE64`**; `ANDROID_KEYSTORE_PATH` is a plain env var that
> `.github/workflows/release.yml`'s "Materialize upload keystore" step sets to
> wherever it decoded that secret on disk, and `android/app/build.gradle` reads
> it from the environment. The four secrets actually in use are
> `ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`,
> `ANDROID_KEY_PASSWORD`. See `docs/devel/ANDROID_PLAY_STORE.md` for current
> truth — this file is a point-in-time plan and is not maintained.
>
> Also settled since this was written: **accept Google's app signing key** at
> the Play App Signing prompt during the first manual upload (section 4/9). The
> two-signature consequence for GitHub-release APKs is written up in
> `ANDROID_PLAY_STORE.md`.

---

## 1. Developer Account

- [ ] Go to https://play.google.com/console
- [ ] Sign in with Google account for 356C LLC
- [ ] Pay $25 one-time registration fee
- [ ] Complete identity verification (select Organization, enter 356C LLC details)
- [ ] Wait for verification (1-2 business days for orgs)

## 2. Privacy Policy

- [ ] Publish `docs/user/PRIVACY_POLICY.md` to https://helixscreen.org/privacy
- [ ] Verify the URL is live and accessible

## 3. Generate Upload Keystore

- [ ] Run `scripts/generate-upload-keystore.sh`
- [ ] Save passwords securely (password manager)
- [ ] Add GitHub Actions secrets:
  - `ANDROID_KEYSTORE_BASE64` — base64-encode the .jks: `base64 -i ~/.android-keystore/helixscreen-upload.jks | pbcopy`
    (this said `ANDROID_KEYSTORE_PATH`; see the correction at the top of this file)
  - `ANDROID_KEYSTORE_PASSWORD`
  - `ANDROID_KEY_ALIAS` — `helixscreen-upload`
  - `ANDROID_KEY_PASSWORD`

## 4. Create App in Play Console

- [ ] Click "Create app"
- [ ] App name: **HelixScreen**
- [ ] Default language: English (United States)
- [ ] App type: App (not Game)
- [ ] Free
- [ ] Accept Developer Program Policies and US export laws declarations

## 5. Store Listing

- [ ] **Short description** (80 char max):
  > Klipper touchscreen UI — monitor and control your 3D printer from any Android device.

- [ ] **Full description:**
  > HelixScreen is a full-featured touch interface for Klipper 3D printers. Connect to your printer over your local network and get the same powerful UI that runs on dedicated touchscreens — right on your phone or tablet.
  >
  > Features:
  > • 30+ panels: dashboard, temperature graphs, motion control, bed mesh 3D view, console, macros, and more
  > • Multi-material support for AFC, Happy Hare, ACE, AD5X IFS, CFS, Snapmaker U1, and tool changers
  > • Exclude objects mid-print — tap the failing part on an overhead map
  > • 80+ printer models auto-detected with first-run wizard
  > • Calibration suite: input shaper, PID tuning, Z-offset, firmware retraction
  > • 17 themes with light/dark mode
  > • 9 languages
  >
  > Requirements: A 3D printer running Klipper + Moonraker on the same local network.
  >
  > Open source (GPL v3) — no accounts, no cloud, no tracking.

- [ ] **App icon:** Upload 512x512 PNG (use existing app icon)
- [ ] **Feature graphic:** Upload `docs/store/android/feature-graphic.png` (1024x500)
- [ ] **Phone screenshots:** Upload all 8 from `docs/store/android/01-*.png` through `08-*.png`
- [ ] **App category:** Tools
- [ ] **Contact email:** privacy@helixscreen.org
- [ ] **Privacy policy URL:** https://helixscreen.org/privacy

## 6. Content Rating

- [ ] Go to Policy > App content > Content rating
- [ ] Start questionnaire (IARC)
- [ ] Answer: no violence, no user-generated content, no personal info collected, no ads
- [ ] Expected rating: Everyone

## 7. Target Audience & Content

- [ ] Go to Policy > App content > Target audience
- [ ] Select: target audience is not children (18+)
- [ ] News apps: No
- [ ] Ads: No

## 8. Data Safety

- [ ] Go to Policy > App content > Data safety
- [ ] Does your app collect or share user data? **No** (if telemetry is opt-in and anonymized, you can declare no data collected since it's opt-in)
- [ ] Or if declaring telemetry: Usage data (optional, not shared, anonymized)
- [ ] Submit data safety form

## 9. Create Release (Open Testing)

- [ ] Go to Release > Testing > Open testing
- [ ] Click "Create new release"
- [ ] First time: enroll in Play App Signing (accept — Google manages distribution key)
- [ ] Upload `.aab` file from CI release artifacts (or build locally: `cd android && ./gradlew bundleRelease`)
- [ ] Release name: version number (e.g., `1.0.4`)
- [ ] Release notes:
  > Initial Early Access release. Connect to your Klipper 3D printer over your local WiFi network for full touchscreen control — dashboard, temperature graphs, motion control, bed mesh, file browser, multi-material management, and more.
- [ ] Review release summary
- [ ] Click "Start rollout to Open testing"

## 10. Post-Submission

- [ ] Verify app appears in Play Store search within 24-48 hours
- [ ] Test install on a real Android device
- [ ] Share Play Store link with early testers / Discord community
