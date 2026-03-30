#!/usr/bin/env python3
"""
ClaWD Status Daemon — bridges Claude Code hooks to the ESP32 via serial.

Listens on a Unix domain socket (~/.clawd/clawd.sock) for state change
messages from hook scripts, and forwards them to the ESP32 over USB serial.

Tracks idle timeout to automatically switch to "inactive" state.
"""

import json
import os
import selectors
import signal
import socket
import sys
import time
import glob as glob_mod
import logging

try:
    import serial
except ImportError:
    print("ERROR: pyserial is required. Install with: pip3 install pyserial", file=sys.stderr)
    sys.exit(1)

# ── Paths ──────────────────────────────────────────────────────
CONFIG_DIR = os.path.expanduser("~/.clawd")
SOCKET_PATH = os.path.join(CONFIG_DIR, "clawd.sock")
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")
LOG_PATH = os.path.join(CONFIG_DIR, "clawd_daemon.log")
PID_PATH = os.path.join(CONFIG_DIR, "clawd.pid")

# ── Defaults ───────────────────────────────────────────────────
DEFAULT_CONFIG = {
    "serial_port": "auto",
    "baud_rate": 115200,
    "idle_timeout": 300,
    "brightness": 255,
    "speed": 2,
}

# ── Logging ────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
    ]
)
log = logging.getLogger("clawd")


class ClaWDDaemon:
    def __init__(self):
        self.config = dict(DEFAULT_CONFIG)
        self.ser = None
        self.sock = None
        self.sel = selectors.DefaultSelector()
        self.current_state = "inactive"
        self.last_activity = time.time()
        self.running = True

    # ── Config ─────────────────────────────────────────────────
    def load_config(self):
        if os.path.exists(CONFIG_PATH):
            try:
                with open(CONFIG_PATH) as f:
                    user_cfg = json.load(f)
                self.config.update(user_cfg)
                log.info("Config loaded from %s", CONFIG_PATH)
            except (json.JSONDecodeError, IOError) as e:
                log.warning("Failed to load config: %s", e)

    # ── Serial port detection ──────────────────────────────────
    def find_serial_port(self):
        """Auto-detect ESP32 serial port on macOS/Linux."""
        patterns = [
            "/dev/cu.usbmodem*",
            "/dev/cu.usbserial*",
            "/dev/cu.wchusbserial*",
            "/dev/ttyACM*",
            "/dev/ttyUSB*",
        ]
        candidates = []
        for pat in patterns:
            candidates.extend(glob_mod.glob(pat))

        for port in sorted(candidates):
            try:
                log.info("Probing %s ...", port)
                s = serial.Serial(port, self.config["baud_rate"], timeout=2)
                time.sleep(2)  # ESP32-C3 resets on serial open; wait for boot
                s.reset_input_buffer()
                s.write(b"PING\n")
                s.flush()
                deadline = time.time() + 3
                while time.time() < deadline:
                    line = s.readline().decode("utf-8", errors="ignore").strip()
                    if line == "PONG" or line == "BOOT":
                        log.info("Found ESP32 on %s", port)
                        return s
                s.close()
            except (serial.SerialException, OSError) as e:
                log.debug("Probe failed for %s: %s", port, e)

        return None

    def connect_serial(self):
        """Establish serial connection."""
        port_cfg = self.config.get("serial_port", "auto")
        if port_cfg != "auto":
            try:
                self.ser = serial.Serial(port_cfg, self.config["baud_rate"], timeout=1)
                time.sleep(2)
                self.ser.reset_input_buffer()
                log.info("Connected to %s", port_cfg)
                return True
            except (serial.SerialException, OSError) as e:
                log.error("Failed to open %s: %s", port_cfg, e)
                return False
        else:
            self.ser = self.find_serial_port()
            return self.ser is not None

    def send_serial(self, msg):
        """Send a command to the ESP32. Returns True on success."""
        if not self.ser:
            return False
        try:
            self.ser.write((msg + "\n").encode("utf-8"))
            self.ser.flush()
            # Read response (non-blocking-ish, short timeout)
            self.ser.timeout = 0.5
            resp = self.ser.readline().decode("utf-8", errors="ignore").strip()
            if resp:
                log.debug("ESP32 response: %s", resp)
            return True
        except (serial.SerialException, OSError) as e:
            log.error("Serial write failed: %s", e)
            self.ser = None
            return False

    def apply_settings(self):
        """Send current config settings to ESP32."""
        self.send_serial("SET:brightness:" + str(self.config.get("brightness", 255)))
        self.send_serial("SET:speed:" + str(self.config.get("speed", 2)))

    # ── Socket ─────────────────────────────────────────────────
    def setup_socket(self):
        """Create the Unix domain socket for receiving hook messages."""
        os.makedirs(CONFIG_DIR, exist_ok=True)

        # Clean up stale socket
        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)

        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(SOCKET_PATH)
        self.sock.listen(5)
        self.sock.setblocking(False)
        self.sel.register(self.sock, selectors.EVENT_READ, data="listener")
        log.info("Listening on %s", SOCKET_PATH)

    def handle_client(self, conn):
        """Read a single message from a hook script connection."""
        try:
            data = conn.recv(256).decode("utf-8", errors="ignore").strip()
            if data:
                self.handle_hook_message(data)
        except (OSError, UnicodeDecodeError) as e:
            log.debug("Client read error: %s", e)
        finally:
            conn.close()

    def handle_hook_message(self, msg):
        """Process a message from a hook script."""
        msg = msg.strip().lower()

        if msg == "working":
            self.set_state("working")
        elif msg == "attention":
            self.set_state("attention")
        elif msg == "reload":
            log.info("Reloading config...")
            self.load_config()
            self.apply_settings()
        else:
            log.warning("Unknown hook message: %s", msg)

    # ── State management ───────────────────────────────────────
    def set_state(self, state):
        """Update state and forward to ESP32."""
        self.last_activity = time.time()

        if state == self.current_state:
            # Still update activity timestamp, but don't resend
            return

        self.current_state = state
        log.info("State → %s", state)
        self.send_serial("STATE:" + state)

    def check_idle_timeout(self):
        """Switch to inactive if idle for too long."""
        if self.current_state == "inactive":
            return

        timeout = self.config.get("idle_timeout", 300)
        elapsed = time.time() - self.last_activity
        if elapsed >= timeout:
            log.info("Idle timeout (%.0fs) — switching to inactive", elapsed)
            self.current_state = "inactive"
            self.send_serial("STATE:inactive")

    # ── Main loop ──────────────────────────────────────────────
    def run(self):
        """Main daemon loop."""
        # Write PID file
        os.makedirs(CONFIG_DIR, exist_ok=True)
        with open(PID_PATH, "w") as f:
            f.write(str(os.getpid()))

        self.load_config()
        self.setup_socket()

        # Signal handlers
        signal.signal(signal.SIGTERM, lambda *_: self.shutdown())
        signal.signal(signal.SIGINT, lambda *_: self.shutdown())

        # Initial serial connection
        if self.connect_serial():
            self.apply_settings()
            self.send_serial("STATE:inactive")
        else:
            log.warning("No ESP32 found — will retry in background")

        last_reconnect_attempt = 0

        log.info("ClaWD daemon running (PID %d)", os.getpid())

        while self.running:
            # Select on socket with 1s timeout for idle checks
            events = self.sel.select(timeout=1.0)

            for key, _ in events:
                if key.data == "listener":
                    conn, _ = self.sock.accept()
                    self.handle_client(conn)

            # Periodic checks
            self.check_idle_timeout()

            # Reconnect serial if lost
            if self.ser is None:
                now = time.time()
                if now - last_reconnect_attempt >= 5:
                    last_reconnect_attempt = now
                    log.info("Attempting serial reconnection...")
                    if self.connect_serial():
                        self.apply_settings()
                        self.send_serial("STATE:" + self.current_state)

        self.shutdown()

    def shutdown(self):
        """Clean shutdown."""
        self.running = False
        log.info("Shutting down...")

        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass

        if self.sock:
            self.sel.unregister(self.sock)
            self.sock.close()

        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)

        if os.path.exists(PID_PATH):
            os.unlink(PID_PATH)

        log.info("ClaWD daemon stopped")
        sys.exit(0)


if __name__ == "__main__":
    daemon = ClaWDDaemon()
    daemon.run()
