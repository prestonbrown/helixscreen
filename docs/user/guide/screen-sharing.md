# Screen Sharing and Remote Control

HelixScreen can show its screen in your web browser, live, and let you use it from there: tap buttons, scroll lists, drag sliders, exactly as if you were standing at the printer. The screen also appears as a webcam called **HelixScreen Screen** in Mainsail and Fluidd, so it sits next to your printer camera.

Useful when:

- you want to check what the printer's screen says from your desk;
- you are helping someone else with their printer and want to see what they see;
- your printer's screen is hard to reach, small, or mounted somewhere awkward.

Both sharing and control are **off** until you turn them on, on the printer itself.

If HelixScreen controls more than one printer, each printer keeps its own Screen Sharing settings. Turning sharing on while one printer is selected shares the screen with that printer's Mainsail or Fluidd only. Switching to another printer uses that printer's settings, so the screen stops being shared if sharing is off there.

![HelixScreen in a desktop browser](../../images/user/screen-sharing-browser.png)

---

## Turning it on

1. On the printer, open **Settings > Connection > Screen Sharing**.
2. Turn on **Share Screen** ("Show this screen in a web browser"). The screen is now visible to browsers on your network, and a **HelixScreen Screen** webcam appears in this printer's Mainsail and Fluidd.
3. To use the screen from a browser too, turn on **Allow Remote Control** ("Let a browser tap and drag on this screen").

**Add to Mainsail/Fluidd Webcams** is on unless you turn it off. Turn it off to keep sharing the screen at its browser address without listing it among the printer's webcams. Turning it off removes the **HelixScreen Screen** webcam, and turning it back on adds it again. This row and **Allow Remote Control** are greyed out while **Share Screen** is off.

![Screen Sharing settings](../../images/user/screen-sharing-settings.png)

On some printers a **Pairing Code** row and a **Forget Paired Browsers** row appear once **Allow Remote Control** is on. See [Two ways it works](#two-ways-it-works) below for which printers, and why.

> The **Screen Sharing** row is hidden on printers whose own firmware already shares the screen, such as the Snapmaker U1 with its stock screen page. Use that instead.
>
> Screen sharing isn't available in the Android app or on the BigTreeTech K-Touch.

It is off by default, and the setting belongs to the printer you have selected.

Turning **Share Screen** off stops it straight away and removes the webcam from Mainsail and Fluidd.

When you switch to a printer with sharing off, the previous printer's **HelixScreen Screen** webcam stays in its list but shows no picture until you switch back to that printer.

---

## Opening the screen in a browser

**From Mainsail or Fluidd:** open the webcam panel (or the webcam page) and pick **HelixScreen Screen**.

- With **Allow Remote Control** off, it shows as a live picture, like any camera.
- With it on, it shows as a page you can click and drag on.

**Directly:** the address depends on how your printer is set up (next section).

| Your printer | Address |
|---|---|
| Asks for a pairing code | `http://<printer address>:7130/` |
| No pairing code | `http://<printer address>/helixscreen/screen/` (the same address you use for Mainsail, followed by `/helixscreen/screen/`) |

`<printer address>` is the IP address or name you already use to reach Mainsail or Fluidd, for example `http://192.168.1.50:7130/`.

The page works on phones and tablets too: tap and drag with your finger.

---

## Two ways it works

Who may see and use the screen depends on what your printer runs in front of Moonraker.

### Printers that ask for a pairing code

Any printer where HelixScreen runs on a different computer from the printer's Moonraker, and any printer whose web server cannot check Moonraker's access rules. That includes the Creality K1 series and K2 series. If the **Pairing Code** row appears, your printer works this way.

- **Watching** is open to anyone on your home network.
- **Controlling** needs pairing. The first time a browser tries to use the screen, it asks for a six-digit code:

![The pairing prompt](../../images/user/screen-sharing-pair.png)

  1. On the printer, open **Settings > Connection > Screen Sharing** and read the **Pairing Code**.
  2. Type it into the browser and press **Pair**.
  3. That browser is now paired and won't ask again. Each new browser or device needs the code once.

- Up to eight browsers stay paired at once. Pairing a ninth drops the one used least recently, and a paired browser stays paired for a year.
- **Forget Paired Browsers** (under the pairing code) asks you to confirm, then signs every browser out and makes a new code. Use it if someone you shared the code with should no longer have access.
- After five wrong codes in a minute the browser has to wait before trying again. After fifty wrong codes the printer changes the code on its own.

### Printers where Moonraker decides (no code)

**Raspberry Pi and similar boards running MainsailOS, or set up with KIAUH**, where Mainsail or Fluidd is served by nginx.

The installer adds the screen to that web server, and Moonraker's own access rules decide who gets in, exactly as they do for Mainsail:

- If your computer is on Moonraker's **trusted clients** list (the usual home setup), the screen opens with no code.
- If Moonraker has logins turned on and your computer is **not** trusted, the screen won't show. A webcam picture can't carry a Mainsail login, so only trusted addresses can see it.

This is set up when HelixScreen is installed or updated, and only when the printer's web server supports it and Moonraker has its authorization section enabled. Otherwise the printer uses a pairing code instead.

---

## Knowing when someone is using it

While a browser is controlling the screen, a small icon appears in the top-right corner of the printer's screen. It stays for ten seconds after the last remote tap, so someone standing at the printer can always tell that another person is using it.

![The remote-control icon](../../images/user/screen-sharing-badge.png)

Opening the screen page in a browser (or the HelixScreen Screen webcam in Mainsail or Fluidd with **Allow Remote Control** on) wakes the printer's screen if it was asleep, just as a touch would. While the page is open, the screen never goes to sleep, so the picture never goes black. It still dims and runs its screensaver, and sleeps about ten seconds after the last page is closed or its tab hidden, once its sleep time has passed. Apps that only show the screen as a camera (Obico, OctoEverywhere, Home Assistant, timelapse, the view-only webcam tile) do not keep it awake.

---

## How much it costs the printer

- **Nothing while nobody is watching.** The screen is only captured while a browser has it open.
- **While someone watches,** HelixScreen makes at most 5 pictures a second, and makes a new one only when something on the screen changed.
- **During a print** it sends 2 a second, or 1 a second on low-memory or single-core printers such as the K1, so the printer's own work comes first. Measured on a Creality K1C during a print, watching the screen did not slow Moonraker or Klipper at all.

---

## Troubleshooting

**HelixScreen Screen doesn't appear in Mainsail or Fluidd.**
Check that **Share Screen** and **Add to Mainsail/Fluidd Webcams** are on, with this printer selected on the screen. Each printer has its own settings. The webcam is added when HelixScreen connects to Moonraker; if you just turned it on, reload Mainsail. If your printer's address changed, HelixScreen updates the webcam the next time it reconnects.

**The page says "Waiting for the printer screen…".**
The printer is starting up, or HelixScreen isn't running. The page keeps trying every second and shows the screen as soon as it can.

**"That code did not match."**
Read the code again from **Settings > Connection > Screen Sharing**. If someone pressed **Forget Paired Browsers**, the code has changed.

**"Too many tries. Wait a minute and try again."**
Five wrong codes in a minute pause pairing for everyone. Wait a minute.

**Taps do nothing, and the page says "View only".**
**Allow Remote Control** is off on the printer.

**Nothing at `http://<printer address>/helixscreen/screen/` (no-code printers).**
Your computer isn't on Moonraker's trusted list and Moonraker requires logins. Add your network to `trusted_clients` in `moonraker.conf`, or use the printer from a trusted computer.

**Nothing at `http://<printer address>:7130/` (pairing printers).**
Check **Share Screen** is on, and that nothing on your network blocks port 7130.

**I use HelixScreen on a separate screen (for example a Pi) to drive a different printer.**
Then it always uses a pairing code, and its webcam is named **HelixScreen Screen (your-pi's-name)** so it never replaces the printer's own entry.

---

[Back to User Guide](../USER_GUIDE.md) | [Settings: Connection](settings/connection.md)
