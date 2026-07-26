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
3. **Local render for OUTPUT; waveform only for INPUT + registration.** The waveform's *return*
   path (`rx_stream_out` → radio monitor → client) is **hard-capped at ~2.8 kHz** (§8, 1b), so
   routing decoded speech back through it truncates RADE's wideband output — this is exactly the
   Flex-FreeDV *container's* quality loss. We instead render the decoded speech locally at full
   FARGAN bandwidth. Local render is therefore **required for fidelity parity, not optional.**
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

The spike drove the risky unknowns to ground before committing to this design. Probes:
`tools/` (dev-only; the throwaway `flex_probe*.py` scripts are not shipped).

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

### 9.1 RX audio output — current expectation: local render only

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

**7. Still open — `digu_offset` semantics.** The field is present and reads 1500 with the slice
in the waveform mode, but this probe did **not** establish whether it shifts the demodulated
passband or only the dial display. That needs a tonal reference (e.g. a known carrier) rather
than the noise-based method used here, and remains a Phase 2 item.

**Incidental, useful for the provider and for probing:**

- The radio **auto-restores a pan + slice** for a GUI client on connect. The 8400 is 2/2
  pans/slices, so `slice create` then fails `0x50000003`. This — not a secondary-client
  restriction — is the precise explanation for the contention Phase 0 hit. Use the restored
  slice.
- On `waveform remove`, client disconnect, **or an abrupt probe kill**, the slice reverted
  cleanly to its previous mode every time. Nothing stranded — a favourable contrast to V1's
  hand-unwound mute.

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
- **`RAD2` registration checks (Phase 2, no codec required):** DIGU acceptance (§10.2) and
  `rx_filter` shape (§10.3) are done. Remaining: `digu_offset` passband-vs-display semantics,
  which needs a tonal reference (§10.3 item 7).
- **Regression guard for the two accepted-but-not-honoured traps (§10.2, §10.3):** assert that
  the provider sets `rx_filter` *before* mode entry, and that the imaginary component of
  `rx_stream_in` is negated before reaching the codec. Both are silent-wrong-answer failures,
  not crashes.
- Automation-bridge assertions for the `RAD2` mode lifecycle (activate/deactivate, no mute strand,
  status sub-state).
- OTA A/B on the FLEX-8400: local-render fidelity vs. the container; PTT release timing; the inline
  callsign channel.
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

1. **`rx_stream_out` other-client feed — DIRECTION SET, final decision deferred (see §9.1).**
   Current expectation: **local-render-only** (baseline). Feeding real audio for other clients
   double-backs onto us (radio-mixed monitor can't be un-mixed) and the only fix (global
   `audio_mute`) both strands *and* defeats the feed — so it's not a clean opt-in. **Plan:** build
   it behind a dev toggle and A/B compare (spectral + perceptual, on real RADE speech) local vs.
   round-trip, and fully investigate the multiflex case, before choosing local-only / remote-only /
   toggle. Remaining sub-item: does the waveform framework require a steady `rx_stream_out`
   (cadence/health) — if so the baseline feeds *silence* rather than nothing (Phase 2 check).
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
   convention is our belief, awaiting an authoritative RADE V2 citation; (b) `DIGU` accepted
   (§10.2) and `rx_filter [800, 2200]` measured good on hardware (§10.3) — only `digu_offset`
   passband-vs-display semantics remain; (c) Phase 4/5 optimises the width against real decode
   performance.
4. TX path validation into a dummy load (Phase 0 was RX-only).
5. Per-slice audio port on `IRadioBackend` for future non-Flex hosting — tracked, maintainer-gated.

## 17. References

- `docs/architecture/digital-voice-thumbdv-waveform.md` — D-Star waveform (closest pattern).
- `docs/architecture/aetherd-iradiobackend-design.md` — the backend seam.
- `drowe67/radae_nopy` (`dr-radev2`) — the V2 C port and `rade_api.h`.
- FlexRadio public `waveform-sdk` — the waveform protocol authority.
- Phase 0 spike results (internal memory: `radev2-phase0-findings`).
