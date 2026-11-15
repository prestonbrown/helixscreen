# Filament Drying and Humidity

Back to [Filament Management](filament.md).

---

Many filament materials absorb moisture from the air over time. Wet filament prints poorly - you may see popping, stringing, reduced layer adhesion, or a rough surface finish. Drying the filament before or during a print removes that moisture and restores print quality. Hygroscopic materials that benefit most include Nylon, PA-CF, TPU, and PETG; PLA is less sensitive but still benefits after long storage.

HelixScreen groups all of this into **boxes**. A box is one enclosure it can measure, heat, or both. What counts as a box depends on your hardware: a box can be a whole unit, a group of slots inside a unit, or a single lane with its own heater and sensor.

The home screen can show every drying cycle at a glance with the [Filament Dryer widget](home-panel.md#filament-dryer-widget).

> **A box is not a unit.** The slot range printed under the box name is the only thing that tells you which lanes that box covers. Read it before you start a dry.

## Supported Systems

| System | What you get |
|--------|--------------|
| **Anycubic ACE Pro** | Temperature, humidity, and drying (built-in heated chamber) |
| **QIDI Box** | Temperature, humidity, and drying (PTC heater; QIDI PLUS4, Q2, MAX4) |
| **Happy Hare** | Temperature and humidity wherever sensors are configured, drying wherever a filament heater is. A setup with per-lane heaters and sensors reports each lane as its own box |
| **Creality CFS** | Temperature and humidity, one box per unit. The CFS has no heater, so there is nothing to start |

AFC (Box Turtle, OpenAMS), AD5X IFS, Snapmaker U1 and tool changers report no environment data at all, so nothing appears for them.

> **QIDI Box note:** QIDI Box drying control requires recent QIDI firmware that exposes the `box_extras` Klipper plugin. On older firmware the heater still works, but the session timer isn't tracked - the heater runs until you tap Stop.

## Opening a Box

On the **Filament** panel, any box with a reading or a heater shows a small badge beside its slots: a thermometer with the temperature, and a water drop with the humidity where a sensor exists. The drop fills up to the reading: nearly empty in a dry box, nearly full in a humid one. Tap the badge. That is the whole route - there is nothing to open under Settings.

What opens depends on how many boxes that unit has:

| Boxes | What you get |
|-------|--------------|
| One | That box's screen |
| Two to four of the same kind | That box's screen, with a tab per box across the top |
| More than four, or a mix of heated and unheated boxes | A list of boxes; tap a row to open one |

Each tab carries a small icon for the box it names, so you can tell which box is heating, which is waiting and which has finished without selecting it first.

A mix always goes to the list rather than to tabs, because a tab would hide the difference between a box you can heat and one you can only watch behind a selection you have to make first.

When the screen is showing fewer boxes than the printer has, a **View all boxes** line appears above the card. Tap it for the full list across every unit, with a count of units, boxes and dryers under the title. The list itself has no controls: each row gives the box name, its slot range, its current reading and a one-word status. Tap a row to open that box, where the controls are.

## Reading a Box

The title names the box, and the line under it gives its slot range.

- **Temperature** is always shown.
- **Humidity** is shown only where that box has its own humidity sensor. A box without one reads `--`, and the **Material Comfort** strip is hidden rather than guessed at. That is deliberate: with no humidity reading, a comfort verdict would be a made-up all-clear.
- **While a cycle runs**, the temperature reads current then target, a countdown gives the time left, and a progress bar fills.

The same rule reaches the status column in the box list: a box with a heater but no humidity sensor reads `--` there instead of a status word, and a box with no heater at all reads **passive**.

A box with no heater says so on its own screen, and points you at a sealed dry box with desiccant instead.

The reverse case also shows up: a heated box with no sensor of its own still gets a badge, with a dash where the temperature would be, so its controls are still one tap away.

## Starting and Stopping a Dry

The controls are on the right of the box's screen:

1. **Pick a preset** from the dropdown. Each entry names a material with its temperature and time, like "PLA 55°C/4h", and fills in both fields below.
2. **Or set the values yourself.** Tap the temperature field or the minutes field to bring up a keypad.
3. Tap **Start Drying**.

While a cycle runs, that same button reads **Stop Drying**, and it is also the cancel - there is no separate cancel button. Stopping is safe at any point: the heater goes off and the remaining time is discarded. There is no fan control.

Three things about the fields are worth knowing:

- **The duration field is in minutes**, not hours. Only the preset labels read in hours, so a preset called "4h" puts `240` in the field. Cycles shorter than an hour are fine.
- **The temperature range is that box's range**, and it is printed in the field's own label - "Temp °C (35-65)". The keypad won't accept anything outside it. A preset whose usual temperature sits above the ceiling is shown and sent clamped, so a 70 °C preset on a 65 °C box reads 65 °C, and 65 °C is what runs.
- **Your last values come back.** HelixScreen remembers the temperature and duration you last started a dry with, per printer, and fills them in the next time you open a box.

Left alone, HelixScreen picks the preset that suits the filament in that box - the most conservative one, meaning the lowest drying temperature among the materials loaded there, so a mixed box isn't dried at the temperature its most tolerant spool would take. Type a value yourself and that stops for the rest of the visit; pick a preset by hand and it starts again.

## Choosing Lanes to Dry (Happy Hare 4)

On a Happy Hare 4 unit that has its own heater on every lane (an EMU-style build), the box's screen shows a **Dry** row of lane chips where the lane line would be, one chip per heated lane on that unit. Every chip starts selected each time you open the screen, which dries the whole unit exactly as before.

- **Tap a chip to leave that lane out** of the next dry, and tap it again to put it back. The last selected chip can't be cleared: a dry needs at least one lane.
- **Rotate** appears when the unit has an eSpooler. Turn it on and the selected spools are turned while they dry. Happy Hare only rotates a spool whose filament end has been pulled back out of the gate and fixed to the spool, and it warns in the console about any lane that isn't.
- **Stop Drying with some chips cleared** stops only the selected lanes, and the notice names them ("Stopped drying Gate 2, Gate 3"); the others keep drying. With every chip selected it stops the whole cycle.

To make room for the chips, the Material Comfort list wraps into two columns while they show, and on the smallest screens (480x272) it is hidden; the humidity reading above it stays.

Happy Hare runs one drying cycle per unit, so pick your lanes before you tap **Start Drying**. A lane can't be added to a cycle that is already running: stop it and start again with the new selection. On these units the button and the time left follow the unit's cycle, so a lane that isn't drying still shows **Stop Drying** while its neighbors are.

Each lane's part in the cycle shows on its spool on the **Filament** panel, as a small badge on the spool face:

| Badge | Meaning |
|-------|---------|
| Heat waves | Drying now |
| Clock | Waiting for a heater (Happy Hare limits how many lane heaters run at once) |
| Check mark | Finished |
| Cross | Stopped before it finished |

A lane that isn't part of a cycle has no badge. Shared-heater units, Happy Hare 3 and every other filament system don't show the chips or the badges, and dry exactly as they always have.

## Material Comfort

Where a box has a humidity sensor, a **Material Comfort** strip sits below the readouts. It lists the materials loaded in that box, each with a verdict - **OK**, **Marginal** or **Too humid** - and the humidity ceiling that material is judged against, written as "PLA: OK (< 50%)". Materials differ enormously in what they tolerate: nylon and the high-temperature engineering plastics want a far drier box than PLA does, so the same reading can be comfortable for one spool and too wet for the one beside it. If none of the box's slots report a material, the strip falls back to the materials on your [temperature presets](temperature.md#reassigning-a-presets-filament-type).

The one-word status in the box list is a second, more general scale, applied to the box rather than to any material in it. So a box can read **OK** in the list while a demanding material reads **Marginal** in the strip. For what you actually have loaded, the strip is the one to go by.

## Typical Drying Parameters

These are general-purpose starting points. Your filament manufacturer's guidance takes priority.

| Material | Temperature | Time |
|----------|-------------|------|
| PLA | 45 °C | 4–6 h |
| PETG | 55 °C | 4–6 h |
| ABS / ASA | 60 °C | 4 h |
| TPU / TPE | 50 °C | 4–8 h |
| Nylon (PA) | 60 °C | 8–12 h |
| PA-CF / PA-GF | 60 °C | 8–12 h |

Filament that has been stored open for a long time may need the longer end of the range.

Remember that the duration field takes minutes: 4 hours is `240`, 12 hours is `720`.

## Several Boxes at Once

Boxes are measured independently, but they aren't always heated independently. Some hardware runs one heater at a time. Where your filament system reports that, starting a dry on a second box doesn't begin heating it - the box waits its turn, and the screen says so:

- A banner with a clock icon reads "Waiting for *box* to finish. One heater at a time." (or just "Waiting for another box to finish." when the box holding the heater can't be named)
- That box's tab shows a clock instead of the heat symbol
- No progress bar appears until the box is actually heating

Nothing has gone wrong and there's nothing to retry. When the running cycle ends, the waiting box takes the heater.

Where the hardware has no such limit, boxes heat independently and more than one cycle can run at a time.

Whichever shape you have, the readouts, the Material Comfort strip and the drying controls all belong to the box named in the title - not to the unit, and not to the first box on the printer.

