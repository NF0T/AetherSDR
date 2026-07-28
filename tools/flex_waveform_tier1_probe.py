#!/usr/bin/env python3
"""RADE V2 RFC §16 Q4 "Tier 1" — the TX questions that need NO transmitter.

§10.5 derived the TX contract from the vendored D-Star provider without touching
a radio. Three items it could not settle need the radio but not RF:

  T1. Which stream id does the radio HONOUR on emit? The shipped D-Star provider
      replies on the INBOUND id (hal_listener.c:604 stamps the received id in and
      nothing remaps it; the *_out_id fields are parsed and never read), while our
      Phase 0 1b probe emitted on rx_stream_out_id. Both reportedly work. Inject a
      distinct tone on each candidate and see which reaches the monitor.
  T2. Does `tx_filter` clamp like `rx_filter` for a USB-family underlying mode?
      D-Star registers a ONE-SIDED tx_filter [0,4800] against a SYMMETRIC
      rx_filter ±3500, so tx_filter does not obviously follow the rx_filter family
      rules (§10 "filter-cut units"). Probe negative/zero/positive low_cut.
  T3. Does `tx_stream_in` flow BEFORE PTT, or only while transmitting? Decides
      whether the provider sources mic audio from the radio or from AetherSDR's
      own AudioEngine (§9 currently assumes the latter without having looked).

SAFETY — THIS PROBE NEVER KEYS THE TRANSMITTER.
  Not sent, at any point: `xmit`, `tune`, `transmit set ptt`, any keying verb.
  It DOES send `waveform set <X> tx=1` (declares the waveform TX-capable) and
  `slice set <id> tx=1` (designates which slice would transmit). Neither keys the
  radio; both are recorded and restored in the teardown block. T3's answer for the
  keyed case is deliberately out of scope -- that is Tier 2, on a dummy load, with
  the operator present.

RESULTS, 2026-07-28, FLEX-8400 fw 4.2.20.41343 (written up as RFC §10.6):
  T1. The radio honours the INBOUND id only. rx_stream_in_id -> monitor RMS
      4.2e-02; rx_stream_out_id -> exactly 0.000e+00, twice, including a retry
      after the path was proven live. Matches the shipped D-Star provider, and
      CONTRADICTS the Phase 0 1b claim -- which puts 1b's ~2.8 kHz round-trip
      cap in doubt, since 1b injected on the id that is never honoured.
  T2. All tx_filter values accepted (low_cut -3500/-1/0/300, high_cut
      2400/4800) -- but no read-back exists, so honouring is UNKNOWN. Half
      answered; needs Tier 2.
  T3. tx_stream_in does NOT flow before PTT (0 pkts/6 s, while rx_stream_in
      delivered 1125 over the same window). The pump is started AND stopped by
      the radio, so the EOO tail needs synthesized buffers.

METHOD NOTE -- three ways this probe produced false results before it produced
true ones, all of the same shape (a real measurement of the wrong thing):
  1. remote_audio_rx is a CLIENT stream; without `client udpport` nothing
     arrives and every candidate reads as "inconclusive".
  2. The monitor is real-time; capturing AFTER the injection finishes returns
     only the following silence. Capture concurrently.
  3. Analysing the FIRST window of a capture that is bracketed by silence sees
     only the lead-in. Analyse the middle.
Hence the positive control: if the monitor does not carry ordinary slice audio
in the current setup, T1 is not interpretable and the probe says so instead of
reporting a null as a finding.

Uses the slice the radio auto-restores for a GUI client rather than creating one
(§10.3). Restores slice mode + TX-slice designation in a finally block.
"""

import os
import socket
import struct
import sys
import threading
import time

import numpy as np

RADIO = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
STATION = "AetherRadeProbe"
WF_NAME = "AetherRadeProbe"
WF_MODE = "RADP"
UNDERLYING = os.environ.get("PROBE_UNDERLYING", "DIGU")
FS = 24000
SPP = 128                      # samples/packet, = HAL_TX_BUFFER_SIZE (§10.5)
VITA_PORT = 4991               # vita_output.c:121
OUI = 0x001C2D                 # FLEXRADIO_OUI
AUDIO_CLASS = 0x534C03E3       # SL_VITA_SLICE_AUDIO_CLASS
RX_LOW, RX_HIGH = 800, 2200    # §10.1

# T1 tone per candidate: distinct so a stray/both-work outcome is still readable.
# Ordered PASSES, not a dict: rx_stream_out is retried LAST, after rx_stream_in
# has proven the path is live, so a null on it cannot be blamed on warm-up or on
# being the first injection after mode entry.
T1_PASSES = [("rx_stream_out_id", 1200.0),
             ("rx_stream_in_id", 1700.0),
             ("rx_stream_out_id", 900.0)]
T1_SECS = float(os.environ.get("PROBE_T1_SECS", "4.0"))
T3_SECS = float(os.environ.get("PROBE_T3_SECS", "6.0"))


class Flex:
    def __init__(self, host, port=4992):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = ""
        self.seq = 0
        self.handle = None
        self.version = None
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
        elif line.startswith("V"):
            self.version = line[1:]
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

    def fields(self, prefix):
        """Merge every `S...|<prefix> k=v ...` status seen so far."""
        merged = {}
        for line in self.status:
            body = line.split("|", 1)[-1]
            if body.startswith(prefix):
                for tok in body[len(prefix):].split():
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


def try_cmd(r, text):
    """Send a command whose REJECTION is itself a result (T2)."""
    code, payload = r.cmd(text)
    print(f"  [{'ok ' if ok(code) else f'0x{int(code,16):08X}' }] {text}"
          + (f"  -> {payload}" if payload else ""))
    return ok(code), code, payload


class Emitter:
    """Outbound waveform audio, framed per §10.5.

    Per-STREAM 4-bit rolling packet count (vita_output.c:79-103) -- a global
    counter would desync the moment two streams are used.
    """

    def __init__(self, udp, host):
        self.udp = udp
        self.host = host
        self.counts = {}

    def _next_count(self, stream_id):
        c = self.counts.get(stream_id, 0)
        self.counts[stream_id] = (c + 1) & 0xF
        return c

    def packet(self, stream_id, mono):
        n = len(mono)
        hdr_word = (0x10000000            # IF_DATA_WITH_STREAM_ID
                    | 0x08000000          # CLASS_ID_PRESENT
                    | 0x00400000          # TSI_UTC
                    | 0x00200000          # TSF_REAL_TIME
                    | (self._next_count(stream_id) << 16)
                    | (7 + n * 2))
        now = time.time()
        sec = int(now)
        frac_ps = int((now - sec) * 1e12)
        hdr = struct.pack(">IIIIIII", hdr_word, stream_id, OUI, AUDIO_CLASS,
                          sec, (frac_ps >> 32) & 0xFFFFFFFF, frac_ps & 0xFFFFFFFF)
        # §10.5 item 3: outbound is STEREO AUDIO -- duplicate mono into both
        # slots, exactly as the shipped D-Star provider does.
        inter = np.empty(n * 2, dtype=">f4")
        inter[0::2] = mono
        inter[1::2] = mono
        return hdr + inter.tobytes()

    def send_tone(self, stream_id, freq, secs, amp=0.3):
        npk = int(secs * FS / SPP)
        t0 = time.perf_counter()
        dt = SPP / FS
        phase = 0.0
        dphi = 2 * np.pi * freq / FS
        for i in range(npk):
            idx = np.arange(SPP)
            mono = (amp * np.sin(phase + dphi * idx)).astype(np.float32)
            phase = (phase + dphi * SPP) % (2 * np.pi)
            self.udp.sendto(self.packet(stream_id, mono), (self.host, VITA_PORT))
            target = t0 + (i + 1) * dt
            while True:
                rem = target - time.perf_counter()
                if rem <= 0:
                    break
                if rem > 0.002:
                    time.sleep(rem - 0.001)
        return npk


def collect(udp, stream_id, secs, settle=0.8):
    """Gather payload floats for one stream id."""
    chunks, pkts, first = [], 0, None
    udp.settimeout(0.5)
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
            continue
        pkts += 1
        pay = data[28:]
        n = (len(pay) // 8) * 2
        chunks.append(np.frombuffer(pay[: n * 4], dtype=">f4").astype(np.float64))
    if not chunks:
        return np.array([]), 0
    return np.concatenate(chunks), pkts


def count_stream(udp, stream_id, secs):
    """T3: just count packets on a stream id. No payload interpretation."""
    pkts, seen_other = 0, {}
    udp.settimeout(0.5)
    end = time.time() + secs
    while time.time() < end:
        try:
            data, _ = udp.recvfrom(8192)
        except (socket.timeout, TimeoutError):
            continue
        if len(data) < 28:
            continue
        sid = struct.unpack(">I", data[4:8])[0]
        if sid == stream_id:
            pkts += 1
        else:
            seen_other[sid] = seen_other.get(sid, 0) + 1
    return pkts, seen_other


class BackgroundCapture:
    """Collect one stream id on a thread WHILE audio is being injected.

    The monitor is a real-time stream: capturing after the injection finishes
    returns only the silence that followed it. The first version of this probe
    did exactly that and reported a false "not honoured" for both candidates.
    """

    def __init__(self, udp, stream_id):
        self.udp = udp
        self.stream_id = stream_id
        self.chunks = []
        self.pkts = 0
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
            if len(data) < 28 or struct.unpack(">I", data[4:8])[0] != self.stream_id:
                continue
            self.pkts += 1
            pay = data[28:]
            n = (len(pay) // 4)
            self.chunks.append(
                np.frombuffer(pay[: n * 4], dtype=">f4").astype(np.float64))

    def __enter__(self):
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        if self._t:
            self._t.join(timeout=2.0)
        return False

    def left(self):
        """Monitor audio is interleaved stereo; take one channel."""
        if not self.chunks:
            return np.array([])
        flat = np.concatenate(self.chunks)
        return flat[0::2]


def dump_tx_status(r, tag):
    """T2 read-back. The exact status key for the transmit filter is not
    documented here, so print every candidate line rather than guessing a
    prefix -- guessing produced a bogus `None` on the first run."""
    hits = []
    for line in r.status:
        body = line.split("|", 1)[-1]
        if body.startswith("transmit") or ("filter" in body and
                                           not body.startswith("slice ")):
            hits.append(body[:200])
    print(f"  [tx-status {tag}] {len(hits)} candidate line(s)")
    for h in hits[-4:]:
        print(f"      {h}")


def tone_level(sig, freq, nfft=8192):
    """dB of `freq` relative to the broadband median -- a crude but decisive SNR.

    Analyse the MIDDLE of the capture. The capture brackets the injection with
    silence on both sides, so a window taken from the start sees only that lead-in
    silence and reports a false negative -- which is exactly what the first run of
    this check did.
    """
    if sig.size < nfft:
        return None
    mid = sig.size // 2
    start = max(0, mid - nfft // 2)
    seg = sig[start:start + nfft] * np.hanning(nfft)
    spec = np.abs(np.fft.rfft(seg)) ** 2
    freqs = np.fft.rfftfreq(nfft, 1.0 / FS)
    i = int(np.argmin(np.abs(freqs - freq)))
    peak = spec[max(0, i - 2): i + 3].max()
    med = np.median(spec)
    if peak <= 0.0:
        return None          # all-zero capture: report as no-signal, not -inf
    return 10 * np.log10(peak / (med + 1e-30))


def main():
    r = Flex(RADIO)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    uport = udp.getsockname()[1]
    emit = Emitter(udp, RADIO)
    print(f"connected handle={r.handle} udp={uport} underlying={UNDERLYING}")
    print(f"greeting version: {r.version}")

    saved_mode = saved_txslice = sid = None
    rax_id = None
    try:
        must(r, "client gui")
        must(r, f"client station {STATION}")
        # remote_audio_rx is a CLIENT stream and is delivered to the client's
        # registered UDP port -- NOT to the waveform's `udpport`. Omitting this
        # makes T1 silently capture nothing (first run of this probe did exactly
        # that and produced a false "inconclusive").
        must(r, f"client udpport {uport}")
        for s in ("slice", "pan", "waveform", "radio", "client", "tx",
                  "audio_stream"):
            r.cmd(f"sub {s} all")
        r.pump(2.5)

        # Standing task: re-verify firmware on every OTA session.
        _, ver = r.cmd("version")
        print(f"\nfirmware: {ver}")

        ids = r.slice_ids()
        if not ids:
            raise RuntimeError("no slice available")
        sid = ids[0]
        f0 = r.fields(f"slice {sid} ")
        saved_mode = f0.get("mode")
        saved_txslice = f0.get("tx")
        print(f"slice {sid}: mode={saved_mode} tx={saved_txslice} "
              f"filter={f0.get('filter_lo')}/{f0.get('filter_hi')}")
        dump_tx_status(r, "before")

        # POSITIVE CONTROL, before anything is changed. If the monitor does not
        # carry ordinary slice audio in THIS setup, then a null result from the
        # T1 injections says nothing at all about stream ids. Establish the
        # baseline first, in the slice's original mode.
        print(f"\n-- control: monitor audio with slice {sid} in {saved_mode} --")
        code, pay = r.cmd("stream create type=remote_audio_rx compression=none")
        if ok(code) and pay.strip():
            rax_id = int(pay.split()[0], 16)
            print(f"  remote_audio_rx = 0x{rax_id:08X}")
            with BackgroundCapture(udp, rax_id) as cap:
                time.sleep(2.5)
            ctl = cap.left()
            ctl_rms = float(np.sqrt(np.mean(ctl ** 2))) if ctl.size else 0.0
            print(f"  {cap.pkts} pkts, {ctl.size} samples, RMS={ctl_rms:.3e}")
            control_ok = ctl_rms > 1e-6
            print("  -> " + ("monitor DOES carry slice audio; T1 is meaningful"
                             if control_ok else
                             "monitor is SILENT even in a normal mode -- T1 "
                             "cannot be interpreted (setup issue, not a finding)"))
        else:
            control_ok = False
            print(f"  could not create remote_audio_rx ({code})")

        # ---------------- T2: tx_filter acceptance/clamping ----------------
        print("\n=== T2: tx_filter cut conventions (registration-time) ===")
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
        print("  streams: " + " ".join(f"{k}=0x{v:08X}" for k, v in streams.items()))
        rx_in = streams["rx_stream_in_id"]
        tx_in = streams.get("tx_stream_in_id")

        must(r, f"waveform set {WF_NAME} tx=1")   # TX-capable; does NOT key
        for lc in (-3500, -1, 0, 300):
            try_cmd(r, f"waveform set {WF_NAME} tx_filter low_cut={lc}")
        for hc in (2400, 4800):
            try_cmd(r, f"waveform set {WF_NAME} tx_filter high_cut={hc}")
        try_cmd(r, f"waveform set {WF_NAME} tx_filter depth=256")
        must(r, f"waveform set {WF_NAME} rx_filter low_cut={RX_LOW}")
        must(r, f"waveform set {WF_NAME} rx_filter high_cut={RX_HIGH}")
        must(r, f"waveform set {WF_NAME} rx_filter depth=256")
        must(r, f"waveform set {WF_NAME} udpport={uport}")

        print(f"\n-- slice {sid} -> {WF_MODE} --")
        must(r, f"slice set {sid} mode={WF_MODE}")
        r.pump(2.0)
        fa = r.fields(f"slice {sid} ")
        print(f"  slice: mode={fa.get('mode')} filter={fa.get('filter_lo')}/"
              f"{fa.get('filter_hi')} audio_mute={fa.get('audio_mute')}")
        r.status.clear()
        r.pump(1.5)
        dump_tx_status(r, "after mode")

        # Designate as TX slice (does NOT key) so TX-side state is observable.
        print("\n-- designate TX slice (no keying) --")
        try_cmd(r, f"slice set {sid} tx=1")
        r.status.clear()
        r.pump(1.5)
        dump_tx_status(r, "after tx-slice")

        # ---------------- T3: does tx_stream_in flow unkeyed? ----------------
        print(f"\n=== T3: tx_stream_in before PTT ({T3_SECS:.0f}s, UNKEYED) ===")
        if tx_in is None:
            print("  no tx_stream_in_id in create response -- skipped")
        else:
            n_tx, others = count_stream(udp, tx_in, T3_SECS)
            rate = n_tx / T3_SECS
            print(f"  tx_stream_in (0x{tx_in:08X}): {n_tx} pkts ({rate:.1f}/s)")
            print("  other stream ids seen: "
                  + (", ".join(f"0x{k:08X}:{v}" for k, v in sorted(others.items()))
                     or "none"))
            print("  ->  " + ("FLOWS UNKEYED" if n_tx > 0 else
                              "SILENT while unkeyed (keyed case = Tier 2)"))

        # ---------------- T1: which stream id is honoured on emit? ----------
        print("\n=== T1: which stream id does the radio honour on emit? ===")
        if rax_id and not control_ok:
            print("  SKIPPED -- the positive control failed, so any result here")
            print("  would be uninterpretable. Fix the monitor path first.")
        elif rax_id:
            # Silent baseline IN the waveform mode: §9.1 predicts the RAD2 slice
            # contributes nothing to the monitor. The injections are measured
            # against this, not against the USB control.
            with BackgroundCapture(udp, rax_id) as cap:
                time.sleep(2.0)
            base = cap.left()
            base_rms = float(np.sqrt(np.mean(base ** 2))) if base.size else 0.0
            print(f"  silent baseline in {WF_MODE}: {cap.pkts} pkts "
                  f"RMS={base_rms:.3e}")
            for key, tone in T1_PASSES:
                cand = streams.get(key)
                if cand is None:
                    print(f"  {key}: absent from create response -- skipped")
                    continue
                # Capture CONCURRENTLY with injection -- see BackgroundCapture.
                with BackgroundCapture(udp, rax_id) as cap:
                    time.sleep(0.4)
                    sent = emit.send_tone(cand, tone, T1_SECS)
                    time.sleep(0.4)
                sig, pkts = cap.left(), cap.pkts
                lvl = tone_level(sig, tone) if sig.size else None
                rms = float(np.sqrt(np.mean(sig ** 2))) if sig.size else 0.0
                print(f"  emit on {key}=0x{cand:08X} tone={tone:.0f} Hz "
                      f"({sent} sent, {pkts} back, RMS={rms:.3e})")
                if lvl is None:
                    print("      no monitor audio captured -- INCONCLUSIVE")
                else:
                    verdict = ("HONOURED" if lvl > 15 else
                               "not honoured" if lvl < 6 else "ambiguous")
                    print(f"      tone {lvl:+.1f} dB over median -> {verdict}")
                time.sleep(0.5)
        else:
            print("  no remote_audio_rx -- T1 skipped")

    finally:
        print("\n-- teardown --")
        try:
            if rax_id:
                print(f"  stream remove -> {r.cmd('stream remove 0x%08X' % rax_id)[0]}")
            if sid is not None and saved_txslice is not None:
                print(f"  restore tx={saved_txslice} -> "
                      f"{r.cmd(f'slice set {sid} tx={saved_txslice}')[0]}")
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
