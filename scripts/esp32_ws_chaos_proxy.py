#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""TCP proxy that drops the client-side connection on demand, to soak-test the
ESP32 K-Touch's Moonraker reconnect logic (Plan 4 Task 9: F5/F8/R3/R4) against
a stand-in for a server-side WebSocket disconnect, WITHOUT restarting the real
printer's services.

Point the device at this proxy (sdkconfig.local URL override) instead of the
Voron directly; the proxy forwards every byte both ways transparently and only
misbehaves when told to. Each "drop" tears down EVERY currently-live proxied
session (both the K-Touch-facing socket and its upstream socket, for every
session tracked — not just whichever one happens to be "current") so the
device sees exactly what a server-side disconnect looks like — the real
Voron's Moonraker service itself is never touched or restarted; the proxy
just opens a fresh upstream connection for the device's next reconnect
attempt.

Usage:
    # Drop the current client connection every 45s, forever:
    ./scripts/esp32_ws_chaos_proxy.py --listen-port 7125 \\
        --upstream-host 192.168.1.100 --upstream-port 7125 --drop-every 45

    # Drop on demand instead, from another terminal:
    kill -USR1 <pid>          # or: ./scripts/esp32_ws_chaos_proxy.py --pid-file /tmp/chaos.pid
                               #     kill -USR1 $(cat /tmp/chaos.pid)

Stdlib only (socket + threading + signal) — no external dependencies, matches
the brief's "host-side, stdlib-only Python" requirement.
"""
import argparse
import signal
import socket
import sys
import threading
import time

# Chunk size for the bidirectional pump. Moonraker JSON-RPC messages are well
# under this; large ones just take a few extra recv() calls, no framing here —
# this proxy is a dumb byte pipe.
BUF_SIZE = 65536


class ChaosProxy:
    def __init__(self, listen_host, listen_port, upstream_host, upstream_port):
        self.listen_host = listen_host
        self.listen_port = listen_port
        self.upstream_host = upstream_host
        self.upstream_port = upstream_port
        self.cycle = 0
        # Every currently-live (client_sock, upstream_sock) session, guarded
        # by _sessions_lock. See request_drop() for why this replaced a
        # shared threading.Event + one polling watcher thread per connection.
        self._sessions_lock = threading.Lock()
        self._live_sessions = []

    def request_drop(self):
        """Tear down EVERY currently-live proxied session, immediately —
        called from the SIGUSR1 handler or the --drop-every timer thread.

        The prior design used a single shared threading.Event that each
        connection's own watcher thread polled; whichever watcher happened to
        notice the flag first consumed it (clearing it for everyone else),
        so if MORE THAN ONE session was ever alive at once — e.g. a device
        that opened a fresh TCP connection while an older one was still
        technically open proxy-side, for whatever reason — the older session
        could be skipped by every subsequent drop indefinitely, since a newer
        session's watcher kept winning the race. Measured in a Task 9 soak:
        one device connection stayed alive 5+ minutes and outlived several
        drop cycles that all landed on newer, unrelated connections instead.
        Iterating every tracked session here, under one lock, makes a drop
        affect ALL of them — no session can be perpetually skipped — and
        removes the up-to-100ms watcher polling latency as a side effect.
        """
        with self._sessions_lock:
            sessions = list(self._live_sessions)
        if not sessions:
            self._log("DROP requested but no live sessions to tear down")
            return
        self.cycle += 1
        self._log(f"DROP cycle={self.cycle} — tearing down {len(sessions)} live session(s)")
        for client_sock, upstream_sock in sessions:
            # shutdown(SHUT_RDWR) on each socket — not a bare close()/
            # SO_LINGER RST — is the reliable mechanism: each pump thread is
            # concurrently blocked in a recv() on one of these two sockets,
            # and closing an fd out from under a thread blocked in recv() on
            # it is a well-known race (measured: an RST is silently
            # swallowed and the remote peer sees nothing until ITS OWN
            # multi-second timeout, defeating the point of a deterministic
            # soak drop). shutdown() is documented-safe to call from another
            # thread specifically to unblock a peer's in-progress blocking
            # I/O, and is delivered to the remote socket immediately
            # (measured: FIN observed client-side in <10ms).
            for sock in (client_sock, upstream_sock):
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def _log(self, msg):
        ts = time.strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{ts}] {msg}", flush=True)

    def _pump(self, src, dst, stop_event):
        try:
            while not stop_event.is_set():
                data = src.recv(BUF_SIZE)
                if not data:
                    break
                dst.sendall(data)
        except OSError:
            pass
        finally:
            stop_event.set()

    def handle_connection(self, client_sock, client_addr):
        # Runs in its own thread (see serve_forever) so a slow/hung teardown
        # of one connection can never block the accept loop from taking the
        # device's next reconnect.
        self._log(f"client connected: {client_addr}")
        try:
            upstream_sock = socket.create_connection(
                (self.upstream_host, self.upstream_port), timeout=10
            )
        except OSError as e:
            self._log(f"upstream connect failed: {e}")
            client_sock.close()
            return

        session = (client_sock, upstream_sock)
        with self._sessions_lock:
            self._live_sessions.append(session)

        stop_event = threading.Event()
        threads = [
            threading.Thread(
                target=self._pump, args=(client_sock, upstream_sock, stop_event), daemon=True
            ),
            threading.Thread(
                target=self._pump, args=(upstream_sock, client_sock, stop_event), daemon=True
            ),
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        with self._sessions_lock:
            try:
                self._live_sessions.remove(session)
            except ValueError:
                pass  # already removed — shouldn't happen, but never crash a soak over it

        for sock in (client_sock, upstream_sock):
            try:
                sock.close()
            except OSError:
                pass
        self._log(f"client disconnected: {client_addr}")

    def serve_forever(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((self.listen_host, self.listen_port))
        listener.listen(5)
        self._log(
            f"listening on {self.listen_host}:{self.listen_port} -> "
            f"{self.upstream_host}:{self.upstream_port}"
        )
        try:
            while True:
                client_sock, client_addr = listener.accept()
                threading.Thread(
                    target=self.handle_connection, args=(client_sock, client_addr), daemon=True
                ).start()
        except KeyboardInterrupt:
            pass
        finally:
            listener.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--listen-host", default="0.0.0.0", help="Interface to listen on (default: all)")
    parser.add_argument("--listen-port", type=int, required=True, help="Port the device connects to")
    parser.add_argument("--upstream-host", required=True, help="Real Moonraker host (e.g. 192.168.1.100)")
    parser.add_argument("--upstream-port", type=int, required=True, help="Real Moonraker port (e.g. 7125)")
    parser.add_argument(
        "--drop-every", type=float, default=None,
        help="Seconds between automatic drops of the current client connection (omit for SIGUSR1-only)",
    )
    parser.add_argument("--pid-file", default=None, help="Write this process's PID here for `kill -USR1`")
    args = parser.parse_args()

    if args.pid_file:
        with open(args.pid_file, "w") as f:
            f.write(str(__import__("os").getpid()))

    proxy = ChaosProxy(args.listen_host, args.listen_port, args.upstream_host, args.upstream_port)

    def on_sigusr1(signum, frame):
        proxy.request_drop()

    signal.signal(signal.SIGUSR1, on_sigusr1)

    if args.drop_every:
        def timer_loop():
            while True:
                time.sleep(args.drop_every)
                proxy.request_drop()

        threading.Thread(target=timer_loop, daemon=True).start()
        proxy._log(f"auto-drop every {args.drop_every}s (also responds to SIGUSR1, pid={__import__('os').getpid()})")
    else:
        proxy._log(f"drop on SIGUSR1 only (pid={__import__('os').getpid()})")

    proxy.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
