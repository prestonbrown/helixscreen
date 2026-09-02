# Sound System (Developer Guide)

How the sound system works internally, how to extend it with new themes/backends/sounds, and full reference for the JSON theme schema and C++ API.

**User-facing doc**: [Sound Settings](../user/guide/settings/display-sound.md#sound) (enabling/disabling, choosing themes, troubleshooting)

---

## Architecture Overview

The sound system has four layers, each with a clear responsibility:

```
SoundManager (singleton, public API)
  |  Respects SettingsManager toggles. Looks up sounds by name in the theme.
  |
  +-> SoundTheme / SoundThemeParser (JSON parser, note/duration conversion)
  |     Loads config/sounds/*.json, converts note names to Hz, musical durations to ms.
  |
  +-> SoundSequencer (dedicated thread, LFO/sweep, priority queue)
  |     Ticks at ~1ms (backend-dependent). Computes modulation and filter sweep
  |     each tick. Sends envelope params to PCM backends at step boundaries.
  |
  +-> SoundBackend (abstract interface)
        |
        +-> SDLSoundBackend   (desktop, per-sample envelope + biquad filter)
        +-> ALSASoundBackend  (Linux SBCs, per-sample envelope + biquad filter)
        +-> PWMSoundBackend   (AD5M hardware buzzer, sysfs /sys/class/pwm)
        +-> JzPwmSoundBackend (AD5X piezo, jz_pwm DMA one-shot buffers via fx-pwm)
        +-> M300SoundBackend  (Klipper printers, G-code via Moonraker)
```

### Thread Model

```
LVGL thread (main)                  Sequencer thread              Audio render thread
  |                                   |                             (SDL callback / ALSA loop)
  SoundManager::play("button_tap")    |                             |
    |                                 |                             |
    +-- mutex lock --+                |                             |
    |  push to queue |                |                             |
    +-- unlock ------+                |                             |
    |                    queue_cv_ wakeup                            |
    |                                 |                             |
    |                    pop request from queue                      |
    |                    priority check -> preempt or drop           |
    |                                 |                             |
    |                    begin_playback():                           |
    |                      publish_note() --------> VoiceSlot (generation++)
    |                                 |                             |
    |                    tick loop @ ~1ms:                           |
    |                      (PCM backends: step timing only)         |
    |                      (non-PCM: compute_lfo, compute_sweep,    |
    |                       backend->set_tone, backend->set_filter) |
    |                                 |                             |
    |                                 |           audio_callback():
    |                                 |             detect new note via generation counter
    |                                 |             snapshot ALL params atomically (VoiceSlot)
    |                                 |             VoiceSlot::render_sample():
    |                                 |               envelope + sweep + LFO -> amplitude
    |                                 |               generate_samples() -> waveform
    |                                 |               apply_filter()     -> output
    |                                 |                             |
    |                    advance_step() when step completes          |
    |                    end_playback() when sequence completes      |
```

**NoteEvent publishing**: At step boundaries the sequencer calls `publish_note()`, which writes a complete `NoteEvent` (frequency, amplitude, duty, waveform, ADSR, LFO, sweep, filter) into a `VoiceSlot` and bumps a generation counter. The audio callback detects the new generation, snapshots all parameters at once, and `VoiceSlot::render_sample()` computes envelope + modulation + waveform per-sample. This eliminates timing-dependent pitch variation from independent atomic writes. Backends without note-event rendering (PWM, M300) do not use `publish_note()`; the sequencer continues to drive per-tick computation for them. (PWM's PCM path is a separate render-source mechanism -- see [PWM PCM mode (ad5m)](#pwm-pcm-mode-ad5m) below.)

The sequencer thread sleeps on a condition variable when idle (no sound playing, queue empty). When a sound is queued, it wakes and ticks at the backend's `min_tick_ms()` interval until playback completes.

---

## Backend Auto-Detection

`SoundManager::create_backend()` tries backends in this order. First success wins.

| Order | Backend | Condition | Target Hardware |
|-------|---------|-----------|-----------------|
| 1 | SDL | `#ifdef HELIX_DISPLAY_SDL` + `SDL_OpenAudioDevice` succeeds | Desktop/simulator |
| 2 | ALSA | `#ifdef HELIX_HAS_ALSA` + ALSA PCM device opens (saved/env device, then `default`) | Linux SBCs with audio hardware |
| 3 | JzPwm | `#ifdef HELIX_HAS_JZ_PWM` (ad5x builds) + `/dev/jz_pwm` exists and fx-pwm is executable | AD5X piezo (on-rig install) |
| 4 | PWM | `/sys/class/pwm/pwmchip0` exists (channel auto-exported by `initialize()` on ad5m/ad5m-br) | AD5M hardware buzzer |
| 5 | M300 | Printer answers M300 gcode: a beeper `output_pin` or an M300 macro in objects/list, or the `speaker` capability override forced on — plus a `MoonrakerClient` set via `set_moonraker_client()` | Klipper printers with a gcode beeper (no local audio) |
| 6 | None | All above failed | Sounds silently disabled |

On the AD5X rig both JzPwm and M300 can reach the same transducer; JzPwm wins
the eager ladder (its probe succeeds first), which is the desired shape: the
native backend renders full chords as duty-encoded buffers with no protocol
bottleneck, while the M300 path — capped at one frequency per 50 ms command —
remains the answer for remote-UI installs where HelixScreen runs off the
printer and the buzzer must be driven over the network.

M300 is not probed at `initialize()`; it is installed lazily from
`PrinterCapabilitiesState::set_hardware()` once hardware discovery has seen the
beeper signal, so the M300 commands cannot fire on a printer that would answer
them with `!! Unknown command:M300` (which surfaces as an error toast, plays
`error_tone`, and sends more M300 — the loop the lazy gate exists to prevent).
Two signals open the gate: an `output_pin` whose name contains
BEEPER/BUZZER/SPEAKER, and a `gcode_macro M300` (Z-Mod's AD5X buzzer config has
only the macro). A `speaker` capability override of `enable` opens it without
either signal; `disable` keeps it closed even when one fires.

### Backend Capabilities

The sequencer adapts to what the backend can do. Features not supported by the backend are silently skipped.

| Backend | Waveforms | Amplitude | Filter | NoteEvent | min_tick_ms | Notes |
|---------|-----------|-----------|--------|-----------|-------------|-------|
| SDL     | yes       | yes       | yes    | yes       | 1.0         | Full synthesis: 4 waveforms, biquad filter, 64-sample buffer (~1.5ms) |
| ALSA    | yes       | yes       | yes    | yes       | 1.0         | Same synthesis as SDL, hardware-negotiated buffer size |
| PWM     | no*       | yes       | no     | no        | 2.0         | Tone mode: waveform approximation via duty cycle ratios, sequencer per-tick. Tracker playback on ad5m rides the same tone path (PC-speaker mode, below) |
| M300    | no        | no        | no     | no        | 50.0        | Frequency only, 100-10000 Hz, deduplicates commands; sequencer drives per-tick |
| JzPwm   | yes       | yes       | no     | yes       | 60.0        | AD5X piezo: full per-sample synthesis (ADSR/sweep/LFO/waveforms via VoiceSlot), 4-voice chords, one duty-encoded buffer per theme step; tracker PC-speaker mode drives it through set_voice (mods on the piezo) |

*PWM `supports_waveforms()` returns `false`, but `set_waveform()` stores the waveform internally to adjust the duty cycle ratio: Square=50%, Saw=25%, Triangle=35%, Sine=40%. This gives perceptually different timbres even on a single-pin buzzer.

### Tracker playback on ad5x (PC-speaker mode)

`HELIX_HAS_TRACKER` is enabled for ad5x: the tracker's synth fallback
(the way the AD5M plays modules on its piezo — per-channel note
frequencies, arpeggio/portamento/vibrato applied) drives the JzPwm
backend through `set_voice`. Each tracker row's four-voice burst
coalesces (40 ms debounce) into one sustained 150 ms chord buffer
through the same renderer. No PCM path is involved — the SCHED_IDLE
render loop that kept tracker off ad5x is not compiled in. PCM streaming
on this engine is dead for good, with numbers: the update handler
refuses buffer swaps while a loop is armed, and the legal chunk cycle
(disable → copy → arm) costs a FIXED ~500 ms of silence per chunk — a
`chunks` probe measured 502 ms overhead at both 200 ms and 500 ms
chunks. Module playback happens as tone language, not audio.

### JzPwm one-shot buffer model (ad5x)

The AD5X piezo hangs off the Ingenic X2600 PWM2 DMA engine (`/dev/jz_pwm`,
driven by the `fx-pwm serve` daemon in the Forge-X rootfs). The engine replays a
buffer of period words — each word one PWM cycle whose 16-bit halves set
inactive/active counts — and refuses buffer updates while a loop runs, so
there is no sample streaming: **audio is one complete buffer per theme
step**. The backend renders each step in-process with the full
`VoiceSlot` synthesis (ADSR, sweep, LFO, four waveforms, up to four chord
voices), duty-encodes the mix around a 32 kHz carrier, and hands the words
to the daemon over `/tmp/fx-pwm.sock`.

Rig-measured properties that shaped the design:

- **Calibration**: the DMA waveform steps at 385 MHz regardless of the
  channel's prescale register (two-point phone-tuner measurement), so the
  encoder computes against a 385 MHz clock — the same calibration
  tone_player uses (`base 770 MHz / prescale 2`).
- **Audibility floor, MEASURED**: 10 ms — thirty 10 ms duty-encoded
  beeps were each clearly audible; 200 ms was an order-of-magnitude
  guess. Short theme steps tile their content to 10 ms.
- **Word-copy cost, MEASURED**: the driver's dma_update ioctl copies
  buffer words at ~30 µs each (3,504 words → 106 ms, 14,024 → 429 ms,
  dead linear). Upload time therefore scales with words-per-second of
  audio: at the 32 kHz carrier, uploading one second of audio takes ~one
  second. This is the hardware ceiling that settled the AD5X on
  **UI sounds only** — continuous music needs a duty cycle no code on
  this driver can reach (50% at an ultrasonic carrier; ~80% only by
  dropping the carrier into the audible band, whine included). The
  tracker phrase path (`jz_pwm_render_phrase`) remains in the tree,
  tested but unused on ad5x: mods do not ship there.

#### The sound daemon (`fx-pwm serve`)

One long-lived `fx-pwm serve` process owns `/dev/jz_pwm` and the channel
claim; the backend connects to its socket and sends frames
(`magic + hold_ms + nwords + words`, one-byte ack: 0 played, 1 busy,
protocol violations drop the connection). The backend spawns it on the
first sound and respawns it if it dies; a dedicated sender worker is the
only thread that touches the socket (a 250 KB phrase body can block for
seconds and that must never stall the UI). The daemon idle-exits after
30 s without traffic so its flock does not starve klippy's per-tone
`fx-pwm` one-shots (M300/TONE).

Four protocol lessons, all rig-measured:

- **SIGPIPE kills daemons**: a client that times out and closes the
  socket turns the daemon's next `send()` into a death; the daemon
  ignores SIGPIPE and loses only the reply.
- **Multi-shot needs the claim dance**: the driver refuses a second
  `dma_init` while the channel reads "working" (EPERM), so every frame
  re-runs the release → re-request → config → prescale sequence. That
  dance is itself a wedge surface (`pwm2_release` → `disable_loop` can
  D-wedge; recovery is a reboot) — accept it for short UI buffers and
  never loop it hot.
- **Holds need absolute deadlines**: the daemon's hold loop polls the
  socket for cancels, and poll's timeout restarts on every readable
  event — a client that resends instantly keeps the socket perpetually
  readable and the hold (with `enable_loop`) replays one phrase forever.
- **The per-buffer exec was never the cost**: the fork+exec+chroot hop
  the original per-sound child paid was ~200 ms but the word copy inside
  the driver was the same order; the daemon removed the exec and kept
  the gap, which is how the copy rate got measured.

The piezo demodulates duty encoding (verified by ear and tuner: chords
rendered this way are recognizable), with its ~5 kHz mechanical resonance
coloring the timbre bright.

### PWM tracker playback: PC-speaker mode (ad5m)

`supports_render_source()` returns **false** on PWM, so tracker playback (MOD/MED music) routes through the note fallback: `TrackerPlayer::apply_to_backend()` computes each channel's note frequency (`3546895 / period`) and calls `set_voice()`, whose base implementation forwards slot 0 to `set_tone()` -- the channel-0 lead line as note-frequency square waves, PC-speaker style. Channels 1-3 are dropped (single sysfs channel), and instrument samples are not reproduced (their note pitches are).

This is a hardware verdict, not a preference: verified on an AD5M Pro 2026-08-30, the buzzer is a resonant piezo with no reconstruction filter, so a duty-modulated carrier demodulates as static -- an audible beat, not music. Note-frequency square waves are what the transducer is built for.

**Known limitation**: while a tracker melody plays, tone SFX are dropped (the sound manager only layers SFX under a tracker on render-source backends); ALARM-priority sounds still stop the tracker and reclaim the channel.

Tone efficiency: the fallback re-sends the same note every tracker tick, so `set_tone()` deduplicates held tones (keyed on the written period/duty values, mirroring `M300SoundBackend::last_freq_`), and `silence()` guards against the per-tick rest spam (the fallback calls `silence_voice(0)` every ~2 ms through rests). The Makefile gates tracker to `PLATFORM_TARGET=ad5m` (`HELIX_HAS_TRACKER` + `HELIX_PWM_AUTO_EXPORT`; ad5m-br and ad5x stay tone-SFX-only pending hardware validation).

### PWM PCM machinery (dormant)

The PCM render path stays compiled and unit-tested for hardware that can actually demodulate duty-modulated PWM (a filtered speaker circuit) -- on the AD5M's piezo it is unreachable because nothing installs a render source. The render loop is built to be printer-safe above all (`src/system/pwm_sound_backend.cpp:489`):

- **8 kHz sample rate** -- the piezo's response rolls off around 3-4 kHz, so rendering faster adds no audible content (`PCM_SAMPLE_RATE`, `include/pwm_sound_backend.h:90`).
- **62.5 kHz carrier**, above the audible range; each sample becomes a duty-cycle value within that period (`PCM_CARRIER_HZ`).
- **Render thread at SCHED_IDLE + 1 ns timerslack** (`apply_render_thread_priority()`). SCHED_IDLE means the thread runs only when nothing else wants the CPU, so its pacing can never starve klippy, and setting it needs no privileges. Timerslack affects the relative polls only (the 1 ms no-source poll, the 10 ms park poll) -- the sample loop paces with `TIMER_ABSTIME`, which timerslack does not touch.
- **Absolute pacing with a 20 µs spin budget** -- sleep (`TIMER_ABSTIME`) to each sample deadline minus 20 µs, then spin the final stretch. This absorbs hrtimer wake lateness without burning real CPU (`PCM_SPIN_BUDGET_NS`).
- **Bounded catch-up** -- more than 2 samples late snaps the sample clock forward (resync) instead of bursting the missed writes. The burst is what starved klippy under the old loop (`PCM_CATCHUP_MAX_SAMPLES`).
- **Silence auto-park** -- 8 consecutive exactly-silent buffers (~512 ms at 64 ms/buffer) park the channel (duty 0, enable 0) and drop to a 10 ms poll. Each poll pulls an 80-frame probe (10 ms @ 8 kHz) from the render source at 1x real time and resumes the moment real audio shows up (`PCM_PARK_SILENT_BUFFERS`, `park_probe_frames()`).
- **Channel auto-export** -- the stock AD5M kernel ships the beeper channel unexported; nothing materializes pwm6 until `initialize()` writes the channel number to `pwmchip0/export` (`HELIX_PWM_AUTO_EXPORT`, ad5m/ad5m-br only). This one is live for tone mode too: without it the backend never initializes and the AD5M has no audio at all after boot.

History: PCM playback was disabled 2026-04 (003c195ac) because the render loop's busy-wait at normal priority starved the single-core CPU; rewritten printer-safe (b8c141b4a) and verified harmless on-device 2026-08-30 -- then retired from active use the same day by the transducer verdict above.

---

## Priority System

| Priority | Value | Use Case | Behavior |
|----------|-------|----------|----------|
| `UI`     | 0     | Button taps, nav sounds, toggles | Interrupted by anything |
| `EVENT`  | 1     | Print complete, errors | Only interrupted by ALARM |
| `ALARM`  | 2     | Critical failures | Never interrupted |

Rules:
- Higher priority preempts lower priority (new sound replaces current).
- Same priority: new sound replaces current.
- Lower priority than current: new sound is dropped (not queued).
- All priority checks happen on the sequencer thread when popping from the queue.

---

## Sound Theme JSON Schema

Themes live in `config/sounds/{name}.json`. The parser is lenient -- unknown fields are silently ignored, missing optional fields use defaults.

```jsonc
{
  "name": "theme_name",           // Required: displayed in settings dropdown
  "description": "...",            // Required: human-readable description
  "version": 1,                    // Required: schema version (currently 1)

  "defaults": {                    // Optional: applied when steps omit fields
    "wave": "square",              // square | saw | triangle | sine
    "vel": 0.8,                    // 0.0-1.0 (clamped)
    "env": {                       // ADSR envelope defaults
      "a": 5,                      // Attack time in ms (ramp 0 -> 1.0)
      "d": 40,                     // Decay time in ms (ramp 1.0 -> sustain)
      "s": 0.6,                    // Sustain level 0.0-1.0
      "r": 80                      // Release time in ms (ramp sustain -> 0)
    }
  },

  "sounds": {
    "sound_name": {
      "description": "...",        // Optional: documentation only
      "bpm": 140,                  // Optional: enables musical duration notation
      "repeat": 3,                 // Optional: repeat count (default 1)

      "steps": [
        {
          // === Frequency (one of): ===
          "freq": 440,             // Raw Hz (clamped to 20-20000)
          "note": "A4",            // Note name: A4=440 Hz, 12-TET, C0-B8
                                   // Supports sharps (C#4) and flats (Db4)

          // === Duration (one of): ===
          "dur": 100,              // Raw milliseconds (clamped to 1-30000)
          "dur": "8n",             // Musical: requires "bpm" on the sound
                                   // 1n=whole, 2n=half, 4n=quarter, 8n=eighth,
                                   // 16n=sixteenth, 4n.=dotted, 8t=triplet

          // === OR pause (exclusive with freq/note): ===
          "pause": 50,             // Silence in ms (clamped to 1-30000)

          // === Synthesis params: ===
          "wave": "square",        // square | saw | triangle | sine
                                   // Overrides theme default_wave
          "vel": 0.9,              // Velocity/volume 0.0-1.0
                                   // Overrides theme default_velocity

          // === ADSR envelope: ===
          "env": {                 // Overrides theme default_envelope
            "a": 5,                // Attack ms
            "d": 40,               // Decay ms
            "s": 0.6,              // Sustain level
            "r": 80                // Release ms
          },

          // === LFO modulation: ===
          "lfo": {
            "target": "amplitude", // "freq" | "amplitude" | "duty"
            "rate": 8,             // Oscillation rate in Hz
            "depth": 0.5           // Modulation depth (units depend on target)
          },

          // === Linear sweep: ===
          "sweep": {
            "target": "freq",      // Currently only "freq" is supported
            "end": 2400            // End value -- linearly interpolated over step
          },

          // === Filter (SDL backend only): ===
          "filter": {
            "type": "lowpass",     // "lowpass" | "highpass" (biquad, Butterworth Q)
            "cutoff": 800,         // Initial cutoff frequency in Hz
            "sweep_to": 4000       // Optional: sweep cutoff linearly over step
          }
        }
      ]
    }
  }
}
```

### Parsing Notes

- If both `freq` and `note` are present, `note` takes priority (checked first).
- If `dur` is a string, it requires `bpm` to be set on the sound definition. Without `bpm`, string durations resolve to 0.
- The `pause` field is exclusive with `freq`/`note` -- if `pause` is present, the step becomes a silence gap.
- All ADSR/LFO/sweep/filter fields are optional at every level. Missing fields use struct defaults (not theme defaults, unless the envelope is entirely omitted).
- Filter parameters are only sent to backends that return `supports_filter() == true` (currently SDL only).

---

## All Sound Names

These are the 13 standard sound names recognized by the system. Theme files may include any subset.

| Sound Name | Default Priority | Category | When Played |
|------------|------------------|----------|-------------|
| `button_tap` | UI | Interaction | Any `<ui_button>` clicked |
| `toggle_on` | UI | Interaction | Any `<ui_switch>` turned on |
| `toggle_off` | UI | Interaction | Any `<ui_switch>` turned off |
| `nav_forward` | UI | Navigation | Panel switch, overlay push |
| `nav_back` | UI | Navigation | Overlay close |
| `dropdown_open` | UI | Interaction | Reserved (not currently hooked) |
| `print_complete` | EVENT | Print Status | Print job completed successfully |
| `print_cancelled` | EVENT | Print Status | Print job cancelled |
| `error_alert` | EVENT | Error | Repeated urgent alert |
| `error_tone` | EVENT | Error | Error severity toast shown |
| `alarm_urgent` | ALARM | Critical | Critical failure alarm |
| `test_beep` | UI | Settings | Preview sound buttons in settings |
| `startup` | EVENT | System | HelixScreen boot chime |

**UI sounds** (`button_tap`, `toggle_on`, `toggle_off`, `nav_forward`, `nav_back`, `dropdown_open`) are affected by the `ui_sounds_enabled` toggle. EVENT and ALARM sounds play regardless of the UI toggle -- only the `sounds_enabled` master toggle can disable them.

The list of UI sounds is defined in `SoundManager::is_ui_sound()` in `sound_manager.cpp`.

---

## Adding a New Sound Theme

### Shipped themes (developers)

1. Create assets/config/sounds/mytheme.json
2. Include any subset of the 13 standard sound names (missing sounds just won't play)
3. Set `name`, `description`, `version` fields at the top level
4. Theme appears automatically in Settings > Sound Theme dropdown
5. No code changes or rebuild needed -- `get_available_themes()` scans for `.json` files at runtime

### User drop-in themes (end users)

Users can create custom themes without modifying the installation:

1. Create `~/helixscreen/config/sounds/mytheme.json` on the device
2. The theme appears in the Sound Theme dropdown immediately
3. If a user theme has the same filename as a shipped theme, the user version takes priority (shadows it)

`get_available_themes()` scans two directories and merges the results:
- **Read-only (shipped):** `<data_dir>/assets/config/sounds/` 
- **Writable (user):** `~/helixscreen/config/sounds/` (via `helix::writable_path("sounds")`)

Example minimal structure:

```json
{
  "name": "mytheme",
  "description": "My custom sound theme",
  "version": 1,
  "defaults": {
    "wave": "square",
    "vel": 0.8,
    "env": { "a": 5, "d": 40, "s": 0.6, "r": 80 }
  },
  "sounds": {
    "button_tap": {
      "steps": [
        { "freq": 4000, "dur": 6, "vel": 0.9,
          "env": { "a": 1, "d": 5, "s": 0, "r": 1 } }
      ]
    }
  }
}
```

---

## Adding a New Sound

1. Add the sound definition to theme JSON files (all themes, or just the ones you want)
2. Call `SoundManager::instance().play("new_sound_name")` from C++ code
3. If it's a UI interaction sound (should be gated by `ui_sounds_enabled`), add the name to `SoundManager::is_ui_sound()` in `sound_manager.cpp`:

```cpp
bool SoundManager::is_ui_sound(const std::string& name) {
    return name == "button_tap" || name == "toggle_on" || name == "toggle_off" ||
           name == "nav_forward" || name == "nav_back" || name == "dropdown_open" ||
           name == "my_new_ui_sound";  // <-- add here
}
```

If the sound name isn't found in the current theme, `play()` logs a debug message and does nothing. No crash, no error -- this is by design so themes can include subsets of sounds.

---

## Adding a New Backend

1. Create header include/my_backend.h and source src/system/my_backend.cpp
2. Inherit from `SoundBackend` and implement the required interface:

```cpp
class MyBackend : public SoundBackend {
  public:
    // Required overrides:
    void set_tone(float freq_hz, float amplitude, float duty_cycle) override;
    void silence() override;

    // Optional overrides (defaults are all false/1.0):
    bool supports_waveforms() const override;   // default: false
    bool supports_amplitude() const override;   // default: false
    bool supports_filter() const override;      // default: false
    void set_waveform(Waveform w) override;     // default: no-op
    void set_filter(const std::string& type, float cutoff) override;  // default: no-op
    float min_tick_ms() const override;         // default: 1.0
};
```

3. Add detection logic in `SoundManager::create_backend()` in `sound_manager.cpp`. Order matters -- earlier entries take priority:

```cpp
std::shared_ptr<SoundBackend> SoundManager::create_backend() {
#ifdef HELIX_DISPLAY_SDL
    // ... SDL detection ...
#endif

    // Try my backend before PWM
    auto my_backend = std::make_shared<MyBackend>();
    if (my_backend->initialize()) {
        spdlog::info("[SoundManager] Using my backend");
        return my_backend;
    }

    // ... PWM detection ...
    // ... M300 detection ...
}
```

4. The Makefile auto-discovers new `.cpp` files via wildcard -- no Makefile edits needed.

### Backend Contract

- `set_tone()` is called at `min_tick_ms()` intervals while a step is active. Parameters change smoothly per-tick (ADSR, sweep, LFO).
- `silence()` must stop sound output immediately. May be called redundantly.
- `min_tick_ms()` determines the sequencer's sleep interval. Return a higher value for high-latency backends (e.g., M300 returns 50ms because G-code round-trips are slow).
- `set_waveform()` is only called if `supports_waveforms()` returns true.
- `set_filter()` is only called if `supports_filter()` returns true.

---

## Settings Integration

Three settings control sound behavior. All are persisted across restarts.

### Settings Subjects

| Subject | Type | Persisted | Effect |
|---------|------|-----------|--------|
| `settings_sounds_enabled` | bool (int 0/1) | Yes | Master toggle. When off, ALL sounds are suppressed. Hides sub-settings in UI. |
| `settings_ui_sounds_enabled` | bool (int 0/1) | Yes | UI sounds toggle. When off, only button/nav/toggle sounds are suppressed. |
| `printer_has_speaker` | bool (int 0/1) | No (auto-detected) | Set by `PrinterCapabilitiesState` based on hardware detection. When 0, hides the entire sound settings section. |

### Settings Panel XML

The sound settings live in the Sound section of `settings_display_sound_overlay.xml` (the Display & Sound sub-panel):

```
SOUND section
  +-- row_sounds              (master toggle, bound to settings_sounds_enabled)
  +-- container_ui_sounds     (hidden when master off)
  |     +-- row_ui_sounds     (UI toggle, bound to settings_ui_sounds_enabled)
  +-- container_sound_theme   (hidden when master off)
  |     +-- row_sound_theme   (dropdown, populated from get_available_themes())
  +-- container_preview_sounds (hidden when master off)
  |     +-- row_preview_sounds (opens SoundPreviewOverlay with buttons for each sound)
```

The `container_ui_sounds`, `container_sound_theme`, and test beep button all use `bind_flag_if_eq` to hide when `settings_sounds_enabled` is 0.

### SettingsManager API

```cpp
// Read
bool sounds_on = SettingsManager::instance().get_sounds_enabled();
bool ui_on = SettingsManager::instance().get_ui_sounds_enabled();
std::string theme = SettingsManager::instance().get_sound_theme();

// Write (persists to config)
SettingsManager::instance().set_sounds_enabled(true);
SettingsManager::instance().set_ui_sounds_enabled(false);
SettingsManager::instance().set_sound_theme("retro");

// Subjects for XML binding
lv_subject_t* subj = SettingsManager::instance().subject_sounds_enabled();
lv_subject_t* subj = SettingsManager::instance().subject_ui_sounds_enabled();
```

---

## API Quick Reference

```cpp
// --- Play sounds ---
SoundManager::instance().play("button_tap");
SoundManager::instance().play("print_complete", SoundPriority::EVENT);
SoundManager::instance().play("alarm_urgent", SoundPriority::ALARM);

// --- Backward-compat wrappers ---
SoundManager::instance().play_test_beep();         // play("test_beep")
SoundManager::instance().play_print_complete();     // play("print_complete", EVENT)
SoundManager::instance().play_error_alert();        // play("error_alert", EVENT)

// --- Theme management ---
SoundManager::instance().set_theme("retro");
auto themes = SoundManager::instance().get_available_themes();  // ["default", "minimal", "retro"]
auto current = SoundManager::instance().get_current_theme();    // "retro"

// --- Lifecycle ---
SoundManager::instance().set_moonraker_client(client);  // Before initialize()
SoundManager::instance().initialize();                   // Auto-detect backend, load theme, start sequencer
// ... app runs ...
SoundManager::instance().shutdown();                     // Stop sequencer, cleanup

// --- Availability ---
if (SoundManager::instance().is_available()) {
    // Backend exists AND sounds_enabled is true
}
```

### Thread Safety

- `play()` is safe to call from any thread (LVGL thread, WebSocket callbacks, etc.). It pushes to a mutex-protected queue.
- `set_theme()` should only be called from the LVGL thread (it modifies `current_theme_` which is read by `play()`).
- `initialize()` and `shutdown()` should be called from the LVGL thread during app startup/teardown.
- The sequencer thread is the only thread that calls backend methods.

---

## File Map

| File | Purpose |
|------|---------|
| `include/note_event.h` | `NoteEvent` struct (complete per-step params) + `VoiceSlot` (per-voice state: event, generation, phase, envelope, filter) |
| `include/sound_backend.h` | Abstract backend interface (`set_tone`, `silence`, `publish_note`, `supports_note_events`, capabilities) |
| `include/sound_theme.h` | Theme structs (`SoundTheme`, `SoundStep`, `ADSREnvelope`, etc.) + parser class |
| `include/sound_sequencer.h` | Playback engine (`SoundPriority` enum, sequencer thread) |
| `include/sound_manager.h` | Singleton public API |
| `include/sdl_sound_backend.h` | SDL2 audio backend; owns `VoiceSlot voice_slots_[MAX_VOICES]` (desktop, `#ifdef HELIX_DISPLAY_SDL`) |
| `include/alsa_sound_backend.h` | ALSA PCM audio backend; owns `VoiceSlot voice_slots_[MAX_VOICES]` (Linux SBCs, `#ifdef HELIX_HAS_ALSA`) |
| `include/sound_synthesis.h` | Shared synthesis: waveform generation, biquad filter, `BiquadFilter` struct |
| `include/jz_pwm_sound_backend.h` | AD5X jz_pwm DMA backend (one-shot duty-encoded buffers) |
| `include/pwm_sound_backend.h` | PWM sysfs backend (AD5M buzzer) |
| `include/m300_sound_backend.h` | M300 G-code backend (Klipper via Moonraker) |
| `src/system/sound_theme.cpp` | Theme JSON parsing, note-to-freq, musical duration conversion |
| `src/system/sound_sequencer.cpp` | Sequencer thread + tick loop, LFO/sweep math; `publish_note_for_step()` for PCM backends |
| `src/system/sound_synthesis.cpp` | Waveform generation, biquad coefficient computation, filter application |
| `src/system/sound_manager.cpp` | Manager singleton, backend auto-detection, theme loading |
| `src/system/sdl_sound_backend.cpp` | SDL audio callback, per-sample envelope, biquad filter |
| `src/system/alsa_sound_backend.cpp` | ALSA render thread, per-sample envelope, biquad filter |
| `src/system/jz_pwm_sound_backend.cpp` | AD5X piezo backend: NoteEvent render → duty-encoded words → `fx-pwm serve` daemon socket |
| `src/system/pwm_sound_backend.cpp` | PWM sysfs writes (period, duty_cycle, enable) + PCM render thread (SCHED_IDLE pacing, silence auto-park, channel auto-export) |
| `src/system/m300_sound_backend.cpp` | M300 G-code formatting, frequency deduplication |
| `config/sounds/default.json` | Default theme (13 sounds, balanced) |
| `config/sounds/minimal.json` | Minimal theme (7 sounds, event/alarm only) |
| `config/sounds/retro.json` | Retro chiptune theme (13 sounds, 8-bit style) |
| `config/sounds/miami_vice.json` | Miami Vice theme (13 sounds, punchy 80s electronic) |
| `config/sounds/crocketts_theme.json` | Crockett's Theme (13 sounds, warm Jan Hammer synth) |
| `tests/unit/test_sound_theme.cpp` | Theme parser tests |
| `tests/unit/test_sound_sequencer.cpp` | Sequencer tests |
| `tests/unit/test_sdl_sound_backend.cpp` | SDL backend tests |
| `tests/unit/test_pwm_sound_backend.cpp` | PWM backend tests |
| `tests/unit/test_m300_sound_backend.cpp` | M300 backend tests |

---

## Existing Themes

| Theme | File | Sounds | Style |
|-------|------|--------|-------|
| **default** | `config/sounds/default.json` | 13 | Balanced, tasteful. Saw waves with filter sweeps for nav, clean square taps. |
| **minimal** | `config/sounds/minimal.json` | 7 | Event/alarm sounds only. No UI interaction sounds. |
| **retro** | `config/sounds/retro.json` | 13 | 8-bit chiptune. Fast arpeggios, narrow duty cycles, game-style chirps. |
| **miami_vice** | `config/sounds/miami_vice.json` | 13 | Punchy 80s electronic synth. E minor key, staccato square hits, driving rhythm. |
| **crocketts_theme** | `config/sounds/crocketts_theme.json` | 13 | Warm Jan Hammer saw waves. Bb minor key, long sustains, filter bloom. Startup plays the Crockett's Theme melody. |

---

## Testing

```bash
# All sound tests
./build/bin/helix-tests "[sound]"

# Specific component tests
./build/bin/helix-tests "[sound][theme]"     # Theme parser
./build/bin/helix-tests "[sound][sequencer]" # Sequencer
./build/bin/helix-tests "[sound][sdl]"       # SDL backend
./build/bin/helix-tests "[sound][pwm]"       # PWM backend
./build/bin/helix-tests "[sound][m300]"      # M300 backend

# Full suite with sharding (recommended)
make test-run
```

**Important**: Do NOT run `./build/bin/helix-tests` without a tag filter or `make test-run` sharding -- some non-sound tests hang in single-process mode.

### Testing Backends Without Hardware

- **PWM backend**: Constructor accepts a custom `base_path` parameter. Tests create a temp directory tree mimicking `/sys/class/pwm/pwmchip0/pwm6/` and verify file writes. Render-loop tests inject virtual clock/sleep seams so parking, catch-up, and pacing run without real time.
- **M300 backend**: Constructor accepts a `GcodeSender` callback (lambda). Tests capture sent G-code strings.
- **SDL backend**: Static helper methods (`generate_samples`, `compute_biquad_coeffs`, `apply_filter`) are public for direct unit testing without SDL audio hardware.

---

## ADSR Envelope Details

On PCM backends (SDL, ALSA), the envelope is computed **per-sample** in the audio render thread for sample-accurate timing. At each step boundary the sequencer calls `publish_note_for_step()`, which writes a complete `NoteEvent` into a `VoiceSlot` and bumps a generation counter. The audio callback detects the new generation, snapshots all parameters atomically, and `VoiceSlot::render_sample()` advances the ADSR state machine per-sample. This eliminates timing-dependent volume variations on very short sounds that arose from writing frequency, amplitude, and duty cycle as independent atomics.

On non-PCM backends (PWM, M300), `supports_note_events()` returns `false`. The sequencer still computes the envelope per-tick (~1-50ms) and passes the amplitude directly to `set_tone()`.

Given a step with duration `D` and ADSR values `(A, D, S, R)`:

```
Amplitude
  1.0 |    /\
      |   /  \
  S   |  /    \______________
      | /                     \
  0.0 |/                       \
      +---+---+-----------+----+---> time (ms)
      0   A  A+D     D-R       D

      |att|dec|  sustain  |rel |
```

- **Attack** (0 to A ms): Linear ramp from 0.0 to 1.0
- **Decay** (A to A+D ms): Linear ramp from 1.0 to sustain level
- **Sustain** (A+D to D-R ms): Hold at sustain level
- **Release** (D-R to D ms): Linear ramp from sustain to 0.0

If `A + D + R > duration`, the sustain phase is skipped and release starts immediately after decay.

If all ADSR values are 0, the envelope returns 1.0 (flat, no shaping).

---

## Sequencer Tick Internals

Each tick (~1ms on SDL, ~2ms on PWM, ~50ms on M300):

1. **Check stop request**: If `stop()` was called, end playback immediately.
2. **Process queue**: Pop all pending requests. Higher/equal priority preempts; lower priority is dropped.
3. **Advance elapsed time**: `step_state_.elapsed_ms += dt_ms` (capped at 5ms to prevent scheduling-delay jumps).
4. **Check step completion**: If elapsed >= total, call `advance_step()`. For PCM backends, `advance_step()` calls `publish_note_for_step()` to deliver the next `NoteEvent` to the `VoiceSlot`.
5. **Compute parameters** for current step (non-PCM backends only — PCM backends let the render thread own this):
   - Base frequency from `step.freq_hz`
   - Base amplitude from `step.velocity` * envelope
   - Sweep interpolation from `compute_sweep()` (linear, based on progress 0.0-1.0)
   - LFO offset from `compute_lfo()` (sinusoidal)
6. **Clamp outputs**: freq 20-20000 Hz, amplitude 0.0-1.0, duty 0.0-1.0
7. **Send to backend** (non-PCM only): `set_waveform()`, `set_filter()`, `set_tone()`

When a step completes, `advance_step()` increments the step index. If past the end of the steps array, it decrements `repeat_remaining`. If repeats remain, it resets to step 0. Otherwise, playback ends.

---

## M300 Backend Specifics

The M300 backend sends G-code commands through Moonraker's `gcode_script` API. Key behaviors:

- **Frequency deduplication**: If `set_tone()` is called with the same frequency as the last call (and amplitude > 0), it's a no-op. This prevents spamming Moonraker with redundant commands. Consequence: a held note emits ONE `M300 P50`, so notes longer than 50ms sound as a 50ms beep — themes render staccato on this backend.
- **Frequency clamping**: Hz values are clamped to 100-10000 (M300 safe range).
- **Duration in commands**: Each `M300 S{freq} P{dur}` uses `min_tick_ms()` (50ms) as the duration, bounding how long a stale command can ring if the sequencer is preempted mid-note.
- **Silence**: `M300 S0 P1` stops the beeper. Only sent if not already silent.
- **No amplitude or waveform control**: M300 is frequency-only. The printer firmware controls volume.

The M300 backend requires the printer to answer M300: a beeper `output_pin`, an M300 macro in the Klipper config, or a forced-on `speaker` capability override (see Backend Auto-Detection above).

---

## Future Extensions

- **Duty cycle parsing**: The `duty` field appears in theme JSON files (e.g., retro theme) but is not yet parsed into `SoundStep`. The sequencer defaults duty to 0.5 and only modulates it via LFO. Adding `duty` to `SoundStep` + the parser is a minor change.
- **Volume normalization**: Per-theme or per-backend volume curves.
- **M300 batch mode**: Pre-compute a sound sequence into a list of M300 commands and send them all at once, reducing round-trip latency.
