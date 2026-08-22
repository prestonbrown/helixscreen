# SPDX-License-Identifier: GPL-3.0-or-later

"""Drive a live HelixScreen instance through `helix-screen ctl --json`.

The C++ ctl client is the only JSON-RPC implementation in the tree. This module
shells out to it rather than speaking the protocol directly, so there is nothing
to keep in sync when the server changes.
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

# A unix socket path is capped by sockaddr_un.sun_path — 104 bytes on macOS,
# 108 on Linux — and helix-screen refuses to bind past it
# (src/remote/unix_socket_transport.cpp). pytest's per-test tmp dirs live under
# /var/folders/<...>/T/pytest-of-<user>/pytest-N/<fixture>N on macOS, which is
# already ~110 characters before the filename, so every socket path this
# harness generated was over the limit.
#
# The failure was invisible: the app boots normally and logs one error line,
# the socket file simply never appears, and the only symptom is _await_ready()
# timing out after 30s with a log tail full of unrelated chatter. Keep the
# caller's directory for config and logs; move only the socket somewhere short.
_SUN_PATH_MAX = 104 if sys.platform == "darwin" else 108

# Not tempfile.mkdtemp()'s default: TMPDIR on macOS *is* the long
# /var/folders path, which is the thing being avoided.
_SHORT_TMP = "/tmp"


def _short_socket_path(path: Path) -> tuple[Path, str | None]:
    """(socket path, temp dir to clean up) — relocated only if it must be."""
    if len(str(path).encode()) < _SUN_PATH_MAX:
        return path, None
    short_dir = tempfile.mkdtemp(prefix="hx-", dir=_SHORT_TMP)
    return Path(short_dir) / "c.sock", short_dir

# Seed written into every private HELIX_CONFIG_DIR at boot, as
# "<HELIX_CONFIG_DIR>/settings-test.json". This used to be a copy of the
# repo's own config/settings-test.json — but that file is gitignored (each
# environment's own accumulated runtime state: whichever printer preset last
# got applied, real mock-print history, spool weights drained by past runs,
# theme/dark-mode toggled by hand...), so its actual values vary machine to
# machine and session to session. The harness worked only by accident,
# wherever that local file happened to have animations_enabled=false — on a
# fresh checkout without one (or one that had simply never had the setting
# toggled), goldens captured mid cross-fade at ~100% pixel diff (main went
# red over exactly this, see "Golden corpus scope" in
# docs/devel/UI_TESTING.md).
#
# Every key below is something the 8 committed goldens are actually
# sensitive to — not accumulated state. Anything not listed here falls
# through to the app's own compiled-in defaults (Config::get_default_config()
# et al. in src/system/config.cpp), same as what a brand-new install sees, so
# this seed can't silently drift out of sync with reality the way a copied
# runtime file could.
_TEST_SEED_SETTINGS: dict = {
    # The actual root cause above: desktop/SDL's platform default is
    # animations ON (PlatformCapabilities::detect().supports_animations),
    # so every navigate()/click() lands on a mid-transition frame instead of
    # the settled one a golden expects.
    "display": {
        "animations_enabled": False,
        # DEFAULT_THEME already resolves to "helixscreen" (theme_loader.h),
        # so this doesn't change behavior today — stated explicitly because
        # it's one of the two settings (with dark_mode) that repaints every
        # pixel in every golden, and a future default change shouldn't be
        # able to silently take these goldens with it.
        "theme": "helixscreen",
    },
    # Matches the compiled-in default (config.cpp's get_default_config():
    # {"dark_mode": true}) — explicit for the same reason as theme above.
    "dark_mode": True,
    # Belt and braces alongside SDL_AUDIODRIVER=dummy below: this is already
    # the compiled-in default (audio_settings_manager.cpp:
    # get<bool>("/sounds_enabled", false)), stated explicitly so a headless
    # test run can never audibly beep even if that default ever changes.
    "sounds_enabled": False,
}


class HelixCtlError(RuntimeError):
    """A command the server rejected."""

    def __init__(self, message: str, code: int, command: list[str]):
        super().__init__(f"{message} (code {code}) while running: {' '.join(command)}")
        self.message = message
        self.code = code
        self.command = command


class HelixAppError(RuntimeError):
    """The app failed to start, or died while we were driving it."""


class HelixApp:
    """A running helix-screen instance on a private control socket."""

    #: Seconds to wait for the control socket to appear and answer ping.
    BOOT_TIMEOUT = 30.0

    def __init__(self, binary: Path, socket_path: Path, log_path: Path,
                 extra_args: list[str] | None = None,
                 extra_env: dict[str, str] | None = None):
        self.binary = Path(binary)
        # The instance's own directory, for config and anything else that
        # wants to sit beside the caller's files — kept even when the socket
        # itself has to move somewhere shorter (see _short_socket_path).
        self.workdir = Path(socket_path).parent
        self.socket_path, self._socket_tmpdir = _short_socket_path(Path(socket_path))
        self.log_path = Path(log_path)
        self.extra_args = list(extra_args or [])
        # Mock scenario knobs (HELIX_MOCK_*) for a test that needs printer state
        # the default mock does not produce. Applied over the inherited
        # environment at start(), so it stays scoped to this instance instead of
        # leaking into a session-scoped app spawned by another test.
        self.extra_env = dict(extra_env or {})
        self.proc: subprocess.Popen | None = None

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> "HelixApp":
        env = os.environ.copy()
        env.update(self.extra_env)
        # Default to SDL's headless driver: a visible window steals focus and
        # swallows the developer's keystrokes every time a test spawns an app,
        # and a suite run spawns many. Verified that dummy renders normally —
        # navigate and screenshot both work and produce correct pixels, so the
        # golden-image tests are unaffected.
        #
        # Explicitly exporting SDL_VIDEODRIVER=dummy or =wayland is respected
        # as-is. Any OTHER explicit value (e.g. x11) does NOT "still win" —
        # it gets silently corrected to wayland below whenever WAYLAND_DISPLAY
        # is set, same as scripts/screenshot.sh's rule: XWayland's GLX path
        # crashes, so an x11 request under a Wayland session would trade a
        # focus-stealing window for a crash. To actually get X11/XWayland,
        # run from an X11 session (no WAYLAND_DISPLAY) instead of exporting
        # SDL_VIDEODRIVER=x11 under Wayland.
        #
        # Also worth knowing for anyone debugging a red golden suite: exporting
        # SDL_VIDEODRIVER at all (even =wayland) switches the renderer away
        # from the dummy driver every golden was captured under — a plausible
        # way to turn goldens red for reasons that have nothing to do with the
        # UI change under test. Unset it before running goldens for real.
        if not env.get("SDL_VIDEODRIVER"):
            env["SDL_VIDEODRIVER"] = "dummy"
        headless = env["SDL_VIDEODRIVER"] == "dummy"
        if not headless and env.get("WAYLAND_DISPLAY"):
            env["SDL_VIDEODRIVER"] = "wayland"
        display_index = "0" if env.get("WAYLAND_DISPLAY") else "1"

        # Same story as SDL_VIDEODRIVER above, for audio: a headless run with
        # no visible window is still audibly beeping without this. Respects
        # an explicit caller value the same way — belt and braces alongside
        # sounds_enabled=False in the seed config below, since the SDL driver
        # only silences SDL-backed audio and this app has other sound
        # backends (ALSA, PWM, M300 — see docs/devel/SOUND_SYSTEM.md).
        if not env.get("SDL_AUDIODRIVER"):
            env["SDL_AUDIODRIVER"] = "dummy"

        # Application::acquire_instance_lock() flocks a lock file resolved
        # from HELIX_CONFIG_DIR (default: "config", relative to whatever the
        # process's CWD happens to be) — taken unconditionally, even under
        # --test, before Config::init() ever runs (so it can't rely on that
        # to create the directory). Without an override every HelixApp
        # spawned from the same CWD (the shared instance, a fresh_helix_app,
        # and test_diagnostics.py's pytester sub-process) shares one lock
        # file and only one can ever hold it. workdir is already a private
        # tmp_path per instance, so anchor the config dir there — it's unique
        # for free — and create it ourselves; open(O_CREAT) makes the lock
        # file but not its parent directory. (workdir, not socket_path.parent:
        # the socket may have been relocated out from under it.)
        if not env.get("HELIX_CONFIG_DIR"):
            config_dir = self.workdir / "helix-config"
            config_dir.mkdir(parents=True, exist_ok=True)
            env["HELIX_CONFIG_DIR"] = str(config_dir)

            # Config::init(TEST_CONFIG_PATH) resolves to
            # "<HELIX_CONFIG_DIR>/settings-test.json" (config.cpp keeps the
            # caller's filename, only redirects the directory). Write our own
            # literal seed (_TEST_SEED_SETTINGS, top of file) rather than
            # copying anything from the repo checkout — see that constant's
            # comment for why. Every instance gets its own file (never
            # shared/symlinked) so one instance's edits can't leak into
            # another's.
            (config_dir / "settings-test.json").write_text(
                json.dumps(_TEST_SEED_SETTINGS, indent=2))
        else:
            # Caller supplied HELIX_CONFIG_DIR themselves — respected as-is,
            # per-instance lock isolation and settings-test.json seeding are
            # both skipped (see above). If that directory is shared with
            # another live instance, acquire_instance_lock() blocks and the
            # only visible symptom is a silent 30s boot timeout — flag the
            # actual cause here instead of leaving that to be rediscovered.
            print(f"[HelixApp] note: HELIX_CONFIG_DIR={env['HELIX_CONFIG_DIR']!r} set "
                  f"by caller — no lock isolation from other instances and no "
                  f"settings-test.json seeding; a hang at boot likely means this "
                  f"directory is already locked by another running instance")

        args = [
            str(self.binary),
            "--test", "--skip-wizard", "--skip-splash",
            "--remote", "--remote-socket", str(self.socket_path),
            "--display", display_index,
            *self.extra_args,
        ]
        self.log_file = self.log_path.open("w")
        self.proc = subprocess.Popen(args, stdout=self.log_file,
                                     stderr=subprocess.STDOUT, env=env)
        try:
            self._await_ready()
        except Exception:
            # __enter__ raising skips __exit__, so a boot that never answers
            # ping would otherwise leak this process — kill it ourselves.
            if self.proc is not None and self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait(timeout=5)
            self._discard_socket_tmpdir()
            raise
        return self

    def _bind_refusal(self) -> str | None:
        """The transport's own reason for having no socket, if it logged one."""
        try:
            text = self.log_path.read_text(errors="replace")
        except OSError:
            return None
        for line in text.splitlines():
            if "[RemoteControl]" in line and (
                    "Socket path too long" in line or "Failed to bind" in line):
                return line.strip()
        return None

    def _await_ready(self) -> None:
        deadline = time.monotonic() + self.BOOT_TIMEOUT
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise HelixAppError(
                    f"helix-screen exited with {self.proc.returncode} during boot\n"
                    f"{self._log_tail_from_file()}"
                )
            if self.socket_path.exists():
                try:
                    if self.ctl("ping") == "pong":
                        return
                except (HelixCtlError, HelixAppError):
                    pass  # server thread not up yet
            # A refused bind never produces a socket, so waiting out the full
            # timeout only buries the one log line that says why. Surface it.
            refusal = self._bind_refusal()
            if refusal:
                raise HelixAppError(
                    f"helix-screen refused to bind its control socket: {refusal}")
            time.sleep(0.1)
        raise HelixAppError(
            f"helix-screen never answered ping within {self.BOOT_TIMEOUT}s\n"
            f"{self._log_tail_from_file()}"
        )

    def _raise_if_crashed(self) -> None:
        """Raise if the process's own exit code indicates a crash.

        Only call this on a path where nothing WE did caused the process to
        exit — a graceful `ctl shutdown` response, or finding it already gone.
        Mirrors scripts/smoke-headless.sh's distinction: a segfault or abort
        during shutdown cleanup is just as real a bug as one during normal
        operation, but SIGTERM/SIGKILL that *we* send to force a stuck
        process makes the exit code look "abnormal" (negative, signal-shaped)
        on purpose — that must never be reported as a crash.
        """
        code = self.proc.returncode
        if code == 0:
            return
        if code is not None and code < 0:
            sig = -code
            try:
                name = signal.Signals(sig).name
            except ValueError:
                name = f"signal {sig}"
            raise HelixAppError(
                f"helix-screen crashed during shutdown ({name}, exit code {code})\n"
                f"{self._log_tail_from_file()}"
            )
        raise HelixAppError(
            f"helix-screen exited abnormally during shutdown (status {code})\n"
            f"{self._log_tail_from_file()}"
        )

    def stop(self) -> None:
        if self.proc is None:
            return
        if self.proc.poll() is not None:
            # Already gone before we asked it to stop — nothing we did
            # caused this exit, so an abnormal code here is a genuine crash,
            # not shutdown-signal noise.
            try:
                self._raise_if_crashed()
            finally:
                if hasattr(self, "log_file"):
                    self.log_file.close()
                self._discard_socket_tmpdir()
            return
        try:
            self.ctl("shutdown")
        except (HelixCtlError, HelixAppError):
            pass
        try:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                # It didn't respond to the graceful request in time. From
                # here on every exit is caused by a signal we send ourselves
                # — SIGTERM/SIGKILL make the code negative/"abnormal" on
                # purpose, so it must not be flagged as a crash.
                self.proc.send_signal(signal.SIGTERM)
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=5)
            else:
                # Exited on its own after the graceful `ctl shutdown`
                # request — nothing forced it, so a nonzero/signal exit
                # code here is exactly the shutdown-cleanup crash this
                # check exists to catch.
                self._raise_if_crashed()
        finally:
            if hasattr(self, "log_file"):
                self.log_file.close()
            self._discard_socket_tmpdir()

    def _discard_socket_tmpdir(self) -> None:
        """pytest cleans up its own tmp dirs; a relocated socket sits outside
        them, so it is ours to remove. Idempotent — every exit path calls it."""
        if self._socket_tmpdir:
            shutil.rmtree(self._socket_tmpdir, ignore_errors=True)
            self._socket_tmpdir = None

    def __enter__(self) -> "HelixApp":
        return self.start()

    def __exit__(self, *exc_info) -> None:
        self.stop()

    def _log_tail_from_file(self, n: int = 30) -> str:
        """Read the app's stdout log directly — used when the RPC channel is dead."""
        try:
            lines = self.log_path.read_text(errors="replace").splitlines()
        except OSError:
            return "(no log available)"
        return "\n".join(lines[-n:])

    # -- raw command -------------------------------------------------------

    def ctl(self, *args: Any) -> Any:
        """Run one `ctl --json` command; return the parsed result."""
        command = [str(self.binary), "ctl", "-s", str(self.socket_path), "--json",
                   *[str(a) for a in args]]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=180)

        if completed.returncode != 0:
            stderr = completed.stderr.strip()
            try:
                err = json.loads(stderr)
            except json.JSONDecodeError:
                err = None

            if err is not None:
                raise HelixCtlError(err.get("message", stderr),
                                    int(err.get("code", -1)), command)

            # Client-side usage error, or the app died. Distinguish them,
            # because "no instance at socket" and "you typo'd" need
            # different reactions from whoever reads the failure.
            if self.proc is not None and self.proc.poll() is not None:
                raise HelixAppError(
                    f"helix-screen died (exit {self.proc.returncode})\n"
                    f"{self._log_tail_from_file()}"
                )
            raise HelixCtlError(stderr or "ctl failed", -1, command)

        out = completed.stdout.strip()
        return json.loads(out) if out else None

    # -- typed wrappers ----------------------------------------------------

    def navigate(self, target: str) -> dict:
        return self.ctl("navigate", target)

    def go_back(self) -> dict:
        return self.ctl("go_back")

    def click(self, target: str) -> dict:
        return self.ctl("click", target)

    def ls(self, target: str | None = None) -> dict:
        return self.ctl("ls", target) if target else self.ctl("ls")

    def geom(self, target: str, depth: int = 0) -> dict:
        return self.ctl("geom", target, depth) if depth else self.ctl("geom", target)

    def text(self, target: str) -> str:
        """Read a widget's text. Raises if the widget carries no text at all."""
        return self.ctl("text", target)["text"]

    def state(self, target: str) -> dict:
        """Read a widget's LVGL states/flags (checked, disabled, hidden, ...)."""
        return self.ctl("state", target)

    def get(self, subject: str) -> Any:
        return self.ctl("get", subject)

    def set(self, subject: str, value: Any) -> dict:
        return self.ctl("set", subject, value)

    def wait_for(self, subject: str, value: Any, timeout: float = 30.0) -> dict:
        """Block until a subject's value equals `value` (exact match), or raise on timeout.

        Event-driven: the server attaches an LVGL subject observer rather than
        polling, so this returns the instant the value changes to match —
        faster than a fixed sleep in the common case, and immune to the flake
        a fixed margin has under load.
        """
        return self.ctl("wait_for", subject, value, "--timeout", timeout)

    def wait_idle(self, timeout: float = 10.0) -> dict:
        """Block until the UI has settled.

        Best-effort: raw lv_async_call work, the gcode/thumbnail build threads,
        and the mock backends' own threads are invisible to this. See the design
        spec's determinism section.
        """
        return self.ctl("wait_idle", "--timeout", timeout)

    def freeze(self) -> dict:
        """Stop animations and pause periodic timers for deterministic capture.

        Skips the UpdateQueue processor and display-refresh timers so the
        control channel keeps working and screenshots still render — see
        docs/devel/HELIXCTL.md "Diagnostics & lifecycle".
        """
        return self.ctl("freeze")

    def unfreeze(self) -> dict:
        """Reverse freeze(): resume the timers it paused, re-enable animations."""
        return self.ctl("unfreeze")

    def current(self) -> dict:
        # CLI token is `current`; `get_current` is the wire method name.
        return self.ctl("current")

    def reset(self) -> dict:
        """Return to home with no overlays or modals. Cheaper than a reboot."""
        return self.ctl("reset")

    def log(self, n: int = 50) -> list[str]:
        result = self.ctl("log", "-n", n)
        return result.get("lines", []) if isinstance(result, dict) else []

    def screenshot(self, path: str, target: str | None = None,
                   stable: bool = False) -> str:
        args = ["screenshot", path]
        if target:
            args += ["--target", target]
        if stable:
            args += ["--stable"]
        return self.ctl(*args)["path"]

    def capture(self, target: str | None = None, stable: bool = True):
        """Capture to a temp PNG and return it as a Pillow image.

        `stable` defaults to True here (unlike the raw `screenshot` command)
        because this is the golden/pixel-comparison entry point — a caller
        reaching for an in-memory image almost always wants the frame-hash
        gate, not a best-effort snapshot that might land mid-repaint.
        """
        from PIL import Image

        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
            path = tmp.name
        try:
            self.screenshot(path, target=target, stable=stable)
            with Image.open(path) as img:
                return img.convert("RGBA").copy()
        finally:
            os.unlink(path)

    def shutdown(self) -> None:
        self.stop()
