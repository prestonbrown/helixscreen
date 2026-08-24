# Settings: Display & Sound

The Display & Sound category covers visual preferences, display configuration, and audio settings.

---

## Appearance

### Language

Choose the display language for all UI text. Nine languages are available: English, German, Spanish, French, Italian, Japanese, Portuguese, Russian, and Chinese.

### Timezone

Set your local timezone so that print completion times, ETAs, and the clock widget display the correct time. Most printers default to UTC. Select from a curated list of common timezones — each entry shows the UTC offset (e.g., "Eastern (-5:00)"). DST is handled automatically using your system's timezone database.

**Path:** Settings > Display & Sound > Timezone

### Time Format

Choose between 12-hour and 24-hour clock display. Affects all timestamps shown throughout the UI.

### Animations

Toggle UI motion effects (transitions, panel slides, confetti). Disable for better performance on slower hardware like Raspberry Pi 3.

### System Keyboard *(Android only)*

When on, text fields open Android's native keyboard instead of the built-in on-screen keyboard. Handy on phones and tablets where you already have a preferred keyboard installed.

### Keep Navigation Bar *(Android only)*

When on, the Android navigation bar (back / home / recents) stays onscreen at all times. When off (the default), HelixScreen runs full-screen and you swipe up from the bottom edge to reveal the nav bar — it auto-hides after a few seconds. Turn this on if you use 3-button navigation instead of gestures and want the buttons always available. The status bar stays hidden either way.

### Widget Labels

Toggle labels on home panel widgets. Disable for a cleaner look on small screens.

### Scroll Buttons

Show up/down buttons on long lists — off by default. Turn this on if you'd rather tap through a list than drag it, especially on small screens or displays where touch-drag can feel unresponsive.

When enabled, a screen whose content runs longer than fits gets a slim column of up/down arrow buttons along the right edge. The content shifts left slightly to make room, so the buttons never cover anything. The buttons are for paging through a whole screen, so the individual tiles on the home dashboard don't get them - a tile is small enough that the arrows would cover most of what it's showing, and a short drag scrolls it anyway. Each tap scrolls about one screenful, with a little overlap so you don't lose your place. The up button dims when you're already at the top of the list, and the down button dims at the bottom. Lists that already fit on screen don't get buttons — there's nothing to scroll.

Finger-drag scrolling keeps working normally either way; the buttons are just an additional way to get around. If [Animations](#animations) is also on, pressing a button glides the list smoothly; with Animations off, it jumps straight to the new position.

### Bed Mesh Render

Choose how bed mesh data is visualized: Auto, 3D View, or 2D Heatmap.

---

## Display

### Screen Rotation

Turn the picture to match how the panel is physically mounted: **Normal**, **90° Clockwise**, **180° Upside Down**, or **270° Clockwise**. The row sits at the top of the Display section, so someone facing a sideways screen reaches it without scrolling.

Changing the rotation applies the next time HelixScreen starts — when you pick a new value, HelixScreen offers to restart right away, and re-selecting the current rotation changes nothing. Touch input follows the new orientation automatically. The row is hidden on the desktop simulator, where you rotate the window from your operating system instead.

> If taps land in the wrong places after rotating, that is a touch-calibration question, not a rotation one — see the [Touch Calibration guide](../touch-calibration.md).

### Dark Mode

Switch between light and dark themes. Disabled when the active theme doesn't support both modes.

### Theme Colors

Open the theme explorer to browse, preview, and apply color themes.

When you open **Theme Colors**, the theme explorer shows a **Theme Preset** dropdown, a live preview of sample cards, buttons, inputs, and status colors, and — for themes that ship both a light and a dark palette — a **Dark Mode** toggle. Pick a preset and tap **Apply** to switch to it.

### Built-in Themes

Eighteen themes ship in the box, listed alphabetically in the Theme Preset dropdown and previewed live as you scroll through them. Every theme has a dark palette; all but the four marked **dark only** also carry a matching light palette, so the Dark Mode toggle works with them.

| Theme | Character | Modes |
|-------|-----------|-------|
| **Ayu** | Warm gold accent with coral errors on slate surfaces | Light + dark |
| **Catppuccin** | Soft pastels, gentle lavender surfaces | Light + dark |
| **ChatGPT** | Understated greys with green/red status colours | Light + dark |
| **Cupertino** | Apple-style system colours on near-black | Light + dark |
| **Dracula** | Violet on deep slate with bright green highlights | Dark only |
| **Everforest** | Muted forest green and clay tones | Light + dark |
| **Gruvbox** | Warm retro earth tones — amber, olive, brick | Light + dark |
| **Hazard** | Safety-vest yellow, warning orange and danger red on powder-black, with sharp corners and brass seams | Dark only |
| **HelixScreen** | The default — the house palette, balanced blue on charcoal | Light + dark |
| **Kanagawa** | Soft wave-blue and gold on deep indigo | Light + dark |
| **Material Design** | Google-material blues and greens on grey | Light + dark |
| **Midnight** | Near-black navy, dim and low-glare | Dark only |
| **Nord** | Arctic blues and frost tones | Light + dark |
| **One Dark** | The Atom editor look — blue and green on charcoal | Light + dark |
| **Rose Pine** | Muted rose, iris and pine on soft charcoal | Light + dark |
| **Solarized** | The classic muted scheme — restrained accents on deep teal | Light + dark |
| **Tokyo Night** | City-at-night blues with pastel highlights | Light + dark |
| **Yami** | Vivid blue on deep grey, high contrast | Dark only |

Applying a theme takes effect immediately everywhere — no restart needed. The [theme editor](#editing-a-custom-theme) can recolor any of them, and **Save As New** keeps the original untouched.

### Editing a Custom Theme

Tap **Edit** in the theme explorer to open the theme editor, where you can recolor a palette and adjust its shape and shadow styling. Every change previews live on the surrounding UI as you make it.

The editor is organized into two sections:

**Theme Colors** — a grid of 16 color swatches, each labeled with what it controls (background, text, primary, success, warning, and so on). Tap any swatch to open a color picker with ready-made swatches and a custom color selection; pick a color and the swatch — and the live preview — update immediately.

**Style Properties** — four sliders that shape the overall look:

| Slider | What It Does |
|--------|--------------|
| **Border Radius** | Corner roundness, from square to fully rounded |
| **Border Width** | Default border thickness |
| **Border Opacity** | Border transparency (0 = invisible, 255 = solid) |
| **Shadow Intensity** | Drop shadow strength (0 = disabled) |

### Light and Dark Palettes

A theme can carry a separate palette for light mode and dark mode, and the editor works on **one at a time**. Choose which one you're editing *before* you tap **Edit**, using the **Dark Mode** toggle in the theme explorer: turn it on to edit the dark palette, off to edit the light palette. (The toggle only appears for themes that support both modes.) Saving keeps both palettes — the one you didn't touch this session is preserved.

> **Tip:** To give a theme a matching light and dark look, edit one palette and save, then flip the Dark Mode toggle, tap **Edit** again, and adjust the other.

### Saving Your Changes

Three buttons sit at the bottom of the editor:

| Button | What It Does |
|--------|--------------|
| **Reset** | For a built-in theme, restores the factory colors. For a theme you created, reverts to its last saved state. |
| **Save As New** | Creates a new named custom theme, leaving the original untouched. |
| **Save** | Overwrites the current theme with your changes. Stays disabled until you've made a change. |

Tapping **Save As New** opens a **Save Theme As** dialog. Enter a name (it suggests "<current theme> Copy") and tap **Save**, or **Cancel** to back out. Your new theme is saved, applied immediately, and appears in the Theme Preset dropdown.

Both **Save** and **Save As New** apply the theme live — no restart needed. If you try to leave the editor with unsaved changes, HelixScreen asks **"Discard Changes?"** first so you don't lose your work.

> **Note:** Custom and edited themes are stored on your printer under `~/helixscreen/config/themes/`. They survive updates and are yours to back up or copy between printers.

### Brightness

Slider from 10–100%. Only shown on hardware with backlight control (hidden on Android).

### Screen Dim

When the screen dims to lower brightness: Never, 30s, 1m, 2m, 5m, or 10m of inactivity.

### Display Sleep

When the screen turns off completely: Never, 1m, 5m, 10m, 20m, or 30m of inactivity.

### Screensaver

Choose a screensaver to display during inactivity instead of dimming the screen:

| Option | Description |
|--------|-------------|
| **Off** (default) | No screensaver — the screen dims/sleeps normally |
| **Flying Toasters** | Classic flying toasters animation |
| **Starfield** | Scrolling starfield |
| **3D Pipes** | Animated 3D pipes |

When any option other than **Off** is selected, a **Test Screensaver** button appears below the dropdown. Tap it to preview the selected screensaver immediately, without waiting for the inactivity timeout.

### Sleep While Printing

Allow the display to sleep during active prints. Off by default so you can monitor progress.

> **Tip:** Touch the screen to wake from sleep.

---

## Sound

### Sounds

Master toggle — turns all sounds on or off. All other sound options are hidden when sounds are disabled.

> Sound options appear as soon as HelixScreen starts — you don't need to wait for the printer to connect. If no audio hardware is detected after connection, the sound options are hidden entirely.

### Volume

Master volume slider (0–100%). Only shown when sounds are enabled.

### UI Sounds

Controls button taps, navigation, and toggle sounds. When off, only important sounds still play (print complete, errors, alarms).

### Sound Theme

Choose sound style. A test sound plays when you switch themes.

### Output Device

*(Only shown on devices using the ALSA audio backend — hidden on desktop/SDL, PWM-buzzer, and Moonraker-beeper hardware.)*

Choose which audio output device UI sounds and notifications play through. Useful when a device exposes more than one sound card (e.g., an HDMI display with its own audio plus an onboard speaker). The list is populated from the system's available ALSA output devices.

### Sound Themes

HelixScreen comes with five built-in themes:

| Theme | Description |
|-------|-------------|
| **Default** | Balanced, tasteful sounds. Subtle clicks, smooth navigation chirps, and a melodic fanfare when your print completes. |
| **Minimal** | Only plays sounds for important events: print complete, errors, and alarms. No button or navigation sounds at all. |
| **Retro** | 8-bit chiptune style. Punchy square-wave arpeggios, a Mario-style victory fanfare, and buzzy retro alarms. |
| **Miami Vice** | Punchy 80s electronic synth. Staccato square-wave hits, driving rhythm, and a soaring saw-wave lead for print complete. |
| **Crockett's Theme** | Warm, cinematic 80s synth. Smooth saw waves with long sustains and filter sweeps. Startup plays the Crockett's Theme melody. |

### Preview Sounds

Tap **Preview Sounds** in the Sound section to open an overlay with buttons for every sound in the current theme. Tap any button to hear that sound immediately. This is useful for comparing themes or testing custom sounds.

### Custom Sound Themes

You can create your own sound themes without modifying the HelixScreen installation:

1. SSH into your printer
2. Create the sounds directory if it doesn't exist: `mkdir -p ~/helixscreen/config/sounds`
3. Copy an existing theme as a starting point: `cp ~/helixscreen/assets/config/sounds/default.json ~/helixscreen/config/sounds/mytheme.json`
4. Edit the file: change the `"name"` field and modify the sounds
5. Your theme appears in the Sound Theme dropdown immediately

Custom themes support the full synthesis engine: four waveform types (square, saw, triangle, sine), ADSR envelopes, frequency sweeps, lowpass/highpass filters with sweep, LFO modulation, polyphonic chords (up to 4 voices), musical note names (C4, F#5, Bb3), and musical duration notation (8n, 4n., 16t) with BPM.

If your custom theme has the same filename as a built-in theme, your version takes priority.

See the [Sound System developer docs](../../../devel/SOUND_SYSTEM.md#sound-theme-json-schema) for the complete JSON schema reference.

### What Sounds When

| Event | Sound | When It Plays |
|-------|-------|---------------|
| Button press | Short click | Any button tapped |
| Toggle on | Rising chirp | A switch turned on |
| Toggle off | Falling chirp | A switch turned off |
| Navigate forward | Ascending tone | Opening a screen or overlay |
| Navigate back | Descending tone | Closing an overlay or going back |
| Print complete | Victory melody | Print finished successfully |
| Print cancelled | Descending tone | Print job cancelled |
| Error alert | Pulsing alarm | A significant error occurred |
| Error notification | Short buzz | An error toast appeared |
| Critical alarm | Urgent siren | Critical failure requiring attention |
| Test sound | Short beep | Preview Sounds overlay in settings |
| Startup | Theme jingle | HelixScreen launches (plays once at startup) |

The first five (button press, toggles, navigation) are **UI sounds** and respect the "UI Sounds" toggle. The rest always play as long as the master toggle is on.

### Supported Hardware

| Hardware | How It Works |
|----------|-------------|
| **Desktop (SDL)** | Full audio synthesis through your computer speakers. Best sound quality. |
| **ALSA (Linux)** | Direct audio output on devices with ALSA sound support. 4-voice polyphony with MOD/MED tracker music support for richer sound themes. |
| **Creality AD5M / AD5M Pro** | Hardware PWM buzzer. Supports different tones and volume levels. |
| **Other Klipper printers** | Beeper commands sent through Moonraker. Requires `[output_pin beeper]` in your Klipper config. Basic beep tones only. |

If no audio hardware is detected, the sound options are hidden entirely.

**Disabling sound entirely:** On some hardware (e.g., Artillery M1 Pro), audio drivers are present but using them causes excessive CPU load. If you experience performance issues with sound enabled, add `"disable_sound": true` to your `settings.json` or start HelixScreen with `--no-sound`. This prevents the audio backend from initializing at all, unlike the Sounds toggle which just mutes playback.

### Sound Troubleshooting

**I don't see Sound options in Settings.**
Your printer doesn't have a detected speaker or buzzer. For Klipper printers, make sure you have `[output_pin beeper]` configured in your `printer.cfg`, then restart HelixScreen.

**Sounds are too quiet or too loud.**
Adjust the Volume slider. Volume also varies by theme — try switching themes.

**Print complete sound doesn't play.**
Make sure the master "Sounds" toggle is on. The "UI Sounds" toggle does not affect print completion sounds.

**Button click sounds are annoying.**
Turn off "UI Sounds". This disables button, toggle, and navigation sounds while keeping important notifications.

**Sounds work on desktop but not on my printer.**
Confirm your printer has audio hardware. For Klipper printers, verify `[output_pin beeper]` is present and correctly configured. Test by sending an `M300` command from the Klipper console.

---

[Back to Settings](../settings.md) | [Next: Printing](printing.md)
