# Command Line and Environment Variables

Two ways to change how HelixScreen starts without editing `settings.json`: options on the `helix-screen` command line, and variables in the `helixscreen.env` file. Both win over the saved settings, so they suit one-off tests and the rare case where the settings file cannot be reached.

[Back to the Configuration Reference](../CONFIGURATION.md)

---

## Command-line options

```bash
helix-screen [options]
```

### Display

| Option | What it does |
|--------|--------------|
| `-s, --size <size>` | Sets the screen size: `micro` (480x272), `tiny` (480x320), `small` (480x400), `medium` (800x480, the default), `large` (1024x600), `xlarge` (1280x720), or any `WxH` such as `1920x1080`. |
| `--layout <type>` | Overrides the detected layout: `auto`, `standard`, `ultrawide`, `portrait`, `micro`, `micro-portrait`, `tiny`, `tiny-portrait`. Ultrawide and the portrait layouts are alpha; see [Display](display.md#rotation-size-and-layout). |
| `--rotate <degrees>` | Rotates the display: `0`, `90`, `180` or `270`. |
| `--dpi <n>` | Sets the display DPI (50 to 500, default 160). |
| `--dark`, `--light` | Uses the dark or the light theme for this run. |
| `--skip-splash` | Skips the splash screen. |
| `--no-sound` | Turns off all sound output and stops the audio system from starting. |
| `--render-2d`, `--render-3d` | Forces the G-code viewer to the 2D layer renderer or the 3D renderer for this run. |

### Navigation and setup

| Option | What it does |
|--------|--------------|
| `-w, --wizard` | Runs the first-run setup wizard. |
| `--skip-wizard` | Does not run the first-run wizard. |
| `--calibrate-touch` | Runs touch calibration at start. |

### Connection

| Option | What it does |
|--------|--------------|
| `--moonraker <url>` | Connects to this Moonraker address instead of the saved one, for example `ws://192.168.1.100:7125`. |

### Logging

| Option | What it does |
|--------|--------------|
| `-v, --verbose` | Raises the log detail: `-v` info, `-vv` debug, `-vvv` trace. |
| `--log-level <level>` | Sets the level directly: `trace`, `debug`, `info`, `warn`, `error`, `critical` or `off`. |
| `--log-dest <dest>` | Chooses where the log goes: `auto`, `journal`, `syslog`, `file` or `console`. |
| `--log-file <path>` | The log file, used when the destination is `file`. |

### Diagnostics and information

| Option | What it does |
|--------|--------------|
| `--debug-touches` | Draws a ripple at every touch to check touch accuracy. |
| `--screenshot [sec]` | Takes a screenshot after a delay (default 2 seconds). |
| `-t, --timeout <sec>` | Quits automatically after this many seconds (1 to 3600). |
| `-h, --help` | Shows the help text. |
| `-V, --version` | Shows the version. |

```bash
# Start in dark mode and skip the splash screen
helix-screen --dark --skip-splash

# Connect to a different printer for this run
helix-screen --moonraker ws://192.168.1.50:7125

# Debug logging
helix-screen -vv

# Take a screenshot after 5 seconds
helix-screen --screenshot 5
```

Test-mode options (`--test` and the `--real-*` family) and the remote-control options are for developers and are not listed here. They are described in the developer documentation.

---

## Environment variables

Put these in your `helixscreen.env` file, normally `~/helixscreen/config/helixscreen.env`, or set them in the environment before running the program by hand. Do not put them in `/etc/systemd/system/helixscreen.service`, because that file is rewritten from the install folder's template on every start and your edits are discarded.

The rules for the file:

- Values are plain text. One pair of surrounding quotes is removed, and nothing is expanded or run. A line whose value contains `$VAR`, `$(...)`, `${...}` or a backtick is skipped with a warning, so write the final value itself.
- Only the variables listed on this page and in the file's own comments are read. Any other line is ignored and noted in the log.
- A variable used as an on-off switch is on for `1`, `true`, `yes` or `on` and off for anything else, including `0`, `false` and an empty value.
- The file is used only when it is owned by root or by the user HelixScreen runs as and nobody else can write to it. If a stray write permission is the only fault, HelixScreen repairs it to `0644` and loads the file.
- Most people never need any of this. The settings file is preferred. Variables are for testing and for when the settings file cannot be reached.

### Display and input

| Variable | What it does |
|----------|--------------|
| `HELIX_DISPLAY_BACKEND` | Chooses the display path: `drm`, `fbdev`, `sdl` or `egl`. Without it HelixScreen probes for GPU presentation and uses it when a hardware renderer answers. `drm` declines that and presents through plain DRM buffers, and `egl` takes the GPU path without probing. |
| `HELIX_DRM_DEVICE` | The DRM device, for example `/dev/dri/card1`. On a Raspberry Pi 5 use `card1` for a DSI screen and `card2` for HDMI. Same as the `drm_device` setting. |
| `HELIX_FB_DEVICE` | Accepted in the file but not used: the fbdev display path always draws to `/dev/fb0`. |
| `HELIX_DISPLAY_ROTATION` | Rotates the display: `0`, `90`, `180` or `270`. It outranks `--rotate` and the `rotate` setting. |
| `HELIX_COLOR_SWAP_RB` | Swaps the red and blue channels, `1` to force it on and `0` to force it off. It fixes red and blue swapped on a BGR panel and applies only to the fbdev path. |
| `HELIX_BACKLIGHT_DEVICE` | Forces the backlight method: `sysfs`, `allwinner`, `brightness` (Creality Sonic Pad), or `none` to disable backlight control. It fixes a brightness slider that does nothing. |
| `HELIX_DPI` | Overrides the panel's DPI (50 to 500). Above 225 it enlarges the interface (500 is about 188%). At or below 225 it changes nothing, and it never shrinks the interface. The **UI Scale** setting overrides it unless that is Automatic. |
| `HELIX_SCREEN_SIZE` | Forces the screen resolution: `micro`, `tiny`, `small`, `medium`, `large`, `xlarge` or `WxH`. It is the saved equivalent of `-s`, and `-s` wins over it. Despite the name this is a resolution, not a scale. Use **Settings > Display > UI Scale** to scale the interface. |
| `HELIX_TOUCH_DEVICE` | The touch input device, for example `/dev/input/event1`. |
| `HELIX_TOUCH_SWAP_AXES` | `1` swaps the touch X and Y axes. |
| `HELIX_TOUCH_CALIBRATE` | Runs touch calibration at the next start. Remove the line after calibration succeeds, because the variable does not clear itself. |
| `HELIX_MOUSE_DEVICE` | A USB mouse device, for example `/dev/input/event4`. |
| `HELIX_KEYBOARD_DEVICE` | A USB keyboard device, for example `/dev/input/event5`. |
| `HELIX_REQUIRE_POINTER` | `0` lets HelixScreen start with no touchscreen or mouse, for headless setups. |
| `HELIX_SCROLL_GUARD`, `HELIX_SCROLL_GUARD_COOLDOWN_MS` | Override the Scroll Guard switch and its cooldown. See [Touch and Input](input.md). |
| `HELIX_DEBUG_TOUCH` | `1` logs touch and calibration detail, which helps when reporting a calibration problem. |

### Theme, G-code and sound

| Variable | What it does |
|----------|--------------|
| `HELIX_THEME` | Overrides the theme, for example `dracula`, `nord` or `gruvbox`. |
| `HELIX_GCODE_MODE` | `3D` or `2D`, in exactly those capital letters. Unset means Auto, and any other value means 2D. |
| `HELIX_GCODE_STREAMING` | `on`, `off` or `auto`. Overrides `streaming_mode`. |
| `HELIX_SSAO` | `0` turns off the extra shading and anti-aliasing of the 2D G-code preview, which is on by default. |
| `HELIX_ALSA_DEVICE` | The sound card, for example `plughw:CARD=vc4hdmi0,DEV=0`. Use it when the default card is the wrong one. List the cards with `aplay -l`. |
| `HELIX_PWM_SOUND` | A buzzer on a PWM pin, as `<chip>:<channel>`, for a trial run. For a permanent setup use `sound.pwm_channel` in [Sound](sound.md). |

### Logging and startup

| Variable | What it does |
|----------|--------------|
| `HELIX_LOG_LEVEL` | `trace`, `debug`, `info`, `warn`, `error`, `critical` or `off`. It overrides the in-app **Log Level**, so leave it unset unless you need a level before the settings file loads. |
| `HELIX_LOG_DEST` | `auto`, `journal`, `syslog`, `file` or `console`. `auto` never chooses a file. |
| `HELIX_LOG_FILE` | The log file, used only when the destination is `file`. It must be an absolute path ending in `.log` under `/tmp`, `/var/log` or the install folder. |
| `HELIX_SKIP_SPLASH` | `1` skips the splash screen. |
| `HELIX_DISABLE_AUTO_UPDATES` | `1` declares that updates are managed outside HelixScreen and hides its own update flow. Meant for firmware images. |
| `HELIX_USB_AUTOMOUNT` | `0` stops HelixScreen mounting USB drives itself. Without it, a root HelixScreen mounts an unmounted removable drive read-only after three seconds when nothing else on the device does. |

### Example

```bash
# ~/helixscreen/config/helixscreen.env
HELIX_DRM_DEVICE=/dev/dri/card1
HELIX_TOUCH_DEVICE=/dev/input/event0
```

Developer variables, such as the mock and test variables in the [Environment Variables Reference](../../devel/ENVIRONMENT_VARIABLES.md), are skipped when they appear in this file. Set them in the environment of a command you start by hand.
