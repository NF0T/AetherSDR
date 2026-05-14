#!/usr/bin/env python3
"""
quickkeys_aethersdr.py — Xencelabs Quick Keys daemon for AetherSDR.

Reads HID reports from the Quick Keys (USB VID:PID 28BD:5202) and sends
TCI commands to AetherSDR's TCI WebSocket server (ws://localhost:50001).

HID report format (9 bytes, report ID 0x02):
  byte 0: 0x02  (report ID)
  byte 1: 0xF0  (always)
  byte 2: button bitmask  (0x01=Btn1 .. 0x80=Btn8, 0=release)
  byte 3: 0x01=left button press, 0x02=knob press, 0x00=release
  byte 7: 0x01=knob CW, 0x02=knob CCW, 0x00=idle

Usage:
  python3 quickkeys_aethersdr.py [--config path/to/config.json] [--verbose]

Requires:
  pip install websocket-client hid
"""

import argparse
import json
import logging
import os
import sys
import threading
import time

try:
    import websocket
except ImportError:
    print("ERROR: websocket-client not installed. Run: pip install websocket-client")
    sys.exit(1)

try:
    import hid
except ImportError:
    print("ERROR: hid not installed. Run: pip install hid")
    sys.exit(1)

# ── Constants ──────────────────────────────────────────────────────────────────

VENDOR_ID   = 0x28BD
PRODUCT_ID  = 0x5202
REPORT_ID   = 0x02
REPORT_LEN  = 9
REPORT_SYNC = 0xF0

# Byte 2 — 8 labeled buttons
_BTN_BITS = {
    0x01: "btn1", 0x02: "btn2", 0x04: "btn3", 0x08: "btn4",
    0x10: "btn5", 0x20: "btn6", 0x40: "btn7", 0x80: "btn8",
}

# Band table (name, default_freq_hz)
_BANDS = [
    ("160m",  1_900_000), ("80m",  3_800_000), ("60m",  5_357_000),
    ("40m",   7_200_000), ("30m", 10_125_000), ("20m", 14_225_000),
    ("17m",  18_130_000), ("15m", 21_300_000), ("12m", 24_950_000),
    ("10m",  28_400_000), ("6m",  50_150_000),
]

# SmartSDR standard tune step sizes in Hz.  Inferred steps are snapped to the
# nearest value in this list to avoid spurious steps from click-tunes that
# happen to land on a "valid" delta.
_KNOWN_STEPS = [1, 10, 50, 100, 500, 1_000, 5_000, 10_000, 50_000, 100_000, 500_000]

# UI-driven VFO deltas larger than this are band-changes or click-tunes — skip.
_MAX_INFER_STEP_HZ = 500_000

log = logging.getLogger("quickkeys")


# ── Device discovery ───────────────────────────────────────────────────────────

def find_hid_devices() -> list[dict]:
    """Return all hidapi device-info dicts matching the Xencelabs Quick Keys VID:PID."""
    devices = hid.enumerate(VENDOR_ID, PRODUCT_ID)
    if devices:
        for d in devices:
            path_str = d.get("path", b"")
            if isinstance(path_str, bytes):
                path_str = path_str.decode("utf-8", errors="replace")
            log.info(
                "Found Quick Keys: %s (usage_page=0x%04X usage=0x%04X interface=%d)",
                path_str,
                d.get("usage_page", 0),
                d.get("usage", 0),
                d.get("interface_number", -1),
            )
    return devices



# ── TCI client ─────────────────────────────────────────────────────────────────

class TciClient:
    def __init__(self, host: str, port: int):
        self._url = f"ws://{host}:{port}"
        self._ws = None
        self._connected = False
        self._lock = threading.Lock()
<<<<<<< HEAD
<<<<<<< HEAD
        self._callbacks = []
=======
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
        self._callbacks = []
>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def send(self, command: str):
        with self._lock:
            if self._ws and self._connected:
                try:
                    self._ws.send(command)
                    log.debug(f"TCI → {command!r}")
                except Exception as e:
                    log.error(f"TCI send error: {e}")
            else:
                log.warning(f"TCI not connected, dropped: {command!r}")

<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)
    def add_message_callback(self, cb):
        """Register a callable(message: str) to receive all inbound TCI messages."""
        self._callbacks.append(cb)

<<<<<<< HEAD
=======
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)
    def is_connected(self) -> bool:
        return self._connected

    def _run(self):
        while True:
            try:
                log.info(f"Connecting to TCI at {self._url}")
                self._ws = websocket.WebSocketApp(
                    self._url,
                    on_open=self._on_open,
<<<<<<< HEAD
<<<<<<< HEAD
                    on_message=self._on_message,
=======
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
                    on_message=self._on_message,
>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)
                    on_close=self._on_close,
                    on_error=self._on_error,
                )
                self._ws.run_forever()
            except Exception as e:
                log.error(f"TCI error: {e}")
            self._connected = False
            log.info("TCI disconnected, retrying in 3s...")
            time.sleep(3)

    def _on_open(self, ws):
        with self._lock:
            self._connected = True
        log.info("TCI connected")

<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)
    def _on_message(self, ws, message):
        for cb in self._callbacks:
            try:
                cb(message)
            except Exception as e:
                log.error(f"TCI callback error: {e}")

<<<<<<< HEAD
=======
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)
    def _on_close(self, ws, code, msg):
        with self._lock:
            self._connected = False
        log.info(f"TCI closed ({code})")

    def _on_error(self, ws, error):
        log.error(f"TCI error: {error}")


# ── Built-in action handler ────────────────────────────────────────────────────

<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
_VFO_RE     = __import__("re").compile(r"^vfo:(\d+),\d+,(\d+);?$")
_TXEN_RE    = __import__("re").compile(r"^tx_enable:(\d+),(true|false);?$")

<<<<<<< HEAD

=======
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
_VFO_RE = __import__("re").compile(r"^vfo:(\d+),\d+,(\d+);?$")
=======
_VFO_RE     = __import__("re").compile(r"^vfo:(\d+),\d+,(\d+);?$")
_TXEN_RE    = __import__("re").compile(r"^tx_enable:(\d+),(true|false);?$")
>>>>>>> ed608aa (Track TX slice via tx_enable: and direct all actions to it)

# SmartSDR standard step sizes — used to validate inferred steps.
# Only deltas that are multiples of 10 Hz and ≤ 500 kHz are treated as steps;
# larger jumps (click-tune, band changes) are ignored.
_MAX_INFER_STEP_HZ = 500_000

=======
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)

>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)
class BuiltinActions:
    def __init__(self, tci: TciClient, config: dict):
        self._tci = tci
        self._tune_step = int(config.get("tune_step_hz", 1000))
<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> ed608aa (Track TX slice via tx_enable: and direct all actions to it)
        self._tx_trx = 0            # TRX index that currently has TX focus
        self._freq_hz = {0: 14_225_000}  # per-trx frequency cache
        self._last_sent_hz = None   # freq of the last vfo: command we sent
        self._band_idx = 5          # 20m default
=======
        self._freq_hz = 14_225_000
<<<<<<< HEAD
        self._band_idx = 5   # 20m default
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
        self._last_sent_hz = None   # freq of the last vfo: command we sent
        self._band_idx = 5          # 20m default
>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)
        self._volume = 50
        self._muted = False
        self._mox = False

<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
        # Subscribe to inbound TCI messages to track TX slice and infer step size
        tci.add_message_callback(self._on_tci_message)

    def _on_tci_message(self, message: str):
        msg = message.strip()

        # Track which TRX has TX focus
        m = _TXEN_RE.match(msg)
        if m:
            trx, state = int(m.group(1)), m.group(2)
            if state == "true":
                if trx != self._tx_trx:
                    log.info(f"TX focus → trx:{trx}")
                    self._tx_trx = trx
            return

        # Track VFO frequency per TRX and infer step from UI changes
        m = _VFO_RE.match(msg)
        if not m:
            return
        trx = int(m.group(1))
        hz  = int(m.group(2))
        prev = self._freq_hz.get(trx, hz)

        # If this matches what we just sent, it's our own echo — just update cache.
        if hz == self._last_sent_hz:
            self._freq_hz[trx] = hz
            self._last_sent_hz = None
            return

        # UI-driven change on the TX slice — infer step if it looks like one.
        # Snap to the nearest known SmartSDR step size to avoid treating a
        # click-tune that happens to land on a "valid" delta as a step change.
<<<<<<< HEAD
        if trx == self._tx_trx:
            delta = abs(hz - prev)
            if 10 <= delta <= _MAX_INFER_STEP_HZ and delta % 10 == 0:
                snapped = min(_KNOWN_STEPS, key=lambda s: abs(s - delta))
                if snapped != self._tune_step:
                    log.info("Step size inferred from UI: %d Hz (raw delta: %d Hz)", snapped, delta)
                    self._tune_step = snapped

        self._freq_hz[trx] = hz

    @property
    def _freq(self) -> int:
        return self._freq_hz.get(self._tx_trx, 14_225_000)

    @_freq.setter
    def _freq(self, hz: int):
        self._freq_hz[self._tx_trx] = hz

    def run(self, action: str):
        t   = self._tci
        trx = self._tx_trx
        if action == "tune_up":
            self._freq = self._freq + self._tune_step
            self._last_sent_hz = self._freq
            t.send(f"vfo:{trx},0,{self._freq};")
        elif action == "tune_down":
            self._freq = self._freq - self._tune_step
            self._last_sent_hz = self._freq
            t.send(f"vfo:{trx},0,{self._freq};")
        elif action == "band_up":
            self._band_idx = (self._band_idx + 1) % len(_BANDS)
            _, hz = _BANDS[self._band_idx]
            self._freq = hz
            self._last_sent_hz = hz
            t.send(f"vfo:{trx},0,{hz};")
=======
=======
        # Subscribe to inbound TCI messages to track frequency and infer step size
=======
        # Subscribe to inbound TCI messages to track TX slice and infer step size
>>>>>>> ed608aa (Track TX slice via tx_enable: and direct all actions to it)
        tci.add_message_callback(self._on_tci_message)

    def _on_tci_message(self, message: str):
        msg = message.strip()

        # Track which TRX has TX focus
        m = _TXEN_RE.match(msg)
        if m:
            trx, state = int(m.group(1)), m.group(2)
            if state == "true":
                if trx != self._tx_trx:
                    log.info(f"TX focus → trx:{trx}")
                    self._tx_trx = trx
            return

        # Track VFO frequency per TRX and infer step from UI changes
        m = _VFO_RE.match(msg)
        if not m:
            return
        trx = int(m.group(1))
        hz  = int(m.group(2))
        prev = self._freq_hz.get(trx, hz)

        # If this matches what we just sent, it's our own echo — just update cache.
        if hz == self._last_sent_hz:
            self._freq_hz[trx] = hz
            self._last_sent_hz = None
            return

        # UI-driven change on the TX slice — infer step if it looks like one.
=======
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)
        if trx == self._tx_trx:
            delta = abs(hz - prev)
            if 10 <= delta <= _MAX_INFER_STEP_HZ and delta % 10 == 0:
                snapped = min(_KNOWN_STEPS, key=lambda s: abs(s - delta))
                if snapped != self._tune_step:
                    log.info("Step size inferred from UI: %d Hz (raw delta: %d Hz)", snapped, delta)
                    self._tune_step = snapped

        self._freq_hz[trx] = hz

    @property
    def _freq(self) -> int:
        return self._freq_hz.get(self._tx_trx, 14_225_000)

    @_freq.setter
    def _freq(self, hz: int):
        self._freq_hz[self._tx_trx] = hz

>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)
    def run(self, action: str):
        t   = self._tci
        trx = self._tx_trx
        if action == "tune_up":
            self._freq = self._freq + self._tune_step
            self._last_sent_hz = self._freq
            t.send(f"vfo:{trx},0,{self._freq};")
        elif action == "tune_down":
            self._freq = self._freq - self._tune_step
            self._last_sent_hz = self._freq
            t.send(f"vfo:{trx},0,{self._freq};")
        elif action == "band_up":
            self._band_idx = (self._band_idx + 1) % len(_BANDS)
            _, hz = _BANDS[self._band_idx]
            self._freq = hz
            self._last_sent_hz = hz
<<<<<<< HEAD
            t.send(f"vfo:0,0,{hz};")
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
            t.send(f"vfo:{trx},0,{hz};")
>>>>>>> ed608aa (Track TX slice via tx_enable: and direct all actions to it)
            log.info(f"Band → {_BANDS[self._band_idx][0]}")
        elif action == "band_down":
            self._band_idx = (self._band_idx - 1) % len(_BANDS)
            _, hz = _BANDS[self._band_idx]
<<<<<<< HEAD
<<<<<<< HEAD
            self._freq = hz
            self._last_sent_hz = hz
            t.send(f"vfo:{trx},0,{hz};")
            log.info(f"Band → {_BANDS[self._band_idx][0]}")
        elif action == "ptt_on":
            t.send(f"trx:{trx},true;")
        elif action == "ptt_off":
            t.send(f"trx:{trx},false;")
        elif action == "mox_toggle":
            self._mox = not self._mox
            t.send(f"trx:{trx},{'true' if self._mox else 'false'};")
        elif action == "mute_toggle":
            self._muted = not self._muted
            t.send(f"mute:{trx},0,{'true' if self._muted else 'false'};")
=======
            self._freq_hz = hz
=======
            self._freq = hz
>>>>>>> ed608aa (Track TX slice via tx_enable: and direct all actions to it)
            self._last_sent_hz = hz
            t.send(f"vfo:{trx},0,{hz};")
            log.info(f"Band → {_BANDS[self._band_idx][0]}")
        elif action == "ptt_on":
            t.send(f"trx:{trx},true;")
        elif action == "ptt_off":
            t.send(f"trx:{trx},false;")
        elif action == "mox_toggle":
            self._mox = not self._mox
            t.send(f"trx:{trx},{'true' if self._mox else 'false'};")
        elif action == "mute_toggle":
            self._muted = not self._muted
<<<<<<< HEAD
            t.send(f"mute:0,0,{'true' if self._muted else 'false'};")
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
            t.send(f"mute:{trx},0,{'true' if self._muted else 'false'};")
>>>>>>> ed608aa (Track TX slice via tx_enable: and direct all actions to it)
        elif action == "vol_up":
            self._volume = min(100, self._volume + 5)
            t.send(f"volume:{self._volume};")
        elif action == "vol_down":
            self._volume = max(0, self._volume - 5)
            t.send(f"volume:{self._volume};")
        else:
            log.warning(f"Unknown built-in action: {action!r}")

<<<<<<< HEAD
<<<<<<< HEAD
=======
    # Called by TCI status messages to keep freq in sync (future enhancement)
    def sync_freq(self, hz: int):
        self._freq_hz = hz
=======
>>>>>>> f13d182 (Infer step size from TCI vfo: messages to honour RX Controls step)

<<<<<<< HEAD
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)

=======
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)
# ── HID event dispatcher ───────────────────────────────────────────────────────

class QuickKeysDispatcher:
    _BUILTINS = {
        "tune_up", "tune_down", "band_up", "band_down",
        "ptt_on", "ptt_off", "mox_toggle", "mute_toggle",
        "vol_up", "vol_down", "tune_cw", "tune_ccw",
    }

    def __init__(self, tci: TciClient, config: dict):
        self._tci = tci
        self._builtin = BuiltinActions(tci, config)
        self._buttons_cfg = config.get("buttons", {})
        self._knob_cw  = config.get("knob_cw",  "tune_cw")
        self._knob_ccw = config.get("knob_ccw", "tune_ccw")
        # Track previous button state for release detection
        self._prev_btn_mask = 0x00
        self._prev_b3       = 0x00
<<<<<<< HEAD
<<<<<<< HEAD
        self._lock = threading.Lock()
=======
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
        self._lock = threading.Lock()
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)

    def _dispatch(self, action: str):
        if not action:
            return
        if action in self._BUILTINS:
            # Map tune_cw/ccw to tune_up/down
            if action == "tune_cw":
                self._builtin.run("tune_up")
            elif action == "tune_ccw":
                self._builtin.run("tune_down")
            else:
                self._builtin.run(action)
        else:
            self._tci.send(action)

    def handle_report(self, report: bytes):
        if len(report) < REPORT_LEN:
            return
        if report[0] != REPORT_ID or report[1] != REPORT_SYNC:
            return

        btn_mask = report[2]
        b3       = report[3]
        b7       = report[7]

<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)
        # Serialize access since multiple reader threads may call handle_report.
        with self._lock:
            # ── 8 labeled buttons (byte 2 bitmask) ──
            changed = btn_mask ^ self._prev_btn_mask
            for bit, name in _BTN_BITS.items():
                if changed & bit:
                    cfg = self._buttons_cfg.get(name, {})
                    if btn_mask & bit:
                        log.debug(f"{name} press")
                        self._dispatch(cfg.get("press", ""))
                    else:
                        log.debug(f"{name} release")
                        self._dispatch(cfg.get("release", ""))
            self._prev_btn_mask = btn_mask

            # ── Left button (byte 3 bit 0) ──
            left_now  = bool(b3 & 0x01)
            left_prev = bool(self._prev_b3 & 0x01)
            if left_now != left_prev:
                cfg = self._buttons_cfg.get("left", {})
                if left_now:
                    log.debug("left press")
<<<<<<< HEAD
                    self._dispatch(cfg.get("press", ""))
                else:
                    log.debug("left release")
                    self._dispatch(cfg.get("release", ""))

            # ── Knob press (byte 3 bit 1) ──
            knob_now  = bool(b3 & 0x02)
            knob_prev = bool(self._prev_b3 & 0x02)
            if knob_now != knob_prev:
                cfg = self._buttons_cfg.get("knob", {})
                if knob_now:
                    log.debug("knob press")
                    self._dispatch(cfg.get("press", ""))
                else:
                    log.debug("knob release")
                    self._dispatch(cfg.get("release", ""))

            self._prev_b3 = b3

            # ── Knob rotation (byte 7) ──
            if b7 == 0x01:
                log.debug("knob CW")
                self._dispatch(self._knob_cw)
            elif b7 == 0x02:
                log.debug("knob CCW")
                self._dispatch(self._knob_ccw)


# ── HID read loop ──────────────────────────────────────────────────────────────

def _reader_thread(path: bytes, dispatcher: QuickKeysDispatcher, stop_evt: threading.Event):
    """Open one HID interface by path and dispatch reports until stop_evt is set."""
    path_str = path.decode("utf-8", errors="replace") if isinstance(path, bytes) else str(path)
    dev = hid.device()
    try:
        dev.open_path(path)
        log.info("Opened %s", path_str)
        while not stop_evt.is_set():
            # read() returns a list of ints (one complete report) or [] on timeout.
            data = dev.read(REPORT_LEN, timeout_ms=500)
            if data:
                dispatcher.handle_report(bytes(data))
    except PermissionError:
        log.error("Permission denied: %s — on Linux add your user to the 'input' group.", path_str)
        stop_evt.set()
    except OSError as e:
        log.warning("%s: device error: %s", path_str, e)
        stop_evt.set()
    finally:
        try:
            dev.close()
        except Exception:
            pass


def run(dispatcher: QuickKeysDispatcher):
    """Enumerate Quick Keys HID interfaces and dispatch events in a reconnect loop."""
    while True:
        devices = find_hid_devices()
        if not devices:
            log.error("Xencelabs Quick Keys not found. Is it plugged in? Retrying in 5s.")
            time.sleep(5)
            continue

        stop_evt = threading.Event()
        threads = []
        for info in devices:
            t = threading.Thread(
                target=_reader_thread,
                args=(info["path"], dispatcher, stop_evt),
                daemon=True,
            )
            t.start()
            threads.append(t)

        log.info("Started %d reader thread(s) for Quick Keys", len(threads))
        # Block until any thread signals a device error or disconnect.
        stop_evt.wait()
        log.info("Device disconnected — re-enumerating in 2s")
        time.sleep(2)
=======
        # ── 8 labeled buttons (byte 2 bitmask) ──
        changed = btn_mask ^ self._prev_btn_mask
        for bit, name in _BTN_BITS.items():
            if changed & bit:
                cfg = self._buttons_cfg.get(name, {})
                if btn_mask & bit:
                    log.debug(f"{name} press")
=======
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)
                    self._dispatch(cfg.get("press", ""))
                else:
                    log.debug("left release")
                    self._dispatch(cfg.get("release", ""))

            # ── Knob press (byte 3 bit 1) ──
            knob_now  = bool(b3 & 0x02)
            knob_prev = bool(self._prev_b3 & 0x02)
            if knob_now != knob_prev:
                cfg = self._buttons_cfg.get("knob", {})
                if knob_now:
                    log.debug("knob press")
                    self._dispatch(cfg.get("press", ""))
                else:
                    log.debug("knob release")
                    self._dispatch(cfg.get("release", ""))

            self._prev_b3 = b3

            # ── Knob rotation (byte 7) ──
            if b7 == 0x01:
                log.debug("knob CW")
                self._dispatch(self._knob_cw)
            elif b7 == 0x02:
                log.debug("knob CCW")
                self._dispatch(self._knob_ccw)


# ── HID read loop ──────────────────────────────────────────────────────────────

def _reader_thread(path: bytes, dispatcher: QuickKeysDispatcher, stop_evt: threading.Event):
    """Open one HID interface by path and dispatch reports until stop_evt is set."""
    path_str = path.decode("utf-8", errors="replace") if isinstance(path, bytes) else str(path)
    dev = hid.device()
    try:
        dev.open_path(path)
        log.info("Opened %s", path_str)
        while not stop_evt.is_set():
            # read() returns a list of ints (one complete report) or [] on timeout.
            data = dev.read(REPORT_LEN, timeout_ms=500)
            if data:
                dispatcher.handle_report(bytes(data))
    except PermissionError:
        log.error("Permission denied: %s — on Linux add your user to the 'input' group.", path_str)
        stop_evt.set()
    except OSError as e:
        log.warning("%s: device error: %s", path_str, e)
        stop_evt.set()
    finally:
        try:
            dev.close()
        except Exception:
            pass


def run(dispatcher: QuickKeysDispatcher):
    """Enumerate Quick Keys HID interfaces and dispatch events in a reconnect loop."""
    while True:
        devices = find_hid_devices()
        if not devices:
            log.error("Xencelabs Quick Keys not found. Is it plugged in? Retrying in 5s.")
            time.sleep(5)
            continue

<<<<<<< HEAD
        try:
            while True:
                readable, _, _ = select.select(list(fds.keys()), [], [], 1.0)
                for f in readable:
                    chunk = f.read(64)
                    if not chunk:
                        raise OSError("Device disconnected")
                    bufs[f] += chunk
                    bufs[f] = _consume(bufs[f], dispatcher)
        except OSError as e:
            log.warning(f"Device error: {e} — reopening in 2s")
            for f in fds:
                try:
                    f.close()
                except OSError:
                    pass
            time.sleep(2)
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
        stop_evt = threading.Event()
        threads = []
        for info in devices:
            t = threading.Thread(
                target=_reader_thread,
                args=(info["path"], dispatcher, stop_evt),
                daemon=True,
            )
            t.start()
            threads.append(t)

        log.info("Started %d reader thread(s) for Quick Keys", len(threads))
        # Block until any thread signals a device error or disconnect.
        stop_evt.wait()
        log.info("Device disconnected — re-enumerating in 2s")
        time.sleep(2)
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)


# ── Entry point ────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Xencelabs Quick Keys → AetherSDR TCI daemon")
    parser.add_argument("--config", default=os.path.join(os.path.dirname(__file__), "config.json"),
                        help="Path to config.json (default: config.json next to this script)")
<<<<<<< HEAD
<<<<<<< HEAD
=======
    parser.add_argument("--device", default=None,
                        help="hidraw device path (default: auto-detect by VID:PID)")
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Enable debug logging")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    # Load config
    try:
        with open(args.config) as f:
            config = json.load(f)
        log.info(f"Config loaded from {args.config}")
    except FileNotFoundError:
        log.warning(f"Config not found at {args.config}, using defaults")
        config = {}
    except json.JSONDecodeError as e:
        log.error(f"Config parse error: {e}")
        sys.exit(1)

<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
=======
    # Find device
    hidraw_path = args.device or find_hidraw_device()
    if not hidraw_path:
=======
    # Find device(s)
    if args.device:
        hidraw_paths = [args.device]
    else:
        hidraw_paths = find_hidraw_devices()
    if not hidraw_paths:
>>>>>>> c56e9be (Fix Quick Keys daemon to open all matching hidraw interfaces)
        log.error("Xencelabs Quick Keys not found. Is it plugged in?")
        sys.exit(1)

>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)
    # Connect TCI
    host = config.get("tci_host", "localhost")
    port = int(config.get("tci_port", 50001))
    tci = TciClient(host, port)

    dispatcher = QuickKeysDispatcher(tci, config)

<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
    log.info(f"Quick Keys daemon starting — tci={host}:{port}")
    run(dispatcher)
=======
    log.info(f"Quick Keys daemon starting — device={hidraw_path} tci={host}:{port}")
    run(hidraw_path, dispatcher)
>>>>>>> 51396b4 (Add Xencelabs Quick Keys daemon for AetherSDR TCI control)
=======
    log.info(f"Quick Keys daemon starting — interfaces={hidraw_paths} tci={host}:{port}")
    run(hidraw_paths, dispatcher)
>>>>>>> c56e9be (Fix Quick Keys daemon to open all matching hidraw interfaces)
=======
    log.info(f"Quick Keys daemon starting — tci={host}:{port}")
    run(dispatcher)
>>>>>>> c338e72 (Replace Linux-only hidraw with cross-platform hidapi in Quick Keys daemon)


if __name__ == "__main__":
    main()
