#!/usr/bin/env python3
"""Phase 0 OQ#1 probe for RADE V2 waveform-in-process feasibility.

This is the article that produced RADE V2 RFC §8 "OQ#1 — GO": the single most
load-bearing result in the design, since a rejection here would have forced a
dedicated socket or an out-of-process waveform daemon.

Stage 1 (default): read-only handshake validation — connect, read greeting,
send a benign `version`/`sub radio all`, print everything. No state change.

Stage 2 (--waveform): register as a GUI client, then attempt `waveform create`
on that same connection (the OQ#1 test), capture the response, then remove it.
RX-only, no TX, transient (waveforms auto-clear on disconnect).

Preserved verbatim from the 2026-07-26 Phase 0 session apart from the radio host
being made overridable (it was hardcoded). Kept for provenance: RFC §8 cites its
output, so the article should be inspectable alongside the claim.
"""
import os, socket, sys, time, threading

HOST = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
PORT = int(os.environ.get("FLEX_RADIO_PORT", "4992"))

class Flex:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=5)
        self.s.settimeout(0.5)
        self.buf = b""
        self.seq = 0
        self.lines = []
        self.replies = {}       # seq -> (code, message)
        self.stop = False
        self.rx = threading.Thread(target=self._reader, daemon=True)
        self.rx.start()

    def _reader(self):
        while not self.stop:
            try:
                data = self.s.recv(4096)
                if not data:
                    break
                self.buf += data
                while b"\n" in self.buf:
                    line, self.buf = self.buf.split(b"\n", 1)
                    txt = line.decode("utf-8", "replace").rstrip("\r")
                    if not txt:
                        continue
                    self.lines.append(("<", txt))
                    if txt[0] == "R":
                        try:
                            body = txt[1:]
                            seq_s, rest = body.split("|", 1)
                            parts = rest.split("|", 1)
                            code = int(parts[0], 16)
                            msg = parts[1] if len(parts) > 1 else ""
                            self.replies[int(seq_s)] = (code, msg)
                        except Exception:
                            pass
            except socket.timeout:
                continue
            except Exception:
                break

    def send(self, cmd):
        self.seq += 1
        s = self.seq
        wire = f"C{s}|{cmd}\n"
        self.lines.append((">", wire.strip()))
        self.s.sendall(wire.encode())
        return s

    def wait_reply(self, seq, timeout=4.0):
        end = time.time() + timeout
        while time.time() < end:
            if seq in self.replies:
                return self.replies[seq]
            time.sleep(0.05)
        return None

    def close(self):
        self.stop = True
        try: self.s.close()
        except Exception: pass


def dump(f):
    for d, t in f.lines:
        print(f"  {d} {t}")


def main():
    do_wf = "--waveform" in sys.argv
    f = Flex(HOST, PORT)
    try:
        time.sleep(1.5)  # collect greeting (V/H/status)
        print("=== greeting / initial traffic ===")
        dump(f)
        f.lines.clear()

        print("\n=== read-only: version ===")
        s = f.send("version")
        print("  reply:", f.wait_reply(s))
        dump(f); f.lines.clear()

        if not do_wf:
            print("\n(stage 1 only; re-run with --waveform for the OQ#1 test)")
            return

        print("\n=== register as GUI client ===")
        s = f.send("client gui")
        print("  client gui reply:", f.wait_reply(s))
        time.sleep(0.5)
        s = f.send("client station AetherRadeProbe")
        print("  client station reply:", f.wait_reply(s))
        dump(f); f.lines.clear()

        print("\n=== OQ#1: waveform create on this GUI connection ===")
        f.send("waveform remove AetherRadeProbe")  # idempotent pre-clean
        time.sleep(0.3)
        s = f.send("waveform create name=AetherRadeProbe mode=RADP underlying_mode=USB version=0.0.1")
        rep = f.wait_reply(s)
        print("  >>> waveform create reply:", rep)
        time.sleep(0.5)
        dump(f); f.lines.clear()

        print("\n=== cleanup: waveform remove ===")
        s = f.send("waveform remove AetherRadeProbe")
        print("  remove reply:", f.wait_reply(s))
        dump(f)

        print("\n=== VERDICT ===")
        if rep is None:
            print("  NO REPLY to waveform create — inconclusive")
        elif rep[0] == 0:
            print("  SUCCESS: a GUI-client connection CAN create a waveform (OQ#1 = single connection works)")
            print("  create message payload:", repr(rep[1]))
        else:
            print(f"  REJECTED: code=0x{rep[0]:08X} msg={rep[1]!r} — GUI connection may need a dedicated socket")
    finally:
        f.close()

if __name__ == "__main__":
    main()
