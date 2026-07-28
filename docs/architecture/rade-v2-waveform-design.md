# RFC: RADE V2 as an In-Process Flex Waveform

> **Status: DRAFT — internal, not yet posted.** This document is being iterated
> privately and hardened by implementation before any wide distribution or upstream
> PR (per our bulletproof-before-distribution policy). No tracking issue exists yet.
> Nothing here authorizes merging to `aethersdr/AetherSDR`; the RFC is the gate on
> the *upstream PR*, not on fork development.
>
> **Author:** NF0T · **Started:** 2026-07-26 · **Radio under test:** FLEX-8400, fw 4.2.20.41343
>
> **Companion docs:** `digital-voice-thumbdv-waveform.md` (D-Star waveform — the closest
> existing pattern), `aetherd-iradiobackend-design.md` / `aetherd-headless-engine-design.md`
> (the radio-backend seam this must respect).

## 1. Summary

Add **RADE V2** (Radio Autoencoder, FreeDV's neural voice codec) as a first-class,
in-client digital-voice mode in AetherSDR, implemented as an **in-process Flex
*waveform*** rather than the current V1 "fake DIGU mode + DAX tap" approach. The
waveform registers a real slice mode (`RAD2`) on the radio; AetherSDR receives the
modem passband on the waveform input stream, decodes it in-process, and **renders the
decoded speech locally at full fidelity**. RADE **V1 is left entirely untouched** and is
deleted later, in one PR, if/when upstream FreeDV drops it.

The goal is parity-or-better with the existing in-client RADE experience while removing
V1's structural UX defects (slice muting, split-brain mode display, a parallel audio
side-channel), working across **all** Flex models, and positioning the codec above the
`IRadioBackend` seam so future non-Flex backends can host it when they gain voice TX.

## 2. Goals / Non-goals

**Goals**
- In-client RADE V2 with no separate app required (no FreeDV-GUI / no external container).
- Works on **all** Flex models (6000 and 8000 class) — a **PC-side** waveform, not on-radio Docker.
- RX audio fidelity **at least equal to RADE V1** (full-bandwidth decoded speech).
- No slice muting, no split-brain mode, no parallel audio side-channel.
- Codec core decoupled from the Flex transport, so other radio backends can host it later.
- V1 remains shippable and unmodified throughout; its removal is a separate, clean subtraction.

**Non-goals (for this RFC)**
- On-radio Docker packaging (FreeDV is expected to ship that; out of scope here).
- Non-Flex backend support today (no other backend can transmit voice yet — see §12).
- Changing or refactoring the RADE V1 implementation.

## 3. Background

### 3.1 RADE V1 today, and why it needs replacing (not mutating)
RADE V1 is **not a real radio mode**. Activating it rewrites the slice to `DIGU`/`DIGL`,
**mutes the slice**, taps the slice's **DAX** audio, decodes in `RADEEngine`, and plays the
result through a dedicated overlay buffer (`m_radeRxBuffer`) mixed into the speaker output.
Consequences, all verified on hardware:
- **Split-brain UI** — the mode field/dropdown read `DIGU` while a bolt-on "RADE" badge
  shows sync/SNR; the operator sees two contradictory indicators.
- **Mute stranding** — the slice mute is a **radio-side, persisted, shared** control
  (`slice set N audio_mute=1`); it's hand-stashed/restored across ~6 exit paths, and any
  missed restore strands the slice muted for every client (issue-#2856 shape).
- **A parallel audio side-channel** — `m_radeRxBuffer` + a dedicated resampler + mix branch
  in the audio hot path.

These are structural to the "fake mode" approach, which is why V2 is a **new** engine and
mode rather than a mutation of V1.

### 3.2 What changed in RADE V2 (protocol)
V2 drops the pilot symbols from the *data* frames → a narrower modem waveform, ~3 dB more
sensitive at low SNR, with different sync/framing. The text channel also changes fundamentally:
V1 sent the callsign as a single LDPC burst at end-of-over (`rade_tx_eoo`); **V2 streams it
inline** as one BPSK data symbol per modem frame (`rade_tx_set_data_symbol` /
`rade_rx_get_data_symbol`).

**The EOO frame itself does not disappear.** What left the EOO is the *callsign*, not the burst.
V2 still emits an end-of-over frame — `rade_tx_v2_eoo()` writes `RADE_V2_NEOO = 960` complex
samples (6 × `pend_cp` pilot symbols) = **120 ms at 8 kHz** — and the receiver consumes it as an
end-of-transmission detector (`eoo_smooth`, threshold `TEOO`, `eoo_flag` in `rade_rx_v2.c`). It
carries **no data bits**, unlike V1's. See §9.2 for what this means for PTT.

### 3.3 The V2 C port exists
The V2 C port lives in **`drowe67/radae_nopy`**, branch `dr-radev2` (not `drowe67/radae`).
`rade_api.h` is unified: one opaque `struct rade *` holds both V1 and V2 state; `RADE_MODE_V2
= 0x10` OR'd into `rade_open()` selects V2. V1 and V2 compile side by side (~108 MB combined
weight source). We already vendor the V1 C port at `third_party/radae`; the V2 bump replaces it
from `dr-radev2`.

## 4. Protocol authority (Constitution Principle I)

- **RADE codec:** `drowe67/radae_nopy` (`rade_api.h`, `RADE_MODE_V2`) — BSD-licensed; vendored
  with provenance (the `crdv` clean-room provenance in the D-Star work is the template).
- **Flex Waveform API:** FlexRadio's public `waveform-sdk` and the documented `waveform`/VITA-49
  wire protocol. We implement a **clean-room** provider against that public protocol; we do **not**
  reuse the GPL-3.0 `third_party/smartsdr-dsp` provider glue in-process (see §11, Licensing).

## 5. Architecture overview

```
                         ┌──────────────────────── AetherSDR (in-process) ───────────────────────┐
   Flex radio            │                                                                        │
   ┌────────┐  rx_stream_in (24kHz Cf32)  ┌───────────────┐   passband PCM   ┌──────────────┐    │
   │ RAD2   │ ─────────────────────────►  │ FlexWaveform  │ ───────────────► │ RADEV2Engine │    │
   │ slice  │   waveform VITA-49          │  Provider     │                  │  (codec core)│    │
   │        │ ◄───────────────────────    │ (backends/flex)│ ◄─────────────── │              │    │
   └────────┘  rx_stream_out (optional,   └───────────────┘   modem PCM /     └──────┬───────┘    │
        │       for OTHER clients only)          ▲            data symbols            │            │
        │  registration: `waveform create RAD2`  │                          decoded speech (wideband)
        │  on AetherSDR's OWN TCP-4992 conn      │                                    ▼            │
        │                                        │                          ┌──────────────┐      │
        └──── raw modem diverted here ───────────┘                          │ AudioEngine  │──► speaker
             (never enters the monitor mix → NO MUTE)                       │ (local render)│      │
                                                                            └──────────────┘      │
                         └────────────────────────────────────────────────────────────────────────┘
```

Key: the radio diverts the RAD2 slice's raw modem audio to the waveform (so it never reaches
the monitor mix — **nothing to mute**), we decode in-process, and we **render the decoded
speech locally** (full fidelity). Feeding `rx_stream_out` back to the radio is *optional* and
only serves other SmartSDR clients (see §10, §11-fidelity).

## 6. Design decisions (with rationale and evidence)

Each decision below is backed by the Phase 0 spike (§8) or by verified source analysis.

1. **In-process, not a separate daemon.** The codec sits above `IRadioBackend`, so it can be
   fed by any backend's audio path AetherSDR already has; a daemon sits *outside* the backend
   abstraction and would duplicate it or need IPC. RADE is pure software (no external vocoder
   hardware, unlike D-Star's ThumbDV), so there's no hardware reason for a separate process.
2. **Flex Waveform API, not a DAX tap.** A real `RAD2` waveform makes the radio divert the
   slice's raw modem audio to us — eliminating the mute and the split-brain by construction
   (it's a real mode). *Verified:* Phase 0 OQ#1 — a GUI-client connection can register a
   waveform in-process on its own connection (§8).
3. **RX audio output — REOPENED. The fidelity argument for local render has been refuted.**
   This decision previously read "local render is *required* for fidelity parity" because the
   return path was believed hard-capped at ~2.8 kHz (§8, 1b). **That measurement was invalid and
   the cap does not exist** — §10.7 measures the return path as flat to at least 8 kHz and
   unaffected by the registered `rx_filter`. Decoded speech can be handed back to the radio
   without meaningful loss.
   The live choice is now **local render only** vs **return-path only**, decided on latency,
   multiflex reach and WAN compression rather than on bandwidth. Doing *both* remains rejected
   (they double against each other with no clean suppression — §9.1). Baseline stays local render
   pending the latency measurement; see §9.1 and §16 Q1.
   (Whether to *also* feed `rx_stream_out` for other clients is a separate, deferred question —
   it double-backs onto our own render and can't be muted away cleanly; see §9.1.)
4. **On-client (PC-side) waveform, not on-radio Docker.** A PC-side waveform works on 6000 and
   8000 class alike; on-radio Docker is 8000-class/Aurora only and is FreeDV's job to ship.
5. **Registry-native `RAD2` pseudo-mode.** Register through `DigitalVoiceModeRegistry` (the
   D-Star pattern) so claim/release/restore is owned and can't strand — fixing the V1 UX.
6. **V1 left entirely intact.** No shared base class forced onto V1, no duplication of V1's
   wiring. V1 is deleted later as a clean subtraction.
7. **Codec core ↔ transport internal seam.** Core contract: *passband PCM in ↔ modem PCM out +
   control (PTT / callsign bits / sync / SNR)*; one `Transport` implementor today
   (`FlexWaveformTransport`). No speculative multi-backend framework (§12).
8. **Clean-room provider.** Implement the (small) waveform provider protocol against the public
   waveform-sdk; do not link GPL-3.0 `smartsdr-dsp` in-process (§11).
9. **`RAD2` registers as `underlying_mode=DIGU`, `rx_filter 800–2200 Hz`, upper sideband on
   every band.** Derived from the V2 OFDM source, not from V1 (V1's modem is wider and
   pilot-bearing, so it is the *wrong* proxy). DIGU rather than USB follows D-Star's own
   pattern — it registers `DFM`, the *data* variant of its demod family, not `FM`. The filter
   is deliberately **not** tightened onto the modem, because the modem already band-passes
   itself and a narrower upstream filter corrupts the reported SNR. Full derivation and the
   numbers in §10.1.

## 7. Component design & placement

All new code is gated behind a compile flag **`ENABLE_RADE_V2`** (off by default) until release.

| Component | Location | Notes |
|---|---|---|
| `FlexWaveformProvider` | `src/core/backends/flex/` | Flex wire protocol → behind `FlexBackend`; keeps EB3 clean and puts the Flex-specific transport where a future backend's transport would sit |
| `RADEV2Engine` | `src/core/` (`libaethercore`) | Worker-thread QObject; codec core with the transport-agnostic seam |
| `RAD2` mode | `DigitalVoiceModeRegistry` | New `DigitalVoiceModeId::RadeV2` + descriptor (`radioMode "RAD2"`, `underlyingMode "DIGU"` — see §10.1; no `DIGL` variant) |
| GUI wiring | `src/gui/` | New parallel `activateRADEV2()`; V1's `activateRADE()` untouched |
| Vendored codec | `third_party/radae` | Re-vendored from `radae_nopy@dr-radev2` (Phase 4; vendor **last**) |

## 8. Phase 0 evidence (hardened by coding, live FLEX-8400 fw 4.2.20.41343)

The spike drove the risky unknowns to ground before committing to this design. Every claim below
is reproducible — the probe that produced it is preserved in `tools/` (dev-only, not built or
shipped):

| Finding | Article |
|---|---|
| OQ#1 (in-process registration) | `tools/flex_waveform_phase0_oq1_probe.py` |
| 1a (`rx_stream_in` VITA-49 format) | `tools/flex_waveform_phase0_rxformat_probe.py` |
| 1b (round-trip ~2.8 kHz cap) | `tools/flex_waveform_phase0_roundtrip_probe.py` |
| §10.2 (`underlying_mode` acceptance) | `tools/flex_waveform_probe.py` |
| §10.3 (data plane, no-mute, conjugate I/Q) | `tools/flex_waveform_dataplane_probe.py` |

An earlier draft recorded the Phase 0 three as lost with their session scratchpad; they were
recovered intact on 2026-07-26 and committed. Each carries a header noting what it produced and,
where applicable, **which of its own conclusions was later corrected** — 1a's "real passband"
reading is superseded by §10.3 item 5. They are historical articles, not maintained reference
implementations; read the RFC section, not the script, for what is currently believed true.

- **OQ#1 — GO.** A GUI-client connection (`client gui` → `client station`) successfully ran
  `waveform create name=… mode=RAD2 underlying_mode=USB version=…` on its **own** TCP-4992
  connection (reply code 0). So AetherSDR can host the waveform **in-process, single connection**
  — no dedicated socket, no daemon. Registration adds `RAD2` to the slice `mode_list`.
  *Note:* the spike registered `underlying_mode=USB`; the design has since settled on `DIGU`,
  which was confirmed accepted in a follow-up probe (§10.2).
- **Create returns six streams (fw 4.2.20).** `tx/rx_stream_in`, `tx/rx_stream_out`, **plus a
  `byte_stream_in`/`byte_stream_out` pair.** The byte-stream pair is a candidate transport for
  V2's inline callsign/data channel (the vendored D-Star parser reads only 4; ours must read 6).
- **1a — input format.** `rx_stream_in` is **VITA-49 IF-Data w/ Stream ID**, FlexRadio OUI
  `0x1c2d`, class `0x534C03E3`, **128 Complex-float32 samples/packet (1024 B, big-endian),
  steady-state 24001 Hz.** Same rate/format as internal RADEv1 audio — the transport is **not**
  lower fidelity.
- **1b — the round-trip is voice-band-capped.** Injecting a 100 Hz→11 kHz log sweep on
  `rx_stream_out` and capturing the return via `remote_audio_rx` (uncompressed) shows the monitor
  audio **flat to ~2.5 kHz, −6 dB @ ~2800 Hz, −60…−72 dB by 3–4 kHz**. Sweeping the slice audio
  filter (`filt <idx> 0 <hi>`, accepted) up to **11000 Hz did not move the rolloff** — so the cap
  is **fixed at ~2.8 kHz and not filter-adjustable.** This is the container's quality loss, and
  it is why we render locally (decision §6.3).
  > ❌ **REFUTED — this result is wrong. See §10.7.** 1b injected on `rx_stream_out_id`, which
  > §10.6 T1 shows the radio **never honours**. Re-measured properly on `rx_stream_in_id`, the
  > return path is **flat to at least 8 kHz** and is **not shaped by the registered `rx_filter`
  > at all**. There is no ~2.8 kHz cap. The rest of §8 is unaffected.
- **Firmware** reconfirmed `4.2.20.41343` (three ways).

## 9. Data flow

**RX:** radio (RAD2 slice) → `rx_stream_in` (24 kHz Complex-f32) → `FlexWaveformProvider` →
`RADEV2Engine.decode()` → wideband decoded speech → `AudioEngine` **local render** → speaker.
The inline text channel (V2) is read per-frame (`rade_rx_get_data_symbol`, or the `byte_stream`)
and surfaces the far-end callsign as slice sub-state. No slice mute; the raw modem never enters
the monitor mix.

**TX:** mic → `AudioEngine` → `RADEV2Engine.encode()` (per frame: `rade_tx_set_data_symbol(next
callsign bit)`, then `rade_tx`) → modem PCM → `tx_stream_out` → radio transmitter. The callsign is
streamed inline, so there is no *data-bearing* EOO burst to assemble and drain — but V2 still emits
a 120 ms pilot-only EOO frame, and PTT must be held across it plus buffer flush (§9.2). Much
simpler than V1's three-layer EOO PTT orchestration, though not instantaneous.

**Holding PTT is necessary but not sufficient.** Per §10.5 item 4 the transmit pump is *driven by
inbound `tx_stream_in` packets*, not by a clock of ours — so when the radio stops delivering them
the emit path stops too, and the EOO tail is dropped no matter what PTT is doing. The provider must
synthesize buffers to flush the tail, the way the D-Star path already does for its own tail. Any TX
design that treats "hold PTT ~120 ms" as the whole fix is wrong.

### 9.1 RX audio output — REOPENED 2026-07-28 (the fidelity argument was refuted)

> **Read this first.** Everything below the rule was written when the return path was believed
> hard-capped at ~2.8 kHz. **§10.7 measured it flat to at least 8 kHz**, so the fidelity argument
> that made local render *required* is gone. The section is kept because its *other* argument —
> the double-back — survives intact and is now measured rather than inferred (§10.6 T1 observed
> injected audio returning in our own monitor).
>
> **The corrected framing is a two-way choice, not a baseline plus an optional extra:**
>
> | | **A — local render only** | **B — return path only** |
> |---|---|---|
> | Fidelity | full | **equal** (§10.7); Opus on WAN only |
> | Other clients hear the slice | no | **yes** |
> | Latency | minimal | **+22 ms** remote-over-VPN, ~16 ms on-site (§10.8) |
> | Double-back | none | none |
> | Slice volume / mute work normally | no — needs its own plumbing | **yes** |
> | Parallel audio side-channel | **yes** | none |
>
> **A + B together stays rejected** — that is the only configuration the double-back argument
> below actually rules out, and `audio_mute` cannot fix it (global, and it would silence the very
> clients being fed).
>
> **Recommendation: B**, subject to the operator's call. Latency came in at ~22 ms (§10.8), which
> is negligible beside RADE's own framing and codec delay, and fidelity is equal. The decisive
> argument is not either of those, though — it is §2:
>
> > **§2 Goals: "No slice muting, no split-brain mode, *no parallel audio side-channel*."**
>
> **Option A *is* a parallel audio side-channel.** It rebuilds, in new code, the shape of the V1
> pathology this design exists to remove: a private decode → private buffer → private mix path
> alongside the radio's real one, with its own volume handling, its own failure modes, and a slice
> whose own audio controls do nothing. Option B has the radio carry RAD2 audio the way it carries
> every other mode's — the operator's slice volume and mute simply work, and other clients hear
> the slice for free.
>
> **What would change the recommendation:** SmartLink operation being materially worse under Opus,
> or a use case where AetherSDR must produce RADE audio while the radio's monitor path is
> unavailable. Note the 22 ms was measured **remote over a WireGuard VPN**, not on a bench — the
> radio classifies that link as LAN, so it is uncompressed, and an on-site operator sees ~16 ms
> (§10.8). Opus is therefore a **SmartLink** concern specifically, not a remote-operation one.

---

#### Superseded reasoning (kept for the double-back argument, which still holds)

**Current expectation (this iteration): local-render-only.** By default the RAD2 slice
contributes **silence** to the monitor — the raw modem is diverted to the waveform, and the
decoded speech is rendered locally at full FARGAN bandwidth. This is clean in every case: no
mute, no double, works single- or multi-slice.

**Why the `rx_stream_out` (other-client) feed is not a free option.** Feeding real decoded audio
to `rx_stream_out` — so *other* SmartSDR clients on the radio hear the RAD2 slice — has a hard
constraint found while iterating this design: the monitor (`remote_audio_rx`) is **radio-mixed,
and a client cannot subtract one slice from it** (see [[reference-flex-audio-topology]]). So the
fed audio **comes straight back to AetherSDR** and doubles/echoes against our own local render
(and at the degraded ~2.8 kHz). The only suppression is muting the slice — but `audio_mute` is
**global/per-slice**, so muting (a) **reintroduces the mute-stranding this whole design
eliminates**, *and* (b) **silences the slice for the very other-clients we were feeding.** Net:
the feed coexists with local render *only* in the single-slice case where AetherSDR ignores the
monitor entirely; in multi-slice it doubles with no clean fix on current Flex firmware.

**Plan — implement as an investigation toggle; defer the final decision (hardened by coding).**
We will implement the `rx_stream_out` feed behind a **dev/experimental toggle**, then A/B compare
— spectrally *and* perceptually, on **real decoded RADE speech** (not just the 1b sweep) — our
local render vs. the round-tripped monitor version, to quantify/qualify how audible the ~2.8 kHz
truncation actually is on voice. The **multiflex use case** (the RAD2 slice audible from other
clients) is a legitimate one for many operators even if lower-priority for the primary author, so
investigate it fully — including whether the double-back / global-mute constraint has *any*
workaround. **The final decision — local-only, remote-only, or an operator toggle — is deferred
to that data.** Until then the design's baseline assumption is local-render-only.

### 9.2 Callsign / text channel (V2 inline data) — two distinct layers

The far-end callsign rides RADE's **own inline data channel** — `rade_tx_set_data_symbol` /
`rade_rx_get_data_symbol` — **one BPSK bit per `rade_tx()` step** (a step = `RADE_V2_FRAMES_PER_STEP`
= 4 LPCNet feature frames ≈ 40 ms, so **~25 bit/s**). This is the **only** path to the far station's
RADE decoder; the Flex `byte_stream` (below) is local-only and cannot carry over-the-air data. The
two are different layers, not competing options.

The channel is **raw** — no framing, sync, FEC, or CRC. The upstream V2 test streams MSB-first
ASCII bits, **cycles the message continuously**, and brute-force-searches for it on RX. So the
*application* owns the text protocol (serialize the callsign; add sync/CRC/FEC as desired; repeat it
during the over so a late-joining or re-syncing RX catches the next copy).

**PTT release — improved, but not free.** Because the callsign is continuous and inline, there is
no need to *drain and assemble* a data-bearing EOO burst, which is what made V1's end-of-over
expensive (a ~1.845 s pre-buffer, the root cause behind the V1 callsign work). But V2 still emits a
**120 ms** pilot-only EOO frame (§3.2), and the transmit path must still flush its own buffers, so
PTT must be held for that tail rather than released on the last voice frame. Budget ~120 ms plus
drain — roughly an order of magnitude better than V1, not zero. An earlier draft of this RFC
claimed "no end-of-over burst" and "near-immediate" PTT release; that was wrong, and the TX design
must not assume it.

**Open (Phase 4): is V2's EOO reachable through the public API?** `rade_tx_v2_eoo()` exists
internally, but the public wrapper `rade_tx_eoo()` in `rade_api.h:134` is marked *"V1 only"*. Whether
the vendored public surface exposes a V2 EOO path — and if not, whether we need one or the modem
emits it internally — must be settled when we vendor.

**Interop dependency (flag).** For callsigns to interoperate with other RADE V2 stations
(FreeDV-GUI, the container), our app-level framing on this channel **must match the RADE V2
ecosystem convention.** That convention is **not** in the C port (`radae_nopy` exposes only the raw
symbol API + a bare test); it lives in FreeDV-GUI's integration and may not be finalized while V2 is
in development. We **adopt upstream's scheme at vendor time (Phase 4)** and do not invent an
incompatible one; until it stabilizes our framing is provisional.

**The Flex `byte_stream_in/out`** (returned by `waveform create`, fw 4.2.20; absent from
`rade_api.h`) is a *separate, local* radio↔client data channel — not over-the-air. It is **not
needed** for the baseline: `RADEV2Engine` surfaces the decoded callsign to our own UI directly via
Qt signals. It is a candidate for a *future* nicety (publish decoded text to SmartSDR / other
clients for display, or accept text from SmartSDR to transmit) and is deferred out of baseline scope.

## 10. The waveform provider protocol (clean-room, from public spec + Phase 0 observation)

Registration on the existing TCP-4992 command channel (after the normal GUI bind + subs).
Each filter field is its **own** command — they do not combine on one line
(`third_party/smartsdr-dsp/SmartSDR_Interface/digital_voice_mode_registry.c:178-219`):
```
waveform create name=<X> mode=RAD2 underlying_mode=DIGU version=<v>
        → tx/rx_stream_in, tx/rx_stream_out, byte_stream_in/out  (six stream ids)
waveform set <X> tx=1
waveform set <X> rx_filter low_cut=800     # positive: DIGU/USB cuts are one-sided
waveform set <X> rx_filter high_cut=2200
waveform set <X> rx_filter depth=256
waveform set <X> tx_filter low_cut=0
waveform set <X> tx_filter high_cut=2400
waveform set <X> tx_filter depth=256
waveform set <X> udpport=<P>               (our VITA-49 listener)
```

**Filter-cut units — the trap.** D-Star registers `rx_filter low_cut=-3500` / `high_cut=3500`,
a **negative** low cut. That is not a different convention, it is a different mode *family*:
FlexLib clamps `FM`/`DFM`/`DSTR`/`AM`/`SAM`/`AME` symmetrically about the carrier (±12000),
while `USB`/`DIGU`/`FDV` are clamped one-sided — `if (low < 0) low = 0`
(`FlexLib/Slice.cs:545-560, 611-625, 675-692`). Copying D-Star's signs onto a DIGU waveform
silently clamps `low_cut` to 0 and widens the filter to the full passband.
Data plane: VITA-49 IF-Data over UDP; audio is 24 kHz Complex-float32, big-endian; outbound goes
to `radio:4991`, inbound to our registered `udpport`. Details and exact framing captured in Phase 0.
The `byte_stream_in/out` pair is a *local* radio↔client data channel (not over-the-air; see §9.2) —
out of baseline scope.

### 10.1 `RAD2` registration parameters (§16 Q3)

Derived from the V2 OFDM source at `radae_nopy@5374c52` (`dr-radev2`). **V1 is the wrong proxy
for any of this** — V1's modem is wider and carries pilots.

| Quantity | Value | Source |
|---|---|---|
| `RADE_FS` | 8000 Hz | `src/rade_dsp.h:59` |
| Nc / M / Ncp / Ns | 14 / 128 / 32 / 2 | `src/rade_v2_ofdm.h` |
| Carrier spacing Rs′ = Fs/M | 62.5 Hz | computed |
| First carrier index | 17 → 1062.5 Hz | `src/rade_v2_ofdm.c:51-57` |
| Occupied carriers | 1062.5 – 1875.0 Hz (idx 17–30) | derived |
| Actual centroid | **1468.75 Hz** | `(w0+wN)/2`, `src/rade_rx_v2.c:92` |
| Null-to-null occupancy | ~1000 – 1937.5 Hz (±Rs′ beyond edge carriers) | derived |
| Modem's own internal BPF | **975 Hz wide @ 1468.75 Hz → 981–1956 Hz** | `src/rade_rx_v2.c:86-96` |
| Frequency acquisition range | **±31.25 Hz** (= ±Rs′/2) | `src/rade_rx_v2.c:396-398` |

Note the modem is centred **1468.75 Hz, not 1500 Hz** — the code comment says 1500, but
`carrier_1_freq = 1500 − Rs′·Nc/2` puts the span half a carrier spacing low. 1500 Hz remains the
right *dial* convention (see below); 1468.75 Hz is the right number to centre a filter on.

**Chosen: `rx_filter low_cut=800`, `high_cut=2200`.** That is ~181 Hz of guard below the modem's
own BPF edge and ~244 Hz above — both far beyond the ±31.25 Hz the receiver can acquire over, so
nothing decodable is clipped.

**Do not tighten this filter in pursuit of SNR.** Two reasons, both in the source:

1. **The modem already band-passes itself.** `rade_rx_v2_init(rx, bpf_en)` builds an internal BPF
   at `1.2 × 812.5 = 975 Hz` centred 1468.75 Hz. Our Flex filter is a *second, cascaded* stage;
   narrowing it does not improve the modem's effective SNR, because that work is already done
   inside `rade_rx()`.
2. **A narrow filter makes the reported SNR lie.** `snr_offset_dB = 10·log10(3000 / B_bpf)`
   ≈ **4.88 dB** (`src/rade_rx_v2.c:96`) normalises the estimate to a 3 kHz reference *on the
   assumption that broadband noise reaches the modem*. Strip that noise upstream and
   `snr_est_dB` reads optimistically high — the number we show the operator becomes wrong.

The real justification for a moderate filter is AGC/ADC headroom and adjacent-signal rejection,
**not** modem SNR.

**⚠ This reasoning is RX-ONLY. It does NOT transfer to `tx_filter` — the transmit case is the
opposite.** Verified in the V2 source at `radae_nopy@5374c52`:

| | RX | TX |
|---|---|---|
| Does the modem filter itself? | **Yes** — `rade_rx_v2_init(rx, bpf_en)` builds a 975 Hz BPF at 1468.75 Hz (`rade_rx_v2.c:71-95`) | **No. Nothing at all.** |
| Symbol shaping | n/a | **None** — hard `memcpy` boundaries |
| Effect of tightening the Flex filter | no benefit; biases reported SNR high | **suppresses genuine out-of-band emissions** |

`rade_v2_ofdm_mod_frame()` (`rade_v2_ofdm.c:122-151`) is the entire transmit signal chain: build
the frequency-domain symbol from the latent, IDFT, `memcpy` a cyclic prefix in front, concatenate.
`rade_tx_v2_process()` returns that result directly (`rade_tx_v2.c:87`) — no window, no taper, no
overlap-add, no BPF, no post-scaling. Searching the TX path for `bpf|filter|window|taper|raised|
hann|shap` returns nothing.

**Consequence: V2's transmitted spectrum has unsuppressed sinc sidelobes.** Rectangular symbol
boundaries put the first sidelobe around −13 dB with a slow rolloff, so real energy lands well
outside the 1000–1937.5 Hz occupancy. **The Flex `tx_filter` is the only out-of-band suppression
anywhere in the chain.**

Both legs of the RX argument fail here, and it is worth being explicit about why, because the
symmetry is tempting:

1. *"The modem already band-passes itself"* — **false on TX.** It does no filtering whatsoever.
2. *"A narrow filter makes the reported SNR lie"* — **no TX analogue.** `snr_offset_dB` is a
   receive-side estimator; nothing on transmit reports SNR.

**So `tx_filter` should be matched to the modem, not opened wide** — the opposite of the guidance
above it. §10's draft registration proposes `tx_filter low_cut=0 high_cut=2400`, whose `low_cut=0`
passes sidelobe energy all the way down to DC; that should become roughly **`[800, 2200]`**, the
same shape as `rx_filter`, arrived at for an entirely different reason. §10.3 measured that
response as flat across 1000–2000 Hz with a brick-wall transition, so the outer carriers
(1062.5 and 1875 Hz) sit inside the flat region rather than on the skirts — which is the thing to
avoid, since band-edge ripple would raise EVM on exactly those carriers. Linear-phase FIR gives
flat group delay, so a constant delay is added but no ISI; the CP is 32 samples = 4 ms at 8 kHz.

**This raises the value of the Tier 2 `tx_filter` check rather than lowering it.** If the filter is
*not* honoured (the §10.6 T2 outcome left that unknown), then nothing suppresses the sidelobes and
we need another answer.

**Sideband: `DIGU` on every band — no `DIGL` variant.**

- **`DIGU` rather than `USB`** follows D-Star's own pattern: it registers `DFM`, the *data*
  variant of its demod family, not `FM`. DIGU is the data variant of USB — same upper-sideband
  demodulation, but the radio treats it as a data mode (squelch forced off, DVK excluded, no
  voice-processing assumptions on the transmit chain). AetherSDR already classifies DIGU this way
  (`src/gui/RxApplet.cpp:2676`, `src/gui/MainWindow.cpp:7807`), and RADE **V1** already sets
  DIGU/DIGL for exactly this reason — *"passthrough for the OFDM modem"*
  (`src/gui/MainWindow_DigitalModes.cpp:262`).
- **DIGU also carries the right dial convention.** `digu_offset` defaults to **1500 Hz**
  (`src/models/SliceModel.h:488`), which is RADE's nominal centre — so the operator's VFO reads
  the centre of the signal rather than the suppressed carrier. On plain `USB` our displayed
  frequency would sit ~1.5 kHz off from what other RADE stations report.
  **Sharpened by §10.3 item 7 / §10.4:** `digu_offset` is not merely a *dial* convention — it is
  the **centre a width-specified filter is constructed around** (`lo/hi = offset ∓ width/2`), which
  is why it matters. That arithmetic is applied by the *client*, keyed on the slice mode string, so
  `underlying_mode=DIGU` does **not** confer it on `RAD2`; Phase 3 must add `RAD2` to that family
  or an operator filter preset will clip the modem. Registration here is unaffected — an explicit
  `low_cut`/`high_cut` pair never goes through the width arithmetic.
- **Upper sideband on *all* bands.** Unlike V1 — which picks `DIGL` on LSB-convention bands
  (`src/gui/MainWindow_DigitalModes.cpp:266-269`) — RAD2 uses DIGU everywhere, following the
  FT8/WSJT-X convention that digital modes are upper-sideband regardless of band.
  **This is a stated belief, not yet a citation** (operator recollection of RADE V2's intended
  convention). We implement DIGU-everywhere from the start and **cite the authoritative RADE V2
  operating convention here once it is published**; if it turns out to be band-dependent, this
  becomes a one-line change plus a `DIGL` descriptor.

**Confidence and what still needs measuring.** Everything above is desk-derived from source and
carries no architectural risk — `rx_filter` is a runtime string, so being wrong costs two integers
and a reconnect. It therefore does **not** warrant a pre-vendor test rig. But the split matters:

- **Phase 2 (no codec needed):** `underlying_mode=DIGU` accepted (§10.2) and `rx_filter`
  honoured with `[800, 2200]` measured good (§10.3) — **both DONE.** Still to do: whether
  `digu_offset` shifts the demodulated passband or only the dial display, which needs a tonal
  reference rather than the noise method (§10.3 item 7).
- **Phase 4/5 (needs the vendored codec):** optimise the filter against real decode performance
  (sync rate, `snr_est_dB`, Dtmax). This is the only part that genuinely must wait; building a
  standalone V2 harness beforehand would duplicate the vendor work against a still-churning
  branch.

### 10.2 `underlying_mode` acceptance — probed 2026-07-26 (FLEX-8400, fw 4.2.20.41343)

Registration-only probe on a GUI-bound TCP-4992 connection. **No slice and no panadapter are
required to register a waveform**, so this ran non-disruptively alongside another connected
client (radio reported `slices=1 panadapters=1` throughout).

| `underlying_mode` | Result |
|---|---|
| `USB` | **ACCEPTED** (code 0) — control, matches Phase 0 |
| `DIGU` | **ACCEPTED** (code 0) — the design choice (§10.1) |
| `DIGL` | **ACCEPTED** (code 0) — fallback exists if the all-bands convention proves wrong |

So the waveform API does **not** restrict `underlying_mode` to a narrower set than `slice set`,
and the §10.1 choice of DIGU carries no registration risk. DIGL being accepted also means that if
the upper-sideband-on-all-bands belief turns out to be wrong, the band-dependent variant is a
descriptor change and nothing more.

**What this does *not* prove.** A `code 0` reply confirms the radio *accepted the command*, not
that it applies DIGU demodulation semantics (or the `digu_offset` dial convention) to the
waveform's audio. A radio can accept a mode string and quietly treat it as its family default.
Distinguishing "accepted" from "honoured" needs the data-plane test — a slice on the waveform
mode with `rx_stream_in` captured — which is the remaining §10.1 Phase 2 work and wants a quiet
radio. Treat DIGU as *registration-safe*, not yet *behaviour-confirmed*.

**Incidental finding — stream IDs are recycled.** All three registrations returned the *same* six
stream IDs, identical to Phase 0's (`tx_stream_in 0x81000005`, `rx_stream_in 0x81000004`,
`tx/rx_stream_out 0x01000005`/`0x01000004`, `byte_stream_in/out 0x80100002`/`0x00100002`). IDs are
freed on `waveform remove` and reissued, so the provider must **not** treat a stream ID as unique
across a create/remove cycle or use one as a session identity.

### 10.3 Data-plane probe — 2026-07-26 (FLEX-8400, fw 4.2.20.41343, sole client)

Slice set to the waveform mode with `underlying_mode=DIGU`, `rx_stream_in` captured and
analysed. Probe: `tools/flex_waveform_dataplane_probe.py`. RX only; TX never enabled.

**1. No mute — confirmed on hardware.** With the slice in the waveform mode, `audio_mute=0`.
The central architectural claim of this RFC (§6.2) is now measured rather than inferred: the
radio diverts the modem audio to the waveform without the client having to mute anything.

**2. `rx_filter` is honoured — but only as latched at registration / mode entry.** Registering
with a given `high_cut` and then reading the delivered passband:

| `high_cut` at registration | measured −6 dB edge |
|---|---|
| 1500 | ~1500 Hz (−3.7 dB @ 1500, −101 dB @ 1750) |
| 2200 | 2203 Hz |
| 3200 | 3205 Hz |

But `waveform set <X> rx_filter high_cut=…` issued **while the slice is already in the mode**
returns **code 0 and changes nothing** — sweeping 1200 / 2200 / 3200 live left the delivered
edge pinned at the registration value (3200) in all three cases. This is the same
accepted-but-not-honoured shape as §10.2. **Provider consequence:** set the filter *before*
putting the slice into the mode, and to change it, re-register rather than issuing a live
update. Do not expose an operator filter control that silently no-ops.

**3. The registered filter propagates to the slice's own filter fields.** Slice status moved
from `filter=100/2800` (USB) to `filter=800/3200` (waveform mode) — the registration values.

**4. `[800, 2200]` (§10.1) is validated.** Measured response, relative to in-band: −32 dB @
750, −0.2 dB @ 1000, flat 1000–2000, −30 dB @ 2250, −72 dB @ 2500. The modem's 1000–1937.5 Hz
occupancy passes essentially unclipped, with a brick-wall transition (`depth=256`). Margin
below 1000 Hz is thin, but the receiver can only acquire over ±31.25 Hz, so it is sufficient.

**5. `rx_stream_in` is a complex analytic I/Q pair — in CONJUGATE form.** The two interleaved
float32 components have equal RMS (matching to 4+ significant figures) and correlation ≈ 0,
and the spectrum of `A + jB` is one-sided at **negative** frequencies (+freq/−freq measured at
−77 dB and −133 dB across runs). So the conventional positive-frequency analytic signal is
**`A − jB`**.

> **This corrects Phase 0**, which read the payload as "real passband, not alternating I/Q
> pairs" (§8, 1a). It is an I/Q pair. **Provider consequence:** RADE's `rade_rx()` takes
> `RADE_COMP`, so no Hilbert transform is needed — but the imaginary component must be
> **negated**. Feeding `A + jB` mirrors the spectrum and will not decode. Re-verify the sign
> convention if a lower-sideband (`DIGL`) variant is ever used.

**6. Sample rate 24 kHz confirmed steady-state** (24031 / 24043 Hz measured). Phase 0's warning
holds: a ~1 s startup burst inflates this to ~28 kHz if not discarded.

**7. `digu_offset` semantics — CLOSED, and the original question was a false binary.** This
item previously asked whether `digu_offset` "shifts the demodulated passband or only the dial
display." **It does neither.** It is the **centre frequency the filter is constructed around**
when a filter is specified as a *width*:

```
lo = digu_offset - width/2
hi = digu_offset + width/2          (src/gui/RxApplet.cpp:2464-2469, width < 3000)
```

So a 1500 Hz-wide preset on a slice with `digu_offset = 1500` yields **750 – 2250 Hz**, not
0 – 1500 and not a shifted 1500 – 3000.

*Evidence, two independent routes.* **(a) OTA**, via the automation bridge with a slice put into
a Python-registered waveform mode on `underlying_mode=DIGU`, verified by screenshot — operator-run
on the Linux machine, 2026-07-26. The session hit a usage limit mid-turn and the write-up was lost,
so the screenshots are **not** preserved; this entry is reconstructed from the operator's report.
**(b) Source**, read independently afterwards: the arithmetic above reproduces the reported
750 – 2250 exactly. Two routes agreeing is why this is recorded as closed despite the missing
artifacts — but note the OTA leg is testimony, not a re-runnable article.

**This reconciles with item 2 above, and nothing needs re-deriving.** §10.3's measurements
registered explicit `low_cut`/`high_cut` *pairs*, so no width→centre arithmetic ever applied and
the edges landed on the registered values, exactly as measured. The offset arithmetic only runs
on **width-specified** filters. §10.1's `rx_filter [800, 2200]` is an explicit pair and is
therefore untouched — and, incidentally, is already centred on 1500 with a 1400 Hz width, i.e.
consistent with the DIGU convention by construction.

**But this makes §10.4 defect 3 materially worse than "a control disappears" — see there.**

**Incidental, useful for the provider and for probing:**

- The radio **auto-restores a pan + slice per GUI client on connect** (confirmed repeatedly;
  the restored slice's `client_handle` follows the connection and it disappears when that
  client drops). A probe that then creates its *own* pan and calls `slice create` gets
  `0x50000003`. **The exact cause of that error is NOT established** — two pans and two slices
  have since been observed coexisting fine, so the simple "8400 is 2/2 and the cap is hit"
  reading does not hold. Whatever the cause, **using the auto-restored slice sidesteps it**,
  which is what the probes now do. Phase 0's "secondary-client restriction" reading is still
  wrong; this replaces it with an honest "unexplained, and routed around."
- On `waveform remove`, client disconnect, **or an abrupt probe kill**, the slice reverted
  cleanly to its previous mode every time. Nothing stranded — a favourable contrast to V1's
  hand-unwound mute.

### 10.4 Stock-client GUI observation — 2026-07-26 (what a waveform mode looks like with *no* RAD2 code)

With a waveform registered and slice 0 set to it, an **unmodified AetherSDR 26.7.3** — containing
no `RAD2` code whatsoever — was observed rendering the slice. This is the cheapest possible
experiment for the §6.5 UX claim, and it cuts both ways: it **demonstrates** the headline fix and
it **surfaces four defects** that Phase 3 must fix and that would otherwise have been found late,
during engine bring-up, mixed in with codec bugs.

**The split-brain fix is demonstrated, not merely argued.** The stock client showed:

- the **mode field and dropdown read the waveform's mode**, not `underlying_mode`;
- the registered `rx_filter` drawn correctly as a **one-sided upper-sideband passband**
  (corroborating the §10 sign-clamping analysis from the GUI side);
- **`audioMute = false`.**

Because a waveform **is** a real mode, the V1 pathology (§3.1) cannot occur — nothing had to be
built to get this. This retires the "will the registry-native approach actually fix the UX"
question at zero cost.

**But `underlying_mode` is not inherited by the client-side UI.** Three of the four defects below
share one root cause: **AetherSDR classifies mode families by literal comparison against the
*slice's* mode string**, and a waveform's mode string (`RAD2`) matches no family. The radio may
apply DIGU demodulation semantics; the *client* does not know it should. Phase 3 must add `RAD2`
to each family predicate explicitly — this is not a thing the waveform registration does for us.

| # | Defect | Site | Consequence |
|---|---|---|---|
| 1 | **Squelch auto-arms** | `VfoWidget.cpp:4241,4261` (`isDig`→`sqlDisabled`); `RxApplet.cpp:2676` (same list, independently) | Squelch enabled on a slice whose audio is an OFDM modem — **gates decode**. Two independent sites; both need `RAD2`. |
| 2 | **Auto-notch offered** | `VfoWidget.cpp:4254-4256` — `isVoice = !isRtty && !isCw && !isDig && !isFm && !isFdv` | `RAD2` matches no family ⇒ `isVoice` is true ⇒ ANF/ANFL/ANFT are shown. An adaptive notch on OFDM **chews the carriers** — destructive, not merely useless. |
| 3 | **DIGU offset control disappears — and the filter-width preset silently clips the modem** | `VfoWidget.cpp:4248` (`m_digContainer` visibility); `RxApplet.cpp:2464` (mode-keyed filter geometry), fallthrough at `RxApplet.cpp:2517-2521` | Worse than a missing control — see below. A width preset on `RAD2` falls through to `lo = 95; hi = width`, which **cuts the top of the modem**. |
| 4 | **`rfGain` reset** | **not localised** | Observed on mode entry. `rfGain` is **pan**-scoped (`MainWindow.cpp:8292` `setPanRfGain`), not slice-mode-keyed, so it does **not** share the classification root cause above. Cause **not established** — do not assume it is the same bug, and re-observe before designing a fix. |

**Defect 3 in full — this is a decode-breaking trap, not a cosmetic one.** Now that §10.3 item 7
is closed, the consequence is concrete. `digu_offset` is the **centre a width-specified filter is
built around**, and that arithmetic is gated on `mode == "DIGU"`. On a `RAD2` slice the mode string
matches nothing, so `applyFilterPreset()` falls through to the default at `RxApplet.cpp:2517-2521`:

| Slice mode | Operator picks a 1500 Hz preset | Covers the modem's 1000 – 1937.5 Hz? |
|---|---|---|
| `DIGU` (offset 1500) | `750 – 2250` | **Yes**, comfortably |
| `RAD2` (fallthrough) | `95 – 1500` | **No** — everything above 1500 Hz is cut |

That removes roughly the upper half of the occupied carriers. The failure mode is a slice that
**stops decoding after the operator touches the filter**, with no error and no visible cause —
and during Phase 4 bring-up it would look exactly like a codec bug.

**This sharpens, rather than weakens, §10.1's DIGU rationale.** The offset does real work; it is
not a display convention. What §10.4 establishes is only that the work is **not inherited** — the
client applies it by literal mode-string match, so `RAD2` must be added to that family explicitly
in Phase 3. Registration-side nothing changes: §10.1 registers an explicit `low_cut`/`high_cut`
pair, which never goes through the width arithmetic. The rest of the §10.1 rationale (data-mode
treatment on the radio, the D-Star `DFM`-not-`FM` precedent) is untouched.

**Scope note.** These are Phase 3 (GUI wiring) items, not Phase 2. They are recorded here rather
than only in the phase plan because they are *evidence about the design* — specifically, evidence
bounding how much the waveform approach gives us for free. The answer is: the mode identity and the
mute, yes; the mode *family* semantics, no.

### 10.5 The TX contract — desk-derived (§16 Q4, "Tier 0")

Phase 0 was RX-only, leaving the transmit half of the provider unspecified. Most of it turns out
to be recoverable **without keying a transmitter**, because a working waveform TX implementation is
already vendored: the D-Star provider transmits, so the contract is readable rather than guessable.

**Provenance note.** Everything below is a *wire-observable* fact — packet layout, constants,
cadence, command order — of the kind Phase 0 derived independently from captures (§8, §10.3), and
the overlapping items agree. Reading the vendored tree to establish protocol facts is Principle I
protocol-authority work and does not change the §11 clean-room posture: `vita_output.c`,
`sched_waveform.c` and `hal_listener.c` are **FlexRadio** (2012-2014) and are read, not copied;
`dstar_tx_stream.c` and `digital_voice_tx_gate.c` are **AetherSDR's own** (GPL-3.0-or-later by
virtue of living in that tree), so their *design* is our prior art while the files stay unlinked.

**1. Outbound framing is fully specified — and we have already exercised it.** One function emits
both outbound streams (`vita_output.c:132-155`, `:203-211`):

| Field | Value |
|---|---|
| Header | `IF_DATA_WITH_STREAM_ID \| CLASS_ID_PRESENT \| TSI_UTC \| TSF_REAL_TIME \| (packet_count << 16) \| (7 + samples*2)` |
| `class_id_h` | `FLEXRADIO_OUI` = **0x001C2D** |
| `class_id_l` | `SL_VITA_SLICE_AUDIO_CLASS` = **0x534C03E3** (`0x534C<<16 \| 24 kHz 0x03 \| 32bps 0x60 \| stereo 0x180 \| IEEE-754 0x200`) |
| Timestamp | UTC seconds + fractional **picoseconds** (`usec × 1e6`) |
| Payload | big-endian float pairs, 8 B/sample, **128 samples/packet** (`HAL_TX_BUFFER_SIZE`) |
| Destination | **radio:4991** (`vita_output.c:121`) |

This is byte-identical to what the Phase 0 1b probe already built and the radio already accepted,
so **outbound framing carries no residual risk** — it is exercised code, not a fresh guess.

**2. The packet count is a per-stream 4-bit rolling counter**, starting at 0 for a newly seen
stream and wrapping at 16 (`vita_output.c:79-103`). The provider must keep one counter *per stream
id*, not one globally.

**3. On the outbound streams the two floats are STEREO AUDIO, not I/Q.** D-Star writes
`real = imag = <the same mono sample>` — for decoded speech on `rx_stream_out`
(`sched_waveform.c:1442-1443`) *and* for the modulated passband on `tx_stream_out`
(`sched_waveform.c:1862-1865`). The class id says so literally (`SL_CLASS_AUDIO_STEREO`).

> **This substantially defuses the TX sideband risk flagged for "Tier 3".** Because D-Star
> duplicates the channels *and D-Star demonstrably works on the air*, the radio **cannot** be
> interpreting the outbound pair as I/Q — doing so would transmit a mirrored/double-sideband mess.
> So the TX path takes a **real** passband signal (RADE's `rade_tx()` output → `real()`, exactly as
> V1 already does), duplicated into both slots, and the radio's DIGU stage does the upconversion.
> There is no conjugate convention to get wrong on transmit.
>
> The asymmetry with §10.3's inbound finding is *semantically justified*, not a contradiction:
> `rx_stream_in` is the demodulator's complex baseband, which genuinely needs I/Q; the outbound
> streams are audio.

**4. TX cadence is slaved to the inbound `tx_stream_in` stream.** The scheduler is purely
reactive — unlink an inbound buffer, classify it by stream id
(`digital_voice_mode_registry.c:240-253`), process, emit. There is no free-running transmit clock.

> **This is the single most important Tier 0 finding for RADE V2, because of the EOO.** When the
> radio stops delivering `tx_stream_in` at unkey, the pump stops — and any tail still queued is
> simply never sent. V2 must emit a **120 ms pilot-only EOO frame** *after* the last voice frame
> (§3.2, §9.2), so this would silently truncate it.
>
> D-Star already hit this and solved it: during its drain/ending phases it **synthesizes** buffer
> descriptors stamped with `tx_stream_in_id` so the normal TX path keeps running
> (`sched_waveform.c:1330-1338`). RADE V2 needs the same mechanism for the EOO tail. That file is
> AetherSDR's own, so this is a design pattern we may reuse — it just has to be reimplemented
> rather than linked.

**5. Which stream id to emit on — RESOLVED by Tier 1 measurement (§10.6): the INBOUND id.**
The `*_out_id` values are **not honoured at all**. An earlier draft of this item recorded the
question as "two sources disagree and both work"; the second half of that was wrong. See §10.6
item T1 for the measurement and for what it means for the Phase 0 1b claim it contradicts.

**6. Registration order for TX** (`digital_voice_mode_registry.c:196-220`): `waveform set <X> tx=1`
comes **first**, then the three `tx_filter` fields, with `udpport` **last**. Note D-Star registers
`tx_filter low_cut=0 high_cut=4800` — one-sided — while its `rx_filter` is symmetric `±3500`. So
the tx_filter sign convention does **not** simply mirror the rx_filter family clamping described in
§10, and our DIGU-family values are **unverified**; treat §10's `tx_filter [0, 2400]` as proposed,
not confirmed.

**7. There is a fault taxonomy to design against.** `TX_FAULT`, `TIMEOUT` and `STUCK_INPUT` all
drive the gate to `CANCEL` (`digital_voice_tx_gate.c:217-226`). The provider needs an explicit
cancel path, not just a happy path.

**What Tier 0 does *not* answer — carried to Tier 1 (no RF) and Tier 2 (dummy load):**

- Does `tx_stream_in` flow before PTT, or only while transmitting? (Decides whether we source mic
  audio from the radio or from AetherSDR's own `AudioEngine`, which §9 currently assumes.)
- **Amplitude/scaling convention.** D-Star carries an explicit tunable TX gain
  (`_dstar_scale_tx_sample`, `sched_waveform.c:551-553`), which implies full-scale is not obvious
  and had to be calibrated. → the Tier 2 ALC-linearity sweep.
- Does the radio apply speech processing (ALC/compression) on a **DIGU**-underlying waveform? OFDM
  would not survive it. → Tier 2.
- Which stream id to emit on (item 5). → Tier 1.
- `tx_filter` clamping for a USB-family underlying mode (item 6). → Tier 1.

### 10.6 Tier 1 measurements — 2026-07-28 (FLEX-8400, fw 4.2.20.41343, no RF)

The three §10.5 leftovers that need the radio but **not** a transmitter. Probe:
`tools/flex_waveform_tier1_probe.py`. Never keys; `waveform set tx=1` and `slice set <id> tx=1`
are capability/designation only, and both are restored on teardown.

**Positive control first.** With the slice in its ordinary `USB` mode the `remote_audio_rx`
monitor carried audio at RMS **2.7e-02**. Without this, a null result from T1 would say nothing
about stream ids — only that the monitor path was misconfigured, which is exactly how the first
two runs of this probe went wrong.

**Incidental — §9.1's premise confirmed by measurement.** With the slice in the waveform mode and
nothing injected, the monitor carried **exactly zero** (RMS 0.000e+00 over 376 packets). The
waveform slice really does contribute silence to the monitor mix, as the local-render baseline
assumes.

**T1 — the radio honours the INBOUND stream id; the `*_out` ids are ignored.**

| Pass | Emit on | Tone | Monitor RMS | Verdict |
|---|---|---|---|---|
| 1 | `rx_stream_out_id` `0x01000004` | 1200 Hz | **0.000e+00** | nothing |
| 2 | `rx_stream_in_id` `0x81000004` | 1700 Hz | 4.20e-02 | **HONOURED** (+197 dB over median) |
| 3 | `rx_stream_out_id` `0x01000004` | 900 Hz | **0.000e+00** | nothing |

Pass 3 repeats pass 1 *after* pass 2 has proven the path live, so the null cannot be blamed on
warm-up or on being the first injection after mode entry. **This matches the shipped D-Star
provider exactly** (§10.5 item 5: it emits on the inbound id and never reads the `*_out` ids).
Provider consequence: **emit on `rx_stream_in_id` / `tx_stream_in_id`.** The `*_out` ids returned
by `waveform create` appear to be informational — do not build against them.

**Replicated under `underlying_mode=USB`** (the mode 1b used) with identical numbers — inbound
4.19e-02, `*_out` 0.000e+00 on both passes. So the difference from 1b is **not** the underlying
mode. The only remaining setup difference is that 1b created its own pan+slice while this probe
uses the auto-restored slice, which is not a plausible routing determinant.

> **This puts the Phase 0 1b claim in doubt, and with it part of §6.3's justification.**
> §8 1b reported measuring the round-trip of a sweep *injected on `rx_stream_out_id`* — the id we
> now measure as never honoured. If it was never accepted, the audio 1b analysed cannot have been
> its own injected sweep, and the "flat to ~2.5 kHz, −6 dB @ ~2800 Hz, unmovable by any filter"
> result is measuring something unidentified. What 1b actually captured is **not established** —
> it was non-zero (an all-zero capture would have produced a flat 0 dB trace and no rolloff), so
> it was *something*, just not demonstrably the injected signal.
>
> **A concrete candidate for what 1b actually measured: the slice's ordinary SSB audio.** Every
> Tier 1 run reports slice 0 in `USB` as `filter=100/2800` — the stock SSB filter. 1b's result was
> "flat to ~2.5 kHz, **−6 dB @ ~2800 Hz**", which is that filter's response, and its "sweeping
> the filter did not move the rolloff" is what one would see if the `filt` commands were not
> reaching the slice being listened to. That is a **hypothesis, not a finding** — it is not proven
> that 1b's slice was outside the waveform mode — but the numeric coincidence is exact, and it
> explains both halves of 1b's result at once, including the invariance that made the cap look
> structural.
>
> **What this does and does not change.** It does **not** overturn local render: §9.1's
> independent argument stands (the monitor is radio-mixed, a client cannot un-mix it, so feeding
> decoded audio back doubles against our own render with no clean suppression), and that argument
> never depended on 1b. What it does undermine is the specific claim that the round-trip is
> **hard-capped at ~2.8 kHz**, which is what elevated local render from *preferred* to
> *required* in §6.3. That claim should be treated as **unsupported pending re-measurement**.
> **Re-measurement is now cheap and is still no-RF:** repeat the 1b sweep on `rx_stream_in_id`
> instead, using the positive control and concurrent capture from this probe.

**T2 — every `tx_filter` value is accepted, and this method cannot see whether any is honoured.**
`low_cut` = −3500, −1, 0, 300 and `high_cut` = 2400, 4800 all returned code 0, as did
`depth=256`. So `tx_filter` does **not** reject a negative `low_cut` under a `DIGU` underlying
mode — but per §10.2/§10.3 acceptance proves only that the command was taken.

No read-back was obtainable: the `transmit` status carries per-band `rfpower`/`tunepower`/
`inhibit` and no filter cuts, and no transmit-status line appeared at all on mode entry or on
TX-slice designation. **T2 is therefore only half-answered** — accepted, honouring unknown. The
`rx_filter` precedent (§10.3: honoured, but latched at registration) is suggestive and nothing
more. Settling it needs the transmit path actually running, i.e. **Tier 2**.

**T3 — `tx_stream_in` does NOT flow before PTT.** Zero packets on `0x81000005` across 6 s
unkeyed, while `rx_stream_in` `0x81000004` delivered 1125 packets over the same window — so the
capture path was demonstrably working. The radio does not stream microphone audio to the waveform
until it is transmitting.

Two consequences:

- **It compounds §10.5 item 4.** The transmit pump is both *started* and *stopped* by the radio:
  no packets before key, none after unkey. The EOO tail therefore cannot be flushed by simply
  continuing to respond — the synthesized-buffer mechanism is not an optimisation, it is the only
  way the tail gets out.
- **It leaves §9's mic-source assumption intact but unconfirmed.** §9 sources mic audio from
  AetherSDR's own `AudioEngine` rather than from `tx_stream_in`. Nothing here contradicts that,
  and a stream that is silent until PTT is awkward as a primary source. Whether `tx_stream_in`
  carries usable mic audio *while keyed* is a Tier 2 question.

### 10.7 The return path is NOT band-limited — 2026-07-28 (no RF)

The re-measurement §10.6 called for. Probe: `tools/flex_waveform_return_bandwidth_probe.py`.
A 100 Hz → 11 kHz log sweep injected on **`rx_stream_in_id`** (the honoured id), captured
concurrently from `remote_audio_rx` (uncompressed), across three registered `rx_filter` settings.

Returned response, dB relative to each trace's own 300–2000 Hz level:

| Hz | `[800, 2200]` | `[100, 2800]` | `[0, 8000]` |
|---|---|---|---|
| 1000 | −0.1 | +0.2 | −1.5 |
| 2000 | −4.8 | −3.2 | −2.2 |
| 2800 | −5.9 | −4.7 | −3.8 |
| 4000 | −5.4 | −8.0 | −7.7 |
| 6000 | −7.3 | −10.0 | −9.0 |
| 8000 | −10.1 | −8.3 | −9.5 |

**Two findings, both negative in the useful sense:**

1. **There is no band limit.** At 8 kHz the response is down only ~10 dB, with no knee anywhere.
   And that ~10 dB is *the sweep, not the channel*: a logarithmic sweep distributes energy ∝ 1/f,
   i.e. −3 dB/octave, and 500 Hz → 8 kHz is four octaves = **−12 dB expected on a perfectly flat
   path**. The measurement is flat to within the method's noise. A real brick wall — like 1b's
   claimed −60…−72 dB by 3–4 kHz — is nowhere in this data.
2. **The registered `rx_filter` does not shape the return at all.** The three curves are the same
   inside measurement scatter, even though the slice's filter fields demonstrably followed each
   registration (`800/2200`, `100/2800`, `0/8000` confirmed in slice status). So the filter
   applies to the modem audio the radio sends *to* the waveform, not to the audio the waveform
   sends *back*. This also retires a concern raised while designing the test: that a filter chosen
   for the modem's 1000–1937.5 Hz occupancy would mangle returned speech. It cannot.

> **Do not cite the probe's printed "−6 dB edges" (2754 / 2479 / 2443 Hz) as filter edges.** They
> are an artifact of a first-crossing heuristic run against a gently sloping, slightly rippled
> trace; the response continues at only −7…−10 dB out to 8 kHz. The heuristic was written
> expecting a brick wall and is not meaningful in its absence.

**Caveats.** Measured with a synthetic sweep on a LAN with `compression=none`. The path is flat and
linear, so real decoded speech will behave the same — but a *real* client on a WAN link gets
**Opus** on its monitor stream (`RadioModel::audioCompressionParam()`: opus on WAN/Auto-WAN, none
on LAN), which is a separate and genuine degradation that this probe deliberately excluded.
Latency was not measured and is now the open variable.

**Consequence: §6.3 is reopened.** See §9.1.

### 10.8 Return-path latency — 2026-07-28 (no RF)

The number §9.1 turns on. Probe: `tools/flex_waveform_return_latency_probe.py`. A steady 24 kHz
stream of silence on `rx_stream_in_id` with 40 ms tone bursts punched into it; each burst
timestamped as its packet leaves the socket and again when it reappears in `remote_audio_rx`.
The monitor is exactly silent in the waveform mode, so onset detection is unambiguous.

| burst | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| ms | 22.7 | 22.1 | 25.5 | 35.5 | 45.6 | 43.5 | 46.7 | 46.2 |

**Round trip ≈ 22 ms.** Take the *minimum*, not the median (39.5 ms): the upward drift across the
run is **our sender's clock**, not the radio's latency. The probe paces packets off a local
`perf_counter` at exactly 128/24000 s; any mismatch against the radio's 24 kHz accumulates in its
buffer, and ~24 ms of growth over 7.4 s is a ~0.3 % clock error. **A real provider clocks its
output off the received `rx_stream_in` packets — the radio's own clock — and does not drift.**
Quantisation is one packet, 5.33 ms.

**Test conditions — this is a REMOTE measurement, not a bench one.** The measuring station is not
on site with the radio: it reaches it over a **WireGuard VPN** (same ISP, short path, `TTL=63`
confirming a routed hop). The radio nevertheless classifies the link as **LAN**, so the monitor
stream is `compression=none` — **no Opus is involved.** That is worth stating plainly because it
corrects the earlier framing of this caveat: **Opus is a SmartLink concern, not a
remote-operation concern.** A VPN-connected remote operator gets uncompressed audio.

**Decomposition.** Network RTT to the radio, measured separately: **min 5 ms, avg 7 ms, max 10 ms**.

| component | ms |
|---|---|
| measured round trip (min) | ~22 |
| − network RTT over the VPN | ~6 |
| **= radio-side buffering / mixing / packetisation** | **~16** |

So the ~22 ms is **~16 ms of radio and ~6 ms of network**, which makes the result portable rather
than site-specific:

- **on-site LAN operator:** ~16 ms
- **this station (remote, VPN, same-ISP, uncompressed):** ~22 ms
- **SmartLink or a longer/worse path:** higher, and Opus enters

**This strengthens the §9.1 recommendation rather than weakening it.** 22 ms is not an idealised
bench floor that real deployments would fall short of — it is what a *typical, slightly
better-than-average remote VPN operator* actually experiences, with the on-site case better still.
The remaining unmeasured case is SmartLink.

**Method note — the probe drove the WRONG SLICE for two runs.** `slice_ids()` returns every
in-use slice on the radio, and AetherSDR was connected holding slice 0; our own slice was slice 1.
The probe set *AetherSDR's* slice to the waveform mode while measuring a monitor still carrying
our own untouched slice's band noise — which produced a full page of plausible, entirely bogus
numbers (including a −1500 ms "latency"). Probes now select via `Flex.my_slice_ids()`, matching
`client_handle` against the connection's own handle, and refuse to run if this connection owns no
slice. **Any probe in this series that picks a slice positionally is suspect whenever a second
client is connected.**

## 11. Fidelity & Licensing

**Fidelity.** The waveform *transport* is 24 kHz float — equal to internal RADEv1 (§8, 1a). The
only degradation is the radio's ~2.8 kHz monitor round-trip (§8, 1b), which we avoid by rendering
locally. Net: RADEv2-in-process delivers **full-bandwidth** decoded speech, matching V1 and beating
the off-box container (which is stuck routing decoded audio back through the capped monitor).

**Licensing.** RADE codec is BSD (`rade_api.h`). The Flex waveform provider protocol is a public
interface; we implement it **clean-room** against the public `waveform-sdk` and AetherSDR's own
VITA-49 stack, so no GPL-3.0 `smartsdr-dsp` code is linked into the AetherSDR binary. The V2 vendor
bump updates `THIRD_PARTY_LICENSES` with full provenance.

## 12. Multi-backend forward-compatibility

The codec/transport seam (§6.7) keeps `RADEV2Engine` ignorant of Flex. Today there is exactly one
transport (`FlexWaveformTransport`) and **no** other backend can carry voice (HL2 and future
backends are RX-only; the `IRadioBackend` audio surface has no per-slice tap yet, and adding one is
maintainer-gated). So we do **not** build a speculative multi-backend framework now — we keep the
seam clean so a second transport is additive when a backend actually gains voice TX + a per-slice
audio port.

## 13. Testing

- Reuse the RADE stored-file / `RADE_WAV_TAP` methodology and Dtmax metrics for decode validation.
- **`RAD2` registration checks (Phase 2, no codec required):** DIGU acceptance (§10.2),
  `rx_filter` shape (§10.3) and `digu_offset` semantics (§10.3 item 7) are all done. Nothing
  registration-side remains unmeasured.
- **Filter-preset guard (Phase 3, from §10.3 item 7 + §10.4 defect 3):** assert that a
  width-specified filter preset on a `RAD2` slice produces a passband **centred on 1500 Hz**
  (e.g. 1500 → 750–2250), not the `lo = 95; hi = width` fallthrough. Pin it with the modem's
  1000–1937.5 Hz occupancy as the assertion, so the test states the *reason* rather than the
  numbers. This is the highest-value test in §13: it is the one defect that silently stops
  decode in response to an ordinary operator action.
- **Regression guard for the two accepted-but-not-honoured traps (§10.2, §10.3):** assert that
  the provider sets `rx_filter` *before* mode entry, and that the imaginary component of
  `rx_stream_in` is negated before reaching the codec. Both are silent-wrong-answer failures,
  not crashes.
- Automation-bridge assertions for the `RAD2` mode lifecycle (activate/deactivate, no mute strand,
  status sub-state).
- **Mode-family classification guard (Phase 3, from §10.4):** assert that `RAD2` lands in the
  squelch-disabled family (both sites), is excluded from `isVoice` so auto-notch stays hidden, and
  appears in the DIGU-offset family. All three are *silent* misbehaviours — squelch gating decode
  and a notch eating OFDM carriers look like codec failures, not GUI bugs, which is exactly why
  they are worth a regression test rather than a code review.
- OTA A/B on the FLEX-8400: local-render fidelity vs. the container; PTT release timing; the inline
  callsign channel.
- **EOO tail guard (from §10.5 item 4):** assert the full 120 ms pilot-only EOO actually reaches
  `tx_stream_out` after the last voice frame, by counting emitted samples — not by asserting PTT
  timing. The failure mode is a *truncated* tail when the radio stops driving the pump, which a
  PTT-duration assertion would pass straight through while the far end loses end-of-over detection.
- **TX validation tiers (§16 Q4):** Tier 1 is no-RF (which stream id the radio honours on emit;
  `tx_filter` clamping under a USB-family underlying mode; whether `tx_stream_in` flows before
  PTT). Tier 2 keys into a dummy load at minimum power and reads the radio's own telemetry —
  `FWDPWR`/`REFPWR`/`SWR` and the post-ALC dBFS meter (`MeterModel.h:117,140-148`) — sweeping
  injected amplitude to test **power-transfer linearity**, since any ALC/compression in the path
  would wreck OFDM. Tier 2 keys the transmitter and is operator-supervised, never agent-initiated
  (the automation bridge already refuses TX-keying actions, `AutomationServer.cpp:5488`).
- **`rx_stream_out` feed A/B (§9.1):** with the dev toggle on, capture the `remote_audio_rx`
  round-trip of *real decoded RADE speech* and compare — spectrally and perceptually — against the
  local render; quantify the ~2.8 kHz truncation's audibility on voice. Also characterize the
  multiflex double-back (what a 2nd client hears; whether any workaround exists) to inform the
  local-only / remote-only / toggle decision.

## 14. Rollout / phasing

Per the implementation plan (fork development ungated; RFC gates only the upstream merge):
Phase 0 spike (done) → **Phase 1 this RFC (draft → harden → post when bulletproof)** → Phase 2
clean-room provider in `backends/flex` (gated, provable with a loopback/V1 codec) → Phase 3 engine +
seam + `RAD2` registry mode → Phase 4 vendor V2 + thin codec glue (vendor **last**) → Phase 5
convergence + flip `ENABLE_RADE_V2` at upstream V2 release. V1 deletion is a separate later PR.

## 15. Alternatives considered

- **Mutate RADE V1 / share an interface with it** — rejected: V1 is leaving; coupling to it just has
  to be unwound at deletion.
- **External daemon** (like D-Star) — rejected for RADE: no external-hardware/licensing reason, and it
  sits outside the backend abstraction (worse for multi-backend). D-Star is a daemon *because* of the
  ThumbDV/AMBE hardware; RADE has none.
- **DAX tap (V1's approach)** — rejected: requires the slice mute (radio can't un-mix the monitor
  client-side) and the side-channel; the waveform avoids both.
- **Full-loop render** (decode → `rx_stream_out` → monitor → client) — rejected as the *primary* audio
  path: ~2.8 kHz cap (§8, 1b), and it double-backs onto our own local render with no clean mute
  (§9.1). Kept only as a *deferred, toggle-gated* other-client feed pending the A/B investigation.
- **On-radio Docker waveform** — out of scope: 8000-class only; FreeDV's to ship.

## 16. Open questions

1. **RX audio output — REOPENED, and the premise it rested on was WRONG (see §9.1, §10.7).**
   The ~2.8 kHz cap does not exist; the return path is flat to at least 8 kHz and is not shaped by
   the registered `rx_filter`. So this is no longer "local render, with an optional degraded feed
   for other clients" — it is a straight choice between **A: local render only** and **B: return
   path only**, at equal fidelity. A+B together remains rejected (double-back, now measured).
   **Latency is now measured: ~22 ms remote over a WireGuard VPN, ~16 ms of which is the radio
   itself, so ~16 ms on-site (§10.8)** — negligible beside RADE's own framing and codec delay, and
   measured under realistic remote conditions rather than on a bench. With fidelity equal and latency small, **the recommendation is B** (§9.1), on the
   §2 grounds that option A *is* the "parallel audio side-channel" this design set out to remove.
   **Still open before it is settled:** SmartLink behaviour under Opus (a VPN link is classified
   LAN and stays uncompressed), and whether any use case needs RADE audio while the radio's
   monitor path is unavailable. Operator's call.
   *Superseded sub-question, retained for history:*
   Current expectation: **local-render-only** (baseline). Feeding real audio for other clients
   double-backs onto us (radio-mixed monitor can't be un-mixed) and the only fix (global
   `audio_mute`) both strands *and* defeats the feed — so it's not a clean opt-in. **Plan:** build
   it behind a dev toggle and A/B compare (spectral + perceptual, on real RADE speech) local vs.
   round-trip, and fully investigate the multiflex case, before choosing local-only / remote-only /
   toggle. Remaining sub-item: does the waveform framework require a steady `rx_stream_out`
   (cadence/health) — if so the baseline feeds *silence* rather than nothing (Phase 2 check).
   **Updated by §10.6:** the feed, if built, must go out on **`rx_stream_in_id`** — the `*_out`
   ids are not honoured. And the A/B this question was waiting on is **cheaper than thought and
   needs no codec**: the ~2.8 kHz cap is now disputed (§10.6 T1), so re-running the 1b sweep on
   the honoured id is the immediate next measurement, not a Phase 4/5 one.
2. **Callsign/text transport — CLARIFIED (see §9.2): not an either/or.** OTA callsign uses RADE's
   `data_symbol` channel (the only OTA path; ~25 bit/s raw BPSK, app-framed, repeated inline). The
   Flex `byte_stream` is a separate *local* channel, deferred (optional future SmartSDR/other-client
   text surfacing). **Remaining open:** the app-level framing (serialize/sync/CRC/FEC) must match the
   RADE V2 ecosystem convention for interop — adopt upstream's at vendor time (Phase 4); it may not
   be finalized yet, so our framing is provisional until then. **Also open:** whether V2's 120 ms
   pilot-only EOO frame is reachable through the public API (`rade_tx_eoo()` is marked "V1 only"
   while `rade_tx_v2_eoo()` exists internally) — settle at vendor time (§9.2).
3. **`RAD2` `underlying_mode` + `rx_filter` — CLOSED by derivation (see §10.1).**
   `underlying_mode=DIGU` (data variant, per D-Star's `DFM`-not-`FM` pattern; carries the
   1500 Hz dial convention), **upper sideband on every band, no `DIGL`**; `rx_filter
   low_cut=800 high_cut=2200`. The premise of the original question was wrong: tightening the
   filter does **not** improve modem SNR — the modem band-passes itself at 975 Hz and a narrow
   upstream filter biases the *reported* SNR high. **Remaining open:** (a) the DIGU-on-all-bands
   convention is our belief, awaiting an authoritative RADE V2 citation; ~~(b) `digu_offset`
   semantics~~ **CLOSED** — `DIGU` accepted (§10.2), `rx_filter [800, 2200]` measured good on
   hardware (§10.3), and `digu_offset` resolved as the width-centring frequency rather than a
   shift of anything (§10.3 item 7); (c) Phase 4/5 optimises the width against real decode
   performance.
4. **TX path — largely CLOSED on paper by §10.5; RF validation still outstanding.** The wire
   contract (framing, packet counting, stereo-not-I/Q outbound, radio-driven cadence, registration
   order, fault taxonomy) is desk-derived from the vendored provider and needs no transmitter. Two
   consequences worth carrying: the outbound pair is **audio, so there is no TX conjugate/sideband
   convention to get wrong**, and the TX pump is **slaved to inbound `tx_stream_in`**, so V2's
   120 ms EOO tail requires synthesized buffers or it is silently truncated. **Tier 1 is now DONE
   (§10.6):** emit on the **inbound** stream id (the `*_out` ids are ignored); `tx_stream_in` does
   **not** flow before PTT, so the pump is both started and stopped by the radio and the
   synthesized-buffer tail flush is mandatory; `tx_filter` accepts everything including a negative
   `low_cut`, with honouring unverifiable without transmitting. **Remaining — all Tier 2, dummy
   load, operator-supervised:** the amplitude/ALC-linearity sweep (does the radio apply speech
   processing OFDM would not survive), whether `tx_filter` is honoured, and whether `tx_stream_in`
   carries usable mic audio while keyed.
5. Per-slice audio port on `IRadioBackend` for future non-Flex hosting — tracked, maintainer-gated.

## 17. References

- `docs/architecture/digital-voice-thumbdv-waveform.md` — D-Star waveform (closest pattern).
- `docs/architecture/aetherd-iradiobackend-design.md` — the backend seam.
- `drowe67/radae_nopy` (`dr-radev2`) — the V2 C port and `rade_api.h`.
- FlexRadio public `waveform-sdk` — the waveform protocol authority.
- Phase 0 spike results (internal memory: `radev2-phase0-findings`).
