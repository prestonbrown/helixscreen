# Printer Support

One page per printer family HelixScreen runs on directly: hardware, build target, deploy loop,
detection, firmware quirks and known limitations. User-facing install steps are in
[`../../user/guide/`](../../user/guide/) (`install-*.md`, `creality-k1c-setup.md`); background
research and firmware reverse-engineering are in [`../printer-research/`](../printer-research/README.md),
whose overview matrix lists every printer's target and status. Build targets are described in
[`../BUILD_SYSTEM.md`](../BUILD_SYSTEM.md) and `mk/cross.mk`.

| Doc | Printer | Target |
|-----|---------|--------|
| [FLASHFORGE_AD5M_SUPPORT.md](FLASHFORGE_AD5M_SUPPORT.md) | FlashForge Adventurer 5M / 5M Pro (Forge-X, Klipper Mod, ZMOD), including the `ad5m-br` buildroot variant | `ad5m`, `ad5m-br` |
| [FLASHFORGE_AD5X_SUPPORT.md](FLASHFORGE_AD5X_SUPPORT.md) | FlashForge Adventurer 5X on ZMOD | `mips` |
| [FLASHFORGE_CREATOR5_PRO_SUPPORT.md](FLASHFORGE_CREATOR5_PRO_SUPPORT.md) | FlashForge Creator 5 and 5 Pro (Z-Mod, Reforge), the Z-Mod tool changer | `mips` |
| [CREALITY_K1_SUPPORT.md](CREALITY_K1_SUPPORT.md) | Creality K1, K1C, K1 Max, K1 SE | `mips`, `k1-dynamic` |
| [CREALITY_K2_SUPPORT.md](CREALITY_K2_SUPPORT.md) | Creality K2, K2 Plus, K2 Pro | `k2` |
| [SNAPMAKER_U1_SUPPORT.md](SNAPMAKER_U1_SUPPORT.md) | Snapmaker U1 (stock 1.2+ and PAXX) | `snapmaker-u1` |
| [QIDI_SUPPORT.md](QIDI_SUPPORT.md) | QIDI Q2 and Max 4 on-device; Plus 4 and 3-series as remote targets | `pi` |
| [ELEGOO_CENTAURI_CARBON_YOCTO.md](ELEGOO_CENTAURI_CARBON_YOCTO.md) | Elegoo Centauri Carbon 1 on OpenCentauri COSMOS: the two targets and the Yocto dev loop | `cc1`, `yocto` |

Filament systems that come with a printer have their own backend docs one level up
(`../FILAMENT_BACKEND_*.md`), and firmware-specific z-offset handling is in
[`../Z_OFFSET_PERSISTENCE.md`](../Z_OFFSET_PERSISTENCE.md).

Printers with a build target or install guide but no page here yet: the Creality Sonic Pad
(`pi32`, [`install-sonicpad.md`](../../user/guide/install-sonicpad.md)) and the BigTreeTech
K-Touch ESP32 firmware (`firmware/helixscreen-esp32/`,
[`install-esp32.md`](../../user/guide/install-esp32.md)). The Centauri Carbon page covers the
Yocto loop but not yet the `cc1` platform's quirks, which live in
[`install-cc1.md`](../../user/guide/install-cc1.md).
