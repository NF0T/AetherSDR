# Vendoring: RADE V2 (radae_nopy)

- **Upstream:** https://github.com/drowe67/radae_nopy
- **Branch:** `dr-radev2`
- **Commit:** `5374c52013aaded4264b605ba8abdafdef872f43` (2026-07-14, David Rowe)
- **Licence:** BSD-2-Clause (see `LICENSE`)
- **API guide:** `RadeAPIUse.md`, shipped by upstream.

Vendored 2026-07-28 for RFC `docs/architecture/rade-v2-waveform-design.md` stage 6.

## Why this is a SEPARATE tree from `third_party/radae`

`third_party/radae` holds RADE **V1** and is a shipped, working codec. It was
**not** replaced, for a reason that was checked rather than assumed:
`dr-radev2` does not contain the files V1's callsign feature depends on.

    rade_text.c / rade_text.h      absent upstream
    ldpc_codes.*, gp_interleaver.*, mpdecode_core.*, HRA_56_56.*   absent upstream

`RADEEngine` decodes the far-end callsign through
`rade_text_set_rx_callback()` (`src/core/RADEEngine.cpp:129`), which is part of
that subsystem. A straight re-vendor would have deleted it and broken
OTA-confirmed V1 behaviour with no compile error at the call site until link
time — and the RFC's own principle is that **V1 is never touched**; it is
removed later as a clean subtraction, not as a side effect of adding V2.

Upstream also dropped two public functions this project had been using the
shape of (`rade_tx_set_eoo_callsign` / `rade_rx_get_eoo_callsign`, upstream
commit `e57958f`) — in V2 the aux text channel is inline BPSK via
`rade_tx_set_data_symbol()` / `rade_rx_get_data_symbol()` instead.

## The two trees cannot be linked together

Both define `rade_open`, `rade_tx`, `rade_rx`, … so linking both into one
binary is a duplicate-symbol error. They are therefore **mutually exclusive at
link time**: `ENABLE_RADE_V2=ON` builds this tree instead of V1's. That is
acceptable on the development branch and disappears when V1 is deleted.

## Local deltas from upstream

Kept deliberately minimal. Anything here must be re-applied on the next
re-vendor.

1. **Upstream test/tool programs removed** — not built by AetherSDR:
   `rade_ber_test.c`, `rade_bpf_test.c`, `rade_dec_v2_test.c`,
   `rade_enc_v2_test.c`, `rade_v1_text_test.c`, `rade_v2_text_test.c`,
   `rade_tx_wav.c`, `rade_rx_wav.c`, `real2iq.c`, `lpcnet_demo.c`.
   (V1's vendoring does not build `lpcnet_demo.c` either.)
2. **Upstream `CMakeLists.txt` kept as `CMakeLists.upstream.txt`** — reference
   only; AetherSDR drives the build from its own top-level `CMakeLists.txt`,
   as it does for V1.
3. **`RADE_EXPORT` on Win32** — see below. This one is load-bearing.
4. **Upstream `cmake/BuildOpus.cmake` is NOT used.** It fetches Opus over the
   network at configure time (`OPUS_URL`, a GitHub zip of commit `940d4e5a`).
   AetherSDR builds Opus from a vendored local snapshot
   (`third_party/opus-rade`, via `third_party/radae/cmake/BuildOpus.cmake`) so
   builds stay offline and reproducible. The V2 build reuses that same script —
   Opus is one shared dependency, not one per codec version. Upstream's copy is
   retained here for reference only.
   > The prediction here was that a mismatch would "fail loudly at link time."
   > **That was wrong, and the real failure is worse — see below.**

## ⚠ OPEN: V2 needs a SECOND Opus patch that V1 never did

**Status: diagnosed, not yet fixed. The V2 codec builds, links, and then dies
at runtime.**

`rade_v2_codec_test` gets as far as modulating and then aborts:

```
Fatal (internal) error in .../build_opus/dnn/nnet.c, line 128:
assertion failed: layer->nb_inputs <= MAX_CONV_INPUTS_ALL
```

The cause is not the Opus *version*. AetherSDR's vendored snapshot is already
commit `940d4e5a`, byte-identical to the commit upstream's `BuildOpus.cmake`
pins — that hypothesis was checked and disproved.

The cause is that upstream applies **two** patches to Opus and AetherSDR
applies only one. `third_party/radae/src/` carries `opus-nnet.h.diff`;
`third_party/radae_v2/src/` carries that **and** `opus-nnet.c.diff`, which says
exactly what it is for:

```diff
-#define MAX_CONV_INPUTS_ALL DRED_MAX_CONV_INPUTS
+/* Raised from DRED_MAX_CONV_INPUTS (1536) to support RADE V2 decoder
+   conv layers which need up to 1792 inputs (896 channels * kernel_size 2). */
+#define MAX_CONV_INPUTS_ALL 2048
```

V2's decoder has conv layers wider than V1's, so V1's Opus build was correct
for V1 and is simply too small for V2. This is a genuine new requirement, not
drift.

**Why it presents at runtime rather than at link.** `MAX_CONV_INPUTS_ALL` sizes
a stack buffer and guards it with an assertion; nothing about it is visible to
the linker. A release build with `NDEBUG` would skip the assertion and **write
past the buffer instead** — so this must be fixed, not configured around.

**Fix (not yet applied):** AetherSDR's `third_party/radae/cmake/BuildOpus.cmake`
builds from a pre-patched `opus-rade-prepared.tar.gz`, so the second patch has
to be re-baked into that snapshot (or applied post-extract for the V2 path).
Whichever route, the resulting Opus is then **V2-capable and still V1-correct** —
raising the ceiling does not change V1 behaviour — so one shared snapshot can
serve both.

### The Win32 export macro

Upstream declares, unconditionally on Win32:

```c
#if _WIN32
#define RADE_EXPORT __declspec(dllexport) __stdcall     // building
#define RADE_EXPORT __declspec(dllimport) __stdcall     // consuming
#endif
```

AetherSDR links RADE **statically**, where both `dllimport` and `__stdcall`
are wrong: the first makes the linker look for a DLL that does not exist, the
second changes the calling convention out from under a static call. V1's
vendored copy already carries the fix, gated on `RADE_BUILD_DLL`, and the same
shape is applied here.

This is the kind of difference that does not show up until the MinGW link step
and reads as a mysterious missing symbol, so it is recorded rather than
rediscovered.

## Clean-room note

This is BSD-2 and is **linked**, not read-for-protocol. It is unrelated to the
GPL-3.0 `third_party/smartsdr-dsp` posture described in RFC §11, which remains
read-only protocol authority and is never linked.
