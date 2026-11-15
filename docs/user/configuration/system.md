# Logging, Updates, Telemetry and Storage

Settings for the app itself: where it writes its log, how it updates, what it shares, and how much disk it uses. All of these are **per screen**. See [Settings: System](../guide/settings/system.md), [Settings: Updates](../guide/settings/updates.md) and [Telemetry](../TELEMETRY.md) for the walkthroughs.

[Back to the Configuration Reference](../CONFIGURATION.md)

---

## Logging

These keys are at the top level of `settings.json`.

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `log_level` | `"info"` (`"debug"` under `--test`) | `"warn"`, `"info"`, `"debug"`, `"trace"` | How much detail HelixScreen writes. `warn` records only warnings and errors, `info` general milestones, `debug` detailed state changes, and `trace` everything. **Settings > System > Log Level**, applied at once. |
| `log_dest` | `"auto"` | `"auto"`, `"journal"`, `"syslog"`, `"file"`, `"console"` | Where the log goes. `auto` picks the best option, which is the systemd journal where there is one and the console otherwise. `journal` is read with `journalctl -u helixscreen`. `syslog` is the traditional system log. `file` writes to a file, and `console` prints to standard output. |
| `log_path` | `""` | an absolute path ending in `.log` | The log file when `log_dest` is `"file"`. Empty uses `/var/log/helix-screen.log` if that is writable, then `~/.local/share/helix-screen/helix.log`. The path must sit under `/tmp`, `/var/log` or the install folder, may not contain `..` and may not be a symbolic link. A subfolder of those must be owned by root or the HelixScreen user and not be writable by the group or others. Anything else is refused with a warning and the default location is used, because `settings.json` can be edited from the web and must not be able to aim the log at arbitrary files. |

A level given on the command line wins over the settings file. The order is `--log-level`, then the `HELIX_LOG_LEVEL` variable, then `-v` flags (`-v` info, `-vv` debug, `-vvv` trace), then `log_level`. For the destination it is `--log-dest`, then `HELIX_LOG_DEST`, then `log_dest`. See [Command line and environment](environment.md).

---

## Updates

The update channel is in the `update` section. **Settings > Updates > Update Channel** changes it.

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `update.channel` | `0` | `0` Stable, `1` Beta, `2` Dev | Which releases HelixScreen offers. Stable and Beta come from HelixScreen's release servers and fall back to GitHub releases. Dev needs a dev address (below) and is offered only when `beta_features` is on. |
| `beta_features` (top level) | `false` | `true` or `false` | Turns on beta features, including the **Dev** entry in the channel list. Always on under `--test`. You can also switch it by tapping the version button seven times in **Settings > Help & About > About**. See [Beta Features](../guide/beta-features.md). |

The server addresses are **not** in `settings.json`. The updater installs whatever those addresses serve, and `settings.json` can be edited from the web, so they live in a separate file that only root can write: `/var/lib/helixscreen/update_urls.json`. HelixScreen ignores `r2_url` and `dev_url` in `settings.json` and logs a warning if it finds them.

```json
{
  "r2_url": "",
  "dev_url": ""
}
```

| Key | Default | What it does |
|-----|---------|--------------|
| `r2_url` | `""` (HelixScreen's own server, `https://releases.helixscreen.org`) | The base address for update manifests. Every channel reads `<r2_url>/<channel>/manifest.json`. Self-hosters can point this at their own server. A trailing slash is removed. |
| `dev_url` | `""` | The address of the Dev channel. When set and the channel is Dev, HelixScreen reads `<dev_url>/manifest.json` directly. It must start with `http://` or `https://`. Choosing Dev in the menu while this is empty is refused with the message "Dev channel requires dev_url in update_urls.json". |

The file is used only when it is owned by root or the user HelixScreen runs as, and neither the group nor others can write to it. The folder `/var/lib/helixscreen` is created by the service unit or init script on every platform. Where it is missing:

```bash
sudo mkdir -p /var/lib/helixscreen && sudo chown root:root /var/lib/helixscreen && sudo chmod 755 /var/lib/helixscreen
sudo install -o root -g root -m 644 update_urls.json /var/lib/helixscreen/update_urls.json
```

### Update reminders

`upgrade_nudge` controls how insistently HelixScreen tells you an update is waiting. It has no menu.

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `upgrade_nudge.intensity` | `"off"` | `"off"`, `"normal"`, `"aggressive"` | `off` shows nothing, so an available update is visible only in **Settings > Help & About**. `normal` adds a red dot on the Settings icon. `aggressive` adds the dot and a banner across the top of the screen that stays until you dismiss it. Nothing appears while a print is running. |
| `upgrade_nudge.dismissed_version` | `""` | a version | The version whose banner you dismissed. HelixScreen sets it when you close the banner, and it hides only that version, so the next release shows the banner again. Clear it to see the banner again. |

---

## Telemetry

| Key | Default | What it does |
|-----|---------|--------------|
| `telemetry_enabled` (top level) | `false` | Anonymous usage telemetry. It is off until you opt in, either in the setup wizard or at **Settings > System > Share Usage Data**. While `false`, nothing is collected, queued or sent. What is and is not collected is in [Telemetry](../TELEMETRY.md). |

---

## Disk use

The thumbnail cache keeps the small pictures of your G-code files. These keys are in the `cache` section. The cache sizes itself to 5% of the available disk, never above `thumbnail_max_mb`.

| Key | Default | What it does |
|-----|---------|--------------|
| `cache.thumbnail_max_mb` | `20` | The largest the thumbnail cache may grow, in MB. |
| `cache.disk_critical_mb` | `5` | HelixScreen stops caching when free disk space falls below this many MB, so it cannot fill the filesystem. |
| `cache.disk_low_mb` | `20` | When free space falls below this many MB, HelixScreen clears the cache more aggressively, down to half its normal limit. |

If `disk_critical_mb` is not smaller than `disk_low_mb`, HelixScreen sets the critical level to half of the low one.

Two further keys choose where cached files go. They are for unusual setups.

| Key | What it does |
|-----|--------------|
| `cache.directory` | A folder to use for the thumbnail cache instead of the automatic choice. HelixScreen falls back to the automatic choice if it cannot use it. |
| `cache.base_directory` | A base folder for HelixScreen's other cached files. The `HELIX_CACHE_DIR` variable takes priority over it. |

The Storage page at **Settings > System > Storage** shows what fills the disk and lets you clear it. See [Storage](../guide/settings/system.md#storage).
