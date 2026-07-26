#!/usr/bin/env python3
"""RADE V2 RFC §10.1 Phase 2 data-plane probe.

Answers on live hardware:
  A. Is `waveform set <X> rx_filter high_cut=` HONOURED on the audio the radio
     delivers to rx_stream_in? (differential: sweep high_cut, compare spectra)
  B. What is the rx_stream_in payload layout — I/Q, or duplicated stereo audio?

Uses the slice the radio restores for a GUI client rather than creating one (the
8400 is 2/2 pans/slices and the restored pair already holds one). Saves and
restores the slice mode in a finally block.

RX ONLY. Never sets tx=1, never keys the transmitter.
"""

import os
import socket
import struct
import sys
import time

import numpy as np

RADIO = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
STATION = "AetherRadeProbe"
WF_NAME = "AetherRadeProbe"
WF_MODE = "RADP"
UNDERLYING = os.environ.get("PROBE_UNDERLYING", "DIGU")
CAPTURE_S = float(os.environ.get("PROBE_CAPTURE_S", "3.0"))
HIGH_CUTS = [int(x) for x in os.environ.get("PROBE_CUTS", "1200,2200,3200").split(",")]
LOW_CUT = int(os.environ.get("PROBE_LOWCUT", "800"))
FS = 24000


class Flex:
    def __init__(self, host, port=4992):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = ""
        self.seq = 0
        self.handle = None
        self.status = []
        self._drain(1.5)

    def _readline(self, t):
        self.sock.settimeout(t)
        while "\n" not in self.buf:
            c = self.sock.recv(65536)
            if not c:
                raise RuntimeError("connection closed")
            self.buf += c.decode("utf-8", "replace")
        line, self.buf = self.buf.split("\n", 1)
        return line.strip()

    def _drain(self, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                self._absorb(self._readline(0.3))
            except (socket.timeout, TimeoutError):
                continue

    def _absorb(self, line):
        if line.startswith("H"):
            self.handle = line[1:]
        elif line.startswith("S"):
            self.status.append(line)

    def cmd(self, text, t=5.0):
        self.seq += 1
        n = self.seq
        self.sock.sendall(f"C{n}|{text}\n".encode())
        end = time.time() + t
        while time.time() < end:
            try:
                line = self._readline(0.5)
            except (socket.timeout, TimeoutError):
                continue
            if line.startswith(f"R{n}|"):
                p = line.split("|", 2)
                return (p[1] if len(p) > 1 else ""), (p[2] if len(p) > 2 else "")
            self._absorb(line)
        raise RuntimeError(f"no reply: {text}")

    def pump(self, secs=1.0):
        self._drain(secs)

    def slice_fields(self, idx):
        want, merged = f"slice {idx} ", {}
        for line in self.status:
            body = line.split("|", 1)[-1]
            if body.startswith(want):
                for tok in body[len(want):].split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        merged[k] = v
        return merged

    def slice_ids(self):
        ids = set()
        for line in self.status:
            body = line.split("|", 1)[-1]
            if body.startswith("slice ") and "in_use=1" in body:
                ids.add(body.split()[1])
        return sorted(ids)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def ok(code):
    try:
        return int(code, 16) == 0
    except ValueError:
        return False


def must(r, text):
    code, payload = r.cmd(text)
    print(f"  [{'ok ' if ok(code) else 'ERR'}] {text}"
          + (f"  -> {payload}" if payload else ""))
    if not ok(code):
        raise RuntimeError(f"failed ({code}): {text}")
    return payload


def flush(udp, secs=0.4):
    """Drain queued datagrams. Bounded by wall clock: at 187.5 pkt/s a packet
    arrives every ~5 ms, so a socket-timeout-only loop would never exit."""
    udp.settimeout(0.02)
    end = time.time() + secs
    while time.time() < end:
        try:
            udp.recvfrom(8192)
        except (socket.timeout, TimeoutError):
            return


def capture(udp, stream_id, secs, settle=1.0):
    """Collect rx_stream_in samples. Phase 0 saw a ~27 kHz startup burst, so the
    first `settle` seconds are discarded before the rate is measured."""
    chunks, pkts, first = [], 0, None
    udp.settimeout(1.0)
    end = time.time() + secs + settle
    while time.time() < end:
        try:
            data, _ = udp.recvfrom(8192)
        except (socket.timeout, TimeoutError):
            continue
        if len(data) < 28 or struct.unpack(">I", data[4:8])[0] != stream_id:
            continue
        now = time.time()
        if first is None:
            first = now
        if now - first < settle:
            continue                      # discard startup burst
        pkts += 1
        pay = data[28:]
        n = (len(pay) // 8) * 2
        chunks.append(np.frombuffer(pay[: n * 4], dtype=">f4").astype(np.float64))
    if not chunks:
        return np.array([]), np.array([]), 0, 0.0
    dur = time.time() - (first + settle)
    flat = np.concatenate(chunks)
    return flat[0::2], flat[1::2], pkts, dur


def psd(sig, nfft=4096):
    if sig.size < nfft:
        return np.array([]), np.array([])
    win = np.hanning(nfft)
    step = nfft // 2
    nblk = 1 + (sig.size - nfft) // step
    acc = np.zeros(nfft // 2 + 1)
    for b in range(nblk):
        seg = sig[b * step: b * step + nfft] * win
        acc += np.abs(np.fft.rfft(seg)) ** 2
    acc /= nblk
    freqs = np.fft.rfftfreq(nfft, 1.0 / FS)
    return freqs, 10 * np.log10(acc + 1e-30)


def inband_ref(freqs, db, lo=1000, hi=1800):
    m = (freqs >= lo) & (freqs <= hi)
    return 10 * np.log10(np.mean(10 ** (db[m] / 10))) if m.any() else float("nan")


def edge_6db(freqs, db, ref, start=1800):
    for f, d in zip(freqs, db):
        if f > start and d < ref - 6.0:
            return f
    return None


def main():
    r = Flex(RADIO)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    uport = udp.getsockname()[1]
    print(f"connected handle={r.handle}  udp={uport}  underlying_mode={UNDERLYING}")

    saved_mode, sid = None, None
    try:
        must(r, "client gui")
        must(r, f"client station {STATION}")
        for s in ("slice", "pan", "waveform", "radio", "client"):
            r.cmd(f"sub {s} all")
        r.pump(2.5)

        ids = r.slice_ids()
        if not ids:
            raise RuntimeError("no slice available (2/2 cap; cannot create)")
        sid = ids[0]
        f0 = r.slice_fields(sid)
        saved_mode = f0.get("mode")
        print(f"\nslice {sid}: mode={saved_mode} RF={f0.get('RF_frequency')} "
              f"filter={f0.get('filter_lo')}/{f0.get('filter_hi')} "
              f"digu_offset={f0.get('digu_offset')} audio_mute={f0.get('audio_mute')}")

        print("\n-- register waveform --")
        r.cmd(f"waveform remove {WF_NAME}")
        payload = must(r, f"waveform create name={WF_NAME} mode={WF_MODE} "
                          f"underlying_mode={UNDERLYING} version=0.0.1")
        streams = {}
        for tok in payload.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    streams[k] = int(v, 16)
                except ValueError:
                    pass
        rx_in = streams["rx_stream_in_id"]
        must(r, f"waveform set {WF_NAME} rx_filter low_cut={LOW_CUT}")
        must(r, f"waveform set {WF_NAME} rx_filter high_cut={HIGH_CUTS[-1]}")
        must(r, f"waveform set {WF_NAME} rx_filter depth=256")
        must(r, f"waveform set {WF_NAME} udpport={uport}")

        print(f"\n-- slice {sid} -> {WF_MODE} --")
        must(r, f"slice set {sid} mode={WF_MODE}")
        r.pump(2.0)
        fa = r.slice_fields(sid)
        print(f"  after: mode={fa.get('mode')} filter={fa.get('filter_lo')}/"
              f"{fa.get('filter_hi')} digu_offset={fa.get('digu_offset')} "
              f"audio_mute={fa.get('audio_mute')}")

        print("\n-- B: payload layout --")
        A, B, pkts, dur = capture(udp, rx_in, 2.0)
        if not pkts:
            raise RuntimeError("no rx_stream_in packets received")
        corr = float(np.dot(A, B) / (np.linalg.norm(A) * np.linalg.norm(B) + 1e-30))
        print(f"  pkts={pkts} pairs={A.size} rate={A.size/dur:.0f} Hz per component")
        print(f"  RMS A={np.sqrt(np.mean(A**2)):.6e}  B={np.sqrt(np.mean(B**2)):.6e}")
        print(f"  corr(A,B)={corr:+.5f}  ->  "
              + ("DUPLICATED (mono in both) " if corr > 0.999
                 else "orthogonal / independent"))

        # Decisive analytic test: a true I/Q analytic signal has energy ONLY at
        # positive frequencies. A real signal duplicated into both components
        # gives a spectrum symmetric about DC.
        n = 1 << 15
        if A.size >= n:
            z = A[:n] + 1j * B[:n]
            Z = np.abs(np.fft.fft(z * np.hanning(n))) ** 2
            pos = Z[1: n // 2].sum()
            neg = Z[n // 2 + 1:].sum()
            ratio = 10 * np.log10(pos / (neg + 1e-30))
            if ratio > 10:
                verdict = "ANALYTIC I/Q, conventional (energy at +freq): use A + jB"
            elif ratio < -10:
                verdict = ("ANALYTIC I/Q, CONJUGATE (energy at -freq): "
                           "the positive-frequency signal is A - jB")
            else:
                verdict = "symmetric -> A,B are NOT an I/Q pair (real duplicated)"
            print(f"  A+jB spectrum: +freq/-freq = {ratio:+.1f} dB  ->  {verdict}")

        print("\n-- A: is rx_filter honoured? --")
        traces = {}
        for hc in HIGH_CUTS:
            must(r, f"waveform set {WF_NAME} rx_filter high_cut={hc}")
            time.sleep(1.0)
            flush(udp)
            A, _, pkts, dur = capture(udp, rx_in, CAPTURE_S)
            if not pkts:
                print(f"  high_cut={hc}: NO PACKETS")
                continue
            fr, db = psd(A)
            ref = inband_ref(fr, db)
            edge = edge_6db(fr, db, ref)
            traces[hc] = (fr, db, ref)
            print(f"  high_cut={hc:5d} -> in-band {ref:7.1f} dB, -6 dB edge at "
                  + (f"{edge:.0f} Hz" if edge else "not found"))

        if traces:
            print("\n  spectra (dB relative to each trace's own 1000-1800 Hz level):")
            print("      Hz " + "".join(f"{hc:>10d}" for hc in HIGH_CUTS))
            for target in range(500, 4501, 250):
                row = f"  {target:6d} "
                for hc in HIGH_CUTS:
                    if hc not in traces:
                        row += f"{'-':>10}"
                        continue
                    fr, db, ref = traces[hc]
                    i = int(np.argmin(np.abs(fr - target)))
                    row += f"{db[i]-ref:>10.1f}"
                print(row)

    finally:
        print("\n-- teardown --")
        try:
            if sid is not None and saved_mode:
                code, _ = r.cmd(f"slice set {sid} mode={saved_mode}")
                print(f"  restore slice {sid} mode={saved_mode} -> {code}")
            print(f"  waveform remove -> {r.cmd(f'waveform remove {WF_NAME}')[0]}")
        except Exception as e:  # noqa: BLE001
            print(f"  teardown error: {e}")
        udp.close()
        r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
