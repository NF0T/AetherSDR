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
V2 drops the pilot symbols → a narrower modem waveform, ~3 dB more sensitive at low SNR, with
different sync/framing. The end-of-over/text channel also changes fundamentally: V1 sent the
callsign as a single LDPC burst at end-of-over (`rade_tx_eoo`); **V2 streams it inline** as one
BPSK data symbol per modem frame (`rade_tx_set_data_symbol` / `rade_rx_get_data_symbol`) — no
drain-and-send-EOO step.

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

## 7. Component design & placement

All new code is gated behind a compile flag **`ENABLE_RADE_V2`** (off by default) until release.

| Component | Location | Notes |
|---|---|---|
| `FlexWaveformProvider` | `src/core/backends/flex/` | Flex wire protocol → behind `FlexBackend`; keeps EB3 clean and puts the Flex-specific transport where a future backend's transport would sit |
| `RADEV2Engine` | `src/core/` (`libaethercore`) | Worker-thread QObject; codec core with the transport-agnostic seam |
| `RAD2` mode | `DigitalVoiceModeRegistry` | New `DigitalVoiceModeId::RadeV2` + descriptor (`radioMode "RAD2"`, `underlyingMode "USB"`) |
| GUI wiring | `src/gui/` | New parallel `activateRADEV2()`; V1's `activateRADE()` untouched |
| Vendored codec | `third_party/radae` | Re-vendored from `radae_nopy@dr-radev2` (Phase 4; vendor **last**) |

## 8. Phase 0 evidence (hardened by coding, live FLEX-8400 fw 4.2.20.41343)

The spike drove the risky unknowns to ground before committing to this design. Probes:
`tools/` (dev-only; the throwaway `flex_probe*.py` scripts are not shipped).

- **OQ#1 — GO.** A GUI-client connection (`client gui` → `client station`) successfully ran
  `waveform create name=… mode=RAD2 underlying_mode=USB version=…` on its **own** TCP-4992
  connection (reply code 0). So AetherSDR can host the waveform **in-process, single connection**
  — no dedicated socket, no daemon. Registration adds `RAD2` to the slice `mode_list`.
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
callsign bit)`, then `rade_tx`) → modem PCM → `tx_stream_out` → radio transmitter. No burst-EOO
drain step (callsign streamed inline), so PTT release is near-immediate — simpler than V1's
three-layer EOO PTT orchestration.

**Optional other-client audio:** if desired, also feed `rx_stream_out` so other SmartSDR clients
hear the slice — those clients get the ~2.8 kHz-limited version (unavoidable; the local operator
still gets full fidelity from the local render). Default: TBD (see §16).

## 10. The waveform provider protocol (clean-room, from public spec + Phase 0 observation)

Registration on the existing TCP-4992 command channel (after the normal GUI bind + subs):
```
waveform create name=<X> mode=RAD2 underlying_mode=USB version=<v>
        → tx/rx_stream_in, tx/rx_stream_out, byte_stream_in/out  (six stream ids)
waveform set <X> tx=1
waveform set <X> rx_filter low_cut=… high_cut=…   (modem passband; RADE fits an SSB channel)
waveform set <X> udpport=<P>                       (our VITA-49 listener)
```
Data plane: VITA-49 IF-Data over UDP; audio is 24 kHz Complex-float32, big-endian; outbound goes
to `radio:4991`, inbound to our registered `udpport`. Details and exact framing captured in Phase 0.

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
- Automation-bridge assertions for the `RAD2` mode lifecycle (activate/deactivate, no mute strand,
  status sub-state).
- OTA A/B on the FLEX-8400: local-render fidelity vs. the container; PTT release timing; the inline
  callsign channel.

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
  path: ~2.8 kHz cap (§8, 1b). Retained only as the *optional* other-client feed.
- **On-radio Docker waveform** — out of scope: 8000-class only; FreeDV's to ship.

## 16. Open questions

1. Whether to feed `rx_stream_out` by default (other-client audio at ~2.8 kHz) or leave it off
   (local-operator-only, full fidelity). Likely a setting.
2. Inline callsign transport: the per-frame `rade_rx/tx_data_symbol` API vs. the discovered
   `byte_stream_in/out` pair — which is cleaner/robust. Investigate in Phase 2/4.
3. Exact `RAD2` `underlying_mode` and `rx_filter` width for best modem SNR (the modem is narrow;
   confirm the passband that maximizes decode without over-widening).
4. TX path validation into a dummy load (Phase 0 was RX-only).
5. Per-slice audio port on `IRadioBackend` for future non-Flex hosting — tracked, maintainer-gated.

## 17. References

- `docs/architecture/digital-voice-thumbdv-waveform.md` — D-Star waveform (closest pattern).
- `docs/architecture/aetherd-iradiobackend-design.md` — the backend seam.
- `drowe67/radae_nopy` (`dr-radev2`) — the V2 C port and `rade_api.h`.
- FlexRadio public `waveform-sdk` — the waveform protocol authority.
- Phase 0 spike results (internal memory: `radev2-phase0-findings`).
