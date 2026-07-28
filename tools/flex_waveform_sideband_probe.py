#!/usr/bin/env python3
"""Sideband sense — does our TX audio land ABOVE or BELOW the dial?

The last question blocking on measurement. RFC §10.5 infers from the shipped
D-Star provider that the outbound waveform streams are stereo AUDIO (not I/Q),
and therefore that there is no TX conjugate convention to get wrong. That is a
code reading, not a measurement, and it is a one-bit choice in our own provider
that decides whether any other station can decode us.

NO OFF-AIR RECEIVER NEEDED. The Flex will draw its own transmitted signal on the
panadapter (`transmit set show_tx_in_waterfall=1`), and the panadapter is in the
RF frequency domain — exactly where the question lives. A 1500 Hz tone on a slice
at dial 50.340 lands at 50.3415 MHz if the sideband is right, 50.3385 if it is
inverted. We read the FFT bins numerically rather than looking at pixels.

THE CONTROL, which is what makes this rigorous rather than assumed:
  This 8400 has num_scu=1, so RX is blanked while transmitting — whatever the pan
  shows during TX is radio-generated, not a received measurement. It has to earn
  trust before it can answer anything. So the same tone is sent through the same
  path under `underlying_mode=DIGU` and then `DIGL`:

    * mirrored results  -> the display resolves sideband correctly, AND we read
                           off directly which sideband our audio convention makes
    * same side both    -> the display is not sideband-aware; this method cannot
                           answer the question and we fall back to a real receiver

  A second control varies the tone (1000 vs 2000 Hz) and checks the peak moves by
  the right amount in the right direction — proving the axis is a real frequency
  mapping and not a fixed graphic.

Keyed time is roughly 10 s total, in bursts under 5 s. Station ID in CW at the end.
"""

import argparse
import os
import socket
import struct
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flex_waveform_tier1_probe import Flex, must, ok, try_cmd  # noqa: E402
from flex_waveform_tier2_probe import (  # noqa: E402
    AUDIO_CLASS, FS, OUI, SPP, VITA_PORT, ContinuousSender, TxSession,
    fresh_slice, global_transmit, pan_fields, parse_vita, read_rfpower,
)

RADIO = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
STATION = WF_NAME = "AetherRadeProbe"
WF_MODE = "RADP"
TEST_FREQ_MHZ = float(os.environ.get("PROBE_TX_FREQ", "50.340"))
TEST_BAND = os.environ.get("PROBE_TX_BAND", "6")
MAX_WATTS = float(os.environ.get("PROBE_MAX_WATTS", "1.0"))
PAN_BW_MHZ = float(os.environ.get("PROBE_PAN_BW", "0.020"))   # 20 kHz span
PAN_XPIX = 1024
FFT_PCC = 0x8003
TX_AMP = 0.5

# (underlying_mode, tone Hz, what a CORRECT upper-sideband convention predicts)
PASSES = [("DIGU", 1000.0, +1000.0),
          ("DIGU", 2000.0, +2000.0),
          ("DIGL", 1000.0, -1000.0)]


class FftReader(threading.Thread):
    """Assemble panadapter FFT frames (PCC 0x8003) per stream id.

    Bin values are PIXEL values: 0 = top of the dBm range. So the strongest bin
    is the one with the LOWEST raw value — the reverse of the obvious reading.
    """

    def __init__(self, udp):
        super().__init__(daemon=True)
        self.udp = udp
        self.frames = {}        # stream -> {idx, total, buf}
        self.complete = {}      # stream -> list of completed np arrays
        self._stop = threading.Event()
        self._lock = threading.Lock()

    def run(self):
        self.udp.settimeout(0.2)
        while not self._stop.is_set():
            try:
                data, _ = self.udp.recvfrom(16384)
            except (socket.timeout, TimeoutError):
                continue
            except OSError:
                break
            if len(data) < 40:
                continue
            pcc, _ = parse_vita(data)
            if pcc != FFT_PCC:
                continue
            sid = struct.unpack(">I", data[4:8])[0]
            sub = data[28:40]
            start, nbins, binsz, total = struct.unpack(">HHHH", sub[:8])
            fidx = struct.unpack(">I", sub[8:12])[0]
            if not nbins or not total or binsz != 2:
                continue
            body = data[40:]
            avail = min(nbins, len(body) // 2)
            if avail <= 0:
                continue
            vals = np.frombuffer(body[:avail * 2], dtype=">u2").astype(np.int32)
            with self._lock:
                f = self.frames.get(sid)
                if f is None or f["idx"] != fidx:
                    f = {"idx": fidx, "total": total,
                         "buf": np.full(total, -1, dtype=np.int32), "got": 0}
                    self.frames[sid] = f
                end = min(start + avail, total)
                if end > start:
                    f["buf"][start:end] = vals[:end - start]
                    f["got"] += end - start
                if f["got"] >= total and (f["buf"] >= 0).all():
                    self.complete.setdefault(sid, []).append(f["buf"].copy())
                    self.frames[sid] = {"idx": -1, "total": total,
                                        "buf": np.full(total, -1, dtype=np.int32),
                                        "got": 0}

    def stop(self):
        self._stop.set()

    def reset(self):
        with self._lock:
            self.complete.clear()
            self.frames.clear()

    def best(self):
        """Per stream: the frame with the single strongest (lowest-raw) bin."""
        out = {}
        with self._lock:
            for sid, frames in self.complete.items():
                if not frames:
                    continue
                pick = min(frames, key=lambda a: int(a.min()))
                out[sid] = (pick, len(frames))
        return out


def fresh_pan(r, pan):
    """Re-read pan status from a CLEARED buffer.

    Same trap as fresh_slice: pan_fields() merges everything accumulated, and
    Flex sends PARTIAL status updates, so a later line carrying only
    `bandwidth=` leaves an older `center=` in place. Reading without clearing
    reported centre 50.125 while the slice had demonstrably tuned to 50.340 —
    which the pan could not have spanned if that centre were real.
    """
    r.status.clear()
    r.cmd("sub pan all")
    r.pump(1.5)
    return pan_fields(r, pan)


def peak_offset_hz(bins, center_mhz, bw_mhz):
    """-> (offset_hz_from_centre, contrast_dB_ish, bin_index)."""
    n = bins.size
    if n < 8:
        return None, None, None
    strongest = int(np.argmin(bins))         # lowest pixel value = strongest
    peak_raw = int(bins[strongest])
    floor_raw = int(np.median(bins))
    span_hz = bw_mhz * 1e6
    # bin 0 = centre - span/2, bin n-1 = centre + span/2
    off = (strongest / (n - 1.0) - 0.5) * span_hz
    return off, floor_raw - peak_raw, strongest


def main():
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="store_true", help="actually transmit")
    ap.add_argument("--stay", action="store_true",
                    help="leave the slice on the test frequency (keeps ATU state)")
    args = ap.parse_args()

    r = Flex(RADIO)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    uport = udp.getsockname()[1]
    fft = FftReader(udp)
    fft.start()

    saved = {}
    sid = pan = sess = sender = None
    results = []
    try:
        must(r, "client gui")
        must(r, f"client station {STATION}")
        must(r, f"client udpport {uport}")
        for s in ("slice", "pan", "waveform", "radio", "client", "tx", "cwx"):
            r.cmd(f"sub {s} all")
        r.pump(3.0)

        mine = r.my_slice_ids()
        if not mine:
            raise RuntimeError("this connection owns no slice")
        sid = mine[0]
        f0 = r.fields(f"slice {sid} ")
        saved["mode"] = f0.get("mode")
        saved["freq"] = f0.get("RF_frequency")
        saved["tx"] = f0.get("tx")
        pan = f0.get("pan")
        pf = pan_fields(r, pan)
        saved["pan_band"] = pf.get("band")
        saved["pan_center"] = pf.get("center")
        saved["pan_bw"] = pf.get("bandwidth")
        print(f"slice {sid} on pan {pan}: mode={saved['mode']} "
              f"RF={saved['freq']}  pan center={saved['pan_center']} "
              f"bw={saved['pan_bw']}")

        tx0 = global_transmit(r)
        saved["show_tx"] = tx0.get("show_tx_in_waterfall")
        print(f"show_tx_in_waterfall was {saved['show_tx']}")
        must(r, "transmit set show_tx_in_waterfall=1")

        # Pan onto the test frequency, narrow span for bin resolution.
        must(r, f"display panafall set {pan} band={TEST_BAND}")
        r.pump(1.8)
        must(r, f"display panafall set {pan} center={TEST_FREQ_MHZ:.3f}")
        r.pump(1.5)
        try_cmd(r, f"display pan set {pan} xpixels={PAN_XPIX} ypixels=256")
        try_cmd(r, f"display panafall set {pan} bandwidth={PAN_BW_MHZ:.4f}")
        r.pump(2.0)
        pf = fresh_pan(r, pan)
        cen = float(pf.get("center", TEST_FREQ_MHZ))
        bw = float(pf.get("bandwidth", PAN_BW_MHZ))
        print(f"pan now: center={cen} bandwidth={bw} "
              f"({bw*1e6/PAN_XPIX:.1f} Hz/bin at {PAN_XPIX} bins)")
        if abs(cen - TEST_FREQ_MHZ) > bw / 2.0:
            raise RuntimeError(
                f"pan centre {cen} does not span {TEST_FREQ_MHZ} at bw {bw} — "
                f"the frequency axis would be wrong and every offset with it")

        for attempt in range(3):
            r.cmd(f"slice tune {sid} {TEST_FREQ_MHZ:.6f}")
            r.pump(1.0)
            now_f = fresh_slice(r, sid).get("RF_frequency")
            if now_f and abs(float(now_f) - TEST_FREQ_MHZ) <= 0.0005:
                break
        if not now_f or abs(float(now_f) - TEST_FREQ_MHZ) > 0.0005:
            raise RuntimeError(f"slice on {now_f}, not {TEST_FREQ_MHZ}")
        print(f"slice tuned {now_f}")

        rfpower = read_rfpower(r)
        print(f"rfpower={rfpower} W")
        if rfpower is None or float(rfpower) > MAX_WATTS:
            raise RuntimeError(f"rfpower {rfpower} W exceeds {MAX_WATTS} W limit")

        fft.reset()
        time.sleep(2.0)
        seen = fft.best()
        print(f"\npan FFT streams delivering: "
              + (", ".join(f"0x{k:08X}({v[1]} frames, {v[0].size} bins)"
                           for k, v in seen.items()) or "NONE"))
        if not seen:
            raise RuntimeError("no panadapter FFT data — cannot read the display")

        sess = TxSession(r, None, args.arm, sid)
        sender = None

        def register(under):
            r.cmd(f"slice set {sid} mode={saved['mode']}")
            r.cmd(f"waveform remove {WF_NAME}")
            payload = must(r, f"waveform create name={WF_NAME} mode={WF_MODE} "
                              f"underlying_mode={under} version=0.0.1")
            st = {}
            for tok in payload.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    try:
                        st[k] = int(v, 16)
                    except ValueError:
                        pass
            must(r, f"waveform set {WF_NAME} tx=1")
            must(r, f"waveform set {WF_NAME} udpport={uport}")
            must(r, f"slice set {sid} mode={WF_MODE}")
            r.cmd(f"slice set {sid} tx=1")
            r.pump(1.2)
            return st["tx_stream_in_id"]

        if not args.arm:
            print("\nPRE-FLIGHT OK — pan data flowing, gates passed. NOTHING KEYED.")
            print("Re-run with --arm to transmit.")
            return 0

        # Re-check power immediately before any RF.
        #
        # An earlier run read 5 W here and refused. I guessed that was a stale
        # read of the previous band's memory caught mid-transition. It was not:
        # the operator had genuinely left the radio at 5 W and lowered it to 1 W
        # between runs. So the gate reported the TRUTH both times, and the
        # "stale read" theory was an invented explanation for correct behaviour.
        # Worth keeping the re-check anyway — power is a live setting a human can
        # change between setup and use, which is exactly what happened.
        final_pwr = read_rfpower(r)
        if final_pwr is None or float(final_pwr) > MAX_WATTS:
            raise RuntimeError(f"power re-check at arm time: {final_pwr} W")
        print(f"\nARMED — {TEST_FREQ_MHZ} MHz at {final_pwr} W, ~10 s keyed total")

        for under, tone, predicted in PASSES:
            print(f"\n=== underlying_mode={under}, tone {tone:.0f} Hz "
                  f"(upper-sideband convention predicts {predicted:+.0f} Hz) ===")
            stream = register(under)
            if sender is None:
                sender = ContinuousSender(udp, RADIO, stream)
                sender.start()
            sender.stream_id = stream
            sender.set(freq=tone, amp=TX_AMP)
            fft.reset()
            with sess.keyed(f"sideband {under} {tone:.0f}Hz", max_s=4.5) as live:
                if not live:
                    break
                sender.enabled = True
                time.sleep(2.2)
                sender.enabled = False
            got = fft.best()
            for stream_id, (bins, nframes) in got.items():
                off, contrast, idx = peak_offset_hz(bins, cen, bw)
                if off is None:
                    continue
                print(f"  pan 0x{stream_id:08X}: peak bin {idx}/{bins.size} "
                      f"-> {off:+.0f} Hz from centre, contrast {contrast} px "
                      f"({nframes} frames)")
                results.append((under, tone, predicted, stream_id, off, contrast))

        print("\n" + "=" * 68)
        print("VERDICT")
        print("=" * 68)
        best_stream = None
        by_stream = {}
        for under, tone, pred, st, off, contrast in results:
            by_stream.setdefault(st, []).append((under, tone, pred, off, contrast))
        for st, rows in by_stream.items():
            strong = [x for x in rows if x[4] and x[4] > 20]
            if len(strong) < len(PASSES):
                print(f"  pan 0x{st:08X}: only {len(strong)}/{len(PASSES)} passes "
                      f"produced a clear peak — inconclusive on this stream")
                continue
            best_stream = st
            digu = [x for x in rows if x[0] == "DIGU"]
            digl = [x for x in rows if x[0] == "DIGL"]
            print(f"\n  pan 0x{st:08X}:")
            for under, tone, pred, off, c in rows:
                print(f"    {under} {tone:>6.0f} Hz : predicted {pred:+6.0f}, "
                      f"measured {off:+7.0f} Hz")
            # control 1: does the axis track tone frequency?
            if len(digu) == 2:
                d = abs(abs(digu[1][3]) - abs(digu[0][3]))
                want = abs(digu[1][1] - digu[0][1])
                print(f"\n    CONTROL 1 (axis is a real frequency mapping): tone "
                      f"moved {want:.0f} Hz, peak moved {d:.0f} Hz -> "
                      + ("PASS" if abs(d - want) < want * 0.25 else "FAIL"))
            # control 2: does the display resolve sideband at all?
            if digu and digl:
                mirrored = (digu[0][3] > 0) != (digl[0][3] > 0)
                print(f"    CONTROL 2 (display is sideband-aware): DIGU "
                      f"{digu[0][3]:+.0f} Hz vs DIGL {digl[0][3]:+.0f} Hz -> "
                      + ("PASS — opposite sides" if mirrored else
                         "FAIL — same side; this method cannot answer the "
                         "question, use a real receiver"))
                if mirrored:
                    if digu[0][3] > 0:
                        print("\n    ==> DIGU puts our audio ABOVE the dial.")
                        print("        Upper sideband is CORRECT. §10.5's inference")
                        print("        holds: no conjugation needed on transmit.")
                    else:
                        print("\n    ==> DIGU puts our audio BELOW the dial.")
                        print("        THE TRANSMITTED SIDEBAND IS INVERTED. The")
                        print("        provider must conjugate before emitting.")
        if best_stream is None:
            print("  No stream gave a clean result on every pass.")

    finally:
        print("\n-- teardown --")
        try:
            if sender:
                sender.enabled = False
                sender.stop()
            r.cmd("xmit 0")
            if sess and sess.armed and sess.first_tx:
                sess.send_cw_id()
            if saved.get("show_tx") is not None:
                r.cmd(f"transmit set show_tx_in_waterfall={saved['show_tx']}")
                print(f"  show_tx_in_waterfall -> {saved['show_tx']}")
            if sid is not None and not args.stay:
                for cmd in (f"slice set {sid} mode={saved.get('mode')}",
                            f"display panafall set {pan} band={saved.get('pan_band')}",
                            f"display panafall set {pan} center={saved.get('pan_center')}",
                            f"slice tune {sid} {saved.get('freq')}"):
                    if "None" not in cmd:
                        r.cmd(cmd)
                        r.pump(0.8)
                print("  slice/pan restored")
            elif sid is not None:
                print(f"  --stay: leaving slice on {TEST_FREQ_MHZ} MHz")
            if saved.get("pan_bw"):
                r.cmd(f"display panafall set {pan} bandwidth={saved['pan_bw']}")
            if sid is not None and saved.get("tx") is not None:
                r.cmd(f"slice set {sid} tx={saved['tx']}")
            print(f"  waveform remove -> {r.cmd(f'waveform remove {WF_NAME}')[0]}")
        except Exception as e:  # noqa: BLE001
            print(f"  TEARDOWN ERROR: {e}")
            try:
                r.cmd("xmit 0")
            except Exception:  # noqa: BLE001
                pass
        fft.stop()
        udp.close()
        r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
