# RFC: RADE V2 as an In-Process Flex Waveform

> **Status: DRAFT — internal, not yet posted.** This document is being iterated
> privately and hardened by implementation before any wide distribution or upstream
> PR (per our bulletproof-before-distribution policy). No tracking issue exists yet.
> Nothing here authorizes merging to `aethersdr/AetherSDR`; the RFC is the gate on
> the *upstream PR*, not on fork development.
>
> **Sequencing (operator, 2026-07-28):** write and test the code on the fork **first**, then post
> the RFC, then open the upstream PR — in that order. Neither the posting nor the PR is expected
> before the FreeDV team finalizes and publishes the RADE V2 C API and begins their own
> development builds that make real on-air interoperability testing possible. There is no clock on
> any of it.
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
- **FreeDV Reporter integration — deliberately deferred, not forgotten (§9.3).** V1 reports to the
  public map; V2 must not until its callsign framing and mode string are settled with the
  ecosystem, because the failure mode is publishing wrong data to a network other operators
  trust.

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
3. **RX audio output — DECIDED: the waveform RETURN PATH, not local render.** Decoded speech is
   handed back to the radio and reaches the operator through the slice's normal audio, exactly as
   every other mode does.
   This decision was reversed by measurement. It previously read "local render is *required* for
   fidelity parity" on the strength of a believed ~2.8 kHz cap on the return path (§8, 1b). **That
   measurement was invalid and the cap does not exist** — §10.7 finds the return path flat to at
   least 8 kHz and unaffected by the registered `rx_filter`, and §10.8 measures the added latency
   at ~22 ms remote over a VPN, ~16 ms of which is the radio itself.
   With fidelity equal and latency negligible against RADE's own framing delay, the deciding
   argument is §2's own goal — **"no parallel audio side-channel"**. Local render *is* one: it
   rebuilds the V1 pathology this design exists to remove, with its own buffer, its own mixing and
   a slice whose audio controls do nothing. The return path gives multiflex reach for free and
   makes slice volume and mute simply work.
   Doing **both** remains rejected (they double against each other with no clean suppression —
   §9.1). **Revisit trigger:** if on-air testing with the real codec shows audible quality
   problems, reopen — the operator's explicit condition on this decision.
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
| `FlexWaveformProvider` | `src/core/backends/flex/` | Control plane: `waveform create/set/remove` on TCP 4992. Flex wire protocol, so EB3 permits it only here — and it is where a future backend's transport would sit. **Beside `FlexBackend`, not behind it** (§7.2) |
| `FlexWaveformStream` | `src/core/backends/flex/` | Data plane: the UDP socket, VITA-49 framing, and the X2 conjugation. Split from the provider because the port must be bound **before** registration (R1 sends `udpport` last) |
| `FlexWaveformTransport` | `src/core/backends/flex/` | Owns the two above and implements the codec↔transport seam (§6 decision 7). The only object `RADEV2Engine` sees. Seam is **signals, not an interface pointer** — the codec is a worker-thread QObject, and queued connections make the threading a compile-time non-issue instead of a runtime race |
| `RADEV2Engine` | `src/core/` (`libaethercore`) | Worker-thread QObject; codec core with the transport-agnostic seam |
| `RAD2` mode | `DigitalVoiceModeRegistry` | New `DigitalVoiceModeId::RadeV2` + descriptor (`radioMode "RAD2"`, `underlyingMode "DIGU"` — see §10.1; no `DIGL` variant) |
| GUI wiring | `src/gui/` | New parallel `activateRADEV2()`; V1's `activateRADE()` untouched |
| Vendored codec | `third_party/radae` | Re-vendored from `radae_nopy@dr-radev2` (Phase 4; vendor **last**) |

## 7.1 Implementation constraints — the measured must-honours

Every constraint below was **measured on hardware**, not inferred. They are consolidated here
because §10 discovered them across eleven separate probe sessions and an implementer should not
have to reconstruct the list by reading all of it.

**🔇 marks a SILENT failure** — no error, no non-zero reply, no crash. The command is accepted, or
the code compiles and runs, and the result is simply wrong. **23 of the 42 rows are silent**, and
several would present during Phase 4 as *"the codec doesn't work"*, which is the most expensive
possible time to find them. **Every 🔇 row wants a regression test**, because code review cannot
catch them: a reviewer reading `imag = b` has no way to know it should be `-b`.

### Registration / control plane

| # | Constraint | If violated | Evidence |
|---|---|---|---|
| R1 | Order: `waveform create` → `set tx=1` → filters → **`udpport` last** | — | §10.5 |
| R2 | Each filter field is its **own** command; they do not combine on one line | rejected | §10 |
| R3 | 🔇 `rx_filter` is **latched at registration / mode entry**. Set it *before* the slice enters the mode; to change it, **re-register** | live `set` returns **code 0 and does nothing** | §10.3 |
| R4 | 🔇 `tx_filter` is **accepted and entirely ignored** — do not rely on it, do not expose it as an operator control | silently no-ops; two registrations differing by 1400 Hz gave identical curves | §10.10 |
| R5 | 🔇 Filter cuts for the **USB/DIGU family are one-sided** (`if (low<0) low=0`). D-Star's negative `low_cut` is an FM-family idiom | copying it clamps to 0 and **silently widens** the filter | §10 |
| R6 | 🔇 Stream IDs are **recycled** across create/remove — never use one as session identity | stale-ID bugs after a re-register | §10.2 |
| R7 | `waveform create` returns **six** streams (incl. the `byte_stream` pair). The vendored D-Star parser reads only four | miss the data channel | §8 |
| R8 | Registration needs **no slice and no panadapter** | — | §10.2 |
| R9 | 🔇 **Register on CONNECT, never on mode-select.** The radio advertises `RAD2` in a slice's `mode_list` only once the waveform is registered | registering from the mode-change path is a **deadlock** — the mode never appears, so it can never be picked, so registration never runs. Nothing logs, nothing fails | §10.12 |

### RX data plane

| # | Constraint | If violated | Evidence |
|---|---|---|---|
| X1 | `rx_stream_in` is **24 kHz Complex-float32, big-endian, 128 samples/packet** | — | §8, §10.3 |
| X2 | 🔇 **Inbound is CONJUGATE I/Q — negate the imaginary component** (`A − jB`) | audio never decodes — but the receiver **still acquires and reports SYNC**; only the SNR estimate tells you (§7.1b) | §10.3, §7.1b |
| X3 | Discard the ~1 s startup burst before measuring rate (it reads ~28 kHz) | wrong rate | §10.3 |
| X4 | **No muting.** `audio_mute=0` with the slice in the waveform mode | — | §10.3 |
| X5 | A waveform-mode slice contributes **exactly zero** to the monitor until we feed it | — | §10.6 |

### Emit — applies to BOTH directions

| # | Constraint | If violated | Evidence |
|---|---|---|---|
| E1 | 🔇 **Emit on the INBOUND id** (`rx_stream_in_id` / `tx_stream_in_id`). The `*_out` ids are **not honoured** — measured at exactly zero, twice, including a retry after the path was proven live | **silence, no error anywhere** | §10.6, §10.9 |
| E2 | 🔇 **Outbound is STEREO AUDIO, not I/Q** — duplicate mono into both float slots and do **not** conjugate | conjugating inverts the sideband: normal on every meter, decodes for nobody | §10.5, §10.11 |
| E3 | 🔇 Packet count is a **per-STREAM** 4-bit rolling counter starting at 0 | a global counter desyncs the moment two streams run | §10.5 |
| E4 | Header `IF_DATA_WITH_STREAM_ID \| CLASS_ID \| TSI_UTC \| TSF_REAL_TIME \| (count<<16) \| (7+n*2)`; `class_id_h=0x001C2D`, `class_id_l=0x534C03E3`; UTC sec + fractional **picoseconds**; → **radio:4991** | rejected/ignored | §10.5 |

### TX

| # | Constraint | If violated | Evidence |
|---|---|---|---|
| T1 | 🔇 The TX pump is **slaved to inbound `tx_stream_in`** — there is no free-running clock | emit stops when the radio stops | §10.5 |
| T2 | `tx_stream_in` does **not** flow before PTT — the pump is started *and* stopped by the radio | — | §10.6 |
| T3 | 🔇 **T1+T2 ⇒ the 120 ms EOO tail needs SYNTHESIZED buffers.** Holding PTT alone emits nothing | far end loses end-of-over detection; **a PTT-duration test passes straight through this** | §10.5, §9.2 |
| T4 | The radio **holds TX through an underrun** (power sags, carrier stays) | — | §10.9 |
| T5 | TX path is **linear, no ALC, no compression**; linear above drive ≈0.35 | — | §10.9, §10.10 |
| T6 | 🔇 **Band-limiting must happen in our own TX chain.** The V2 modulator does no shaping at all and `tx_filter` is ignored | unsuppressed sinc sidelobes | §10.1, §10.10 |
| T7 | **Upper sideband is correct** — `DIGU` puts our audio above the dial | — | §10.11 |
| T8 | `TX_FAULT` / `TIMEOUT` / `STUCK_INPUT` all → `CANCEL`; needs an explicit cancel path | no recovery | §10.5 |
| T9 | 🔇 **The 8→24 kHz TX resampler's FIR memory holds the tail of the EOO.** Use `ReqTransBand=45` (**8.6 ms** measured) — **not** the default (**297 ms** measured) — *and* `flush()` the resampler at end of over, *and* keep the §7.1 T3 drain running at least `latencySamples()` longer | far end never sees end-of-over; **nothing errors** | §7.1a below, `resampler_flush_test` |

### Mode identity / GUI (Phase 3)

| # | Constraint | If violated | Evidence |
|---|---|---|---|
| G1 | 🔇 Add `RAD2` to the squelch-disabled family — **two independent sites** | squelch auto-arms and **gates decode** | §10.4 |
| G2 | Exclude `RAD2` from `isVoice` so auto-notch stays hidden | adaptive notch **chews OFDM carriers** | §10.4 |
| G3 | 🔇 Add `RAD2` to the DIGU-offset family | a width preset falls through to `lo=95; hi=width`, **clipping the top half of the modem** | §10.3, §10.4 |
| G4 | `digu_offset` is the **centre a width-specified filter is built around** — not a passband shift, and measured not to shift RF placement either | — | §10.3, §10.11 |
| G5 | 🔇 **`DigitalVoiceModeRegistry::activateMode()` must be called when the waveform registers.** Registering with the radio is only half of it — the registry separately gates slice claims on the mode being *running* | `SliceModel::setMode()` **returns early**: the combo shows `RAD2` while the slice stays in USB, with USB audio and no waveform on key-up. One warning line is the only evidence | §10.12 |

### Persistence / settings store (RFC #4603)

Not measured on hardware like the rows above — these are **derived from the shipped settings-store
design** (upstream `e1d19ea9`, 2026-07-31) and from the G5 failure that was measured. They are
here because all three produce the G5 dead state or its shape, and none of them announces itself.

| # | Constraint | If violated | Evidence |
|---|---|---|---|
| S1 | 🔇 **Restored operating state must never carry `RAD2` as a mode.** `RestoredRadioState::mode` is gated by the `Tuning` domain and is handed to the backend *before* `connectRadio()` — necessarily before any waveform is registered (R9) | lands in the **G5 dead state**: combo reads `RAD2`, slice stays put. Restore the underlying mode (`DIGU`) and let mode-select re-derive `RAD2`, or defer the mode half until registration completes | §9.5, G5 |
| S2 | 🔇 **A recalled memory carrying `RAD2` must be re-derived through the registry**, not written at the slice. The host-side bank stores a mode string, at `radio_settings (local, '', MemoryBank)` since #4603 PR 6 | same G5 shape by a second route — recall before registration and the slice never moves | §9.5, G5 |
| S3 | 🔇 Any future RAD2 persistence is a **versioned `radio_settings` document with a checked `setFeature()` result** — never a flat `AppSettings` key | a refused write (read-only store, reset in progress) that the UI repaints over: the state looks saved and is not | §9.4 |

### Inline data channel (§9.2)

Read from the vendored C rather than measured on air, and re-verified independently before being
written down. They are here because two of them produce a channel that looks alive from every angle
and carries nothing.

| # | Constraint | If violated | Evidence |
|---|---|---|---|
| D1 | 🔇 **The TX data symbol is STICKY, not a queue.** `tx->data_symbol` is one float, initialised to `-1.0f`, re-read on every step until changed | never calling the setter transmits a continuous run of `-1`. **There is no queue to starve and no underrun to report**, so nothing anywhere indicates the channel is empty | `rade_tx_v2.c:48,79` |
| D2 | 🔇 Read `rade_rx_get_data_symbol()` **only when `rade_rx()` returned > 0**, exactly once per successful step | sampling it unconditionally feeds the framer stale repeats and desynchronises our bit stream from the sender's — which presents as "the framing is wrong" | `rade_api.h:173` |
| D3 | The channel is **not a separate carrier**: the symbol rides as the **21st input feature** through the autoencoder's learned latent, and RX recovers it from the *decoder's* output as a **soft** value | — | `rade_tx_v2.c:79`, `rade_rx_v2.c:297` |
| D4 | Exactly **25 bit/s** — one symbol per step, `RADE_V2_FRAMES_PER_STEP` (4) × 10 ms | — | `rade_v2_constants.h:10` |

> **D3 is the one with design consequences.** Because the data rides inside the same learned
> representation as the voice, **the data channel's robustness is coupled to the voice channel's** —
> a callsign is unlikely to survive conditions where the audio is already breaking up. That argues
> *against* elaborate FEC layered on top and *for* repetition plus a cheap integrity check, which is
> what §9.2's interim framing does. It also means the soft decision comes free, and it is worth
> using: magnitude is a per-bit confidence measure for correlation and for soft combining.
>
> Bench measurement (`rade_v2_engine_test`): the callsign survives the autoencoder at confidence
> 1.0 on a clean signal, with **3 of ~4 available copies validating**, so the channel is not
> lossless even at 20 dB SNR on a noise-free bench.
>
> **⚠ ON AIR IT IS FAR WORSE — measured 2026-08-02, and this invalidates a planning assumption.**
> See §10.14. The raw symbol error rate on a real HF path is **~15–16 %**, at which a 60-bit
> uncoded frame passes CRC roughly **3 × 10⁻⁵** of the time. **The §9.2 framing as built will
> essentially never decode over a path like that.** The "repetition plus a cheap integrity check"
> argument was reasoning from the coupling in D3, not from a measurement; the measurement now
> exists and disagrees.

### Environment

| # | Constraint | Evidence |
|---|---|---|
| N1 | Teardown is clean on `waveform remove`, disconnect, **or abrupt kill** — the slice reverts, nothing stranded | §10.3 |
| N2 | 🔇 Select slices by **`client_handle`**, never positionally — another client's slice can sort first | §10.8 |
| N3 | 🔇 `slice tune` outside the panadapter's span returns **code 0 and does nothing**; move the pan first | §10.9 |

> **Pattern worth naming.** R3, R4, N3 and E1 are the same shape: **the Flex API returns code 0 for
> commands it does not honour.** A reply code confirms the command was *accepted*, never that it
> was *applied*. Any provider code that sets something and assumes it took should read it back —
> and where read-back is impossible (as with `tx_filter`), the setting must not be exposed as
> though it works.

## 7.1a T9 in full — the resampler eats the EOO, and T3 does not save it

Raised by the operator during stage 6b, from V1 experience. It is a genuine gap
in the stage 5 design, so it is written out rather than left as a table row.

**What V1 found.** The 8→24 kHz TX upsampler was deliberately loosened:

```cpp
// RADEEngine.cpp:113-118
m_up8to24 = std::make_unique<Resampler>(8000, 24000, 4096, /*ReqTransBand=*/45.0);
// 45 (max, ~42 taps, ~5 ms FIR) instead of default 2.0 (~950 taps, ~118 ms).
```

Prong A measured that the default transition band drops `Dtmax12` below the
detection threshold: the FIR cannot clear inside V1's 60 ms post-EOO silence,
so the tail is still sitting in filter memory when transmission ends.

**The comment's own numbers are optimistic.** Measured directly through the new
`Resampler::latencySamples()` (`resampler_flush_test`):

| 8000 → 24000 | latency (input samples) | |
|---|---|---|
| default band | **2376** | **297.0 ms** — not the ~118 ms estimated |
| `ReqTransBand=45` | **69** | **8.6 ms** |

A 297 ms filter against a **120 ms** EOO smears it across nearly three times its
own duration. So the loosened band is not a tuning preference, it is the
difference between a detectable end-of-over and a legend about one.

**Why "we are pilotless" does not exempt V2.** The mechanism is FIR *latency*,
not modulation structure. V2's EOO is a fixed precomputed waveform
(`rade_v2_ofdm_get_eoo()`), detected at the far end by correlation against that
known template — and a long FIR both delays and smears it. Nothing about
carrying or not carrying pilots changes how much signal is stranded in a filter.

**What genuinely does change, and it cuts both ways:**

- *Better:* V2's EOO is **960 samples = 120.0 ms** of actual signal (measured,
  `rade_v2_codec_test`), against V1's 60 ms silence window — roughly double the
  time for a filter to clear.
- *Worse:* **§7.1 T3's drain does not fix this, and could mask it.** T3 keeps
  the *pump* alive after the radio stops asking. It says nothing about samples
  stranded in our own resampler, which are a separate loss on a separate path.
  A tail can be perfectly clocked out and still arrive truncated.
- *Under our control now:* V1 was bounded by a fixed 60 ms silence. We decide
  when the drain ends, so the deadline is ours to set rather than the codec's.

**Required, therefore:**

1. Carry `ReqTransBand=45.0` onto the V2 TX chain. Cheap, proven, and V2's
   1000–1937.5 Hz occupancy is *narrower* than V1's 750–2200 Hz, so the wider
   transition band is at least as safe.
2. **`flush()` the TX resampler at end of over.** This did not exist when T9 was
   first written — `Resampler` had a `prewarm()` for the head of a stream and
   nothing for the tail — and has since been added (`b7c22034`,
   `resampler_flush_test`). It pushes `latencySamples()` zeros through so the
   signal still in flight is carried out ahead of them, and it recovers 7128
   samples that `process()` alone never emits for a 400-sample burst.
   > `flush()` is **terminal**: afterwards the filter holds zeros, so
   > `process()` refuses and warns until `reset()`. That is deliberate — audio
   > spliced behind silence would be audible, wrong, and unexplained anywhere.
3. Size the T3 drain as **EOO + `latencySamples()`**, not EOO alone. The number
   no longer has to be guessed or padded: ask the resampler.

> **`flush()` makes the loss recoverable; it does not make the long filter
> advisable.** Both are required. With the default band the EOO is still
> smeared across 297 ms of filter response before any of it can be flushed out,
> and correlation detection cares about the shape, not merely the presence, of
> the tail.

**Why this is 🔇.** Every failure here is silent. The modem transmits, power is
normal, the panadapter looks right, our own stats show the tail emitted — and
the far end simply never registers end-of-over. Exactly the shape of §7.1 T3
itself, one layer further down, which is why a T3 test can pass while T9 is
broken.

## 7.1b X2 in full — a conjugation error does not look like one

Measured during stage 6b on the bench (`rade_v2_engine_test`), by pushing the same
`rade_tx()`-generated signal through `RADEV2Engine` three ways. No RF; a clean, noise-free
analytic signal upsampled 8→24 kHz exactly as the radio delivers it.

| Input to the engine | acquires? | feature frames | SNR estimate |
|---|---|---|---|
| `A − jB` — correct (§10.3 item 5) | yes | 128 | **+24.0 dB** |
| `A + jB` — conjugated | **yes** | **165** | **−4.7 dB** |
| `A + j0` — imaginary discarded | yes | 128 | +23.7 dB |

**Three corrections to how X2 was written, none of them to the constraint itself.**

**1. Sync is not a check on sideband sense.** X2 said the mirrored spectrum "never decodes",
which is true of the *audio* and false of everything an operator can see. The receiver acquires
on conjugated input and stays acquired — and produces *more* feature frames than correct input,
not fewer. The mechanism is structural, not a fluke: V2 acquires on the **cyclic-prefix
autocorrelation** (`rade_rx_v2.c:374`, `compute_autocorr` → `detect_signal`), whose detector is
magnitude-based. Conjugating the input conjugates `Ry` and leaves `|Ry|` untouched; only the
frequency-offset estimate flips sign, and the tracker absorbs that.

> **Debugging consequence.** RAD2 silent, SYNC lit, frames counting up — that is *exactly* what
> a sideband error looks like. Anyone who reasons "the modem is synced, so the front end is
> fine" will look in the wrong half of the system. **The SNR estimate is the tell**, and it is a
> ~29 dB tell. Any RAD2 diagnostic surface should show SNR next to sync, not sync alone.

**2. The failure is the WRONG sign, not the absence of a sign.** Discarding the imaginary
component entirely costs 0.3 dB here, while getting it backwards costs ~29 dB. Real-only input
puts equal energy in both images and the receiver's own processing copes; conjugated input puts
*all* of it in the wrong one. So "we must use I/Q" is a weaker claim than X2 implies — what is
load-bearing is not inverting it.

> The 0.3 dB is a **noise-free bench figure and should not be read as the on-air cost** of
> dropping Q. Image rejection buys nothing when there is no interference to reject; under noise,
> an adjacent signal, or QRM in the mirrored band the gap should widen. We use both components
> because the radio hands them to us already separated (§10.3 item 5) — there is no work to save
> by throwing one away.

**3. It follows that this cannot be guarded by asserting sync.** `rade_v2_engine_test` compares
the correct and conjugated inputs in one test and requires a ≥15 dB separation, which fails if
the imaginary component is ever dropped or its sign inverted. Mutation-checked both ways: with
`q = 0` forced in the RX chain the two inputs score identically (23.7 dB each) and the test
fails, while every other test in the file still passes.

## 7.2 Ownership and wiring — `FlexBackend` is not modified

**Decision: `FlexBackend` does not own the waveform provider, does not know RADE exists, and is
not touched by this work at all.** It supplies nothing; the transport is handed a reply-capable
command sink by whoever wires it up.

The architectural reason is the one AetherSDR is already committed to: `IRadioBackend` is a *radio*
abstraction, and there are now three implementors (Flex, HL2, Sim). RADE is a *codec* that happens
to have exactly one transport today. Putting the codec's lifecycle inside a radio driver inverts
that relationship, and the cost is paid by every backend added afterwards — each one inherits the
question "and does this one do RADE?", which is not a question about radios.

There is also a **measured** reason the obvious alternative does not work, recorded here because a
reviewer will reasonably ask why the existing vendor-extension seam was not used:

> `IRadioBackend::invokeExtension()` **cannot carry a reply body.** `FlexBackend`'s implementation
> calls `send(cmd)` and then emits `extensionResult(requestId, QVariant(true))` — an acknowledgment
> that the command was *dispatched*, not the radio's answer. But `waveform create` returns the six
> stream ids **in its reply body** (§7.1 R7), and there is no other way to learn them. Routing
> registration through `invokeExtension` would therefore require changing the semantics of a
> shipped, cross-backend interface that HL2 and Sim also implement — a larger and entirely
> unrelated change, to reach a place we do not want to be anyway.
>
> Note this is the §7.1 pattern one layer up: **`extensionResult` currently means "accepted", not
> "applied"**, exactly as a radio reply code of 0 does. Worth fixing someday; not here.

What is used instead already exists and already carries the body: `RadioConnection::ResponseCallback`
(`void(int resultCode, const QString& body)`) and `commandResponse(seq, resultCode, body)`, with seq
allocation and correlation living above the seam in `RadioModel`. The sink is built from those. No
new wire path and no second socket — the in-process, single-connection result from §8 (OQ#1) holds.

**The rule, stated so it is checkable:** the dependency points **one way** —
`RADEV2Engine → FlexWaveformTransport → command sink`. No file in `src/core/backends/` may include
a RADE header, and `FlexBackend.{h,cpp}` may not name the provider, the stream, or the transport.
That is a grep. **It has landed** as `rade_v2_backend_boundary_test` (§13), deliberately outside the
`ENABLE_RADE_V2` gate, and both rules were mutation-checked before being trusted.

### 7.2a Gating: a declared capability, not a family test

**Added 2026-07-30, after `RadioCapabilities` landed upstream (#4508).** It changes the
recommended shape of one thing this design does, so it is recorded here rather than left for a
reviewer to find.

The Flex Waveform API is a Flex protocol, so RAD2 has to be gated on the radio actually speaking
it — on a backend without the SmartSDR text-command plane the registration is written into a void
and the reply that carries the six stream ids never arrives. A silent hang, exactly the §7.1 shape.

The **wrong way to express that**, which is what the first implementation did, is to ask whether
the radio is a Flex. `src/core/backends/RadioCapabilities.h` is now the canonical answer to that
class of question, and its rule is normative:

> *"Clients render against what the radio **reports**. No call site asks 'is this a Flex' — no
> `caps.family` test, no `dynamic_cast<FlexBackend*>`. This is the structural replacement for the
> model-impersonation anti-pattern."*

**So the gate should be a declared capability**: a backend states whether it accepts runtime
waveform registration, `FlexBackend` sets it true, `Hl2Backend` and `SimBackend` set it false, and
the field is recorded in `docs/architecture/radio-capabilities-map.md` — where the table exists
precisely because *"a capability no consumer reads looks identical, from the backend side, to one
that works."*

Two traps worth naming, both of which the map doc calls out:

- **It is NOT `hasWaveforms`.** That field already exists and means the radio accepts *installable*
  waveform packages, driving the File ▸ Waveforms management menu. Runtime `waveform create` is a
  different thing — transient, connection-scoped, nothing installed. Reusing the flag would make
  one menu and one modem disagree about what it meant.
- **Fields default to `false`**, so a backend that omits one silently declares the feature absent.
  Set it explicitly in all three backends.

**Where the call goes matters too.** `MainWindow::applyCapabilitiesToUi()` is the single fan-out
for capability-driven surfaces, bound to `capabilitiesChanged` — which fires on both connection
edges *and* on mid-session revisions. Registration should hang off that rather than off its own
connect-time lambda; scattered connect lambdas are how two callers end up driving one piece of
state and last-writer-wins.

> **Status: not yet done.** The current implementation gates on
> `RadioModel::usesFlexCommandPlane()` from a connect-time lambda. It is functionally correct on
> every backend today and all tests pass — this is a **merge-readiness** item, not a defect, and it
> is the one piece of this design that is knowingly out of step with the architecture around it.

**A worked precedent now exists (added 2026-07-31).** RFC #4603 shipped
`RadioCapabilities::clientSettingsDomains`, which is this exact pattern carried through end to
end, and it is worth copying rather than re-deriving: it is **typed** (a `Q_DECLARE_FLAGS` set,
not a boolean, because authority is per-domain), **declared explicitly by all three backends**,
**guarded by a CI test** that pins the empty declarations so a future edit cannot quietly grant a
domain, and **recorded in `radio-capabilities-map.md`** with the consumer named. It also models
the naming rule the header states repeatedly — *"Named for the CONCEPT, not the brand"* — which
rules out anything Flex-flavoured for the waveform-registration flag. The CI guard is the part
most worth imitating: the failure it prevents, like ours, is silent.

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

### 9.1 RX audio output — DECIDED 2026-07-28: the waveform RETURN PATH (option B)

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
> **DECIDED: B**, by the operator, 2026-07-28. Latency came in at ~22 ms (§10.8), which
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
> **Revisit trigger, as the operator stated it:** if on-air testing with the real waveform shows
> audible quality problems, this reopens. Also worth watching: SmartLink operation being
> materially worse under Opus,
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

#### The interim framing — IMPLEMENTED, and built to be deleted

`src/core/RadeV2TextChannel.{h,cpp}` (`rade_v2_text_channel_test`). Provisional in the strict sense:
it exists so the channel can be exercised end to end before the ecosystem convention lands, and the
whole protocol is confined to one file so replacing it is a deletion rather than an excavation.

```
┌────────────┬─────┬─────┬──────────────┬─────────┐
│  Barker-13 │ ver │ len │  N × 6 bits  │ CRC-16  │
│    sync    │  3  │  4  │   payload    │         │
└────────────┴─────┴─────┴──────────────┴─────────┘
     13        3     4        6N            16      = 36 + 6N bits
```

**2.4 s** per copy for `NF0T`, **4.8 s** for a 14-character worst case, repeating back-to-back so a
receiver joining mid-over catches the next one. Alphabet is 39 values (`A–Z 0–9 / -` and space);
codes 39–63 are unused and therefore **invalid on receive**, a free integrity check beneath the CRC.

Five decisions, each of which a reviewer would otherwise reasonably question:

1. **CRC-16, not CRC-8, and the 0.32 s it costs is the point.** CRC-8 admits ~1/256 of corrupted
   frames that clear the sync gate. This field gets *displayed*, and publishing a bit error as
   somebody else's callsign is the precise harm §9.3 defers FreeDV Reporter over.
2. **The CRC is computed bitwise over the field bit-string, not over bytes.** The frame is 3+4+6N
   bits and is not byte-aligned, so a byte-oriented CRC needs a padding convention — and an
   undocumented padding convention is what makes a second implementation disagree with the first.
3. **Detection normalises by the stream's mean magnitude; confidence deliberately does not.** Per D3
   the RX value is a learned *reconstruction*, not a ±1 signal, so a fixed correlation threshold
   would track the decoder's output level rather than sync quality. But the *transmitted* symbol is
   exactly ±1.0, so raw magnitude is an absolute quality measure — normalising it too would report
   ~1.0 for every frame and say nothing.
4. **The sign of the Barker correlation is data, not noise.** BPSK carries a 180° ambiguity;
   `|corr|` detects and `sign(corr)` corrects, so an inverted stream decodes rather than producing
   garbage the CRC then rejects.
5. **Nothing is consumed on a failed candidate.** Every frame length that could end at the current
   symbol is tried, and the length field must agree with the length assumed. A "sync, then take the
   next N bits" reader desynchronises permanently on a false Barker inside payload data.

**The correlation threshold is a placeholder** (0.70) and must be re-derived from real captures —
it depends on how the decoder reconstructs the 21st feature under real channel conditions, which
nothing has measured. `rade_v2_decode_wav --dump-symbols` exists to supply that evidence.

The `version` field is what keeps our frames and upstream's eventual ones **distinguishable rather
than mutually corrupting**: a version we do not implement is rejected outright, never best-effort
parsed.

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

### 9.3 FreeDV Reporter — what V1 does, and why V2 must not do it yet

RADE V1 reports to the public FreeDV Reporter map (`FreeDvClient`, shipped in v0.9.3, PR #2173).
V2 does not, and **should not until two upstream conventions exist**. This section records the
shape of the eventual work and, more importantly, the reason for the delay — because "V1 has it
and V2 doesn't" reads like an oversight, and it is a decision.

**What V1 sends today:**

| Event | Payload | Trigger |
|---|---|---|
| station presence | callsign, grid, message, software version, frequency | RADE activation |
| `freq_change` | frequency | QSY |
| `tx_report` | `mode: "RADEV1"`, transmitting | MOX edge, guarded against tune/ATU (#2173) |
| `rx_report` | far-end callsign, `mode: "RADEV1"`, SNR | 1 s timer while synced, **and** immediately on EOO callsign decode |

**Why V2 cannot simply reuse it.** Four things differ, and three of them are correctness issues
rather than plumbing:

**1. The far-end callsign is no longer trustworthy.** V1 decodes it from the EOO frame's
LDPC-coded text channel, with a CRC — if it decodes, it is right. V2 has no `rade_text` at all
(upstream dropped the subsystem). The callsign rides the inline BPSK data channel described in
§9.2: **raw, with no sync, no CRC and no FEC**, and our application framing is explicitly
provisional until the ecosystem settles it. A V2 `rx_report` built on that would publish
**bit-error garbage as station callsigns** onto a shared public map. That is worse than silence:
it invents spots for stations that were never there, and it does so under our name.

**2. `mode` is an ecosystem string, not ours to coin.** V1 sends the literal `"RADEV1"`. The V2
equivalent has to be whatever FreeDV Reporter and FreeDV-GUI agree on — the same dependency, on the
same upstream, as §9.2's framing. Inventing one now means either being alone on the map under a
mode nobody recognises, or colliding with the real one later.

**3. Sync is not a sufficient gate for reporting.** V1 gates `rx_report` on sync. §7.1b measured
that a conjugated or mistuned V2 signal **still acquires and stays acquired**, and §10.13 saw it in
the field: shifted 30–140 Hz off frequency the receiver produced *more* synced blocks than at the
correct offset (489 vs 394) while mean SNR collapsed to ~7 dB. Ported unchanged, that gate would
emit confident spots for signals we are not actually decoding. Any V2 reporting must gate on **SNR
as well as sync**, and the threshold wants deriving from real decodes rather than picking a number.

**4. The EOO trigger does not port.** V1 sends an immediate `rx_report` the moment the EOO callsign
decodes, because sync drops 100–500 ms later and would otherwise clear the callsign before the
timer fires. V2's EOO is **pilot-only** and carries no data, so that trigger does not exist. The
inline channel is continuous instead, which is a better fit for periodic reporting — but it means
the timing logic is V1-shaped and should be rewritten rather than adapted.

**What it does inherit.** The station-presence, `freq_change` and `tx_report` paths are
mode-agnostic and largely reusable, including the tune/ATU guard (#2173) — a tune cycle must never
be reported as a transmission. So is the refusal to enable with an empty callsign or grid, which
exists for exactly the reason this whole section does: **not broadcasting placeholder data to a
public map.** V2 extends that principle from "no placeholder station data" to "no unvalidated
decode data".

**Gates on doing the work** — all three, not any:

1. The RADE V2 **callsign framing convention** is published by the ecosystem (§9.2, §16 Q2).
2. The **FreeDV Reporter mode string** for V2 is agreed.
3. A **decode-confidence gate** is defined from real on-air decodes, so `rx_report` cannot fire on
   a false acquisition (§7.1b, §10.13).

Sequencing: this is **Phase 5** work, after interop with a real V2 station is possible. It is not
on the critical path for anything, and doing it early has negative value — the earliest useful
moment is the one where there is a second station to be right about.

> **Slice selection, when it happens.** There is an existing plan for priority-ordered slice
> selection when several could report (RADE > Docker FreeDV > active slice) plus a union band
> filter. `RAD2` joins that ordering rather than getting a parallel path of its own.

### 9.4 Client-side persistence — RAD2 stores nothing, deliberately

**Added 2026-07-31, after the client settings store moved from XML to SQLite upstream
(RFC #4603 phase 1, `e1d19ea9`).** Like §7.2a, it changes what a reviewer will expect of this
design rather than what it does, so it is recorded here rather than left to be discovered.

RADE V2 holds **no persistent client-side state**. Verified across every file this design
introduces — `RADEV2Engine`, `FlexWaveformProvider`, `FlexWaveformStream`,
`FlexWaveformTransport`, `ModeFamily.h`: no `AppSettings`, no `QSettings`, no `settingsScope()`,
no `sqlite3.h`. The mode is re-registered from scratch on every connect (R9) and the codec is
reconstructed per activation, so there is nothing that would survive a restart to be worth
storing.

Before RFC #4603 that needed no justification: there was one flat key–value store, and not
persisting was simply the default. It needs stating now, because the store has a *right* way to
hold radio-scoped state and the absence of any RAD2 state is otherwise indistinguishable from an
oversight.

**If RAD2 ever does need persistent state**, it takes the radio-scoped path, not flat keys:

- One **versioned JSON feature document** in `radio_settings`, addressed via
  `RadioModel::settingsScope()` — the shape AGENTS.md "Radio-Scoped Feature Documents" requires,
  and the one the `BandStack`, `Identity` and HL2 `OperatingState` documents already use.
- Writers read the **exact** row (`featureExact()`), never the family-wide fallback, so a
  per-radio write cannot clone the family default into a new row.
- **Check the `setFeature()` return** (constraint S3). A refused write that the UI repaints over
  is the same silent-failure class as the 🔇 rows in §7.1 — the state looks saved and is not.
- Never a flat `AppSettings` key. RAD2 state is per-radio by construction: the waveform is
  registered against one radio's command plane.

**The one settings dependency is a read.** RAD2's TX callsign is resolved from the shared FreeDV
keys, identically to V1 and to FreeDV Reporter (`MainWindow_DigitalModes.cpp`):

```
FreeDvUseRadioCallsign == "True" && radio callsign non-empty
    ? radio callsign            // radio-authoritative, Constitution II/III
    : FreeDvMyCallsign          // operator's saved fallback
```

Correct placement under the new rules, and unaffected by the migration: it is an app-scoped
operator preference, **not a credential** — so the `SettingsCredentialPolicy.h` ban and the
QtKeychain-only rule do not apply — and **not radio-scoped**, since the operator's callsign does
not change per radio. Reads need no `save()`.

> **RAD2 must not grow its own callsign key.** Sharing V1's is deliberate. A second source of
> truth for the same value is exactly how the modem and the Reporter would come to disagree about
> who is transmitting — and §9.3 already defers Reporter integration on *data-quality* grounds.

### 9.5 Settings authority — RAD2 and `clientSettingsDomains`

RFC #4603 also made settings authority a **declared capability** rather than a family assumption.
`RadioCapabilities::clientSettingsDomains` is a typed per-domain flag set: empty for Flex and Sim
(guarded by a CI test, because a non-empty Flex declaration would re-introduce the
#2465/#4126/#4261 re-assert-stale-state bug class), six domains declared by HL2, which persists
nothing itself and so relies on the client as its memory.

**RAD2 is unaffected today.** It is Flex-only; Flex declares empty; `RadioStateMemory::shouldEngage()`
is therefore false and nothing about a RAD2 session is captured or restored. Two constraints
follow for whoever changes that, and both would fail silently — they are S1 and S2 in §7.1:

- **S1 — restored state must not carry a waveform mode string.** `RestoredRadioState::mode` is
  gated by the `Tuning` domain, and `RadioModel` hands restored state to the backend **before**
  `connectRadio()` — necessarily before any waveform is registered (R9). A restored `"RAD2"`
  therefore lands in precisely the §10.12 G5 state: `SliceModel::setMode()` returns early, the
  combo reads `RAD2`, and the slice never moves. A backend that later declares both `Tuning` *and*
  runtime waveform registration must restore the **underlying** mode (`DIGU`) and let mode-select
  re-derive `RAD2`, or defer the mode half of the restore until registration completes.
- **S2 — the memory bank stores a mode string, and now stores it in the settings store.** Since
  #4603 PR 6 the host-side bank is one shared document at `radio_settings (local, '', MemoryBank)`,
  engaging on `persistsMemories`, with `Memories` as a declared domain. Recalling a memory saved
  while RAD2 was active reproduces G5 if the waveform has not registered yet.

S2 is the same risk this design already carried informally, but its storage is now **known and
inspectable** — through the Settings Browser and `AetherSDR --config` — which makes it a
regression test rather than a caveat. §13 gains that case.

> **The `sqlite3.h` single-consumer rule needs nothing from us.** No RADE V2 file includes it and
> none should; `third_party/sqlite/README.md` owns that rule. Deliberately **not** added to
> `verify_rade_v2_backend_boundary.cmake` (§7.2) — duplicating a guard is how two guards drift.

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

**So the transmitted signal needs band-limiting that the modem does not provide** — the opposite of
the RX guidance above it.

> **⚠ SUPERSEDED BY MEASUREMENT — see §10.10.** This section originally concluded "set
> `tx_filter` to roughly `[800, 2200]`". **That does not work: the waveform's `tx_filter` is
> accepted and silently ignored.** Two registrations differing by 500 Hz and 1400 Hz on the cuts
> produced curves identical to 0.1 dB. The filtering actually present is the radio's *global*
> transmit filter (`lo=100 hi=2900`), which is shared with every mode.
>
> **The band-limiting therefore has to happen in our own TX chain**, before emitting on
> `tx_stream_in`. That is the one place with per-waveform control. The modem's 1000–1937.5 Hz
> occupancy passes the global filter unclipped, so this is about suppressing sidelobes, not about
> protecting the signal.

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

> **STATUS — G1, G2, G3 RESOLVED (stage 6c).** `RAD2` is registered in
> `DigitalVoiceModeRegistry` (`radioMode "RAD2"`, `underlyingMode "DIGU"`), and the ten inline
> classification sites now route through `src/gui/ModeFamily.h`, whose digital-voice half reads
> the descriptor's `underlyingMode` rather than a literal list. Guarded by
> `rade_v2_mode_identity_test` (§13), mutation-checked four ways.
>
> **G4 (`rfGain` reset) remains OPEN and un-diagnosed**, exactly as this table says: it is
> pan-scoped, not slice-mode-keyed, so it does **not** share the root cause the other three had
> and is not fixed by the classification work. **Re-observe on air before designing anything** —
> it would be easy, and wrong, to assume the 6c change addressed it.

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

> **DECIDED in implementation (stage 5): `tx_stream_in` is the CLOCK, `AudioEngine` is the AUDIO.**
> The transport takes only a sample count from each inbound TX packet and never decodes the
> payload. §10.9 D found the stream flowing while keyed but carrying silence (peak 0.0000, with
> `mic_selection=PC` and no PC audio present), so "can it carry usable mic audio" was never
> answered — and this split means it does not have to be. The open question stops being
> load-bearing and becomes a curiosity: T1's cadence requirement is satisfied either way, and the
> audio comes from the path §9 already specifies. **Re-measuring it is no longer on the critical
> path.**

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

### 10.9 Tier 2 — live TX measurements, 2026-07-28 (FLEX-8400, 50.340 MHz, 1 W)

First transmissions of this project. Probe: `tools/flex_waveform_tier2_probe.py`, run with
`--arm`; ~76 s keyed total in bursts under 5 s, into a 6 m antenna at an indicated 1 W, operator
supervising remotely with AetherSDR plus an external LP-100A, station identified in CW each
session. Everything below is from the radio's own telemetry — no monitor receiver was available.

**A — the waveform TX path works, and emits on the INBOUND stream id.** Keying with audio on
`tx_stream_in_id` (`0x81000005`) produced RF; the interlock reported `TRANSMITTING` and forward
power tracked drive. This matches §10.6's RX result and the shipped D-Star provider: **the `*_out`
ids are not used in either direction.**

**B — THE TX PATH IS LINEAR. No ALC, no compression. RADE V2's OFDM survives it.**
Single 1500 Hz tone, drive swept 0.05 → 1.00:

| step | expected | observed | | | amp | fwd dBm | watts |
|---|---|---|---|---|---|---|---|
| 0.05→0.10 | +6.02 | +0.50 | | | 0.05 | 11.3 | 0.013 |
| 0.10→0.20 | +6.02 | +1.70 | | | 0.20 | 13.5 | 0.022 |
| 0.20→0.35 | +4.86 | +2.90 | | | 0.35 | 16.4 | 0.044 |
| **0.35→0.50** | **+3.10** | **+2.70** | | | 0.50 | 19.1 | 0.081 |
| **0.50→0.70** | **+2.92** | **+2.90** | | | 0.70 | 22.0 | 0.160 |
| **0.70→1.00** | **+3.10** | **+3.40** | | | 1.00 | 25.4 | 0.345 |

Above drive 0.35 the transfer is linear to within **0.4 dB**. The soft bottom is the
**forward-power meter's floor** at ~11 dBm (13 mW) — a 100 W-capable coupler cannot resolve
milliwatts — so those points measure the instrument, not the transmitter. Headroom is ample:
drive 1.0 gave 0.345 W against a 1 W setting.

> ### ⛔ CORRECTION 2026-08-03 — the ALC evidence in this section was measured on a BLIND meter
>
> This section originally added *"with **ALC pinned at −150.0 dBFS** and **COMPPEAK at 0.0** at
> every point"* as evidence that no ALC acts on our signal. **That evidence is void.** §10.15
> enumerated the radio's whole TX chain and found `ALC` (TX- index 23, "after SW ALC") sits
> **upstream of where waveform audio is injected**. It reads −150 whether or not anything
> downstream is compressing us — it is not observing the path under test. The same is true of
> `COMPPEAK`, `SC_MIC` and `AFTEREQ`, which are all on the mic / speech-processor chain that RAD2
> never touches.
>
> The operator caught this after an agent watched those two meters through an entire 65 W
> transmission and reported "no ALC action" from readings that could not have moved.
>
> **The power-transfer linearity above still stands** — it is measured from `FWDPWR`, which does
> observe the output. Only the ALC/COMPPEAK claim is withdrawn. §10.15 re-establishes the
> conclusion on meters that can actually see our signal.
>
> **General lesson, worth more than the specific fact:** an instrument reading its idle value is
> not evidence of absence until you have shown the instrument is connected to the thing you are
> measuring. Two meters pinned at their floor look identical to two meters watching a clean signal.

> **The probe's own automatic verdict said "NON-LINEAR — something is compressing", and it was
> wrong.** It anchored the ideal line to the lowest-drive point, the noisiest one, and projected
> that error across the sweep. Compression flattens the *top* of a transfer curve; this curve has
> a floored *bottom*. The analysis now judges step-to-step gain, excludes the detected meter
> floor, and tests the top end specifically for the sag that compression actually produces.

> **Caveat — antenna mismatch (operator-raised).** These runs were into a mismatched antenna
> (2.4:1 on the LP-100A, 3:1 indicated) with the internal ATU **bypassed**. Rising SWR can trigger
> drive foldback that would masquerade as compression. It does not explain this data: foldback's
> signature is top-end sag, and the last step *over*-performed (+3.40 vs +3.10 expected) with no
> sag anywhere. Note also that `FWDPWR` is **forward** power, not delivered power, so the absolute
> watts above are not radiated watts — which does not affect the linearity conclusion. The probe
> now records SWR and REFPWR per step so this is visible in the data rather than reconstructed
> afterwards. **A confirmatory sweep with the ATU engaged is still worth doing.**

**D — `tx_stream_in` is PTT-gated, and was silent.** 441 packets while keyed against **zero**
unkeyed in §10.6 T3 — so the stream is confirmed gated by transmit. Peak sample was 0.0000, with
`mic_selection=PC` and no PC audio present, so it *flows* but carried silence. Whether it can
carry usable mic audio is **still unproven** and needs a live source on the selected input.

**E — the radio HOLDS TX through an underrun.** Cutting the stream for 1.5 s while keyed left the
interlock in `TRANSMITTING`; forward power sagged 22.0 → 15.3 dBm and recovered fully on resume.
The carrier is not dropped. Favourable for the EOO tail — but it does **not** weaken §10.5 item 4:
the tail still has to be *fed* to be emitted, and holding PTT alone emits nothing.

**C — not run.** The budgeted keyed time was spent (partly to a crash that lost a completed sweep,
see the probe's method notes). Given §10.1 now establishes `tx_filter` as the *only* suppression
for V2's unshaped sidelobes, this deserves an unhurried run rather than a truncated one.

**Deferred at the time of writing — sideband sense has since been CLOSED, see §10.11**, and it
needed no off-air receiver at all. What is still outstanding is transmitted sidelobe level
(measurable in software), IMD, and EVM, none of which blocks Phase 2.

### 10.10 `tx_filter` is NOT honoured — and linearity re-confirmed with a matched antenna

Second armed run, 2026-07-28, ~48 s keyed, ATU **engaged** this time (`--stay` keeps it that way).

**B re-run — linearity confirmed under a good match, retiring the §10.9 mismatch caveat.**

| amp | fwd dBm | watts | COMPPEAK | ALC | SWR | ref dBm |
|---|---|---|---|---|---|---|
| 0.35 | 18.9 | 0.077 | 0.0 | −150.0 | 1.00 | 8.6 |
| 0.50 | 21.9 | 0.153 | 0.0 | −150.0 | 1.00 | 9.1 |
| 0.70 | 24.9 | 0.308 | 0.0 | −150.0 | 1.00 | 10.1 |
| 1.00 | 28.3 | 0.680 | 0.0 | −150.0 | 1.53 | 11.9 |

Worst step error above drive 0.35: **0.35 dB**. Top-end sag: **−0.10 dB** (i.e. none — it
*over*-performs). SWR 1.00 across the sweep, rising only to 1.53 at full drive. Forward power
roughly doubled versus the mismatched run (0.680 W vs 0.345 W at drive 1.0), exactly as a better
match predicts. **The foldback hypothesis is now closed: linear with the mismatch, and linear
without it.**

**C — the waveform's `tx_filter` is ACCEPTED AND IGNORED.** Constant-amplitude swept tone under
two very different registrations:

| Hz | `tx_filter [800, 2200]` | `tx_filter [300, 3600]` | Δ |
|---|---|---|---|
| 500 | 22.2 | 22.4 | +0.1 |
| 1000 | 21.9 | 21.9 | 0.0 |
| 1500 | 21.9 | 21.9 | 0.0 |
| 2000 | 22.2 | 22.3 | 0.0 |
| 2400 | 22.5 | 22.5 | 0.0 |
| 3000 | 14.6 | 14.5 | 0.0 |
| 3500 | 11.7 | 11.7 | 0.0 |

Largest difference between two registrations that differ by 500 Hz on the low cut and 1400 Hz on
the high cut: **0.1 dB.** The waveform filter does nothing.

**But a filter IS acting — the radio's GLOBAL transmit filter.** The identical curves both roll
off above 2400 Hz (−8 dB at 3000, −11 dB at 3500), and the global transmit status carries
**`lo=100 hi=2900 tx_filter_changes_allowed=1`**. A 100–2900 Hz voice filter with a normal
transition produces exactly the measured shape. So the suppression we observed is real, it is just
not ours to control per-waveform.

**Design consequences — this changes §10.1's TX guidance:**

1. **The provider must not rely on `tx_filter`, and must not expose it as an operator control.**
   It is accepted with code 0 and silently ignored — the same trap family as a live `rx_filter`
   update (§10.3) and `slice tune` outside the pan span (§10.9 method notes).
2. **The modem itself is safe.** V2's 1000–1937.5 Hz occupancy sits well inside the global
   100–2900 Hz filter and passes unclipped — confirmed flat 500–2400 Hz above.
3. **Sidelobe suppression must move into our own TX chain.** §10.1 established that the V2
   modulator applies no shaping whatsoever, and §10.1's suggested fix — register a matched
   `tx_filter` — is now known not to work. The remaining options are (a) **shape or band-pass in
   the provider before emitting on `tx_stream_in`**, which is local, deterministic and has no
   side effects, or (b) narrow the *global* transmit filter, which is shared with every other mode
   and would need save/restore. **(a) is the answer.** The global filter still contributes ~8–11 dB
   at 3–3.5 kHz, so this is hardening rather than a hole.

### 10.11 SIDEBAND SENSE — CLOSED. Upper sideband is correct; no TX conjugation.

The last question that was blocking on measurement, and the largest open risk in the TX design:
§10.5 inferred *from reading the shipped D-Star provider* that the outbound waveform streams carry
stereo **audio** rather than I/Q, and therefore that there is no conjugate convention to get wrong
on transmit. That was a code reading. It is now measured.

**No off-air receiver was needed.** The Flex will draw its own transmitted signal on the
panadapter (`transmit set show_tx_in_waterfall=1`, default 0), and the panadapter is in the **RF
frequency domain** — precisely where the question lives. The FFT bins were read numerically
(PCC **0x8003**, 12-byte sub-header, `uint16` bins where **lower raw = stronger**), at a 20 kHz
span across 1024 bins = **19.5 Hz/bin**. Probe: `tools/flex_waveform_sideband_probe.py`.

The instrument had to earn trust first. This 8400 has `num_scu=1`, so RX is blanked while
transmitting — whatever the pan shows during TX is radio-generated, not received. So the same tone
was pushed through the **same** path under two underlying modes, giving a control that shares every
code path with the test:

| pass | predicted | measured | error |
|---|---|---|---|
| `DIGU`, 1000 Hz | +1000 Hz | **+987 Hz** | 13 Hz |
| `DIGU`, 2000 Hz | +2000 Hz | **+1984 Hz** | 16 Hz |
| `DIGL`, 1000 Hz | −1000 Hz | **−1007 Hz** | 7 Hz |

Every error is inside one bin.

- **Control 1 — is the axis a real frequency mapping?** Tone moved 1000 Hz; peak moved **997 Hz**.
  PASS. The display is not a fixed graphic.
- **Control 2 — is the display sideband-aware at all?** `DIGU` +987 Hz versus `DIGL` −1007 Hz:
  **opposite sides of the dial.** PASS. Had both landed on the same side, the method would have
  been incapable of answering and we would have fallen back to a receiver.

> **RESULT: `DIGU` puts our transmitted audio ABOVE the dial. Upper sideband is correct, and the
> provider must NOT conjugate on transmit.** §10.5's inference is confirmed by measurement from a
> completely independent direction — the code reading said "outbound is audio, so no conjugate
> convention applies", and the RF-domain measurement agrees. The TX path stays: `rade_tx()` →
> `real()` → duplicate into both float slots → `tx_stream_in`.
>
> Note the asymmetry with RX is now **measured on both sides**: inbound `rx_stream_in` is conjugate
> I/Q and its imaginary component must be negated (§10.3 item 5); outbound is audio and must not be
> touched. Getting either one wrong produces a signal that looks perfectly normal on every meter
> and decodes for nobody.

**Bonus — this also closes the §10.3 item 7 residual.** That item ended "whether `digu_offset`
shifts the demodulated passband or only the dial display… needs a tonal reference rather than the
noise-based method used here." This *is* a tonal reference, in the RF domain: a 1000 Hz audio tone
landed at dial **+987 Hz** and a 2000 Hz tone at **+1984 Hz** — i.e. at exactly the audio
frequency, with no 1500 Hz displacement anywhere. **`digu_offset` does not shift the transmitted
RF placement.** It is the filter-centring/dial convention established in §10.3 item 7 and nothing
more, now confirmed in the domain that item asked for.

**What remains deferred is now genuinely low-value:** transmitted sidelobe levels (measurable in
software against the samples we generate, since we author them), IMD (a linear transmitter at 1 W,
nowhere near compression), and EVM (needs the codec, and is subsumed by "a real RADE V2 station
decodes us"). **None of it blocks Phase 2.**

### 10.12 First bring-up on air — 2026-07-29 (FLEX-8400, fw 4.2.20.41343)

The first time the wired-up client was launched against the radio. Two defects, both in the
*activation path* rather than in the waveform protocol, and one open question closed.

**CLOSED: `mode_list` refreshes immediately on an already-open slice.** §10.12 was expected to
need a new slice or a reconnect before `RAD2` could appear. It does not — with the waveform
registered at connect time, `RAD2` was present in the mode dropdown of a slice that already
existed, immediately, with no slice churn. **Provider consequence:** registration alone is
sufficient to publish the mode; nothing has to nudge the radio and no slice lifecycle has to be
managed to make it visible.

**Defect 1 — registration on mode-select is a deadlock (now R9).** The first wiring registered
the waveform inside the RAD2 activation path, which is triggered by the slice mode *becoming*
`RAD2`. But the radio only advertises `RAD2` once the waveform is registered. So the mode could
not appear, could not be selected, and the registration could not run. The dropdown simply had no
`RAD2` in it — no error, no warning, nothing to grep for.

> Registration is a property of the **connection**, not of a slice, which R8 already said and
> §10.2 already demonstrated: that probe registered a waveform with no slice in existence at all.
> The evidence was in this document before the code was written.

**A refinement on R9, measured while verifying an upstream rebase (2026-07-30).** The connect
edge we register on — `RadioModel::connectionStateChanged(true)` — fires **before** the `client
gui` registration handshake completes, not after. Registration works anyway, and did on air. This
is recorded because it looks like a bug to anyone who reads the sequence, and "fixing" it by
deferring registration to a later edge would reintroduce the deadlock R9 exists to prevent. If a
slow or strict link ever *does* reject the early `waveform create`, the answer is to retry on the
capability edge (§7.2a), not to move it back behind mode selection.

**Defect 2 — the registry gate (now G5).** With the mode visible and selectable, selecting it
still did nothing. The single logged line:

```
WRN default: SliceModel: digital-voice mode rejected: The requested digital-voice mode is not running
```

`SliceModel::setMode()` resolves `RAD2` through `DigitalVoiceModeRegistry` and calls
`transferSlice()`, which refuses unless `activateMode()` has been called for that mode. On refusal
**`setMode()` returns early** — `m_mode` is never assigned, no `slice set mode=` is sent, and
`modeChanged()` never fires.

The presentation is worth recording, because it points at the wrong subsystem:

| Surface | What it showed |
|---|---|
| Mode dropdown | **`RAD2`** — the combo holds its own selection independently of the model |
| Mode buttons beside it | `USB` |
| Audio | ordinary USB |
| Key-up | no waveform |

So the client looks like it is in RAD2 and behaves like it is in USB, and the natural conclusion —
"the waveform registered but the modem is not running" — is wrong in an expensive direction. The
mode change never happened at all.

D-STAR satisfies the same gate when its helper process starts
(`DigitalVoiceWaveformProcess.cpp:271`). The analogue for RAD2 is the transport's `ready()`
handler: the waveform now exists on the radio, so the mode is genuinely available to be claimed.
Guarded by `rade_v2_mode_identity_test::aSliceCannotClaimRad2UntilTheModeIsRunning`.

> **A second route into G5, from a feature that shipped upstream afterwards.** The host-side
> memory bank (#4590) stores and recalls a slice's **mode**. Recalling a saved `RAD2` channel
> before the waveform has registered lands in exactly the path above — `setMode()` returns early,
> the combo reads RAD2, the slice does not move. The G5 fix covers it once registration has
> happened, but the ordering is now reachable from an operator action that has nothing to do with
> the mode dropdown, and it deserves a test.

> **Both defects are the same shape, and it is worth naming.** Each is a *second* precondition
> hiding behind a first one that looked sufficient. "The waveform is registered with the radio"
> feels like the whole of activation; it is neither necessary at the moment we first tried it
> (R9) nor sufficient once it happened (G5). The radio-side protocol — the part this document
> spent eleven probe sessions measuring — was correct on the first attempt in both cases.

### 10.13 TX validated off-air — 2026-07-29 (websdr recording, 14.192 MHz)

**Two AetherSDR RAD2 transmissions, recorded by a third-party websdr and decoded offline. Both
decode. Both end-of-over frames are detected.** This is the first evidence for the transmit chain
that does not come from our own code: a different receiver, a different antenna, a real HF path,
and a decoder run from a file with nothing of ours in the loop but the modem samples themselves.

Recordings are 8 kHz mono — already the modem rate, so no resampling stands between the file and
`rade_rx()`. Neither transmission carried speech; each was a key, a pause, and an unkey.

| | recording A | recording B |
|---|---|---|
| Duration | 14.02 s | 12.91 s |
| First sync | 3.04 s | 2.80 s |
| Synced blocks | 394 | 354 |
| Feature frames decoded | 200 | 178 |
| **End-of-over detected** | **1, at 11.12 s** | **1, at 9.98 s** |
| SNR peak / mean | 17.7 / 12.3 dB | 17.7 / 12.0 dB |
| Frequency offset | +9.8 Hz | +9.7 Hz |

**§7.1 T9 is confirmed on air, which no bench test could do.** The EOO detector correlates against
a known template (`rade_rx_v2.c:311`), so a tail that had been smeared by a long resampler or
truncated by a missing `flush()` would simply not be found. It is found in both recordings, exactly
once, at the end of each over — after passing through our 8→24 kHz upsampler at `ReqTransBand=45`,
the terminal `flush()`, the transport's synthesized drain clock, the radio's transmit chain, an HF
path, and a websdr's own receive chain. The bench measured 3087 samples of tail; this is what
happened to it.

**Frame recovery is essentially total while synced.** 200 frames × 40 ms = 8.0 s against a sync
window of 3.04–11.12 s = 8.08 s. Whatever the receiver was locked to, it decoded nearly all of it.

**The ~9.8 Hz offset costs nothing and is not ours.** It is the websdr's tuning against our dial,
comfortably inside V2's ±31.25 Hz acquisition window. Shifting the input +10 Hz nulls the residual
to −0.2 Hz and leaves peak SNR unchanged (17.7 → 17.6 dB), so there is no hidden penalty being paid
for it.

**Sync dropped and re-acquired 2–3 times per over**, at a mean SNR around 12 dB. **Not diagnosed.**
Both recordings share one path, one receiver and one transmitter, so nothing here separates a
propagation or websdr-AGC explanation from something in our transmit chain. Worth reproducing on a
second path before drawing any conclusion — and worth noting that an over carrying no speech gives
the vocoder nothing to work with, which may itself be part of it.

**A field corroboration of §7.1b, from the sweep.** Shifting the input far off frequency (−140 to
−30 Hz) still produced *more* synced blocks than the correct offset — 489 against 394 — while mean
SNR collapsed from 12.3 dB to ~7 dB and the EOO detection mostly vanished. Exactly the §7.1b
pattern, now seen on real off-air audio rather than a bench signal: **a high sync count is not
evidence of anything.** The EOO detector held up better than sync as a validity indicator, which is
a useful second opinion for anyone debugging a marginal decode.

**Tooling:** `tools/rade_v2_decode_wav.cpp` → `rade_v2_decode_wav`, the V2 analogue of V1's
`rade_demod_wav` and the basis for the Phase 5 OTA validation campaign. `--sweep` exists because
"did not decode" is ambiguous on a third-party recording — it separates a bad transmission from a
receiver tuned outside the acquisition window.

### 10.14 The inline data channel on a real path — 2026-08-02 (reanalysis of the §10.13 recordings)

**The single most consequential measurement for §9.2, and it did not need a new transmission.** The
two websdr recordings from §10.13 were re-run through `rade_v2_decode_wav --dump-symbols`, which
now records every soft symbol the decoder produces.

Both recordings predate the §9.2 framing — but **not** the bit source. `setTxCallsign` landed in
the engine at stage 6b (2026-07-28) sending raw MSB-first ASCII, cycled, and stage 6d
(2026-07-29) wired it into `activateRADEV2`; both are before these transmissions. A
four-character callsign under that scheme is exactly **32 bits**, and that is what the symbol
stream shows.

**What the session log confirms** (`aethersdr-20260729-131926.log`): the radio reported
`callsign="NF0T"` — four characters — and `RadioModel: info — callsign: "NF0T"` at 13:19:30; the
waveform registered and `slice set 0 mode=RAD2` at 13:19:46; and `PTT_REQUESTED` fired at 13:47:16
and 13:47:41, matching the two recordings. `FreeDvUseRadioCallsign` defaults to `True` and is not
overridden, so `NF0T` is what the callsign path would have carried.

> **What the log does NOT confirm, stated so nobody over-reads this.** The engine's own
> `tx callsign` line is absent, because `aether.radev2.codec` emitted **zero lines** that session —
> it is a `qCDebug` and the category defaults to WARNING. So the call is *deduced* from the code
> path plus the settings default, not *observed*. Two independent lines of evidence converge on 32
> bits (the code would send a 4-character callsign; the recordings show a 32-bit period), which is
> considerably better than one, but it is convergence rather than proof. **Enable the category on
> the next OTA run** — it costs nothing and turns this into an observation.

| lag | recording A | recording B |
|---|---|---|
| 28 | 65.1 % | 60.7 % |
| 30 | 68.2 % | 62.8 % |
| **32** | **72.6 %** | **74.0 %** |
| 34 | 64.5 % | 70.1 % |
| 64 | 65.2 % | — |

Sign agreement at lag 32 peaks in **both** recordings independently, with its harmonic at 64 also
raised. So the inline channel **was carrying data over a real HF path** — this is the first
evidence that it works at all off-air, and it is a positive result.

**The error rate is the problem.** For a repeating pattern, agreement at the period is
`(1−p)² + p²`, so `p = (1 − √(1 − 2(1−a)))/2`:

| | agreement | implied symbol error rate |
|---|---|---|
| recording A | 0.726 | **16.4 %** |
| recording B | 0.740 | **15.4 %** |

At 16 % per symbol, an uncoded 60-bit frame survives with probability `0.84⁶⁰ ≈ 3 × 10⁻⁵`. **The
§9.2 framing will essentially never decode on a path like this**, and neither would any comparable
uncoded frame — this is a property of the channel, not of the frame layout.

**Caveats, two of which cut in the favourable direction:**

- The 32-bit period is *inferred* from the sender's old encoding — see the log note above for
  exactly which links in that chain are observed and which are deduced. Two independent recordings
  peaking at exactly 32, and a code path that predicts exactly 32, is strong; it is still not a
  direct observation of what was transmitted.
- **The figure is an upper bound.** Symbols exist only for steps where `rade_rx()` returned > 0
  (§7.1 D2) — 200 of 394 synced blocks in recording A. Any step the receiver skipped shifts the
  stream against the sender's and registers as extra disagreement, so the true per-symbol error
  rate is at most this and probably lower.
- **Neither recording carried speech.** Per D3 the symbol rides through the autoencoder alongside
  the voice features, so a latent asked to carry data *and* silence is not the operating point that
  matters. Whether speech helps or hurts is genuinely unknown and is the next thing to measure.

**Also measured: the soft values are weak.** Mean |soft| is **0.489** against the ±1.0 transmitted,
with 18 % of symbols below 0.15. On the bench the same figure is ~1.0. This vindicates normalising
detection by the stream's mean magnitude (§9.2 decision 3) — a fixed threshold calibrated on bench
values would never fire on air — and it means the *magnitude* carries real information that hard
slicing throws away.

> **Consequence for the design, stated plainly.** The cheapest fix is not FEC. We already transmit
> the same frame continuously, so several noisy copies of every bit are already on the air and we
> are discarding them by hard-slicing each copy independently. **Soft accumulation across
> repetitions** — deliberately scoped out of §9.2 as "possible and out of scope" — is where the
> margin is, and it costs no extra airtime and no change to the wire format, so the framing stays
> deletable. Decide it against the next set of numbers, not these: an OTA run **with speech** is
> the missing measurement, and the tooling to read it now exists.

### 10.15 The radio's TX chain, measured on meters that can see us — 2026-08-03

**Supersedes §10.9's ALC evidence.** 63 W, 20 m, RAD2 with speech, ~10 s keyed, sampled at ~3.7 Hz
through the automation bridge (`get meters`). The radio exposes its entire transmit chain in signal
order, and reading all of it — rather than the two meters §10.9 picked — is what made this
answerable.

| stage (TX- index) | reading | |
|---|---|---|
| `CODEC` 17, `TXAGC` 18, `SC_MIC` 19, `AFTEREQ` 20, `SC_FILT_1` 22, `ALC` 23 | **−150.0** | mic / speech-processor path — **RAD2 never enters it** |
| `COMPPEAK` 21 | 0.0 | 0 dB compression (idle value) |
| **`RM_TX_AGC` 24** | **−4.1** | ⟵ **our audio enters here** |
| `SC_FILT_2` 25 | −6.2 | −2.1 dB vs previous, constant across every sample |
| `TX_AGC` 26, `B4RAMP` 27, `AFRAMP` 28, `POST_P` 29 | −6.2 | track each other identically |
| `ATTN_FPGA` 30 | −7.0 / 0.0 | alternates — **unexplained, see below** |
| `FWDPWR` 8 | 48.2 dBm | ≈63 W |

**Findings:**

1. **The waveform injects at `RM_TX_AGC`.** Every stage before it is the microphone chain. This is
   why §10.9's ALC reading was meaningless and why any future TX-side measurement must start by
   establishing *where in the chain the signal appears* before interpreting any level.
2. **Injection → `POST_P` is flat within ~0.5 dB**, and those stages track each other sample for
   sample. The single −2.1 dB step at `SC_FILT_2` is constant across all 38 samples, which is the
   signature of fixed filter insertion loss rather than gain control.
3. **Nothing pumps.** `RM_TX_AGC` spans −3.8…−4.8 dB and `FWDPWR` 47.6…48.6 dBm over ten seconds.
   Compare the **−14.5 dB** excursion measured in a *received* recording the same day (§10.16) —
   that was the receiving websdr's AGC, not us.

> **Limit of this measurement, stated so it is not over-read.** These meters sample at ~3.7 Hz and
> are smoothed. This is strong evidence against *slow* pumping; it cannot see per-symbol limiting on
> a millisecond scale. It is evidence, not proof.

**⚠ `ATTN_FPGA` is unexplained.** It alternates between ≈−7.0 dBFS and exactly 0.0 sample to
sample. Either 0.0 is a "no fresh value" sentinel for that meter — plausible, since `COMPPEAK` also
sits at a constant 0.0 — or it genuinely means 0 dBFS at the last stage before the antenna, which
would be a 7 dB overshoot worth understanding. **Do not cite this row either way until §13's
ATTN_FPGA validation has run.**

### 10.16 The receiving websdr's AGC was a major impairment — 2026-08-03

Established by comparing the `tx5` tap against a websdr recording of the *same* transmission, then
repeating with the receiver's RF gain fixed.

| | transmitted envelope | received envelope | implied channel gain |
|---|---|---|---|
| receiver AGC on | sd **0.57 dB** | sd 2.90 dB | sd 2.84, range **−14.5 … +1.7 dB** |
| receiver RF gain fixed | sd **0.57 dB** | sd 2.32 dB | sd 2.27, range −6.2 … +3.5 dB |

Our transmitted envelope is identical in both (OFDM is constant-envelope). The **13 dB collapse
lasting ~600 ms with full recovery** existed only in the received copy, and fixing the receiver's
RF gain removed it — after which the operator reported the decoded audio was fully present bar one
small dropout.

**Method warnings, both of which produced wrong answers first:**

- **A per-window correlation that normalises each window by its own norm is BLIND to gain
  variation slower than the window.** It reported ρ = 0.987 while a 13 dB dip was happening inside
  it. Measure the envelope directly if the question is about gain.
- **The tap keeps only the LAST over.** An earlier comparison paired a tap against a recording of a
  *different* transmission and produced a confident decorrelation result that meant nothing. Check
  that durations and timestamps match before believing any tap-vs-capture comparison.

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

The load-bearing half of that promise is **§7.2**: `FlexBackend` is not modified and does not know
RADE exists. This is what makes "additive" true rather than aspirational — a second transport is a
new class beside `FlexWaveformTransport`, not an edit to a radio driver, and no existing backend
acquires a RADE-shaped hole it has to answer for. AetherSDR went from one backend to three inside a
year; the cost of getting this wrong compounds per backend, which is why the decision is recorded
before the transport is written rather than after.

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
- **The stored-`RAD2` guard (§9.5, constraints S1/S2) — NOT YET WRITTEN.** A mode string reaching
  a slice before the waveform has registered produces the G5 dead state, and there are two stored
  sources that can do it: a recalled memory-bank entry, and a restored `Tuning` domain. Assert
  that recalling a memory whose mode is `RAD2` **before registration** either re-derives the mode
  through `DigitalVoiceModeRegistry` or leaves the slice on the underlying mode — and never leaves
  the combo reading `RAD2` over a slice that did not move.
  > Worth writing now that #4603 made it cheap: the bank's storage is a known, inspectable
  > document (`radio_settings (local, '', MemoryBank)`) rather than an opaque side file, so the
  > test can seed the precondition directly instead of driving the UI to create it.
- **The T9 tail guard — LANDED (`resampler_flush_test`).** Asserts `flush()` recovers signal that
  `process()` alone strands, by running two identical resamplers — one flushed, one not — so the
  method cannot pass by being decorative. Also pins the latency figures the T3 drain is sized on.
  > Its first version checked total output *length* and **passed trivially** (8327 samples against
  > an ideal of 1200), because `flush()` emits latency-worth of output. Rewritten to assert *where
  > the signal ends* — the last sample above 1% of peak must reach the theoretical output length.
  > Worth recording as a caution: a length check on a flush is a test that cannot fail.
- **The X2/E2 inversion guard — LANDED (`flex_waveform_stream_test`).** X2 and E2 are opposites:
  inbound I/Q must be conjugated, outbound stereo audio must **not** be. The receive path's `-b`
  looks like it should be symmetric on transmit, and reusing it there inverts the sideband — normal
  power, normal occupancy, normal on every meter, decodable by nobody. Guarded in both directions
  and **mutation-checked**: flipping either sign in production fails the matching test and nothing
  else. `outboundRoundTripsThroughRxDecode` states the asymmetry as a property rather than as two
  independent assertions, so the pair cannot drift into agreement.
- **The seam guard — LANDED (`rade_v2_seam_test`).** The engine and the transport each had their
  own passing tests while the thing between them did not exist. This wires them exactly as
  `MainWindow::activateRADEV2()` does, **with the engine on a real QThread**, and runs an over
  end to end: key → mic audio → the radio's clock → unkey → tail → release.
  > **The thread is not incidental.** The seam carries `std::vector<float>` across a queued
  > connection; if the metatype is unresolvable Qt drops the call at runtime with a warning and
  > the slot is never invoked — no audio, no error. A same-thread test uses direct connections
  > and cannot see that at all.
  >
  > **Two assertions do the work.** Every tick that carried the tail must be flagged
  > `synthesized` (otherwise the test is measuring the radio, not the drain — §7.1 T3's trap),
  > and the release must land well inside the transport's 500 ms backstop. Without the second,
  > a broken handshake still *looks* like a completed one, because the safety net releases the
  > transmitter too.
  >
  > **Mutation-checked, and one mutation usefully failed to fire.** Deleting the codec's
  > `txTailQueued()` hangs two tests out to their timeouts — the tail is never clocked out and
  > nothing ever grants permission to unkey. But swapping the queued/complete announcement order
  > did *not* fail, and the reason is worth keeping: the queue is filled synchronously
  > microseconds earlier and the drain timer ticks every 2 ms, so that ordering is cheap
  > insurance rather than a load-bearing constraint. The comments were corrected to say so
  > rather than left claiming a guarantee the test does not demonstrate.
- **The codec-core guards — LANDED (`rade_v2_engine_test`), all five mutation-checked.** The
  engine's silent failures are the T9 pair plus one the design did not anticipate:
  - *T9 (1)* the TX upsampler's transition band — mutated to the default, the queued tail grows
    from 3087 to over 10 000 samples and the test fails on the upper bound.
  - *T9 (2)* the missing `flush()` — mutated out, the tail is *exactly* the EOO (2880 samples)
    and fails the lower bound. Note both bounds are expressed against
    `Resampler::latencySamples()`, not constants, so the test states its reason.
  - *T9 (3)* completion announced on generation rather than emission — mutated, PTT would drop
    with the EOO still queued.
  - **`flush()` is terminal, so `beginTx()` must `reset()`.** Not in the original T9 write-up.
    Mutated out, the FIRST over transmits perfectly and every one after it is silent — so every
    single-transmission test in the file still passes, and only `aSecondOverStillTransmits`
    catches it. Worth naming as a shape: a resource that is correct to refuse reuse turns a
    missing reset into a second-occurrence bug.
  - *X2* the I/Q sign, guarded by SNR separation rather than by sync — see §7.1b for why sync
    cannot do the job.
- **The inline-data framing — LANDED (`rade_v2_text_channel_test`, 18 tests), five
  mutation-checked.** Dropping the CRC check, dropping `sign(corr)`, dropping the version check,
  dropping the alphabet range check, and removing the magnitude normalisation each break exactly
  one test and nothing else.
  - The corruption test asserts **no *wrong* text**, not merely no output. Those are different
    claims, and the second is the one that matters: every payload bit is flipped in turn across
    47 runs and the assertion is that zero frames decode to anything other than the transmitted
    callsign.
  - **Deliberately outside the `ENABLE_RADE_V2` gate**, like the class itself. It is pure bit
    manipulation with no codec, no Flex protocol and no platform dependency, so it is compiled and
    run by the existing cross-platform CI jobs — none of which set `ENABLE_RADE_V2`. That makes it
    the one piece of this work with macOS/Linux evidence today.
  - **Honest gap:** the "non-consuming rescan" property (§9.2 decision 5) has behavioural coverage
    (`joinsMidStream`, `joinsAfterArbitraryNoise`, `survivesAFalseSyncWord`) but **no mutation
    proof**, because the property is structural — the scan method is `const` and mutates nothing on
    failure, so there is no line to break. Unlike the other five, those three are not known to be
    capable of failing.
- **End-to-end through the codec — LANDED (`rade_v2_engine_test::rxRecoversTheInlineCallsign`).**
  The framing test proves the protocol against ideal ±1 symbols; this proves it survives the
  *autoencoder*, which is not a given — per D3 the symbol is not a separate carrier, so what comes
  back is a reconstruction rather than the bit that went in. Measured: 278 raw symbols, **3 frames
  validated at confidence 1.0**, all matching. Note **3 of roughly 4 available copies** validated on
  a clean, noise-free bench signal, so the channel is already not lossless — direct support for
  repetition-plus-integrity-check over layered FEC, and a number worth re-measuring on air.
- **§7.2 backend-boundary guard — LANDED (`rade_v2_backend_boundary_test`).** Nothing under
  `src/core/backends/` may reference RADE **in code**, and `FlexBackend` may not name the provider,
  the stream or the transport. Deliberately **not** gated on `ENABLE_RADE_V2`: a check that runs
  only with the feature on cannot catch the commit that turns the feature off and leaves the
  coupling behind. Both rules mutation-checked independently, with a comment-only control proving
  prose does not trip it.
  > It strips comments before matching, and that is load-bearing rather than cosmetic. The first
  > version did not, and it fired on the transport's own header for drawing the dependency arrow in
  > a comment — a check that punishes the documentation trains people to delete the documentation.
  > Worse, its own mutation test had used a *commented-out* declaration, so the guard had appeared
  > to work while only proving it could find a string. Re-mutated with real code afterwards.
- **The EOO tail guard — LANDED (`flex_waveform_transport_test`), and the most easily faked test in
  this document.** §7.1 T1+T2 ⇒ T3: with no free-running transmit clock and the radio stopping
  `tx_stream_in` at unkey, the ~120 ms tail needs **synthesized** ticks. A test that keys, waits out
  the tail duration and unkeys **passes with the entire drain mechanism deleted**, because while PTT
  is held the real clock is still running — that is the bug reproduced inside the test meant to
  catch it. Verified empirically: under mutation, `realClockDrivesTicksWhileKeyed` still passes
  while the three tail tests fail. So the assertions are that ticks continue **with inbound
  stopped**, and `tailIsNotDrivenByInbound` pins it by asserting the stream received *zero*
  datagrams during the drain.
  > The mutation also surfaced something the design had not called out: with no drain timer the
  > **tail timeout never fires either**, so a stuck codec would hold the transmitter keyed
  > indefinitely. `setTailTimeoutMs` is therefore a safety property, not a tuning knob — a truncated
  > EOO is a decode problem, a stuck carrier is an interference problem.
- Automation-bridge assertions for the `RAD2` mode lifecycle (activate/deactivate, no mute strand,
  status sub-state).
- **Mode-family classification guard — LANDED (`rade_v2_mode_identity_test`), four mutations.**
  G1–G4 are all *silent*: squelch gating decode and a notch eating OFDM carriers look like codec
  failures, not GUI bugs. They are also invisible to code review — a reviewer reading
  `mode == "DIGU" || mode == "DIGL" || mode == "NT"` has no way to see which mode is missing.
  > **The decisions moved before they could be tested.** The classification was duplicated across
  > **ten** inline sites (four in `RxApplet`, six in `VfoWidget`), so a guard could only have
  > asserted a predicate nothing used. They now route through `src/gui/ModeFamily.h`
  > (`isDigital()`, `usesDiguOffsetPresets()`), which is what makes the test meaningful — and the
  > digital-voice half is **data-driven off the registry's `underlyingMode`**, so RAD2 joins the
  > DIG family because it registered on DIGU, and D-STAR stays out because it registered on DFM.
  > A future mode joins once instead of ten times.
  >
  > **Stated limit:** the test does not instantiate either widget, so it cannot prove a *future*
  > site was wired to the shared functions. It guards the classification, not its adoption.
- **The registry-gate guard — LANDED (`rade_v2_mode_identity_test`, §10.12 G5).** A slice cannot
  claim `RAD2` until `activateMode()` has been called, and `SliceModel::setMode()` returns early
  when the claim is refused — so the combo reads RAD2 while the slice stays in USB. Asserted in
  both directions, including that deactivation releases the claim (it is a global singleton: a
  leaked RAD2 claim blocks every other digital-voice mode for the rest of the session).
  > **Stated limit, same shape as the mode-family guard above:** this asserts the registry's
  > contract, not that `MainWindow` honours it. Nothing here can prove `activateMode()` is still
  > called from the transport's `ready()` handler. The on-air symptom is what found it the first
  > time, and would be what finds it again.
- **A trap found while doing it, with nothing to do with RADE.**
  `filterUnavailableDigitalVoiceModes()` stripped **every registered digital-voice mode** from
  every mode list on a build without `AETHER_ENABLE_DIGITAL_VOICE_HELPER`. That test was
  indistinguishable from "does it need the helper" while D-STAR was the only entry — and RAD2's
  codec is in-process. RAD2 would have vanished from every mode combo, unselectable, with no
  error anywhere. Fixed with a `requiresLocalHelper` flag on the descriptor and guarded in both
  directions (D-STAR's own gating is asserted unchanged).
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
- **§7.1 T6 band-limiting — NOT YET IMPLEMENTED.** Recorded explicitly because everything around
  it in this section is marked LANDED, and a reader would reasonably assume it too. The V2
  modulator does no shaping and `tx_filter` is accepted-and-ignored (§10.10, measured), so our own
  TX chain is the only place sidelobes can be suppressed — and nothing in it does. The 8→24 kHz
  upsampler suppresses *some* incidentally, which is what makes the gap easy to miss. The
  verification method is the panadapter capture immediately below.
- **TX spectrum via the radio's own panadapter (Phase 2/3) — the only integration check available
  for §10.10.** §10.10 concluded the provider must band-limit in its own TX chain, because the V2
  modulator does no shaping (§10.1) and the waveform `tx_filter` is ignored. Without a measurement,
  "we added a filter and it presumably works" would go unverified all the way to on-air decode
  attempts. `transmit set show_tx_in_waterfall=1` makes the radio draw its own TX, and §10.11
  established the pan is a usable instrument: set `y_pixels=256` with an explicit
  `min_dbm`/`max_dbm` window for **~0.37 dB quantisation**, and `bandwidth=0.020` for
  **19.5 Hz/bin** — fine enough to resolve V2's individual carriers at 62.5 Hz spacing.
  - **Method is a before/after, not an absolute.** Capture unshaped and shaped in the *same*
    session with identical pan settings, toggled by a flag. Only our code changes, which cancels
    most calibration concerns. Do not compare captures across sessions.
  - **The baseline must be faithful.** Pure tones will not do — V2's sidelobes come from the
    **hard symbol boundaries**, so the stand-in has to be real OFDM structure (random symbols →
    128-point IDFT → 32-sample CP → hard concatenation at 8 kHz) and then upsampled 8 k→24 k the
    way the provider will. That upsampling filter performs some suppression by itself, which is a
    design detail worth measuring rather than assuming.
  - **Scope limit, stated so it is not overclaimed:** this taps the *digital* TX chain, ahead of
    the analog and PA stages. It shows our shaping composed with the radio's global TX filter and
    where the occupancy lands. It does **not** show PA spectral regrowth or analog filtering, and
    supports **no** spectral-purity or regulatory claim. That still needs an off-air receiver.
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
**FreeDV Reporter integration is Phase 5 as well, and is gated on ecosystem conventions rather
than on us (§9.3).**

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

1. **RX audio output — CLOSED 2026-07-28: option B, the waveform return path** (see §9.1, §6.3).
   The premise this question originally rested on was wrong:
   The ~2.8 kHz cap does not exist; the return path is flat to at least 8 kHz and is not shaped by
   the registered `rx_filter`. So this is no longer "local render, with an optional degraded feed
   for other clients" — it is a straight choice between **A: local render only** and **B: return
   path only**, at equal fidelity. A+B together remains rejected (double-back, now measured).
   **Latency is now measured: ~22 ms remote over a WireGuard VPN, ~16 ms of which is the radio
   itself, so ~16 ms on-site (§10.8)** — negligible beside RADE's own framing and codec delay, and
   measured under realistic remote conditions rather than on a bench. With fidelity equal and
   latency small, **the decision went to B** (§9.1, §6.3), on the §2 grounds that option A *is*
   the "parallel audio side-channel" this design set out to remove.
   **Revisit trigger:** audible quality problems during on-air testing with the real codec.
   Unmeasured and worth watching: SmartLink under Opus (a VPN link is classified LAN and stays
   uncompressed), and whether any use case needs RADE audio while the monitor path is unavailable.
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
   `low_cut`, with honouring unverifiable without transmitting. **Tier 2 is now largely DONE
   (§10.9), on air at 1 W:** the TX path keys and emits on the **inbound** stream id; the power
   transfer is **linear to 0.4 dB with ALC and COMPPEAK at zero**, so RADE V2's OFDM passes
   through intact — the question that could have forced a redesign is answered favourably; the
   radio **holds TX through an underrun** rather than dropping the carrier; and `tx_stream_in` is
   confirmed PTT-gated but carried silence with no mic source connected.
   **Tier 2 is now COMPLETE for everything telemetry can reach (§10.10):** the linearity result is
   re-confirmed with the ATU engaged and SWR at 1.00, closing the mismatch caveat; and `tx_filter`
   is measured **accepted but IGNORED** — two very different registrations gave identical curves,
   with the real filtering coming from the radio's global `lo=100 hi=2900`. Consequence: the
   provider must band-limit **in its own TX chain**, since that is the only per-waveform control.
   **Sideband sense is now CLOSED (§10.11)** — measured via the radio's own panadapter with two
   controls, no receiver required: `DIGU` puts our audio ABOVE the dial, so upper sideband is
   correct and the provider must not conjugate on transmit. That was the largest open risk in the
   TX design. **Q4 is effectively closed**; what is left (sidelobe level, IMD, EVM) is low-value
   and does not block Phase 2.
5. Per-slice audio port on `IRadioBackend` for future non-Flex hosting — tracked, maintainer-gated.
6. **Two clients on one radio — OPEN, and it applies to shipped D-STAR too.** The waveform
   namespace is radio-level: §10.4 saw a *different* client render a slice in a mode registered by
   a script, and §10.12 saw an already-open slice's `mode_list` refresh the moment we registered.
   Registration *lifetime*, though, is connection-scoped — §7.1 N1 measured the radio reaping a
   registration on disconnect and on abrupt kill. A radio-level name with connection-scoped
   ownership is exactly the shape that has a collision question, and **nothing we have measured
   answers it**: no probe has ever run two connections, FlexLib enumerates no duplicate-registration
   error, and the radio publishes no list of *created* waveforms (`waveform` status carries
   `installed_list`, `container` and `wfp_status` — installed packages, not runtime registrations).
   Three possible behaviours:

   | Radio behaviour | Consequence |
   |---|---|
   | rejects the duplicate `create` | client 2 has no RAD2; client 1 unaffected — benign, and already handled |
   | **accepts and replaces** | `udpport` is set per waveform NAME, so client 1's audio is silently rerouted to client 2 |
   | accepts as a distinct instance | both work; `mode_list` may show RAD2 twice |

   The middle one is the §7.1 "code 0 means accepted, never applied" hazard again, and it is
   undetectable from the victim's side. **Our name and mode are compile-time constants and
   registration now fires automatically on every connect**, so two AetherSDR instances on one radio
   both attempt it with no coordination. We also do **not** `waveform remove` before `create`,
   unlike FlexRadio's own reference SDK (`digital_voice_mode_registry.c:170`, *"removing an absent
   registration is harmless"*) and unlike all ten of our probes.

   > **This is not RADE-specific.** D-STAR registers the fixed name `AetherDStar` from a per-PC
   > helper, so two PCs against one radio is the identical collision, live in shipped code today.
   > Whatever is learned here applies there.

   **Settle it with a two-connection experiment before choosing a mitigation** — the probe library
   already exists. Note the obvious fix points the wrong way: adopting the SDK's remove-before-create
   makes *last writer win* deterministically, which is right for clearing our own stale registration
   after an abrupt reconnect and actively wrong for coexisting with another client. `RadioCapabilities`
   now declares `hasMultiClientSessions` (Flex ✅), which is a usable input to that decision.
7. **FreeDV Reporter integration — OPEN, deliberately deferred to Phase 5 (see §9.3).** V1 reports
   to the public map; V2 must not until (a) the callsign framing convention is published (Q2), (b)
   the Reporter `mode` string for V2 is agreed, and (c) a decode-confidence gate stronger than
   `sync` is defined (§7.1b, §10.13). The blocking concern is not effort but **data quality on a
   shared network**: V2's callsign channel is raw and unframed, so reporting it today would publish
   bit errors as other people's callsigns. Station presence, `freq_change` and `tx_report` are
   mode-agnostic and reusable; `rx_report` is the part that must wait.

## 17. References

- `docs/architecture/digital-voice-thumbdv-waveform.md` — D-Star waveform (closest pattern).
- `docs/architecture/aetherd-iradiobackend-design.md` — the backend seam.
- `drowe67/radae_nopy` (`dr-radev2`) — the V2 C port and `rade_api.h`.
- FlexRadio public `waveform-sdk` — the waveform protocol authority.
- Phase 0 spike results (internal memory: `radev2-phase0-findings`).
