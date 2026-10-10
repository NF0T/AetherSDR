# Vendoring: RADE V2 callsign/text channel (freedv-backend)

- **Upstream:** https://github.com/tmiw/freedv-backend
- **Branch:** `radev2-dev`
- **Commit:** `6053eaab16e5e376280db209eb4c1001a83689bf` (2026-10-10)
- **Licence:** BSD-2-Clause, Copyright (c) 2026 Mooneer Salem (see `LICENSE`)
- **Spec:** `docs/reporting-protocol.md` §1 in that repo — 529 lines covering the
  alphabet, packing, payload layout, LDPC, CRC, interleaving and both paths.

Vendored 2026-10-10, replacing AetherSDR's own interim framing
(`src/core/RadeV2TextChannel`, now deleted). This is the wire format FreeDV's
beta freedv-gui 3.0 transmits, so adopting it is what makes our callsign channel
interoperable rather than merely present.

## Why this is a SEPARATE tree from `third_party/radae_v2`

Different upstream, different release cadence. `radae_v2` is the **codec**
(`freedv/rade_c`); this is the **text channel**, which lives in the shared
backend that freedv-gui builds on. They are re-vendored independently and pinned
independently.

## Why we adopted upstream's framing instead of keeping ours

Our interim scheme (Barker-13 sync, CRC-16, 6-bit alphabet, no FEC) was designed
to be deleted the moment the ecosystem published one — RFC §9.2 committed to
that explicitly. Five things made this the moment:

1. **It is specified, not just implemented** — a written 529-line protocol doc.
2. **BSD-2-Clause**, so linking is clean (contrast the GPL-3.0 `smartsdr-dsp`
   posture, which stays read-only protocol authority and is never linked).
3. **The over-the-air format is stable while the receiver improves.** Upstream's
   PR #67 reworked the entire receive side (+1406/−219) and states: *"There is no
   over-the-air change, so it works with existing transmitters."*
4. **The LDPC code is bit-for-bit one we already shipped.** Verified, not
   assumed: upstream's `HRA_56_56.h` stores a dense `[56][112]` matrix,
   `third_party/radae/src/HRA_56_56.c` stores the sparse `H_rows`/`H_cols` index
   form. Reconstructing ours to dense and comparing gives an **identical data
   half (cols 0–55, 168 ones each)**; upstream's extra 111 ones are cols 56–111,
   confirmed to be exactly the dual-diagonal accumulator (`I` + shifted `I`) our
   sparse form leaves implicit. Same code, two representations.
5. **Receive combining we would not have reproduced.** Soft symbols are summed
   modulo the cycle length so every repeat adds SNR. Upstream measured ≥90%
   decode of a 1-block callsign improving from ≈−1 dB to **≈−6 dB** (AWGN) and
   +2 dB to **≈−4.8 dB** (MPP).

Point 5 also settles an open question in our own notes from the other direction:
[[reference_radev2_data_channel]] argued the voice/data coupling meant
"repetition plus a cheap integrity check" rather than FEC, and RFC §10.14's
measurement disagreed and indicated *soft accumulation across repetitions*.
Upstream reached the same conclusion and has the SNR figures for it.

## The wire format, in brief

LDPC(112,56) codeword per block, no sync word. The receiver finds alignment by
sweeping all 112 rotations and accepting only when belief propagation converges
(≤10 iterations), CRC-8 passes, **and** that rotation is the unique winner of the
sweep. 56-bit payload:

    [ CRC-8 (8) | last_block (1) | block_index (2) | 8 chars base-38 (42) | reserved (3) ]

38-symbol alphabet: null, `0-9`, `A-Z`, `/` — **no space and no hyphen.**
Characters pack as one base-38 integer (5.25 bits/char), so 8 fit in 42 bits
where 6 fixed bits each would need 48. Up to 4 blocks = **32 characters**;
interleaver `(37·i) mod 112`. One block is 112 symbols at one per ~40 ms step =
**4.48 s**.

## Local deltas from upstream

**The eight files under `pipeline/` are BYTE-IDENTICAL to upstream** — verified
by hash at vendor time. There is exactly one addition:

1. **`util/logging/ulog.h` is OURS** — a ~50-line shim, not upstream code.

`rade_text.cpp` does `#include "../util/logging/ulog.h"`. Rather than edit that
line, **this tree mirrors upstream's directory shape** (`pipeline/` beside
`util/logging/`) so the relative include resolves to our shim. That is what keeps
the vendored sources unmodified and makes the next re-vendor a straight copy with
no patch step — re-verify the hashes as the control.

The shim maps upstream's two used macros (`log_debug` ×5, `log_info` ×1) onto the
`aether.radev2.text` Qt logging category, so these lines obey the same runtime
filtering as the rest of the codec. Vendoring `ulog.c` plus `ulog_async.cpp`
(~1000 lines, its own thread and ring buffer) to serve six call sites would be
the wrong trade: two macros is a smaller seam than two logging frameworks.

> ⚠ `Q_LOGGING_CATEGORY(lcRadeV2Text, …)` is defined at **global scope** in
> `src/core/RADEV2Engine.cpp`, deliberately. `rade_text.cpp` is upstream code at
> global scope, so its `qCDebug` resolves `::lcRadeV2Text()`. Defining the
> category inside `namespace AetherSDR` compiles and then fails at link with
> *undefined reference to `lcRadeV2Text()`* from `rade_text.cpp.obj`.

## Build

Three translation units (`rade_text.cpp`, `ldpc_encode.cpp`, `ldpc_decode.cpp`)
are appended to `RADE_SOURCES` inside the `ENABLE_RADE_V2` guard in the
top-level `CMakeLists.txt`, with warnings suppressed on the same grounds as the
codec: third-party source we re-vendor wholesale, so a warning is noise we cannot
act on without forking it. The include path needs the tree **root** as well as
`pipeline/`, for the `../util/logging/ulog.h` resolution above.

## What we do NOT take from this repo

Only the text channel. freedv-backend is a whole audio pipeline (its own AGC,
FIFOs, resampling, reporting client); AetherSDR has those already. Keeping the
import to the eight files plus a shim is what makes the boundary reviewable.

## Clean-room note

BSD-2 and **linked**, not read-for-protocol. Unrelated to the GPL-3.0
`third_party/smartsdr-dsp` posture in RFC §11.
