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
   > If V2 ever needs FARGAN symbols newer than the vendored snapshot, it will
   > fail loudly at link time. That is the intended failure mode: a fast,
   > falsifiable signal beats pre-emptively re-vendoring Opus on a guess.

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
