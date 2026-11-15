# Home Panel: Print Widgets

The widgets that work with a print in progress, or with the queue of prints to come: Job Queue, Speed / Flow, Z-Offset, Exclude Objects and Camera. Sizes and hardware needs are in the [Widget Catalog](home-widgets.md).

## Job Queue widget

The **Job Queue** widget is available when Moonraker has its `[job_queue]` component. What it shows depends on the queue and the printer:

- **Printer free, jobs waiting** - the name of the next job and a **Bed clear · Start next** button. At about 2x1 and wider it also shows the job's picture and its time estimate; larger sizes list the jobs behind it. At small sizes the button is shortened to **Start next**, and at the smallest to an icon.
- **A print running** - how many jobs are queued and **Next:** with the first one's name. "Starts automatically" appears only while a print runs and Moonraker's `automatic_transition` is on in `moonraker.conf`; otherwise you start each job yourself.
- **Queue starting a job** - "Starting" and the job's name while Moonraker loads it, and from the moment you tap **Start next** while the widget checks the job. The button is gone until the start ends.
- **Empty queue** - a short note that nothing is queued.

**Start next** starts the job in one tap, with the pre-print options it was queued with, or the defaults when it has none. The job leaves the queue only once the print has started. The same checks run as when you press Print in the file view: a material or spool warning asks you first, and a printer that is busy says so and keeps the job queued.

Two cases open the job in the file view instead, with a notice saying why: a job the widget cannot check (for example a file it cannot read), and a job where a tool has no loaded slot, where you pick **Map Filament…**. In the file view, the Print button reads **Checking file…** while the scan it needs is still running.

Tap anywhere else on the widget to open the [Job Queue Manager](#job-queue-manager).

![Job Queue widget at 2x1 - the next job, its time estimate, how many wait behind it, and the Bed clear · Start next button](../../images/user/home-job-queue-tile.png)

## Job Queue Manager

Tap the **Job Queue** widget to open the queue manager, a full-screen modal for managing your print queue.

### Queue State

At the top, you'll see the current queue state:

- **Queue: Ready** - the queue is active. It starts jobs in order by itself only if Moonraker's `automatic_transition` is enabled in `moonraker.conf`; otherwise you start each one (see [Printing - Queueing a Print](printing.md#queueing-a-print))
- **Queue: Paused** - the queue is paused; queued jobs won't start automatically

Tap the **Start** or **Pause** button to toggle the queue state.

### Job List

Below the state indicator, all queued jobs are listed with:

- **Filename**: the name of the queued G-code file
- **Time queued**: how long the job has been waiting (e.g., "Queued 2h 15m ago", "Just queued")

### Actions

**Open a job:** Tap any job in the list to open it in the file view with the pre-print options it was queued with already set. Check that the bed is clear, then tap **Print** - the job leaves the queue only once the print actually starts, so backing out of the file view leaves it queued. The tap only goes through when the printer is genuinely free - not just finished with the last print, but also not busy starting one (the heating, homing, and leveling before the first layer count as busy). While a job is preparing or printing, the job stays in the queue and a notice tells you so; try again once the printer is free.

Jobs queued from HelixScreen's file view carry their saved options; jobs queued from another interface open with their default options. See [Printing - Queueing a Print](printing.md#queueing-a-print).

**Delete a job:** Tap the **trash icon** on the right side of any job row. The job is immediately removed from the queue.

**Close:** Tap the **X** button in the top-right corner to close the modal.

![Job Queue modal, queue state, Start button, job list](../../images/user/home-job-queue.png)

### Sync with Other Interfaces

The job queue is managed by Moonraker, so jobs added from Mainsail, Fluidd, or the Moonraker API appear here automatically. Likewise, jobs deleted or started from HelixScreen are reflected in those other interfaces.

---

## Speed / Flow Widget

The Speed / Flow widget shows the speed and flow overrides you set in the [Print Tune overlay](printing.md#print-tune-overlay), so you can see at a glance whether a print is running faster or slower than sliced. A reading turns the warning color whenever it is not 100%.

![Speed / Flow at 2x2 during a print: both overrides, the live readings and the step buttons](../../images/user/home-widget-speed-flow.png)

| Size | What you see |
|------|--------------|
| 0.5x0.5 | Speed alone |
| 1x0.5, 0.5x1 | Speed and flow, each a small icon and number |
| 1x1 | Speed and flow. During a print the live toolhead speed (mm/s) and flow (mm³/s) sit under the percentages |
| 2x1 | Both readings with labels and, during a print, the live speed and flow beside each |
| 2x2 and larger, during a print | Large values with **-** and **+** buttons that change speed by 5% and flow by 1% |

If you chose mm/s in the Print Tune overlay, the live readings take the percentages' place on the sizes below 1x1, where there is room for only one number.

**Tap** anywhere except the buttons to open the Print Tune overlay. **Long-press** a value on the largest size to set it back to 100%. The buttons and the long press only work while a print is running.

---

## Z-Offset Widget

The Z-Offset widget shows the live Z-offset, the same number as the Z-Offset row in the Print Tune overlay, and lets you nudge and save it without opening anything. See [Z-Offset / Baby Steps](printing.md#z-offset--baby-steps) for how to judge a first layer.

![Z-Offset at 2x2 with an unsaved adjustment: the offset, baby-step buttons and Save](../../images/user/home-widget-z-offset.png)

| Size | What you see |
|------|--------------|
| 0.5x0.5 | The offset in millimeters, two decimals, under a small icon |
| 1x0.5, 0.5x1 | The offset to three decimals |
| 1x1 | The offset with a caption underneath |
| 2x1 | The same beside a small bar that flashes up or down as the offset moves, plus a **Save** button while there is something to save |
| 2x2 and larger | A large offset, a row of up and down arrows with a step button between them, and a full-width **Save** button. On a tool changer the active tool's name appears too |

- The **arrow buttons** raise or lower the offset by the chosen step. The **step button** between them cycles 0.05, 0.025, 0.01 and 0.005 mm, and the Print Tune overlay uses the same remembered step.
- The caption reads "mm Z-offset" until you change it. After that it shows how far you have moved since the last save, for example "+0.020 unsaved", and the number turns the warning color.
- **Save** writes the offset to your printer's config. Klipper restarts when it does, and you are asked to confirm first. It is grayed out while a print is running.
- On a tool changer the number is the active tool's own offset. Printers whose firmware keeps the offset itself, such as the FlashForge AD5M and AD5X on ZMOD, have nothing to save, so **Save** never appears.
- **Save** also stays hidden when **Save Z-Offset Buttons** is off in the [Motion settings](motion.md#motion-settings), which it is by default on tool changers.

**Tap** anywhere except the buttons to open the Print Tune overlay.

---

## Exclude Objects Widget

The Exclude Objects widget keeps the objects of a multi-object print in view from the home screen, so you can spot a failing part and get to it in one tap. It is a shortcut, not a control: skipping an object still happens on the Print Status screen, with the same confirmation and undo as always. See [Exclude Object](printing.md#exclude-object) for how skipping works.

![Exclude Objects at 2x2 during a six-object print with one skipped: the map, the object printing now, the counts and Skip objects](../../images/user/home-widget-exclude-map.png)

| Size | What you see |
|------|--------------|
| 0.5x0.5 | An icon over how many objects are left out of the total, for example "5/6" |
| 1x0.5 | The number of objects and the number skipped, on two lines |
| 0.5x1 | A small map over the objects left out of the total |
| 1x1 | The map over "5 of 6 printing" |
| 2x1 | The map beside the name of the object printing now and the counts |
| 2x2 and larger | The map over the object's name and the counts, with a **Skip objects...** button |

On the map the object being printed is drawn in the accent color with a heavier outline, the others a little dimmer, and a skipped object faded and hatched. Behind them sit the bed's 50 mm grid and its edge, wherever they fall in view.

When nothing is printing, or the print has a single object and so nothing to skip, the widget shows a quiet icon, captioned "No objects" or "Single object" from 1x0.5 up. A tap does nothing then.

**Tap** the widget, or **Skip objects...**, to open the Print Status screen with its objects map. Only one Exclude Objects widget can be placed.

---

## Camera Widget

The Camera widget shows a live view from your webcam. It works with any MJPEG stream configured in Moonraker (crowsnest, camera-streamer, ustreamer, etc.).

### Setup

1. Make sure your webcam is working in Mainsail or Fluidd first
2. Open **Edit Mode** on the Home Panel (long-press)
3. Tap **+** to open the Widget Catalog
4. Add the **Camera** widget
5. Resize to your preferred size (up to 4x3)

Tap the camera feed to open a **fullscreen view**. Tap again or press back to return.

### Rotation & Flip

If your camera is mounted at an angle or its image comes in mirrored, you can correct it without changing anything in Moonraker:

1. Enter **Edit Mode** (long-press the grid)
2. **Tap** the Camera widget to select it
3. **Tap the gear icon** in the upper-left corner to open the Camera configuration modal
4. Adjust the settings and tap **Save** (or **Cancel** to discard)

The modal offers:

- **Source** - which webcam the widget shows: **Automatic** (the printer's preferred camera), or one of your printer's named webcams. The choice is saved per widget, so two Camera widgets can show two different feeds. If the named camera disappears from Moonraker, the widget falls back to the automatic choice until it returns. See [Camera](camera.md) for details.
- **Rotation**: rotate the feed by **0°, 90°, 180°, or 270°**. The current rotation is highlighted.
- **Flip**: toggle **Horizontal** and/or **Vertical** mirroring. Each can be on or off independently.

Your rotation and flip choices are applied on top of (combined with) any flip Moonraker is already doing, so if Moonraker already mirrors the image, toggling the matching flip here cancels it back out.

### Stream Status

While the feed is loading or unavailable, the widget shows a status message instead of video:

- **No Camera**: no webcam is configured or detected yet
- **Connecting Camera...**: the stream is being established
- **Error messages**: if the stream can't be reached, the underlying error is shown so you know what went wrong

Once the first frame arrives, the status text disappears and the live image takes over.

At the smallest size (1x1) the widget shows only a camera icon and does not stream, tap it to open the fullscreen view, which starts the stream on demand. To save power, the stream also pauses automatically when the screen sleeps or when another overlay covers the widget, and throttles down while you're in Edit Mode.

### Frame Rate

The camera's target frame rate is read from your Moonraker webcam configuration (defaulting to 15 fps if not set). There's no separate frame-rate control in HelixScreen, adjust it in Moonraker (the same place Mainsail and Fluidd read it from).

### Performance Tip

For the best camera streaming performance on Raspberry Pi, install `libturbojpeg0`:

```bash
sudo apt install libturbojpeg0
```

This enables SIMD-accelerated JPEG decoding, which is **3-5x faster** than the built-in software decoder. HelixScreen detects and uses it automatically, no configuration needed. Without it, the camera still works fine, just with slightly higher CPU usage.

> **Note:** This only applies to Raspberry Pi. The installer attempts to install this package automatically, but if it fails (e.g., offline install), you can add it later with the command above.

---

**Back to:** [Home Panel](home-panel.md) | [Widget Catalog](home-widgets.md)
