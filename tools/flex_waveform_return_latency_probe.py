#!/usr/bin/env python3
"""How much latency does the decoded-audio RETURN path cost?

This is the measurement RFC §9.1 / §16 Q1 now turns on. §10.7 established that
routing decoded speech back through the radio costs no *fidelity*, which reopened
the choice between:

  A. local render only  -- minimal latency, other clients hear nothing
  B. return path only   -- multiflex for free, equal fidelity, + a round trip

Latency is the deciding axis and is the only number nobody has. What is measured
here is exactly B's penalty over A: client -> radio -> slice mix -> client.

Method: hold a steady 24 kHz stream of silence on `rx_stream_in_id` (the honoured
id, §10.6) so the path is in steady state, punch periodic tone bursts into it,
timestamp each burst as its packet goes to the socket, and timestamp the burst's
reappearance in `remote_audio_rx`. In the waveform mode the monitor is EXACTLY
silent (§10.6), so onset detection is unambiguous -- no threshold tuning.

Resolution is one packet, 128 samples @ 24 kHz = 5.33 ms, which is far finer than
the distinction that matters (is this 20 ms or 200 ms?). Report min as well as
median: min is the cleanest estimate of transport latency, since jitter and
scheduling only ever add.

RX ONLY. Never keys the transmitter.
"""

import os
import socket
import struct
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flex_waveform_tier1_probe import Flex, must, ok  # noqa: E402

RADIO = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
STATION = WF_NAME = "AetherRadeProbe"
WF_MODE = "RADP"
UNDERLYING = os.environ.get("PROBE_UNDERLYING", "DIGU")
FS = 24000
SPP = 128
VITA_PORT = 4991
OUI = 0x001C2D
AUDIO_CLASS = 0x534C03E3

N_BURSTS = int(os.environ.get("PROBE_BURSTS", "8"))
BURST_MS = 40
GAP_MS = 700
LEAD_MS = 1500          # steady-state silence before the first burst
BURST_HZ = 1000.0
BURST_AMP = 0.5


class TimedCapture:
    """Capture one stream id, retaining per-packet arrival timestamps."""

    def __init__(self, udp, stream_id):
        self.udp = udp
        self.stream_id = stream_id
        self.packets = []          # (arrival_perf, samples ndarray)
        self._stop = threading.Event()
        self._t = None

    def _run(self):
        self.udp.settimeout(0.2)
        while not self._stop.is_set():
            try:
                data, _ = self.udp.recvfrom(8192)
            except (socket.timeout, TimeoutError):
                continue
            except OSError:
                break
            now = time.perf_counter()
            if len(data) < 28 or struct.unpack(">I", data[4:8])[0] != self.stream_id:
                continue
            pay = data[28:]
            n = len(pay) // 4
            arr = np.frombuffer(pay[: n * 4], dtype=">f4").astype(np.float64)
            self.packets.append((now, arr[0::2]))   # left channel

    def __enter__(self):
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        if self._t:
            self._t.join(timeout=2.0)
        return False

    def envelope(self):
        """Per-packet (arrival, peak) — the raw material for onset detection."""
        return [(a, float(np.max(np.abs(s))) if s.size else 0.0)
                for a, s in self.packets]

    def onsets(self, verbose=False):
        """Arrival times of rising edges, one per burst.

        An earlier version assumed the monitor was EXACTLY zero between bursts
        (true in the §10.6 runs) and used a fixed 1e-3 threshold with a simple
        armed/disarmed flag. When the monitor turned out to carry a steady
        background instead, that fired once on the first packet and never
        re-armed -- yielding one bogus onset and a negative latency. The
        threshold is now derived from the capture itself, with hysteresis.
        """
        env = self.envelope()
        if not env:
            return []
        peaks = np.array([p for _, p in env])
        floor = float(np.median(peaks))
        ceiling = float(np.percentile(peaks, 99))
        if ceiling <= floor * 1.5:
            if verbose:
                print(f"  [onset] no clear bursts: floor={floor:.3e} "
                      f"p99={ceiling:.3e} -- signal does not stand out")
            return []
        hi = floor + 0.5 * (ceiling - floor)
        lo = floor + 0.2 * (ceiling - floor)
        if verbose:
            print(f"  [onset] floor={floor:.3e} p99={ceiling:.3e} "
                  f"hi={hi:.3e} lo={lo:.3e}")
        out, armed = [], True
        for (arrival, peak), (_, samples) in zip(env, self.packets):
            if armed and peak > hi:
                idx = int(np.argmax(np.abs(samples) > hi)) if samples.size else 0
                out.append(arrival - (samples.size - idx) / FS)
                armed = False
            elif not armed and peak < lo:
                armed = True
        return out


def build_signal():
    """Lead-in silence, then N bursts separated by gaps. Returns (signal, onsets)."""
    lead = np.zeros(int(FS * LEAD_MS / 1000), dtype=np.float32)
    burst_n = int(FS * BURST_MS / 1000)
    gap_n = int(FS * GAP_MS / 1000)
    t = np.arange(burst_n) / FS
    burst = (BURST_AMP * np.sin(2 * np.pi * BURST_HZ * t)).astype(np.float32)
    gap = np.zeros(gap_n, dtype=np.float32)
    parts, onsets, cursor = [lead], [], lead.size
    for _ in range(N_BURSTS):
        onsets.append(cursor)
        parts.append(burst)
        cursor += burst_n
        parts.append(gap)
        cursor += gap_n
    return np.concatenate(parts), onsets


def main():
    r = Flex(RADIO)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    uport = udp.getsockname()[1]
    print(f"connected handle={r.handle} udp={uport} underlying={UNDERLYING}")

    saved_mode = sid = rax_id = None
    counts = {}
    try:
        must(r, "client gui")
        must(r, f"client station {STATION}")
        must(r, f"client udpport {uport}")
        for s in ("slice", "pan", "waveform", "radio", "client", "audio_stream"):
            r.cmd(f"sub {s} all")
        r.pump(2.5)

        ids = r.my_slice_ids()
        if not ids:
            raise RuntimeError(
                "this connection owns no slice -- refusing to drive another "
                "client's slice (see Flex.my_slice_ids)")
        if len(r.slice_ids()) > len(ids):
            print(f"  note: {len(r.slice_ids())} slices on radio, "
                  f"{len(ids)} ours ({ids}) -- other clients present")
        sid = ids[0]
        saved_mode = r.fields(f"slice {sid} ").get("mode")
        print(f"slice {sid}: mode={saved_mode}")

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
        must(r, f"waveform set {WF_NAME} rx_filter low_cut=0")
        must(r, f"waveform set {WF_NAME} rx_filter high_cut=8000")
        must(r, f"waveform set {WF_NAME} rx_filter depth=256")
        must(r, f"waveform set {WF_NAME} udpport={uport}")
        must(r, f"slice set {sid} mode={WF_MODE}")
        r.pump(1.5)

        code, pay = r.cmd("stream create type=remote_audio_rx compression=none")
        if not (ok(code) and pay.strip()):
            raise RuntimeError(f"no remote_audio_rx ({code})")
        rax_id = int(pay.split()[0], 16)
        print(f"remote_audio_rx = 0x{rax_id:08X}  (compression=none, LAN)")

        signal, onset_idx = build_signal()
        onset_pkt = {i // SPP: k for k, i in enumerate(onset_idx)}
        sent_times = [None] * len(onset_idx)

        print(f"\nstreaming {signal.size/FS:.1f}s with {N_BURSTS} bursts "
              f"({BURST_MS} ms @ {BURST_HZ:.0f} Hz)...")
        with TimedCapture(udp, rax_id) as cap:
            npk = signal.size // SPP
            t0 = time.perf_counter()
            dt = SPP / FS
            for i in range(npk):
                chunk = signal[i * SPP:(i + 1) * SPP]
                n = chunk.size
                c = counts.get(rx_in, 0)
                counts[rx_in] = (c + 1) & 0xF
                hdr_word = (0x10000000 | 0x08000000 | 0x00400000 | 0x00200000
                            | (c << 16) | (7 + n * 2))
                now = time.time()
                sec = int(now)
                fps = int((now - sec) * 1e12)
                hdr = struct.pack(">IIIIIII", hdr_word, rx_in, OUI, AUDIO_CLASS,
                                  sec, (fps >> 32) & 0xFFFFFFFF, fps & 0xFFFFFFFF)
                inter = np.empty(n * 2, dtype=">f4")
                inter[0::2] = chunk
                inter[1::2] = chunk
                send_at = time.perf_counter()
                udp.sendto(hdr + inter.tobytes(), (RADIO, VITA_PORT))
                if i in onset_pkt:
                    sent_times[onset_pkt[i]] = send_at
                target = t0 + (i + 1) * dt
                while True:
                    rem = target - time.perf_counter()
                    if rem <= 0:
                        break
                    if rem > 0.002:
                        time.sleep(rem - 0.001)
            time.sleep(0.6)

        env = cap.envelope()
        if env:
            peaks = np.array([p for _, p in env])
            print(f"\nmonitor envelope: min={peaks.min():.3e} "
                  f"med={np.median(peaks):.3e} p99={np.percentile(peaks,99):.3e} "
                  f"max={peaks.max():.3e}")
            # Coarse ASCII trace so a wrong assumption about the background is
            # visible rather than inferred from a failed detection.
            nb = 72
            step = max(1, len(peaks) // nb)
            bins = [peaks[i:i + step].max() for i in range(0, len(peaks), step)][:nb]
            top = max(bins) or 1.0
            glyphs = " .:-=+*#%@"
            print("  " + "".join(glyphs[min(9, int(9 * b / top))] for b in bins))

        got = cap.onsets(verbose=True)
        print(f"\ncaptured {len(cap.packets)} monitor pkts, "
              f"{len(got)} onsets detected (sent {N_BURSTS})")
        if not got:
            print("  NO onsets -- nothing came back; cannot measure")
            return 1

        lat = []
        for k, recv in enumerate(got):
            if k < len(sent_times) and sent_times[k] is not None:
                lat.append((recv - sent_times[k]) * 1000.0)
        if not lat:
            print("  could not pair sends with receptions")
            return 1

        arr = np.array(lat)
        print("\n  burst  round-trip ms")
        for k, v in enumerate(arr):
            print(f"   {k:3d}    {v:8.1f}")
        print(f"\n  min={arr.min():.1f} ms  median={np.median(arr):.1f} ms  "
              f"max={arr.max():.1f} ms  spread={arr.max()-arr.min():.1f} ms")
        print(f"  packet quantisation = {1000*SPP/FS:.2f} ms")
        print("\n  This is B's penalty over A (local render). Judge it against")
        print("  RADE's own latency and against conversational comfort.")

    finally:
        print("\n-- teardown --")
        try:
            if rax_id:
                r.cmd("stream remove 0x%08X" % rax_id)
            if sid is not None and saved_mode:
                print(f"  restore mode={saved_mode} -> "
                      f"{r.cmd(f'slice set {sid} mode={saved_mode}')[0]}")
            print(f"  waveform remove -> {r.cmd(f'waveform remove {WF_NAME}')[0]}")
        except Exception as e:  # noqa: BLE001
            print(f"  teardown error: {e}")
        udp.close()
        r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
