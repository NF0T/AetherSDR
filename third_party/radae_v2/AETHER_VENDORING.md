# Vendoring: RADE V2 (rade_c)

- **Upstream:** https://github.com/freedv/rade_c  — **the repo MOVED**; see below
- **Branch:** `main`
- **Commit:** `c8a3dc156045cae2cd251e1a4be0c304c9ddf2f9` (2026-10-07)
- **Licence:** BSD-2-Clause (see `LICENSE`)
- **API guide:** `RadeAPIUse.md`, shipped by upstream.

Originally vendored 2026-07-28 from `drowe67/radae_nopy@dr-radev2` for RFC
`docs/architecture/rade-v2-waveform-design.md` stage 6. **Re-vendored 2026-10-10.**

## The upstream repo moved (2026-07-13)

The C port is now **`freedv/rade_c`** under the FreeDV organisation.
`drowe67/radae_nopy` is dormant — its `dr-radev2` branch is frozen at `5374c520`,
which is exactly what we had vendored, and that commit is also `rade_c`'s history
seed. So the move is continuous: no re-baselining was needed, only a content
refresh. `drowe67/radae`'s README now points at `rade_c` for "deployment and
distribution".

**`c8a3dc15` is the commit FreeDV's own test builds compile** — freedv-backend's
`cmake/BuildRADE.cmake` tracks `rade_c` `main`. Matching it is what makes us
interoperable with the beta freedv-gui 3.0 used by the RADE V2 test campaign.
Model weights are unchanged since 2026-05-02 and were verified byte-identical
across this re-vendor, so interop is preserved at the model level.

### What the 2026-10-10 refresh brought in (47 upstream commits, 14 files)

- **Rx input AGC, on by default** (`989770e7`, refined to an IIR power estimate in
  `3ec7453b`). This supersedes our own `src/core/RadeV2RxAgc.h`, which reproduced
  the *memoryless* shape upstream replaced; that header is deleted. Disable with
  `rade_rx_set_agc(r, 0)` — which the Level 1 loopback now does explicitly, so the
  verification report's "no AGC in the path" declaration is enforced rather than
  merely asserted.
- **Tx SSB BPF 300–2700 Hz, on by default**, applied to data and EOO through one
  persistent filter (`d5da802c`). The model was trained under this filter. Opt out
  with the new `RADE_NO_TX_BPF` flag.
- **`timing_adj` was never enabled** in the version we had vendored — dead code
  (`f1d3a684`, upstream issue #15). It is now on, which is also the fix for
  upstream #8 (loss varying with where sync lands in the frame).
- **`limit_pitch` clamp** added to the V2 decoder feature path (`00d03329`, #16).
- **BPF and V1 rx phase renormalisation** (`5cb24369`, `a2d77e85`, #14) — the
  long-run magnitude drift that made SNR read −18…−20 dB while still in sync.
  This tree gets the fix for free; `third_party/radae` (V1) needs it applied
  separately.
- **`rade_get_stats()`** — per-step sync/timing/frequency-offset/**AGC gain**/SNR.
- **`RADE_INT16_SCALE`**, and EOO comments corrected: `rade_tx_eoo()` and
  `rade_n_tx_eoo_out()` are documented "V1 and V2" (they always worked for V2 —
  our own reading of `rade_api.c` had established that before upstream fixed the
  comment).

The API delta is **purely additive**: nothing removed, no signature changed, and
no source files added or removed (64 at both commits). Our `RADE_V2_SOURCES` list
in the top-level `CMakeLists.txt` therefore needed no edit — **the risk in this
refresh is entirely behavioural, which is the kind that does not fail to compile.**

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

> **The set below was MEASURED, not recalled, at the 2026-10-10 re-vendor** — our
> `src/` was diffed against the upstream tarball (CRLF-normalised) before and
> after. Result both times: **10 deleted tool files, 0 added, exactly 1 modified
> (`rade_api.h`)**. Re-run that comparison as the control on any future re-vendor;
> it is the only thing that proves a delta was not silently dropped.
>
> The `rade_api.h` delta also does **not** textually collide with upstream's
> changes — ours sits at lines ~39–67, upstream's hunks all start at line 76 or
> later — so re-applying it is mechanical rather than a merge.

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

## RESOLVED: V2 needs a SECOND Opus patch that V1 never did

**Status: FIXED via a post-extract PATCH_COMMAND. `rade_v2_codec_test` now
passes end to end — 226 frames synced, 113 decoded, SNR 24.5 dB on loopback.**

Before the fix, `rade_v2_codec_test` got as far as modulating and then aborted:

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

**Fix as applied.** `BuildOpus.cmake` builds from a pre-patched
`opus-rade-prepared.tar.gz`, so rather than re-baking the snapshot now, the
change is applied **post-extract** through a new `OPUS_EXTRA_PATCH_COMMAND`
hook on `ExternalProject_Add`. The hook is empty by default — an empty
`PATCH_COMMAND` is a no-op — so **the V1 build is unchanged**; only the
`ENABLE_RADE_V2` path sets it, to `cmake/PatchOpusForV2.cmake`.

That script does a guarded string replacement rather than shelling out to
`patch`: upstream's `sh -c "patch ..."` needs a POSIX shell and a `patch`
binary, neither dependable on Windows, and a missing one fails in a way that
looks nothing like its cause. It is idempotent, and it **hard-errors if the
anchor text moves** rather than silently doing nothing — a no-op replace would
leave the build looking healthy right up to the runtime abort.

**TEMPORARY BRIDGE, by design.** The permanent fix is to re-bake the tarball
with both of upstream's nnet patches, which is best done when V1 is removed and
the snapshot has exactly one consumer. Raising the bound is V1-safe — a larger
ceiling changes no V1 behaviour — so one snapshot can then serve everything and
this hook plus `PatchOpusForV2.cmake` can be deleted outright.

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
