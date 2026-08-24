# Tips & Best Practices

---

## Workflow Tips

**Temperature shortcuts**: Tap temperature displays on the Home panel to jump directly to temperature controls.

**Pre-print options**: These remember your choices per slicer — if you always mesh before PrusaSlicer prints, it persists.

**Quick homing**: Use "Home All" before starting manual positioning to ensure known coordinates.

**Save Z-offset during first layer**: Make baby step adjustments, then save while you can see the results.

---

## Quick Troubleshooting

| Problem | Quick Fix |
|---------|-----------|
| Touchscreen taps in wrong spot | **Settings > System > Touch & Input** (shown only when recalibration is needed) |
| Panel shows "Disconnected" | Check Moonraker is running, network is up |
| Temperature not changing | Verify heater is enabled in Klipper config |
| Can't extrude | Heat nozzle above minimum temp first |
| AMS not showing | Check Happy Hare/AFC is configured in Klipper |

For detailed troubleshooting, see [TROUBLESHOOTING.md](../TROUBLESHOOTING.md).

---

## When to Use Which Panel

| I want to... | Go to... |
|--------------|----------|
| Start a print | Home panel, tap print area |
| Change nozzle temp | Home panel, tap nozzle temp (or Controls) |
| Move print head | Controls, Motion |
| Load filament | Controls, Extrusion (single) or Filament (AMS) |
| Level my bed | Advanced, Bed Leveling |
| Run a custom macro | Advanced, Macros |
| Check print history | Advanced, Print History |
| Change theme | Settings > Display & Sound > Theme Colors |
| Fix touch calibration | Settings > System > Touch & Input (shown for any touchscreen) |

---

## Further Reading

- [Troubleshooting](../TROUBLESHOOTING.md) — Solutions to common problems
- [Configuration](../CONFIGURATION.md) — Detailed configuration options
- [FAQ](../FAQ.md) — Frequently asked questions
- [Installation](../INSTALL.md) — Installation instructions

---

**Prev:** [Beta Features](beta-features.md) | [Back to User Guide](../USER_GUIDE.md)
