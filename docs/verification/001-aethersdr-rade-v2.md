# RADE Integration Verification Report — AetherSDR

> **STATUS: INCOMPLETE — NOT YET SUBMITTABLE.** Level 1 passes. The procedure
> requires **at least one of Level 2 or Level 3**, and neither has been done.
> Level 2 (OTAC) is genuinely N/A for this integration; Level 3 (OTC) is
> blocked on hardware. See *Blockers* at the end.
>
> Filled against `doc/verification/template.md` from
> `drowe67/radae@dr-radev2`. When complete, copy to that repo as
> `doc/verification/<serial>-aethersdr.md` and submit as a PR.

## Application Under Test

| Field | Value |
|---|---|
| Application name | AetherSDR (RADE V2 as an in-process Flex waveform) |
| Application version / git hash | `818fff0a`, branch `feat/rade-v2-waveform-provider` |
| Platform (OS + version) | Windows 11 (10.0.26200), MinGW GCC 13.1.0, Qt 6.11.0 |
| Tester name / callsign | NF0T |
| Date | 2026-08-03 |
| radae repo commit hash | `25d58fe` (branch `dr-radev2`) |
| rade_c repo commit hash | **N/A — see note** |

> **On `rade_c`:** AetherSDR does not use `rade_c`. It vendors
> **`drowe67/radae_nopy@dr-radev2`**, commit
> `5374c52013aaded4264b605ba8abdafdef872f43` (2026-07-14), built directly into
> the application as `third_party/radae_v2`. The device under test is therefore
> AetherSDR's own C integration of that library, not the `rade_c` WAV tools.

## Signal Path Declaration

- [x] No additional signal processing (AGC, noise gate, resampler, EQ,
      compression) between WAV file input and RADE encoder input during
      this verification test

**Basis for the declaration.** The Level 1 test feeds `wav/all.wav` (16 kHz mono)
directly to `lpcnet_compute_single_frame_features()`. AetherSDR's live audio path
does contain rate conversion — the application carries 24 kHz audio, so it has
24↔16 kHz and 8↔24 kHz stages — but **none of them is in the path for this test**,
because the input is already at the rate the encoder wants. The feature vectors
are exported at the encoder input and the decoder output with nothing between
them and the codec.

For completeness, those resamplers have been measured where they *are* in the
path (they will be for Level 3):

| stage | measured effect |
|---|---|
| 24 kHz stereo → 16 kHz mono | mean **+0.05 dB**, sd 0.67 dB over 100–3600 Hz vs an ideal polyphase resample |
| 8 → 24 kHz TX upsampler (`ReqTransBand=45`) + real-part extraction | **0.1 dB** (decoder SNR 15.6 vs 15.5 dB) |

## Baseline Loss (Step 1)

Not re-derived locally — the Python reference baseline quoted in
`verification_procedure.md` for model `250725` / commit `b549586` was used:

| Field | Value |
|---|---|
| Baseline loss | 0.081 |
| 10% tolerance window | 0.0729 – 0.0891 |

## Level 1 — Software Loopback (mandatory)

- [x] **Pass** (loss within ±10% of baseline)

| Field | Value |
|---|---|
| Loss result | **0.081** (`start: 224`, `acq_time: 1.24 s`) |

Command used:

```
# device under test: AetherSDR's own C integration, file in -> file out, no hardware
rade_v2_loopback wav/all.wav out.wav \
    --features-tx all_tx.f32 --features-rx all_rx.f32

# official metric, from the radae repo
python loss.py all_tx.f32 all_rx.f32 --clip_start 100 --clip_end 300
```

Output:

```
Loss between all_tx.f32 and all_rx.f32
  loss: 0.081 start: 224 acq_time:  1.24 s
```

With `--loss_test 0.0902` the script prints `PASS` and exits 0.

**Cross-checks.** The result tracks the worked example in the procedure closely,
including the transient behaviour it describes:

| | procedure's rade_c example | AetherSDR |
|---|---|---|
| unclipped loss | 0.113 | **0.111** |
| clipped loss | 0.082 | **0.081** |
| acq_time | 1.24 s | **1.24 s** |

Per-frame statistics (`--stats`): mean 0.081, median 0.069, p95 0.170,
p99 0.289, max 1.273; 117 / 4548 frames (2.6%) above the 0.208 outlier
threshold.

## Level 2 — OTAC: Over The Audio Cable

- [x] **N/A**

**Reason.** AetherSDR's RADE V2 integration uses **no sound card in the RADE
signal path**. Modulated audio leaves the application as VITA-49 over UDP to the
radio's waveform stream, and decoded audio returns the same way. There is no
DAC/ADC and no audio cable to loop, so the failure modes OTAC exists to catch
(dropped sound-card buffers, sample-rate mismatch, bit-depth errors) do not
apply in the form the procedure anticipates.

**Offered instead, if the RADE team considers it useful:** a VITA-49 loop that
exercises the equivalent transport layer — inject on `rx_stream_in`, capture the
returned slice audio via `remote_audio_rx`, no RF. This covers our packetisation,
the UDP path, the radio's stream ingest and the return stream. It does **not**
cover the transmit modem path, which cannot be looped without RF, so it is
supplementary evidence rather than a substitute.

## Level 3 — OTC: Over The Coax

- [ ] Pass
- [x] **Not performed — BLOCKED**

| Field | Value |
|---|---|
| Tx radio | FLEX-8400, firmware 4.2.20.41343 |
| Rx radio | *none available* |

**Blocker.** The FLEX-8400 in this station reports `num_scu=1`, so receive is
blanked while transmitting. A single-radio coax loopback is therefore impossible
and a second receiver is required. An SDR receiver plus attenuators would
satisfy this and is the intended next step.

## Summary

| Level | Result |
|---|---|
| Level 1 — Software loopback | **PASS** |
| Level 2 — OTAC | N/A (no sound card in the RADE path) |
| Level 3 — OTC | NOT PERFORMED (needs a second receiver) |

**Not submittable as it stands**, since the procedure requires Level 2 or
Level 3 and Level 2 is N/A here.

## Additional notes

**On multi-platform submission.** The procedure asks for a separate form per
platform "as sound hardware drivers differ". That rationale does not apply to
this integration — there are no sound drivers in the RADE path. A different
platform-dependent surface does exist, however, and is worth stating plainly:
the VITA-49 wire format is big-endian and has only ever been exercised on
little-endian Windows, and UDP bind behaviour differs across platforms. Those
are covered by AetherSDR's own cross-platform CI rather than by this procedure.
We would propose one form plus this note, rather than three forms, but will
follow the RADE team's preference.

**Method note offered in case it is useful to others.** A first Level 1 run used
a convenient local speech file and returned loss 0.002 — apparently 40× better
than baseline. That file was itself RADE **V1-decoded** audio, so it already sat
on the manifold the autoencoder reconstructs best. The mandated `wav/all.wav`
immediately produced a plausible number. The insistence on a standard input is
well founded and worth keeping prominent in the procedure.
