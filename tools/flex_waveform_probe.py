#!/usr/bin/env python3
"""Phase 2 check (RADE V2 RFC §10.1): which `underlying_mode` values does the
Flex waveform API accept?

Phase 0 established that a GUI-client connection can `waveform create` on its own
TCP-4992 socket, but only ever tested `underlying_mode=USB`. The RFC now specifies
DIGU (data variant, carries the 1500 Hz digu_offset dial convention). This confirms
DIGU is accepted, with USB as a known-good control and DIGL for the fallback case.

Registration needs no slice and no pan, so this does not contend with a connected
client. Each waveform is removed immediately; the radio also auto-removes on
disconnect.
"""

import os
import socket
import sys
import time

RADIO = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
PORT = int(os.environ.get("FLEX_RADIO_PORT", "4992"))
STATION = "AetherRadeProbe"
PROBE_MODE = "RADP"          # probe mode name, not the real RAD2
CANDIDATES = ["USB", "DIGU", "DIGL"]


class Flex:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(5)
        self.buf = ""
        self.seq = 0
        self.handle = None
        self._drain_greeting()

    def _readline(self):
        while "\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed by radio")
            self.buf += chunk.decode("utf-8", errors="replace")
        line, self.buf = self.buf.split("\n", 1)
        return line.strip()

    def _drain_greeting(self):
        # Radio sends V<ver> then H<handle>, then a burst of status.
        deadline = time.time() + 2.0
        while time.time() < deadline:
            try:
                self.sock.settimeout(0.5)
                line = self._readline()
            except (socket.timeout, TimeoutError):
                break
            if line.startswith("H"):
                self.handle = line[1:]
        self.sock.settimeout(5)

    def cmd(self, text):
        """Send a command, return (code, payload) from its R-reply."""
        self.seq += 1
        seq = self.seq
        self.sock.sendall(f"C{seq}|{text}\n".encode())
        deadline = time.time() + 5.0
        while time.time() < deadline:
            line = self._readline()
            if line.startswith(f"R{seq}|"):
                parts = line.split("|", 2)
                code = parts[1] if len(parts) > 1 else ""
                payload = parts[2] if len(parts) > 2 else ""
                return code, payload
        raise RuntimeError(f"no reply to: {text}")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def ok(code):
    """Flex reply codes are hex; 0 means success."""
    try:
        return int(code, 16) == 0
    except ValueError:
        return False


def main():
    print(f"connecting to {RADIO}:{PORT}")
    r = Flex(RADIO, PORT)
    print(f"  handle={r.handle}")

    code, payload = r.cmd("client gui")
    print(f"  client gui              -> {code} {payload}")
    code, payload = r.cmd(f"client station {STATION}")
    print(f"  client station          -> {code} {payload}")
    print()

    results = {}
    for um in CANDIDATES:
        name = f"{STATION}{um}"
        create = (f"waveform create name={name} mode={PROBE_MODE} "
                  f"underlying_mode={um} version=0.0.1")
        code, payload = r.cmd(create)
        accepted = ok(code)
        results[um] = (code, payload, accepted)
        status = "ACCEPTED" if accepted else "REJECTED"
        print(f"underlying_mode={um:<5} {status}  code={code}")
        if payload:
            print(f"    payload: {payload}")

        if accepted:
            rcode, rpayload = r.cmd(f"waveform remove {name}")
            print(f"    remove -> {rcode} {rpayload}".rstrip())
        time.sleep(0.3)
        print()

    r.close()

    print("=" * 60)
    for um in CANDIDATES:
        code, payload, accepted = results[um]
        print(f"  {um:<5} {'ACCEPTED' if accepted else 'REJECTED'}  (code {code})")

    if not results["USB"][2]:
        print("\nWARNING: the USB control was REJECTED — the probe itself is suspect,")
        print("so a DIGU rejection here proves nothing. Investigate before concluding.")
        return 2
    return 0 if results["DIGU"][2] else 1


if __name__ == "__main__":
    sys.exit(main())
