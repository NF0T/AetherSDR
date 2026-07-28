#!/usr/bin/env python3
"""Is the waveform's decoded-audio RETURN path actually band-limited?

This is the re-measurement RFC §10.6 called for. Phase 0 1b tried to answer it by
injecting on `rx_stream_out_id` and concluded a hard ~2.8 kHz cap -- but §10.6 T1
showed that id is NEVER honoured, so 1b measured something else (probably the
stock SSB filter, `filter=100/2800`). Redo it on the id the radio does honour.

The answer decides a live architectural question (§9.1 / §16 Q1):
  A. local render only          -- full fidelity, other clients hear nothing
  B. return-path only           -- other clients hear it, ONE copy, no double-back,
                                   quality = whatever this probe measures
  C. both                       -- rejected: doubles against our own render

1b's bogus cap was the main thing ruling out B. If the return path turns out to be
wideband, B is live again.

Method: register with a given rx_filter, put the slice in the waveform mode, inject
a 100 Hz -> 11 kHz log sweep on rx_stream_in_id, capture `remote_audio_rx`
CONCURRENTLY, and report the returned spectrum. Repeat across rx_filter settings to
separate three hypotheses:

  * response tracks the registered rx_filter  -> the return is shaped by OUR filter
    (bad for B: a filter chosen for the modem would mangle speech)
  * response fixed regardless of rx_filter    -> a real fixed cap; measure it
  * wideband in every case                    -> no meaningful loss; B is viable

RX ONLY. Never keys the transmitter.
"""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flex_waveform_tier1_probe import (  # noqa: E402
    BackgroundCapture, Emitter, Flex, must, ok,
)

RADIO = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
STATION = WF_NAME = "AetherRadeProbe"
WF_MODE = "RADP"
UNDERLYING = os.environ.get("PROBE_UNDERLYING", "DIGU")
FS = 24000
SWEEP_S = float(os.environ.get("PROBE_SWEEP_S", "5.0"))

# (low_cut, high_cut) registrations to compare.
CONFIGS = [(800, 2200),      # §10.1's modem filter -- the interesting case
           (100, 2800),      # stock SSB width, for comparison with 1b's number
           (0, 8000)]        # as wide as we can ask for


def log_sweep(f0, f1, secs, fs):
    n = int(secs * fs)
    t = np.arange(n) / fs
    k = (f1 / f0) ** (1.0 / secs)
    phase = 2 * np.pi * f0 * ((k ** t - 1) / np.log(k))
    return (0.4 * np.sin(phase)).astype(np.float32)


def response(sig, nfft=4096):
    """Welch-ish PSD of the captured monitor audio."""
    if sig.size < nfft * 2:
        return None, None
    win = np.hanning(nfft)
    step = nfft // 2
    nblk = 1 + (sig.size - nfft) // step
    acc = np.zeros(nfft // 2 + 1)
    for b in range(nblk):
        seg = sig[b * step: b * step + nfft] * win
        acc += np.abs(np.fft.rfft(seg)) ** 2
    acc /= nblk
    return np.fft.rfftfreq(nfft, 1.0 / FS), 10 * np.log10(acc + 1e-30)


def main():
    r = Flex(RADIO)
    import socket
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    uport = udp.getsockname()[1]
    emit = Emitter(udp, RADIO)
    print(f"connected handle={r.handle} udp={uport} underlying={UNDERLYING}")

    saved_mode = sid = rax_id = None
    results = {}
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
        f0 = r.fields(f"slice {sid} ")
        saved_mode = f0.get("mode")
        print(f"slice {sid}: mode={saved_mode} "
              f"filter={f0.get('filter_lo')}/{f0.get('filter_hi')}")

        code, pay = r.cmd("stream create type=remote_audio_rx compression=none")
        if not (ok(code) and pay.strip()):
            raise RuntimeError(f"no remote_audio_rx ({code})")
        rax_id = int(pay.split()[0], 16)
        print(f"remote_audio_rx = 0x{rax_id:08X}")

        # Positive control in the slice's normal mode (see §10.6).
        with BackgroundCapture(udp, rax_id) as cap:
            time.sleep(2.0)
        ctl = cap.left()
        ctl_rms = float(np.sqrt(np.mean(ctl ** 2))) if ctl.size else 0.0
        print(f"control ({saved_mode}): RMS={ctl_rms:.3e} -> "
              + ("monitor live" if ctl_rms > 1e-6 else "MONITOR DEAD, results void"))

        sweep = log_sweep(100.0, 11000.0, SWEEP_S, FS)

        for lo, hi in CONFIGS:
            print(f"\n=== rx_filter [{lo}, {hi}] ===")
            r.cmd(f"slice set {sid} mode={saved_mode}")
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
            # §10.3: the filter is latched at registration -- set BEFORE mode entry.
            must(r, f"waveform set {WF_NAME} rx_filter low_cut={lo}")
            must(r, f"waveform set {WF_NAME} rx_filter high_cut={hi}")
            must(r, f"waveform set {WF_NAME} rx_filter depth=256")
            must(r, f"waveform set {WF_NAME} udpport={uport}")
            must(r, f"slice set {sid} mode={WF_MODE}")
            r.pump(1.5)
            fa = r.fields(f"slice {sid} ")
            print(f"  slice filter now {fa.get('filter_lo')}/{fa.get('filter_hi')}")

            with BackgroundCapture(udp, rax_id) as cap:
                time.sleep(0.5)
                emit.send_tone_samples(rx_in, sweep)
                time.sleep(0.5)
            sig = cap.left()
            rms = float(np.sqrt(np.mean(sig ** 2))) if sig.size else 0.0
            print(f"  returned: {cap.pkts} pkts RMS={rms:.3e}")
            fr, db = response(sig)
            if fr is None or rms < 1e-9:
                print("  NO usable return -- skipped")
                continue
            band = (fr >= 300) & (fr <= 2000)
            ref = 10 * np.log10(np.mean(10 ** (db[band] / 10)))
            results[(lo, hi)] = (fr, db, ref)

        if results:
            print("\n=== returned spectrum, dB relative to each trace's 300-2000 Hz level ===")
            cols = list(results.keys())
            print("     Hz " + "".join(f"{str(c):>14}" for c in cols))
            for target in list(range(500, 4001, 250)) + [5000, 6000, 8000]:
                row = f"  {target:5d} "
                for c in cols:
                    fr, db, ref = results[c]
                    i = int(np.argmin(np.abs(fr - target)))
                    row += f"{db[i] - ref:>14.1f}"
                print(row)
            print("\n  -6 dB edges:")
            for c in cols:
                fr, db, ref = results[c]
                edge = None
                for f, d in zip(fr, db):
                    if f > 1200 and d < ref - 6.0:
                        edge = f
                        break
                print(f"    rx_filter {c}: "
                      + (f"{edge:.0f} Hz" if edge else "none below 12 kHz"))

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
