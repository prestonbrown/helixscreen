# K-Touch Hardware Probe

Minimal ESP-IDF firmware that verifies the BTT K-Touch pin map (borrowed from
`bigtreetech/PandaTouch_PlatformIO`) on real hardware. See
`docs/devel/printer-research/BTT_K_TOUCH_HARDWARE.md` for the recon this validates.

What it does on boot:
1. Brings up the 800×480 RGB panel and draws R/G/B/W/K color bars with a white
   border (timing misalignment shows at the edges).
2. Turns on the backlight (LEDC PWM, GPIO 21).
3. Scans the I²C bus and logs every ACKing address (GT911 expected at 0x5D;
   anything else is bonus recon — e.g. a battery fuel gauge).
4. Reads the GT911 product ID, then logs touch coordinates continuously.

No LVGL — raw esp_lcd + raw I²C so failures point at exactly one layer.

## Build & flash

```bash
. ~/Code/esp-idf/export.sh
cd firmware/ktouch-probe
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 -b 460800 flash monitor   # CH340: 921600 corrupts, use 460800
```

**Before flashing anything:** a full stock-flash backup must exist
(`~/Code/Printing/ktouch-recon/ktouch_stock_full_16MB.bin`). Restore stock with:

```bash
esptool --port /dev/ttyUSB0 --baud 460800 write-flash 0x0 \
    ~/Code/Printing/ktouch-recon/ktouch_stock_full_16MB.bin
```

## Pass criteria

- Clean color bars, stable image, white border fully visible on all four edges
- Log shows `GT911 product ID: '911'`
- Touching the screen logs sensible coordinates (0-799 / 0-479)
