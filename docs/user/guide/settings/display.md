# Settings: Display

**Settings > Display** controls the screen itself. Use it to turn the picture to match how the screen is mounted, make the interface bigger or smaller, set the brightness, and choose when the screen dims, sleeps or shows a screensaver.

On the Settings screen, the **Display** row shows your brightness and sleep time, for example *80% · sleep 10 min*. It reads *80% · never sleeps* when sleep is off, and just *Sleep 10 min* on a screen that can't change its brightness.

![Display settings, top of the page](../../../images/user/settings-display.png)

![Display settings, scrolled to the screensaver rows](../../../images/user/settings-display-2.png)

---

## Screen Rotation

Turns the picture to match how the screen is mounted: **Normal**, **90° Clockwise**, **180° Upside Down** or **270° Clockwise**. It's the first row on the page, so you can reach it without scrolling while the picture is sideways.

The new rotation applies the next time HelixScreen starts. When you pick one, HelixScreen offers to restart right away. Touch follows the new orientation on its own.

This row is hidden on the desktop simulator. There you rotate the window from your operating system instead.

> If taps land in the wrong place after rotating, the screen needs touch calibration, not a different rotation. See the [Touch Calibration guide](../touch-calibration.md).

---

## UI Scale

> Added in 1.1.

Sets how big everything is drawn. Choose **Automatic**, or a size from 100% to 200%.

**Automatic** picks a size from how densely your screen packs its pixels, so buttons and text stay the same physical size on any screen. On every supported printer this works out to 100%. It only makes the interface bigger on very sharp screens, such as an Android phone or tablet. The menu shows the size it picked, for example *Automatic (158%)*.

Pick a percentage yourself if you'd like things bigger or smaller, or if Automatic guesses wrong on a screen HelixScreen doesn't know. 100% always draws the interface at its designed size.

**The new size appears after you restart HelixScreen.** Your choice is saved straight away.

Each size keeps its own home screen layout, because a different size fits a different number of widgets. If you rearrange your home screen at one size and then switch, switching back brings your first arrangement back.

---

## Brightness

A slider from 10% to 100% (80% to start with). Only shown on screens whose backlight HelixScreen can control. Android handles brightness itself, so the slider is hidden there.

On the Creality K2, the bottom of the slider stays a little brighter than the number suggests. The K2 panel turns fully off below about 20% of its range, so HelixScreen never dims it that far.

---

## Screen Dim

How long the screen waits with nobody touching it before it dims: **Never**, **30 seconds**, **1 minute**, **2 minutes**, **5 minutes** or **10 minutes** (the default). Touch the screen to bring it back to full brightness; that touch presses nothing (see [Display Sleep](#display-sleep)).

On a screen that can't change its brightness, such as a phone or tablet running the Android app, nothing can dim, so the same timer starts the screensaver instead. There this row is called **Screensaver after** and offers the same choices. It only appears while a screensaver is selected; with the screensaver **Off** the row is hidden, because the timer would do nothing.

---

## Display Sleep

How long the screen waits before it turns off completely: **Never**, **1 minute**, **5 minutes**, **10 minutes**, **20 minutes** (the default), **30 minutes** or **1 hour**. Touch the screen to wake it.

Opening the HelixScreen screen page in a browser ([Screen Sharing](../screen-sharing.md)) wakes it. While someone has that page open, it dims and runs the screensaver as usual but does not go to sleep; it sleeps once they close the page. Webcam integrations that show the screen (Obico, Home Assistant, timelapse, a view-only webcam tile) never keep it awake.

Sleep never comes before the dim or screensaver stage. If a change would put it first, HelixScreen raises the sleep time to match **Screen Dim** (or **Screensaver after**) and says so. **Never** on either row is always allowed.

The touch that wakes the screen never presses anything, whether the screen was asleep, dimmed or showing a screensaver. The screen comes back as soon as your finger lands, but nothing under it is pressed, held or scrolled, however long you hold or drag. Lift your finger, then tap what you want.

Sleeping turns the backlight off. On the rare screen with no adjustable backlight, HelixScreen powers the panel down instead. If your screen stays faintly lit while asleep, or doesn't come back when you touch it, you can force the behavior in `settings.json`:

```json
"display": { "panel_power_off": 1 }
```

Use `0` if `1` made things worse.

---

## Screensaver

An animation that plays while the screen is idle. It starts when the screen would dim and stops when the screen goes to sleep. On a screen with no brightness control, the screensaver is the only sign that the screen is idle, and **Screensaver after** sets when it starts (see [Screen Dim](#screen-dim)).

The ESP32 firmware (the BigTreeTech K-Touch and other ESP32-S3 panels) has no screensavers, so this row and **Test Screensaver** don't appear there.

| Option | What you see |
|--------|--------------|
| **Off** | No screensaver. The screen just dims and sleeps |
| **Flying Toasters** | The classic flying toasters |
| **Starfield** | Stars streaming past |
| **3D Pipes** | The old Windows pipes: shiny shaded pipes in sixteen materials grow through the screen, turning at elbows and ball joints (and, once in a while, a teapot). When the screen fills it dissolves away in blocks and starts again from a slightly turned view |
| **Bouncing Printer** | Your printer drifts around and bounces off the edges, changing color at every wall. Land a corner and it celebrates |
| **Fireworks** | Fireworks over hills at night |
| **Bouncing Logo** | The HelixScreen logo drifts around a black screen and bounces off the edges, in its own colors |
| **3D Maze** | A walk through a brick maze, as in the old Windows screensaver, with its wide view, a few loops and small rooms, and halls that fade into the dark. Pictures of your own prints hang on the walls, taken from thumbnails your printer screen has already shown, along with your printer. Spinning HelixScreen icons, rocks that flip the view upside down, and a smiley at the exit |
| **Hyperspace** | A flight through glowing rainbow space: star flares stream past, a shimmering wormhole sweeps in and out, and now and then a shell of flares bursts ahead |
| **Flip Clock** (default) | A split-flap clock in the 12- or 24-hour format you chose under Time Format, one card per digit; only the digits that change flip. While a print runs it counts down the time left as hours, minutes and seconds, with the finish time and progress underneath. A paused print freezes the count under PAUSED. When a print ends it shows DONE, FAILED or CANCELED until someone touches the screen |
| **Fireflies** | Colored swarms of glowing fireflies chase each other through the dark, trailing long tails that drift in the wind; now and then a swarm stops, circles, splits in two or joins another |
| **Aqua Surface** | Dark, glassy water sloshing in slow motion, seen through the side of a tank, with crisp highlights, bubbles rising below the surface and the odd droplet flung from a crest. A clock of three rings sits in the water: the hour, the minute, and a small ring for the seconds, with the date underneath. While a print runs, the small ring shows its progress and the date line shows when it ends and how long is left |
| **High Fly** | A flight just above sunlit clouds under a deep blue sky, with tall clouds drifting past and, every minute or two, a flight through a cloud. A large, thin clock sits at the cloud line with the seconds and AM/PM beside it and the date on the left. While a print runs, the date shows when it ends and how long is left |
| **Random** | A different screensaver each time the screen goes idle, chosen from the ones your printer can run, and never the same one twice in a row while another can play. It is last in the list |

Each screensaver checks how much processor time it takes on your printer. If it would slow the printer down, it lowers its frame rate or detail, and once it has run comfortably for a while it steps back up again. A step down taken while a print runs lasts only for that print. If even its lowest setting is too much, your printer can't run it: it disappears from the list, and the Flip Clock (or the next screensaver your printer can run) plays in its place. If that happens during a print, the Flip Clock takes over for the rest of the print so you can still see its progress. The list then shows the one that plays. Your choice is still saved, so moving your settings to a faster printer brings it back. Only if no screensaver fits does the screen go black instead. The check runs again after an update.

### Print progress on the screensaver

Every screensaver shows how a print is going. The Flip Clock, Aqua Surface and High Fly show it in their own way. The others show a small card that drifts slowly around the screen: a ring that fills as the print goes, and the time left in large numbers (**1h05**, **23m**), or the percentage before the printer has worked out a time.

The ring turns amber while a print is paused and shows a pause mark. When a print ends the card shows a check mark in green, or a cross (failed) or a square (canceled) in red, and it stays until someone touches the screen, even if the print finished hours ago. While a screensaver is running, the end of a print doesn't wake the screen: the screensaver shows the result, and the usual print-finished message is waiting when you touch it.

**Show Print Progress** (under the Screensaver row, **On** by default) turns the card off during a running print. A paused or finished print still shows it.

### Test Screensaver

Appears under the Screensaver row when a screensaver is selected. Tap it to see the screensaver right away instead of waiting for the screen to go idle; it plays what the idle screen would. Touch the screen to stop it; like any wake touch, that touch presses nothing.

---

## Sleep While Printing

Lets the screen dim and sleep during a print, on the same timers as the rest of the time. **On** by default. Turn it off to keep the screen lit for the whole print, so you can check progress at a glance without touching it.

---

[Back to Settings](../settings.md) | [Next: Appearance](appearance.md)
