# Run HelixScreen in a Docker Container

HelixScreen only needs network access to your printer's Moonraker, so it can run on a home server instead of the printer. A Docker container on a NAS, an Unraid box or any Linux machine runs the same app as the [Linux desktop package](install-linux-desktop.md), with no window and no panel. You see and use the screen in a web browser through [Screen Sharing](screen-sharing.md).

This suits trying a new HelixScreen release without installing it on the printer. A crash or a network problem in the container never touches the printer, because the printer only sees an ordinary Moonraker client.

> **This is new in HelixScreen 1.1.** The container runs the x86_64 desktop package, so it needs a Docker host with an Intel or AMD processor, and a release that is 1.1 or newer. The 1.0 releases have no desktop package.

## What you get

- The full HelixScreen interface in a browser tab, on a phone or on a tablet.
- Taps, drags and scrolling from the browser, once you pair it with a six-digit code.
- Settings that stay in a Docker volume, so updating the container keeps them.
- No change to your printer.

What it does not do: there is no physical touchscreen, no screen dimming or sleep on a panel, and no screen sharing as a webcam in Mainsail or Fluidd (see [Troubleshooting](#troubleshooting)).

## Requirements

- A Docker host with an x86_64 processor, and Docker 20.10 or newer. Docker Compose is optional.
- Network access from the host to your printer's Moonraker (port `7125` by default). Mainsail or Fluidd is not required.
- Your printer's IP address or name, for example `192.168.1.50`.

## Start it

Every release that carries the Linux desktop package is also published as a container image, so there is nothing to build. The image is `ghcr.io/prestonbrown/helixscreen-homeserver`, and its tag says which release you get:

| Tag | You get |
|---|---|
| `dev` | The newest development build. |
| `beta` | The newest release on the Beta channel: the newest beta, or the newest Stable release when that is newer. |
| `latest` or `stable` | The newest Stable release. |
| A version, such as `1.2.0-alpha.5` | That one release and no other. Use it to stay on a version you have checked. |

Tags appear as releases are published as images: `dev` first, then `beta` and `latest` with the first Beta and Stable releases that get one. If pulling a tag says it does not exist, no such release has been published as an image yet: use `dev`.

Name your printer with `PRINTER_URL`, keep the settings in a volume, and publish port `7130`:

```bash
docker run -d --name helixscreen --restart unless-stopped \
  -e PRINTER_URL=192.168.1.50:7125 \
  -p 7130:7130 \
  -v helixscreen-config:/config \
  ghcr.io/prestonbrown/helixscreen-homeserver:dev
```

Or use the Compose file in the repository, `docker/docker-compose.homeserver.yml`. Edit `PRINTER_URL` in it, then:

```bash
docker compose -f docker/docker-compose.homeserver.yml up -d
```

`PRINTER_URL` takes `192.168.1.50`, `192.168.1.50:7125` or `ws://printer.local:7125`. With no port, HelixScreen uses `7125`. The container stops with a message if you leave it out.

The container is ready about twenty seconds after it starts. `docker ps` then shows it as `healthy`.

### Other settings

| Variable | Meaning |
|---|---|
| `PRINTER_URL` | **Required.** Where the printer's Moonraker is. |
| `PAIR_CODE` | Six digits to use as the pairing code, instead of a generated one. Read only the first time the container starts. |
| `SCREEN_SIZE` | The size of the screen the browser shows: `small`, `medium`, `large` and so on, or exact pixels such as `1024x600`. The default is `800x480`. The layout adapts the same way it does on a printer's screen. |

## Open the screen

In a browser on your network, open:

```
http://<your server's address>:7130/
```

For example `http://192.168.1.20:7130/`. The page works on phones and tablets too.

**Watching** needs nothing. **Using** the screen (tapping and dragging) needs a pairing code the first time each browser connects:

1. Get the code. You set it yourself with `PAIR_CODE`, or read the generated one from the server:
   ```bash
   docker exec helixscreen jq -r .remote_screen.pair_secret /config/settings.json
   ```
2. Type it into the page and press **Pair**.

The screen itself never shows the code in the browser. Screen Sharing blanks any frame that would, so the code has to come from the server. [Screen Sharing and Remote Control](screen-sharing.md) explains pairing, how many browsers can be paired and how to forget them.

The container skips the setup wizard, and the printer's address comes from `PRINTER_URL`. Open **Settings** in the browser for everything else, such as themes and your update preferences.

Anyone on your network can watch the screen, and anyone with the code can use it. Do not publish port `7130` to the internet. To reach it from outside your home, use a VPN.

## Updating

**Settings > Updates** opens the release page and does not update the container in place. Update the container the way you update any other: get the newer image, then recreate the container. The volume keeps your settings.

A container on `dev`, `beta` or `latest` moves to that channel's newest release when you pull the image again:

```bash
docker rm -f helixscreen
docker pull ghcr.io/prestonbrown/helixscreen-homeserver:dev
# then the same docker run command as before
```

With Compose, `docker compose -f docker/docker-compose.homeserver.yml up -d --pull always` does all of it.

A container on a version tag stays on that version. To move it to another release or another channel, change the tag in the `docker run` command or the Compose file, then recreate the container.

## Build the image yourself

You need this only to change what the image contains, or to run a release the published image does not cover. It takes under a minute, because the build only downloads the released package and checks it against the release's published checksum.

```bash
git clone https://github.com/prestonbrown/helixscreen.git
cd helixscreen
docker build -t helixscreen/homeserver -f docker/Dockerfile.homeserver docker
```

The image follows one update channel, chosen when you build it:

| You build with | You get |
|---|---|
| nothing extra | The newest release on the Stable channel. It must be 1.1 or newer; while Stable is still on 1.0, the build stops because that release has no desktop package. |
| `--build-arg CHANNEL=beta` | The newest Beta release. |
| `--build-arg RELEASE=v1.1.0 --build-arg RELEASE_SHA256=<sha256>` | That one release. Give the zip's SHA-256 so the download can be checked: the `zip_sha256` value from a channel manifest that served that release (`https://releases.helixscreen.org/beta/manifest.json`, for example). |

Tag a build `helixscreen/homeserver:beta` (or any name you like) to keep a Stable and a Beta image side by side.

Start it the same way as above, with the name of your image in place of the published one. To update it, `git pull`, build it again, remove the container and start it with the same command.

## Where things are kept

| Path in the container | Holds |
|---|---|
| `/config` | `settings.json`: your settings, the printer, the pairing code and the paired browsers. Mount a volume here. |
| `/opt/helixscreen` | The app itself. It belongs to the image, so it changes when you update. |

To change the printer's address later, change `PRINTER_URL` and recreate the container. The address passed that way wins over the one in `settings.json`.

To start over with fresh settings, remove the volume (`docker volume rm helixscreen-config`) with the container stopped.

## Troubleshooting

**The container exits and the log says `PRINTER_URL is not set`.** Add `-e PRINTER_URL=...` to `docker run`, or set it in the Compose file. `docker logs helixscreen` shows the message.

**The page loads but shows the printer as disconnected.** From the Docker host, open `http://PRINTER_IP:7125/server/info` in a browser or run `curl http://PRINTER_IP:7125/server/info`. If that fails too, the problem is the network or Moonraker, not the container. On Unraid and other hosts that use their own Docker networks, make sure the container can reach the printer's address; a container on a bridge network usually can.

**The page says "Waiting for the printer screen…".** The app is still starting. It tries again every second.

**Nothing opens at port `7130`.** Check that the container is running (`docker ps`) and that you published the port with `-p 7130:7130`. On a host that cannot publish ports, run the container with `--network host` instead of `-p`.

**The pairing code is rejected.** Read it again with the `docker exec` command above. After five wrong codes in a minute, the page makes you wait.

**The screen does not show up as a webcam in Mainsail or Fluidd.** This is on purpose. The webcam entry would carry the container's internal address, which a browser cannot open. Use the page at port `7130` directly.

**Settings did not survive an update.** The container needs a volume or folder mounted at `/config`. Without one, Docker discards the settings with the old container.

**`docker pull` says the tag does not exist.** No release of that kind has been published as an image yet. Use the `dev` tag until then.

**`docker pull` says `denied` or `unauthorized`.** The image is not public yet. Nothing on your side fixes that: tell us on [Discord](https://discord.gg/RZCT2StKhr).

For anything else, see [Troubleshooting](../TROUBLESHOOTING.md) or ask on [Discord](https://discord.gg/RZCT2StKhr).

---

**Next:** [Screen Sharing and Remote Control](screen-sharing.md) | [Back to User Guide](../USER_GUIDE.md)
