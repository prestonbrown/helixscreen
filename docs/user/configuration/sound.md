# Sound

Settings for sound effects. All of these are **per screen**. See [Settings: Sound](../guide/settings/sound.md) for the walkthrough. The Sound page appears only when HelixScreen finds a speaker or buzzer.

[Back to the Configuration Reference](../CONFIGURATION.md)

---

## Switches, volume and theme

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `sounds_enabled` (top level) | `false` | `true` or `false` | The master switch. With `false` nothing plays, neither interface sounds nor event sounds. It mutes playback but still starts the audio system; see `disable_sound` to prevent that. **Settings > Sound > Sounds**. |
| `ui_sounds_enabled` (top level) | `true` | `true` or `false` | Button taps and navigation clicks only, independent of event sounds such as the print-finished chime. It has no effect while `sounds_enabled` is `false`. **UI Sounds**. |
| `sounds.volume` | `80` | `0` to `100` | Master volume in percent. `0` is silent. It scales every sound, while the two switches above decide whether sounds play at all. **Volume**. |
| `sound_theme` (top level) | `"default"` | `"default"`, `"minimal"`, `"retro"`, `"miami_vice"`, `"crocketts_theme"`, or the file name of a theme you added | The style of every sound. **Sound Theme**. `minimal` plays only event sounds such as print complete, print canceled and errors, with no interface sounds. Themes you add are described in [Custom Sound Themes](../guide/settings/sound.md#custom-sound-themes). |
| `disable_sound` (top level) | `false` | `true` or `false` | Turns off all sound output and stops the audio system from starting. Use it where audio drivers are present but unusable and only add processor load, for example on the Artillery M1 Pro. The `--no-sound` flag does the same for one run. |
| `completion_alert` (top level) | `2` | `0` Off, `1` Notification, `2` Alert | How you are told when a print finishes or is canceled. See [Safety and Alerts](safety-alerts.md#notifications-and-print-completion). |

---

## Where sound goes

These settings are in the `sound` section. Leave them out to let HelixScreen choose.

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `sound.output_device` | `""` (the system's default sound card) | a sound card name | Which Linux sound card plays HelixScreen's sounds, for example an HDMI screen's speaker. Choose it at **Settings > Sound > Output Device**; edit it by hand only when that row is not shown. List your cards with `aplay -l`, for example `plughw:CARD=vc4hdmi0,DEV=0`. |
| `sound.pwm_channel` | `""` (off) | `"<chip>:<channel>"` | Plays sounds on a small buzzer wired to a PWM pin of the board instead of a sound card. `"0:0"` means `/sys/class/pwm/pwmchip0/pwm0`. When set it wins over every sound card, since a headphone jack (a Raspberry Pi's, for example) always looks available even with nothing plugged in. There is no menu for it, because choosing the wrong channel could drive something else on the same chip, such as a screen backlight. Restart HelixScreen after changing it. Setup steps are in [Sound: buzzer on a PWM pin](../guide/settings/sound.md#buzzer-on-a-pwm-pin). |

The `HELIX_PWM_SOUND` and `HELIX_ALSA_DEVICE` variables set the same two choices for a trial run without editing `settings.json`.
