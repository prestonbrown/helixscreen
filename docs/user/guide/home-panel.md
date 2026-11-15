# Home Panel

![Home Panel](../../images/user/home.png)

The Home Panel is your printer's dashboard: a grid of widgets spread across one or more pages, like the home screens on a phone. You choose which widgets appear, where they go and how big they are, and you can rearrange everything by dragging.

This page covers the grid and its pages. The rest of the Home Panel is on these pages:

| Page | What it covers |
|------|----------------|
| [Edit Mode](home-edit-mode.md) | Moving, resizing, adding, removing and configuring widgets, resetting the layout, copying the layout from a printer's own screen |
| [Widget Catalog](home-widgets.md) | Every widget with its sizes and hardware needs, Stack and Carousel modes, what each tap does, Macro Button confirmation |
| [Print widgets](home-widgets-print.md) | Job Queue, Speed / Flow, Z-Offset, Exclude Objects, Camera |
| [Filament widgets](home-widgets-filament.md) | Clog Detection, Filament Buffer, Filament Dryer |
| [Control and system widgets](home-widgets-controls.md) | Toolhead, Bed Mesh, Tool Switcher, GCode Console, Shutdown/Reboot, Host Health, LED controls |
| [Printer Manager](printer-manager.md) | Printer name, model and picture, status chips, custom images, detected hardware, switching between printers |

---

<a id="the-widget-grid"></a>
## The Widget Grid

Your dashboard is built from **widgets**, cards that show printer information or give you a control. Widgets sit on a grid:

- The grid is sized from your screen so that **cells come out square**. A small 800x480 panel gets 6 columns by 4 rows, and a 1024x600 or 1280x720 panel gets 8 by 5. The table under [Grid dimensions](#grid-layout-details) lists more screens.
- A widget takes one or more cells. Many widgets can also take half cells.
- Widgets cannot overlap.
- **Everything saves automatically** and survives restarts and updates.

Most widgets share one **card background** that flows around a group of neighbors, so a cluster of small readouts looks like one panel. A few widgets bring their own surface: Print Status, Camera, Nozzle Temperatures and Temperature Graph. Printer Image and Tips float on the bare background. When you move a widget the card reshapes around the new arrangement.

The first time HelixScreen starts it builds a default layout for your screen and your printer: the printer image, print status, temperatures and the other widgets most people use. You can change all of it. A few things about the default layout are on purpose:

- **Notifications sit in a corner**, so the bell is in the same place on every printer.
- **Only hardware you have is shown.** The chamber temperature tile, for example, appears only on printers with a chamber sensor or heater.
- **Empty cells are intentional.** The default never pads a layout with filler. A gap means the printer has nothing for that spot, and you can put any widget from the Widget Catalog there.

### Ultrawide and portrait screens

The grid is worked out from the screen, so an unusually shaped screen gets an unusually shaped grid. A 1920x440 ultrawide screen gets 23 columns by 5 rows, and a 480x800 portrait screen gets 4 columns by 6 rows. A tall, narrow 320x1480 panel gets 4 columns by 17 rows.

Ultrawide and portrait screens start from their own default layouts rather than a stretched landscape one. The portrait layout puts **Tips** as a full-width band above the print status card, except on the shortest portrait panels. The ultrawide layout has two bands: the printer image and the readouts on top, Print Status and the temperature graph below. Anything a default layout leaves out is still in the Widget Catalog.

On portrait panels, buttons, input fields and headers are sized from the screen's height, so a tall screen gets taller, easier-to-hit controls.

> Portrait is still alpha overall. Print Status, Print Tune, Motion, Bed Mesh and the temperature graph rearrange for a tall screen, and other panels fall back to the landscape layout. See [Ultrawide or portrait screen looks stretched, cramped, or clipped](../TROUBLESHOOTING.md#ultrawide-or-portrait-screen-looks-stretched-cramped-or-clipped).

### Emergency Stop and the tool badge

While a print is preparing, running or paused, a red **Emergency Stop** button appears in the navigation bar and halts all printer motion at once. By default a confirmation dialog appears first; turn that off with **Settings > Safety & Alerts > E-Stop Confirmation**. On a printer with more than one extruder, the nozzle icon on temperature tiles and on the print status card carries a small **tool badge**: the active tool's number beside the icon. Single-extruder printers never show it.

---

## Multiple Pages

Your dashboard can have several pages, each with its own grid.

### Navigating Between Pages

- **Swipe left or right** on the widget grid to change page.
- **Dots** at the bottom of the screen show which page you are on. With a single page the dots are hidden.
- One swipe past your last page is the **Add page** tile (see below), so you can swipe over to it even with one page.

### The Main Page

One page is the **main page**, the first one by default. It is the page shown when you connect to your printer.

- Tapping **Home** in the navigation bar from another panel returns you to the page you were last viewing.
- Tapping **Home** again while you are already on the Home panel jumps to the main page.

So the main page is never more than two taps of Home away.

### Adding a Page

**With the Add page tile.** A tile with a **+** and the caption **Add page** sits one swipe past your last page.

1. Swipe to your last page, then once more. The **Add page** tile slides in.
2. Tap the **+**. The new page is created and you land on it, empty and ready for widgets (see [Adding a Widget](home-edit-mode.md#adding-a-widget)).

The tile works whether or not you are in Edit Mode.

**By dragging a widget.** In [Edit Mode](home-edit-mode.md), pick up a widget and carry it across the right edge of your last page. An empty page slides in with the widget still under your finger. Drop the widget anywhere on it and the page is created with the widget on it. Drag back instead and no page is created.

At the **left edge of your first page** nothing slides in. Drag the widget until most of it is past that edge (over the navigation bar, in landscape) and let go. The new page is created **before** your first one, with the widget at its left side, and every other page shifts one to the right with its widgets.

If the widget you dragged was the last one on its page, that page is removed when the new one is created.

### Page Limit

A dashboard holds up to **8 pages**. At the limit there is no Add page tile, and dragging a widget past an edge creates nothing.

### Deleting a Page

In Edit Mode, when the page you are looking at is not the main page and you have more than one page, a red **trash button** appears in the navigation bar. Tap it and confirm **Delete** in the dialog that asks "Remove this page and all its widgets?". The page and every widget on it are removed. The main page cannot be deleted. A page also removes itself when you move or remove its last widget (see [Moving a Widget to Another Page](home-edit-mode.md#moving-a-widget-to-another-page)).

---

<a id="edit-mode"></a>
## Edit Mode

Edit Mode is how you rearrange the dashboard. **Long-press anywhere on the widget grid** to enter it, then drag widgets, resize them, add them from the Widget Catalog or remove them. Tap **Done** in the navigation bar when you finish. The full walkthrough is on the [Edit Mode](home-edit-mode.md) page.

If Edit Mode starts by accident, for example when a finger rests on a tablet lying flat, turn it off with **Settings > Touch & Input > Allow Home Screen Editing**, or raise **Settings > Touch & Input > Long Press Time**.

<a id="available-widgets"></a>
## Available Widgets

HelixScreen has more than 40 widgets in five groups: Print & Status, Temperature & Cooling, Filament, Controls and System. The [Widget Catalog](home-widgets.md) page lists every one with its default, smallest and largest size and what hardware it needs. A widget your printer has no hardware for still appears in the catalog, dimmed and marked as unavailable, and comes back on its own if the hardware shows up later.

---

<a id="grid-layout-details"></a>
## Grid Layout Details

### Grid dimensions

HelixScreen divides your screen by a target cell size, so cells come out roughly square and a bigger screen gets more cells rather than bigger ones. A widget takes about the same share of a 480x272 panel as it does of a 1280x720 one.

| Screen | Grid (columns x rows) | Total cells |
|--------|-----------------------|-------------|
| 320x240 | 4 x 3 | 12 |
| 480x272 | 6 x 4 | 24 |
| 480x320 | 5 x 4 | 20 |
| 480x400 | 5 x 5 | 25 |
| 800x480 | 6 x 4 | 24 |
| 1024x600 | 8 x 5 | 40 |
| 1280x720 | 8 x 5 | 40 |
| 1920x1080 | 12 x 7 | 84 |
| 480x800 (portrait) | 4 x 6 | 24 |
| 600x1024 (portrait) | 5 x 7 | 35 |
| 320x1480 (tall portrait) | 4 x 17 | 68 |
| 1920x440 (ultrawide) | 23 x 5 | 115 |

Rotating a screen turns the grid with it, give or take a cell. A 1024x600 panel gives 8 by 5 and the same panel mounted portrait gives 5 by 7, because the navigation bar and margins take a different bite out of each orientation.

### Auto-placement

When a widget has no position yet (newly added, or after a reset), HelixScreen places it for you. Larger widgets go first so they get room, small ones fill the gaps, and the scan runs from the top left to the bottom right.

<a id="when-the-grid-is-full"></a>
### When the grid is full

If every cell is taken, a widget with nowhere to sit is set aside rather than switched off. It stays enabled and comes back by itself when a cell frees up: when you remove another widget, when hardware another widget depends on goes away, or when the same layout is shown on a screen with a bigger grid. You do not have to add it again.

You see a "'Fan Speeds' removed - grid full" message only when the widget was on screen and lost its spot. A widget that never had a spot is set aside quietly.

A widget that is too big for the grid at any size cannot be placed however much you remove, so it is switched off and put back in the Widget Catalog, and the message says so. See [A widget disappeared from the home screen](../TROUBLESHOOTING.md#a-widget-disappeared-from-the-home-screen).

### What Happens on Upgrade

When you update HelixScreen and it adds new widgets:

- **Your layout is preserved.** Nothing moves.
- New widgets are added with their default enabled or disabled state, and an enabled one places itself into free space.
- If the grid is full, a new widget stays enabled but unplaced until you make room. It still shows in the Widget Catalog, where adding it tells you "Not enough room for this widget" instead of failing silently, and it drops into place by itself as soon as a cell frees up.

If you downgrade to a version that does not know a widget, the widget is dropped from your layout, and upgrading again brings it back.

**Updates that change the grid.** A saved position means "column 5, row 3, two cells wide", so when an update changes how many cells your screen gets, those numbers mean something different. HelixScreen converts your arrangement onto the new grid instead of resetting it:

- **The arrangement carries over in proportion.** A widget that filled the left third of the screen still fills the left third, and widgets that touched still touch.
- **Sizes can shift slightly.** A widget lands on the nearest size it is allowed to hold. Some widgets have a one-cell minimum and grow to it.
- **A widget that will not fit is re-placed automatically**, and only that widget.
- **Which widgets you have is remembered.** A widget you deleted stays gone, and one you added stays added, including extra Macro Buttons and Power widgets.
- **Per-widget settings are kept**: display modes, assigned fans, macros.
- Extra pages are kept, and each widget is converted within its own page.

The conversion happens the first time the home screen is drawn after the update, so the layout you see on that first boot is the one that is saved. Your printer keeps working throughout.

**Going back to 1.0.** A 1.0 build saves positions in its own grid's units, so when you return to 1.1 the home screen can come back rearranged or reset to defaults. Arrange it again in Edit Mode.

---

<a id="printer-manager"></a>
## Printer Manager

Tap the **Printer Image** widget to open the Printer Manager, where you change the printer's name, model and picture, see what HelixScreen detected, and switch between printers. If your layout has no Printer Image widget, open **Settings > Printer** instead. The [Printer Manager](printer-manager.md) page has the details.

---

## Find a topic

Older links to topics that moved still land here. Each entry points to the page that now holds the topic.

- <a id="entering-edit-mode"></a><a id="selecting-a-widget"></a><a id="moving-a-widget"></a><a id="resizing-a-widget"></a><a id="removing-a-widget"></a>[Entering edit mode, selecting, moving, resizing and removing a widget](home-edit-mode.md#entering-edit-mode)
- <a id="hardware-gated-widgets"></a>[Hardware-gated widgets](home-widgets.md#hardware-gated-widgets)
- <a id="emergency-stop"></a>[Emergency stop](motion.md#emergency-stop) and the [tool badge](#emergency-stop-and-the-tool-badge)<a id="active-tool-badge"></a>
- <a id="managing-multiple-printers"></a><a id="switching-between-printers"></a><a id="removing-a-printer"></a>[Managing, switching and removing printers](printer-manager.md#managing-multiple-printers)
- <a id="using-custom-printer-images"></a>[Using custom printer images](printer-manager.md#using-custom-printer-images)
- <a id="rotation--flip"></a><a id="stream-status"></a>[Camera rotation, flip and stream status](home-widgets-print.md#rotation--flip)
- <a id="multiple-pages-note"></a>Page navigation, adding and deleting pages: this page, above.
- <a id="configuring-a-widget"></a>[Configuring a widget](home-edit-mode.md#configuring-a-widget) (the gear button)
- <a id="adding-a-widget"></a>[Adding a widget](home-edit-mode.md#adding-a-widget) and <a id="resetting-to-defaults"></a>[resetting to defaults](home-edit-mode.md#resetting-to-defaults)
- <a id="macro-button-confirmation"></a>[Macro Button confirmation](home-widgets.md#macro-button-confirmation)
- <a id="display-modes-stack-vs-carousel"></a>[Stack and Carousel modes](home-widgets.md#display-modes-stack-vs-carousel)
- <a id="job-queue-widget"></a><a id="job-queue-manager"></a>[Job Queue widget and manager](home-widgets-print.md#job-queue-widget)
- <a id="speed--flow-widget"></a>[Speed / Flow widget](home-widgets-print.md#speed--flow-widget)
- <a id="z-offset-widget"></a>[Z-Offset widget](home-widgets-print.md#z-offset-widget)
- <a id="exclude-objects-widget"></a>[Exclude Objects widget](home-widgets-print.md#exclude-objects-widget)
- <a id="camera-widget"></a>[Camera widget](home-widgets-print.md#camera-widget)
- <a id="clog-detection-widget"></a>[Clog Detection widget](home-widgets-filament.md#clog-detection-widget)
- <a id="filament-buffer-widget"></a>[Filament Buffer widget](home-widgets-filament.md#filament-buffer-widget)
- <a id="filament-dryer-widget"></a>[Filament Dryer widget](home-widgets-filament.md#filament-dryer-widget)
- <a id="toolhead-widget"></a>[Toolhead widget](home-widgets-controls.md#toolhead-widget)
- <a id="bed-mesh-widget"></a>[Bed Mesh widget](home-widgets-controls.md#bed-mesh-widget)
- <a id="tool-switcher-widget"></a><a id="switching-tools"></a>[Tool Switcher widget and switching tools](home-widgets-controls.md#tool-switcher-widget)
- <a id="g-code-console-widget"></a>[GCode Console widget](home-widgets-controls.md#gcode-console-widget)
- <a id="shutdownreboot-widget"></a>[Shutdown/Reboot widget](home-widgets-controls.md#shutdownreboot-widget)
- <a id="host-health-widget"></a>[Host Health widget](home-widgets-controls.md#host-health-widget)
- <a id="led-controls"></a>[LED controls](home-widgets-controls.md#led-controls)
- <a id="changing-the-printer-name"></a>[Changing the printer name](printer-manager.md#changing-the-printer-name)
- <a id="changing-the-printer-image"></a>[Changing the printer image](printer-manager.md#changing-the-printer-image)

---

**Next:** [Printing](printing.md) | **Prev:** [Supported Printers](supported-printers.md) | [Back to User Guide](../USER_GUIDE.md)
