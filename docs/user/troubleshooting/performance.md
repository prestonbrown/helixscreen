# Slow interface and memory problems

Touches lag, scrolling is choppy, or the printer's computer runs out of memory.

Back to the [troubleshooting index](../TROUBLESHOOTING.md).

---

### The interface feels slow or laggy

**Symptoms:**
- Delayed response to touches
- Choppy scrolling
- Slow panel transitions

**Diagnosis:**

```bash
# Check CPU and memory
top -b -n 1 | head -20

# Check if swapping (very slow on SD card)
free -h

# Check HelixScreen memory usage
ps aux | grep helix-screen
```

**Common causes and fixes:**

| Cause | Fix |
|-------|-----|
| Verbose logging left on | Set **Settings > System > Log Level** back to **Info**. A `HELIX_LOG_LEVEL=debug` line in `helixscreen.env` or a `--debug` flag in a service file keeps it on |
| Animations on slow hardware | Turn off **Settings > Appearance > Animations** |
| Too many G-code files | Large directories with thumbnails use more RAM |
| Other processes hogging CPU | Check `top` for culprits |
| Swapping to SD card | Reduce memory usage or add swap to USB |
| Hardware issues | Open **Settings > Devices > Hardware Health** and look for problems |

---

### The printer runs out of memory

**Symptoms:**
- Out of memory errors
- System becomes unresponsive
- Crashes during complex operations

**Solutions:**

**Check memory usage:**
```bash
free -h
```

**Limit print history:**
```ini
[history]
max_job_count: 100
```

---

HelixScreen's own load and memory figures are in **Settings > System > Performance**; see [System settings](../guide/settings/system.md).
