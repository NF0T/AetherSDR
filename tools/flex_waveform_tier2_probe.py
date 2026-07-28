#!/usr/bin/env python3
"""RADE V2 RFC §16 Q4 "Tier 2" — the TX tests that key the transmitter.

DESIGNED TO MAKE PROGRESS WITHOUT A MONITOR RECEIVER. Every test here reads the
radio's OWN telemetry (forward power, compression, interlock state), so all of it
runs today. The tests that genuinely need an off-air receiver are enumerated in
DEFERRED_TESTS at the bottom and are NOT stubbed into the run -- they are listed
so the boundary stays explicit rather than getting quietly forgotten.

    A  does the waveform TX path key and produce RF at all?          telemetry
    B  ALC / compression linearity  <- the one that matters          telemetry
    C  is `tx_filter` honoured?                                      telemetry
    D  does `tx_stream_in` carry mic audio while keyed?              telemetry
    E  underrun behaviour (informs the EOO tail design)              telemetry
    -- needs a receiver: transmitted spectrum, sideband sense, IMD, EVM

WHY B AND C MATTER (they change the provider's design):
  B  RADE V2 is OFDM. If the radio's speech processor / ALC is in the path, the
     waveform does not survive it. A linear power transfer with COMPPEAK at zero
     is the evidence we can get without an off-air receiver.
  C  §10.1: the V2 modulator applies NO filtering or pulse shaping of any kind
     (rade_v2_ofdm_mod_frame is IDFT + memcpy'd cyclic prefix, full stop), so its
     spectrum has unsuppressed sinc sidelobes and the Flex `tx_filter` is the ONLY
     out-of-band suppression in the chain. Tier 1 showed the filter is *accepted*;
     whether it is *honoured* is unknown, and if it is not, nothing is suppressing
     those sidelobes.

SAFETY
  Running this with no arguments does the FULL pre-flight and keys NOTHING.
  Keying requires an explicit --arm. Gates, each of which aborts before any RF:
    * radio-reported rfpower must be <= MAX_WATTS (we never SET power; operator's knob)
    * our slice must be on TEST_FREQ_MHZ, and it must be OUR slice (client_handle)
    * we must own the TX-slice designation (saved and restored)
  While armed:
    * every key is wrapped in `keyed()`, which force-unkeys via a watchdog thread
      at MAX_KEY_S regardless of what the test body is doing
    * `xmit 0` runs in a finally on any exception, including KeyboardInterrupt
    * RF is only believed when the INTERLOCK reports TRANSMITTING. Per
      docs/architecture/flex-meter-learnings.md, neither `slice tx=1` nor stream
      `tx=1` nor `met_in_rx=1` means the radio is keyed.
  Station identification (47 CFR 97.119): CW "de <call>" at the end of the run,
  and automatically again if wall time since the last ID passes ID_INTERVAL_S
  while testing continues. Note the 10-minute clock runs on WALL time, not keyed
  time, so a slow debugging session can cross it without much RF going out.
"""

import argparse
import math
import os
import socket
import struct
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flex_waveform_tier1_probe import Flex, must, ok, try_cmd  # noqa: E402

RADIO = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
STATION = WF_NAME = "AetherRadeProbe"
WF_MODE = "RADP"
UNDERLYING = os.environ.get("PROBE_UNDERLYING", "DIGU")

TEST_FREQ_MHZ = float(os.environ.get("PROBE_TX_FREQ", "50.340"))
TEST_BAND = os.environ.get("PROBE_TX_BAND", "6")   # pan band for TEST_FREQ_MHZ
MAX_WATTS = float(os.environ.get("PROBE_MAX_WATTS", "1.0"))
MAX_KEY_S = float(os.environ.get("PROBE_MAX_KEY_S", "5.0"))
CALLSIGN = os.environ.get("PROBE_CALLSIGN", "nf0t")
ID_INTERVAL_S = 8 * 60          # re-ID before the 10-minute regulatory limit
CW_WPM = 20

FS = 24000
SPP = 128
VITA_PORT = 4991
OUI = 0x001C2D
AUDIO_CLASS = 0x534C03E3
METER_PCC = 0x8002              # VITA packet class code for meter data

# §10.1: matched to the modem (1000-1937.5 Hz occupancy), NOT opened wide --
# the V2 modulator does no filtering, so this is the only sidelobe suppression.
TX_FILTER = (800, 2200)
TX_FILTER_ALT = (300, 3600)     # second registration: the test-C discriminator
RX_FILTER = (800, 2200)

B_AMPLITUDES = [0.05, 0.10, 0.20, 0.35, 0.50, 0.70, 1.00]
B_TONE_HZ = 1500.0
C_TONES_HZ = [500, 1000, 1500, 2000, 2400, 3000, 3500]


# ─────────────────────────── VITA / meters ───────────────────────────

def parse_vita(data):
    """Generic VITA-49 header walk -> (packet_class_code, payload_bytes).

    Parsed generically rather than assuming the 28-byte audio layout, because
    meter packets need not carry the same timestamp fields.
    """
    if len(data) < 4:
        return None, b""
    w = struct.unpack(">I", data[:4])[0]
    ptype = (w >> 28) & 0xF
    has_class = bool(w & 0x08000000)
    has_trailer = bool(w & 0x04000000)
    tsi = (w >> 22) & 0x3
    tsf = (w >> 20) & 0x3
    off = 4
    if ptype in (0x1, 0x3):          # ...WITH_STREAM_ID
        off += 4
    pcc = None
    if has_class:
        if len(data) < off + 8:
            return None, b""
        pcc = struct.unpack(">I", data[off + 4:off + 8])[0] & 0xFFFF
        off += 8
    if tsi:
        off += 4
    if tsf:
        off += 8
    end = len(data) - (4 if has_trailer else 0)
    return pcc, data[off:end]


class MeterReader(threading.Thread):
    """Background reader for VITA meter packets (PCC 0x8002).

    Payload is N x (uint16 meter_id, int16 raw), big-endian; raw/128.0 gives the
    engineering value for dBm/dB/dBFS/SWR units (see MeterModel).
    """

    def __init__(self, udp):
        super().__init__(daemon=True)
        self.udp = udp
        self.latest = {}            # meter_id -> (perf_time, value)
        self.audio_pkts = {}        # stream_id -> count, for test D
        self.audio_rms = {}         # stream_id -> running max |sample|
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
            now = time.perf_counter()
            pcc, payload = parse_vita(data)
            if pcc == METER_PCC:
                n = len(payload) // 4
                with self._lock:
                    for i in range(n):
                        mid, raw = struct.unpack(">Hh", payload[i * 4:i * 4 + 4])
                        self.latest[mid] = (now, raw / 128.0)
            elif len(data) >= 28:
                sid = struct.unpack(">I", data[4:8])[0]
                with self._lock:
                    self.audio_pkts[sid] = self.audio_pkts.get(sid, 0) + 1
                    if payload:
                        a = np.frombuffer(payload[: (len(payload) // 4) * 4],
                                          dtype=">f4")
                        if a.size:
                            peak = float(np.max(np.abs(a)))
                            self.audio_rms[sid] = max(self.audio_rms.get(sid, 0.0),
                                                      peak)

    def stop(self):
        self._stop.set()

    def value(self, mid, max_age=1.5):
        with self._lock:
            got = self.latest.get(mid)
        if not got:
            return None
        t, v = got
        return v if (time.perf_counter() - t) <= max_age else None

    def sample(self, mid, secs=1.2, hz=20):
        """Collect a meter over `secs` and return its samples."""
        out, end = [], time.perf_counter() + secs
        while time.perf_counter() < end:
            v = self.value(mid)
            if v is not None:
                out.append(v)
            time.sleep(1.0 / hz)
        return out

    def reset_audio(self):
        with self._lock:
            self.audio_pkts.clear()
            self.audio_rms.clear()


def dbm_to_w(dbm):
    return 10.0 ** ((dbm - 30.0) / 10.0)


def global_transmit(r):
    """Merge only the GLOBAL `transmit ...` status, excluding band memories.

    `transmit band <n> band_name=6 rfpower=100 ...` lines also start with
    "transmit ", so a naive prefix merge lets a stored per-band memory overwrite
    the live global value — and the value being merged is the one the power gate
    depends on.
    """
    merged = {}
    for line in r.status:
        body = line.split("|", 1)[-1]
        if body.startswith("transmit ") and not body.startswith("transmit band "):
            for tok in body[len("transmit "):].split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    merged[k] = v
    return merged


def fresh_slice(r, sid):
    """Re-read slice status from a CLEARED buffer.

    Flex.fields() merges everything seen so far, so reading straight after a
    command can return the pre-command value if the new status has not landed
    yet — which looked exactly like `slice tune` being accepted-but-ignored.
    """
    r.status.clear()
    r.cmd("sub slice all")
    r.pump(1.2)
    return r.fields(f"slice {sid} ")


def pan_fields(r, pan):
    merged = {}
    want = f"display pan {pan} "
    for line in r.status:
        body = line.split("|", 1)[-1]
        if body.startswith(want):
            for tok in body[len(want):].split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    merged[k] = v
    return merged


def read_rfpower(r):
    """Live TX power, re-read fresh rather than from accumulated status."""
    r.status.clear()
    r.cmd("sub tx all")
    r.pump(1.5)
    return global_transmit(r).get("rfpower")


def meter_manifest(r):
    """meter_id -> (src, nam, unit), from `sub meter all` status.

    Matched by src+nam, never by id: ids are assigned per session
    (docs/architecture/flex-meter-learnings.md).
    """
    fields = {}
    for line in r.status:
        body = line.split("|", 1)[-1]
        if not body.startswith("meter "):
            continue
        for tok in body[len("meter "):].split("#"):
            if "." not in tok or "=" not in tok:
                continue
            idx, rest = tok.split(".", 1)
            k, v = rest.split("=", 1)
            try:
                fields.setdefault(int(idx), {})[k] = v
            except ValueError:
                continue
    return {i: (d.get("src", ""), d.get("nam", ""), d.get("unit", ""))
            for i, d in fields.items()}


def find_meters(manifest, src_prefix, name):
    """All ids matching src+name. Plural on purpose.

    TX-chain meters like COMPPEAK and ALC are repeated PER ACTIVE SLICE (see
    docs/architecture/flex-meter-learnings.md) — on this radio COMPPEAK appears
    as both 155 and 179. Taking the first would silently read the wrong slice's
    block, so callers sample all of them and reduce.
    """
    out = []
    for mid, (src, nam, unit) in sorted(manifest.items()):
        if nam.upper() == name.upper() and src.upper().startswith(src_prefix.upper()):
            out.append((mid, unit))
    return out


def find_meter(manifest, src_prefix, name):
    got = find_meters(manifest, src_prefix, name)
    return got[0] if got else (None, None)


# ─────────────────────────── TX audio sender ───────────────────────────

class ContinuousSender(threading.Thread):
    """Streams paced waveform audio on one stream id, with live parameters.

    Phase is continuous across parameter changes so the amplitude/frequency
    sweeps do not inject discontinuities that would themselves look like
    splatter. `enabled=False` keeps the thread running but stops sending, which
    is exactly what test E needs to create an underrun.
    """

    def __init__(self, udp, host, stream_id):
        super().__init__(daemon=True)
        self.udp = udp
        self.host = host
        self.stream_id = stream_id
        self.freq = B_TONE_HZ
        self.amp = 0.0
        self.enabled = False
        self.sent = 0
        self._phase = 0.0
        self._count = 0
        self._stop = threading.Event()

    def set(self, freq=None, amp=None):
        if freq is not None:
            self.freq = float(freq)
        if amp is not None:
            self.amp = float(amp)

    def run(self):
        t0 = time.perf_counter()
        dt = SPP / FS
        i = 0
        while not self._stop.is_set():
            if self.enabled:
                idx = np.arange(SPP)
                dphi = 2 * math.pi * self.freq / FS
                mono = (self.amp * np.sin(self._phase + dphi * idx)).astype(np.float32)
                self._phase = (self._phase + dphi * SPP) % (2 * math.pi)
                c = self._count & 0xF
                self._count += 1
                hdr_word = (0x10000000 | 0x08000000 | 0x00400000 | 0x00200000
                            | (c << 16) | (7 + SPP * 2))
                now = time.time()
                sec = int(now)
                fps = int((now - sec) * 1e12)
                hdr = struct.pack(">IIIIIII", hdr_word, self.stream_id, OUI,
                                  AUDIO_CLASS, sec, (fps >> 32) & 0xFFFFFFFF,
                                  fps & 0xFFFFFFFF)
                inter = np.empty(SPP * 2, dtype=">f4")
                inter[0::2] = mono
                inter[1::2] = mono
                try:
                    self.udp.sendto(hdr + inter.tobytes(), (self.host, VITA_PORT))
                    self.sent += 1
                except OSError:
                    pass
            i += 1
            target = t0 + i * dt
            rem = target - time.perf_counter()
            if rem > 0:
                time.sleep(rem)
            else:
                t0 = time.perf_counter()
                i = 0

    def stop(self):
        self._stop.set()


# ─────────────────────────── keying ───────────────────────────

class TxSession:
    """Owns keying, the watchdog, interlock confirmation and station ID."""

    def __init__(self, r, meters, armed, slice_id):
        self.r = r
        self.meters = meters
        self.armed = armed
        self.slice_id = slice_id
        self.fallback_mode = WF_MODE   # if the fresh read comes back empty
        self.first_tx = None
        self.last_id = None
        self.keyed_total = 0.0

    def interlock_state(self):
        st = ""
        for line in self.r.status:
            body = line.split("|", 1)[-1]
            if body.startswith("interlock "):
                for tok in body.split():
                    if tok.startswith("state="):
                        st = tok.split("=", 1)[1]
        return st.upper()

    def wait_state(self, want, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            self.r.pump(0.15)
            if want in self.interlock_state():
                return True
        return False

    def keyed(self, why, max_s=None):
        return _Keyed(self, why, max_s or MAX_KEY_S)

    def maybe_id(self):
        """Regulatory re-ID if wall time since the last one is getting long."""
        if self.first_tx is None:
            return
        if self.last_id and (time.time() - self.last_id) < ID_INTERVAL_S:
            return
        print(f"\n  [ID] {ID_INTERVAL_S//60} min since last identification — "
              f"sending CW ID before continuing")
        self.send_cw_id()

    def send_cw_id(self):
        """CW 'de <call>'. CWX keys the transmitter itself — no xmit needed.

        Spaces MUST be 0x7F: FlexLib CWX.cs does s.Replace(' ', '\\u007f')
        before sending, so a literal space would mis-send the message.

        Switches the slice to CW and back itself. The mid-session auto-ID can
        fire while the slice is in the waveform mode, where CWX would not key,
        so the mode handling has to live here rather than at the call sites.
        """
        if not self.armed:
            print("  [ID] (not armed — would send CW ID here)")
            return
        r = self.r
        # Fresh read: the accumulated buffer gets cleared elsewhere, and a None
        # here previously meant the slice was left in CW after a mid-run auto-ID
        # (harmless at teardown, which sets the mode explicitly, but wrong for
        # the auto-ID path that has to hand the slice back mid-test).
        prev = fresh_slice(r, self.slice_id).get("mode") or self.fallback_mode
        if prev and prev != "CW":
            r.cmd(f"slice set {self.slice_id} mode=CW")
            r.pump(0.8)
        msg = f"de {CALLSIGN}".replace(" ", "\x7f")
        try_cmd(r, f"cwx wpm {CW_WPM}")
        code, _ = r.cmd(f'cwx send "{msg}"')
        print(f"  [ID] cwx send \"de {CALLSIGN}\" -> {code}")
        # Wait for the keyer to finish: interlock goes TRANSMITTING then settles.
        self.wait_state("TRANSMITTING", timeout=4.0)
        end = time.time() + 25.0
        quiet_since = None
        while time.time() < end:
            r.pump(0.2)
            if "TRANSMITTING" in self.interlock_state():
                quiet_since = None
            else:
                quiet_since = quiet_since or time.time()
                if time.time() - quiet_since > 1.5:
                    break
        self.last_id = time.time()
        if prev and prev != "CW":
            r.cmd(f"slice set {self.slice_id} mode={prev}")
            r.pump(0.6)
        print(f"  [ID] complete (slice restored to {prev})")


class _Keyed:
    def __init__(self, sess, why, max_s):
        self.s = sess
        self.why = why
        self.max_s = max_s
        self.t0 = None
        self._wd = None
        self._done = threading.Event()

    def _watchdog(self):
        if not self._done.wait(self.max_s):
            print(f"\n  !! WATCHDOG: {self.max_s:.1f}s key limit hit — forcing xmit 0")
            try:
                self.s.r.cmd("xmit 0")
            except Exception:  # noqa: BLE001
                pass

    def __enter__(self):
        if not self.s.armed:
            print(f"  [dry] would key for: {self.why}")
            return False
        self.s.maybe_id()
        self.t0 = time.time()
        self._wd = threading.Thread(target=self._watchdog, daemon=True)
        self._wd.start()
        code, _ = self.s.r.cmd("xmit 1")
        if not ok(code):
            self._done.set()
            raise RuntimeError(f"xmit 1 rejected ({code})")
        if self.s.first_tx is None:
            self.s.first_tx = time.time()
            # Start the regulatory clock at the FIRST transmission. Leaving
            # last_id as None made maybe_id() fall straight through to an ID on
            # the second key, since `last_id and elapsed < interval` is False
            # when last_id is None. §97.119 requires an ID within 10 minutes of
            # the first transmission, which is exactly this.
            if self.s.last_id is None:
                self.s.last_id = self.s.first_tx
        if not self.s.wait_state("TRANSMITTING", timeout=3.0):
            self.s.r.cmd("xmit 0")
            self._done.set()
            raise RuntimeError(
                f"interlock never reported TRANSMITTING (saw "
                f"'{self.s.interlock_state()}') — refusing to trust meters")
        return True

    def __exit__(self, *exc):
        if not self.s.armed:
            return False
        self._done.set()
        self.s.r.cmd("xmit 0")
        if self.t0:
            self.s.keyed_total += time.time() - self.t0
        self.s.wait_state("READY", timeout=2.0)
        time.sleep(0.3)
        return False


# ─────────────────────────── the tests ───────────────────────────

def test_a_and_d(sess, meters, sender, fwd_id, tx_in_id, tx_out_id):
    print("\n=== A: does the waveform TX path key and make RF? ===")
    for label, sid in (("tx_stream_in_id", tx_in_id), ("tx_stream_out_id", tx_out_id)):
        if sid is None:
            continue
        sender.stream_id = sid
        sender.set(freq=B_TONE_HZ, amp=0.5)
        meters.reset_audio()
        with sess.keyed(f"A on {label}", max_s=4.0) as live:
            if not live:
                return None
            sender.enabled = True
            time.sleep(0.4)
            samples = meters.sample(fwd_id, secs=1.6)
            sender.enabled = False
        if samples:
            pk = max(samples)
            print(f"  emit on {label}=0x{sid:08X}: FWDPWR max {pk:+.1f} dBm "
                  f"({dbm_to_w(pk):.2f} W)")
            if dbm_to_w(pk) > 0.05:
                print(f"  -> RF PRODUCED on {label}")
                print("\n=== D: does tx_stream_in carry mic audio while keyed? ===")
                n = meters.audio_pkts.get(tx_in_id, 0)
                pk_in = meters.audio_rms.get(tx_in_id, 0.0)
                print(f"  tx_stream_in 0x{tx_in_id:08X}: {n} pkts, peak {pk_in:.4f}")
                # "flows" and "carries audio" are different claims. Tier 1 saw 0
                # packets unkeyed; packets here confirm the stream is PTT-gated.
                # A zero peak with mic_selection=PC and no PC audio present is
                # expected and must not be reported as carrying mic audio.
                if not n:
                    print("  -> stream does NOT flow even while keyed")
                elif pk_in < 1e-6:
                    print("  -> stream FLOWS while keyed but is SILENT (peak 0).")
                    print("     PTT-gated, confirming Tier 1's unkeyed result. Whether")
                    print("     it carries usable mic audio is UNPROVEN — needs a live")
                    print("     source on the selected mic input.")
                else:
                    print("  -> FLOWS and CARRIES AUDIO while keyed")
                return sid
        else:
            print(f"  emit on {label}: no FWDPWR samples")
    print("  -> NO RF on either stream id")
    return None


def test_b(sess, meters, sender, fwd_id, comp_ids, alc_ids=None,
           swr_id=None, ref_id=None):
    print("\n=== B: ALC / compression linearity  [the one that matters] ===")
    print("  linear path => +6 dB power per doubling of amplitude")
    rows = []
    for amp in B_AMPLITUDES:
        sender.set(freq=B_TONE_HZ, amp=amp)
        with sess.keyed(f"B amp={amp}", max_s=4.0) as live:
            if not live:
                return
            sender.enabled = True
            time.sleep(0.5)
            fwd = meters.sample(fwd_id, secs=1.3)
            comp = []
            for cid in (comp_ids or []):
                comp += meters.sample(cid, secs=0.25)
            # ALC is a SEPARATE meter from COMPPEAK. A previous run reported a
            # non-linear transfer while COMPPEAK sat at 0.0, which says nothing
            # about ALC — the likelier limiter of the two.
            alc = []
            for aid in (alc_ids or []):
                alc += meters.sample(aid, secs=0.2)
            # SWR and reflected power, sampled per step. Into a mismatched
            # antenna the radio may fold back drive as power rises, which would
            # masquerade as compression -- so record the match alongside the
            # transfer rather than reasoning about it afterwards.
            swr = meters.sample(swr_id, secs=0.2) if swr_id else []
            ref = meters.sample(ref_id, secs=0.2) if ref_id else []
            sender.enabled = False
        if not fwd:
            print(f"  amp={amp:.2f}: no samples")
            continue
        dbm = float(np.median(fwd))
        rows.append((amp, dbm, dbm_to_w(dbm),
                     max(comp) if comp else None,
                     max(alc) if alc else None,
                     float(np.median(swr)) if swr else None,
                     float(np.median(ref)) if ref else None))

    if len(rows) < 2:
        return
    print(f"\n  {'amp':>6} {'dBm':>8} {'watts':>8} {'COMPPEAK':>9} {'ALC':>8} "
          f"{'SWR':>6} {'REF dBm':>8}")
    for amp, dbm, w, comp, alcv, swrv, refv in rows:
        c = f"{comp:.1f}" if comp is not None else "n/a"
        al = f"{alcv:.1f}" if alcv is not None else "n/a"
        sw = f"{swrv:.2f}" if swrv is not None else "n/a"
        rf = f"{refv:.1f}" if refv is not None else "n/a"
        print(f"  {amp:6.2f} {dbm:8.1f} {w:8.3f} {c:>9} {al:>8} {sw:>6} {rf:>8}")
    swrs = [r[5] for r in rows if r[5] is not None]
    if swrs and max(swrs) > 2.0:
        print(f"\n  NOTE: SWR {min(swrs):.2f}-{max(swrs):.2f} across the sweep. "
              f"FWDPWR is FORWARD power, not delivered power, and a rising SWR "
              f"can trigger\n  drive foldback that mimics compression. Foldback's "
              f"signature is TOP-END SAG specifically — see the step table below.")
    # Judge linearity on STEP-TO-STEP gain, and anchor to the TOP of the sweep.
    #
    # The first version anchored to rows[0] -- the lowest drive, and therefore
    # the noisiest point -- and reported "NON-LINEAR, something is compressing"
    # from a 12 dB deviation. That was backwards. Compression flattens the TOP
    # of a transfer curve; what this radio shows is a floored BOTTOM, because
    # the forward-power meter cannot resolve a few milliwatts. Anchoring to the
    # noise floor projected that floor across the whole curve.
    print("\n  step-to-step gain (no anchor assumption):")
    print(f"  {'step':>12} {'expected':>9} {'observed':>9} {'error':>7}")
    steps = []
    for (a1, d1, *_), (a2, d2, *_) in zip(rows, rows[1:]):
        exp = 20.0 * math.log10(a2 / a1)
        obs = d2 - d1
        steps.append((a1, a2, exp, obs))
        print(f"  {a1:.2f}->{a2:.2f} {exp:+9.2f} {obs:+9.2f} {obs-exp:+7.2f}")

    good = [s for s in steps if abs(s[3] - s[2]) < 1.0]
    floor_dbm = None
    if good:
        lowest_good = min(s[0] for s in good)
        below = [d for a, d, *_ in rows if a < lowest_good]
        if below:
            floor_dbm = max(below)
            print(f"\n  meter floor: readings below amp {lowest_good:.2f} sit at "
                  f"~{floor_dbm:.1f} dBm ({dbm_to_w(floor_dbm)*1000:.0f} mW) and stop "
                  f"tracking drive.\n  Those points measure the INSTRUMENT, not the "
                  f"transmitter, and are excluded.")
        top = [s for s in steps if s[0] >= lowest_good]
        worst = max(abs(o - e) for _, _, e, o in top) if top else 0.0
        print(f"\n  worst step error above amp {lowest_good:.2f}: {worst:.2f} dB")
        # Compression shows up as the LAST steps falling short. Test that.
        tail = steps[-2:]
        sag = max((e - o) for _, _, e, o in tail) if tail else 0.0
        print(f"  top-end sag (compression signature): {sag:+.2f} dB")
        if worst < 1.0 and sag < 1.0:
            print("  -> LINEAR over the usable range — no ALC, no compression. "
                  "OFDM-safe.")
        elif sag >= 1.0:
            print("  -> TOP-END COMPRESSION — the transfer flattens at high drive; "
                  "OFDM will suffer.")
        else:
            print("  -> irregular transfer; inspect the table before trusting it.")
    else:
        print("\n  -> no linear region found; the sweep may sit entirely in the "
              "meter's noise floor. Raise drive or power and repeat.")
    alcs = [r[4] for r in rows if r[4] is not None]
    if alcs:
        print(f"  ALC max {max(alcs):.1f} dBFS")
    comps = [r[3] for r in rows if r[3] is not None]
    if comps:
        print(f"  COMPPEAK max {max(comps):.1f} dB -> "
              + ("no speech processing" if max(comps) < 1.0 else "PROCESSING ACTIVE"))


def test_c(sess, meters, sender, fwd_id, register, sid_holder):
    print("\n=== C: is tx_filter honoured? ===")
    print("  constant amplitude, swept tone; then RE-REGISTER with a different")
    print("  tx_filter and repeat. If the curve moves with the registration, the")
    print("  waveform filter is what is acting -- not the radio's global TX filter.")
    curves = {}
    for cfg in (TX_FILTER, TX_FILTER_ALT):
        print(f"\n  -- registering tx_filter {cfg} --")
        stream_id = register(cfg)
        if stream_id is None:
            print("  registration failed; skipping")
            continue
        sender.stream_id = stream_id
        row = {}
        for hz in C_TONES_HZ:
            sender.set(freq=hz, amp=0.5)
            with sess.keyed(f"C {cfg} {hz}Hz", max_s=3.5) as live:
                if not live:
                    return
                sender.enabled = True
                time.sleep(0.4)
                s = meters.sample(fwd_id, secs=1.0)
                sender.enabled = False
            row[hz] = float(np.median(s)) if s else None
        curves[cfg] = row

    if len(curves) == 2:
        (c1, r1), (c2, r2) = list(curves.items())
        print(f"\n  {'Hz':>6} {str(c1):>14} {str(c2):>14} {'Δ':>8}")
        moved = 0.0
        for hz in C_TONES_HZ:
            v1, v2 = r1.get(hz), r2.get(hz)
            if v1 is None or v2 is None:
                print(f"  {hz:6d} {'-':>14} {'-':>14} {'-':>8}")
                continue
            print(f"  {hz:6d} {v1:14.1f} {v2:14.1f} {v2-v1:+8.1f}")
            moved = max(moved, abs(v2 - v1))
        print(f"\n  largest change between registrations: {moved:.1f} dB")
        print("  -> " + ("tx_filter IS honoured" if moved > 3.0 else
                         "NO effect — tx_filter appears NOT honoured; nothing is "
                         "suppressing the V2 sidelobes (see §10.1)"))


def test_e(sess, meters, sender, fwd_id):
    print("\n=== E: underrun behaviour (informs the EOO tail design) ===")
    sender.set(freq=B_TONE_HZ, amp=0.5)
    with sess.keyed("E underrun", max_s=5.0) as live:
        if not live:
            return
        sender.enabled = True
        time.sleep(1.0)
        before = meters.value(fwd_id)
        st_before = sess.interlock_state()
        sender.enabled = False          # deliberate underrun
        time.sleep(1.5)
        during = meters.value(fwd_id)
        st_during = sess.interlock_state()
        sender.enabled = True
        time.sleep(0.8)
        after = meters.value(fwd_id)
        sender.enabled = False
    fmt = lambda v: f"{v:+.1f} dBm" if v is not None else "n/a"  # noqa: E731
    print(f"  streaming : {fmt(before)}  interlock={st_before}")
    print(f"  underrun  : {fmt(during)}  interlock={st_during}")
    print(f"  resumed   : {fmt(after)}")
    print("  -> " + ("radio DROPPED TX on underrun — the EOO tail must keep the "
                     "stream fed, not merely hold PTT"
                     if "TRANSMITTING" not in st_during else
                     "radio held TX through the underrun"))


DEFERRED_TESTS = """
NOT RUN — these need an off-air monitor receiver, and no telemetry substitutes:
  * transmitted spectrum: are the sinc sidelobes actually where §10.1 predicts,
    and does tx_filter suppress them in the RF domain rather than just on a
    power meter?
  * SIDEBAND SENSE. §10.5 infers from the shipped D-Star provider that outbound
    is stereo audio, so there is no TX conjugate convention to get wrong. That
    inference is UNVERIFIED ON AIR, and no power meter can see an inverted
    sideband. This is the single most important deferred item.
  * IMD / spectral purity under real OFDM peak-to-average, not a single tone.
  * EVM / whether the OFDM survives the radio's TX chain intact.
"""


def main():
    # Windows consoles default to cp1252, which raises UnicodeEncodeError on the
    # box-drawing and Greek characters used in the reports. That crash landed
    # AFTER test B had already keyed the transmitter seven times, discarding a
    # full sweep of hard-won data. Never let formatting cost RF time again.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="store_true",
                    help="actually key the transmitter (default: pre-flight only)")
    ap.add_argument("--tests", default="ABCDE", help="subset of tests to run")
    ap.add_argument("--stay", action="store_true",
                    help="leave the slice on the test frequency/band at exit "
                         "instead of restoring it. Restoring moves the slice to "
                         "another band, and coming back reloads that band's ATU "
                         "memory — which drops a manually engaged ATU back to "
                         "bypass and silently changes the match between runs.")
    args = ap.parse_args()

    r = Flex(RADIO)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    uport = udp.getsockname()[1]
    meters = MeterReader(udp)
    meters.start()

    saved = {}
    sid = None
    sender = None
    sess = None
    try:
        must(r, "client gui")
        must(r, f"client station {STATION}")
        must(r, f"client udpport {uport}")
        for s in ("slice", "pan", "waveform", "radio", "client",
                  "tx", "meter", "cwx", "audio_stream"):
            r.cmd(f"sub {s} all")
        r.pump(3.0)

        _, ver = r.cmd("version")
        print(f"firmware: {ver}")

        # Snapshot the meter manifest NOW, before anything clears r.status.
        # fresh_slice()/read_rfpower() clear the buffer to defeat stale merges,
        # which also discards the one-shot meter manifest if it is read later —
        # that produced a spurious "no TX/FWDPWR meter" abort.
        manifest = dict(meter_manifest(r))
        print(f"meter manifest: {len(manifest)} entries captured")

        # ---------- gate 1: our own slice ----------
        mine = r.my_slice_ids()
        if not mine:
            raise RuntimeError("this connection owns no slice — refusing to "
                               "drive another client's slice")
        sid = mine[0]
        f0 = r.fields(f"slice {sid} ")
        saved["mode"] = f0.get("mode")
        saved["freq"] = f0.get("RF_frequency")
        saved["tx"] = f0.get("tx")
        others = [s for s in r.slice_ids() if s not in mine]
        print(f"\nour slice {sid}: mode={saved['mode']} RF={saved['freq']} "
              f"tx={saved['tx']}   (other clients' slices: {others or 'none'})")

        # ---------- gate 2: frequency (BEFORE the power check) ----------
        # Order matters. Flex keeps PER-BAND power memories ("transmit band N
        # ... rfpower="), so the effective power can change when the TX slice
        # moves to another band. Checking power before tuning would validate the
        # wrong band's setting.
        # A slice cannot leave its panadapter's span, so the PAN moves first.
        # `slice tune` alone returns code 0 and silently does nothing when the
        # target is outside the current pan.
        pan = f0.get("pan")
        if not pan:
            raise RuntimeError("cannot determine our slice's panadapter")
        pfz = pan_fields(r, pan)
        saved["pan"] = pan
        saved["pan_band"] = pfz.get("band")
        saved["pan_center"] = pfz.get("center")
        print(f"pan {pan}: band={saved['pan_band']} center={saved['pan_center']}")

        must(r, f"display panafall set {pan} band={TEST_BAND}")
        r.pump(2.0)
        must(r, f"display panafall set {pan} center={TEST_FREQ_MHZ:.3f}")
        r.pump(2.0)
        now_f = None
        for attempt in range(3):
            r.cmd(f"slice tune {sid} {TEST_FREQ_MHZ:.6f}")
            r.pump(1.0)
            now_f = fresh_slice(r, sid).get("RF_frequency")
            if now_f and abs(float(now_f) - TEST_FREQ_MHZ) <= 0.0005:
                break
            print(f"  tune attempt {attempt+1}: slice reads {now_f}, retrying")
        print(f"slice {sid} tuned to {now_f} (target {TEST_FREQ_MHZ})")
        if now_f is None or abs(float(now_f) - TEST_FREQ_MHZ) > 0.0005:
            raise RuntimeError(
                f"slice is on {now_f}, not {TEST_FREQ_MHZ} — refusing to key on "
                f"an unverified frequency")

        # ---------- gate 3: power, as it reads ON THIS BAND ----------
        rfpower = read_rfpower(r)
        tx = global_transmit(r)
        print(f"transmit: rfpower={rfpower} (on {TEST_FREQ_MHZ} MHz) "
              f"speech_processor={tx.get('speech_processor_enable')} "
              f"mic={tx.get('mic_selection')}")
        if rfpower is None:
            raise RuntimeError("cannot read rfpower — refusing to key blind")
        if float(rfpower) > MAX_WATTS:
            raise RuntimeError(
                f"rfpower reads {rfpower} W on {TEST_FREQ_MHZ} MHz, over the "
                f"{MAX_WATTS} W limit — REFUSING TO KEY.\n"
                f"    Set the transmit power to {MAX_WATTS:g} W with the TX slice "
                f"on this band, then re-run.\n"
                f"    (This probe never sets power itself — that is the "
                f"operator's control.)")

        # ---------- register ----------
        def register(txf):
            r.cmd(f"slice set {sid} mode={saved['mode']}")
            r.cmd(f"waveform remove {WF_NAME}")
            payload = must(r, f"waveform create name={WF_NAME} mode={WF_MODE} "
                              f"underlying_mode={UNDERLYING} version=0.0.1")
            st = {}
            for tok in payload.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    try:
                        st[k] = int(v, 16)
                    except ValueError:
                        pass
            must(r, f"waveform set {WF_NAME} tx=1")
            must(r, f"waveform set {WF_NAME} tx_filter low_cut={txf[0]}")
            must(r, f"waveform set {WF_NAME} tx_filter high_cut={txf[1]}")
            must(r, f"waveform set {WF_NAME} tx_filter depth=256")
            must(r, f"waveform set {WF_NAME} rx_filter low_cut={RX_FILTER[0]}")
            must(r, f"waveform set {WF_NAME} rx_filter high_cut={RX_FILTER[1]}")
            must(r, f"waveform set {WF_NAME} rx_filter depth=256")
            must(r, f"waveform set {WF_NAME} udpport={uport}")
            must(r, f"slice set {sid} mode={WF_MODE}")
            r.pump(1.2)
            register.streams = st
            return st.get("tx_stream_in_id")

        print(f"\n-- registering waveform, tx_filter {TX_FILTER} --")
        tx_in = register(TX_FILTER)
        tx_out = register.streams.get("tx_stream_out_id")

        # ---------- gate 4: TX slice designation ----------
        try_cmd(r, f"slice set {sid} tx=1")
        r.pump(0.8)

        # ---------- meters ----------
        manifest.update(meter_manifest(r))          # pick up any late additions
        fwd_id, fwd_u = find_meter(manifest, "TX", "FWDPWR")
        comp_ids = [m for m, _ in find_meters(manifest, "TX", "COMPPEAK")]
        alc_ids = [m for m, _ in find_meters(manifest, "TX", "ALC")]
        swr_id, _ = find_meter(manifest, "TX", "SWR")
        ref_id, _ = find_meter(manifest, "TX", "REFPWR")
        print(f"\nmeters: FWDPWR={fwd_id} ({fwd_u})  SWR={swr_id}  REFPWR={ref_id}")
        print(f"        COMPPEAK={comp_ids}  ALC={alc_ids}  "
              f"(per-slice duplicates — sampled together)")
        if fwd_id is None:
            raise RuntimeError("no TX/FWDPWR meter — refusing to key without "
                               "the instrument the tests depend on")
        time.sleep(1.5)
        print(f"  live check: FWDPWR={meters.value(fwd_id)} "
              f"(a value here in RX is normal — met_in_rx, not RF)")

        sess = TxSession(r, meters, args.arm, sid)
        sender = ContinuousSender(udp, RADIO, tx_in)
        sender.start()

        print("\n" + "=" * 68)
        if not args.arm:
            print("PRE-FLIGHT COMPLETE — all gates passed. NOTHING WAS KEYED.")
            print(f"  freq {TEST_FREQ_MHZ}  power {rfpower} W  slice {sid}")
            outs = f"0x{tx_out:08X}" if tx_out else "n/a"
            print(f"  tx_stream_in=0x{tx_in:08X}  tx_stream_out={outs}")
            print("\nRe-run with --arm to transmit.")
            print(DEFERRED_TESTS)
            return 0
        # Final power re-check immediately before any RF: registration and the
        # mode change happened since gate 3, and power is a live setting.
        final_pwr = read_rfpower(r)
        if final_pwr is None or float(final_pwr) > MAX_WATTS:
            raise RuntimeError(f"power re-check failed at arm time: {final_pwr} W")
        print(f"ARMED — will transmit on {TEST_FREQ_MHZ} MHz at {final_pwr} W")
        print(f"  max {MAX_KEY_S}s per key, watchdog enforced, xmit 0 in finally")
        print("=" * 68)

        used = None
        if "A" in args.tests:
            used = test_a_and_d(sess, meters, sender, fwd_id, tx_in, tx_out)
            if used:
                sender.stream_id = used
        if "B" in args.tests:
            test_b(sess, meters, sender, fwd_id, comp_ids, alc_ids,
                   swr_id, ref_id)
        if "C" in args.tests:
            test_c(sess, meters, sender, fwd_id, register, sid)
        if "E" in args.tests:
            test_e(sess, meters, sender, fwd_id)

        print(f"\ntotal keyed time: {sess.keyed_total:.1f}s")
        print(DEFERRED_TESTS)

    finally:
        print("\n-- teardown --")
        try:
            if sender:
                sender.enabled = False
                sender.stop()
            r.cmd("xmit 0")                     # unconditional, always
            # Final identification, only if we actually transmitted. send_cw_id
            # handles the CW mode switch and restores whatever it found.
            if sess and sess.armed and sess.first_tx:
                sess.send_cw_id()
            if sid is not None and args.stay:
                # Deliberately NOT restoring frequency/band/mode. Leaving 6 m
                # keeps whatever ATU state the operator has established; a
                # round trip through another band reloads that band's ATU
                # memory and lands back in bypass, changing the match between
                # runs without anything reporting it.
                print(f"  --stay: leaving slice {sid} on {TEST_FREQ_MHZ} MHz "
                      f"(band/pan/mode NOT restored, ATU state preserved)")
                old_tx = saved.get("tx")
                if old_tx is not None:
                    code = r.cmd(f"slice set {sid} tx={old_tx}")[0]
                    print(f"  tx -> {old_tx}: {code}")
            elif sid is not None:
                old_mode = saved.get("mode")
                old_freq = saved.get("freq")
                old_tx = saved.get("tx")
                if old_mode:
                    code = r.cmd(f"slice set {sid} mode={old_mode}")[0]
                    print(f"  mode -> {old_mode}: {code}")
                if saved.get("pan_band"):
                    code = r.cmd(f"display panafall set {saved['pan']} "
                                 f"band={saved['pan_band']}")[0]
                    print(f"  pan band -> {saved['pan_band']}: {code}")
                    r.pump(1.2)
                if saved.get("pan_center"):
                    code = r.cmd(f"display panafall set {saved['pan']} "
                                 f"center={saved['pan_center']}")[0]
                    print(f"  pan center -> {saved['pan_center']}: {code}")
                    r.pump(1.2)
                if old_freq:
                    code = r.cmd(f"slice tune {sid} {old_freq}")[0]
                    print(f"  freq -> {old_freq}: {code}")
                if old_tx is not None:
                    code = r.cmd(f"slice set {sid} tx={old_tx}")[0]
                    print(f"  tx -> {old_tx}: {code}")
            print(f"  waveform remove -> {r.cmd(f'waveform remove {WF_NAME}')[0]}")
        except Exception as e:  # noqa: BLE001
            print(f"  TEARDOWN ERROR: {e}")
            try:
                r.cmd("xmit 0")
            except Exception:  # noqa: BLE001
                pass
        meters.stop()
        udp.close()
        r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
