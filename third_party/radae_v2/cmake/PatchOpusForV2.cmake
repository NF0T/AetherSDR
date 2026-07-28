# Raise MAX_CONV_INPUTS_ALL in the extracted Opus tree so RADE V2's decoder fits.
#
# Run as an ExternalProject PATCH_COMMAND — after extract, before configure:
#   ${CMAKE_COMMAND} -DOPUS_SRC=<SOURCE_DIR> -P .../PatchOpusForV2.cmake
#
# ─── Why this exists ───────────────────────────────────────────────────────
# V2's decoder has conv layers up to 1792 inputs (896 channels x kernel_size 2).
# Stock Opus sizes a stack buffer with MAX_CONV_INPUTS_ALL = DRED_MAX_CONV_INPUTS
# = 1536 and guards it with an assertion, so V2 aborts on the first rade_tx():
#
#   Fatal (internal) error in dnn/nnet.c, line 128:
#   assertion failed: layer->nb_inputs <= MAX_CONV_INPUTS_ALL
#
# Upstream radae_nopy ships this as src/opus-nnet.c.diff and applies it
# alongside opus-nnet.h.diff. AetherSDR's vendored opus-rade snapshot is
# pre-patched with the .h change only — correct for V1, too small for V2.
#
# NOTE this is a REAL correctness fix, not a debug-only annoyance. The
# assertion is what makes the failure visible; with NDEBUG it is compiled out
# and the code writes past the buffer instead.
#
# ─── Why a CMake script rather than `patch` ────────────────────────────────
# Upstream's PATCH_COMMAND is `sh -c "patch dnn/nnet.c < ..."`, which needs a
# POSIX shell and a `patch` binary. Neither is dependable on Windows, and a
# missing `patch` would fail the build in a way that looks nothing like its
# cause. A string replacement through CMake itself needs no external tooling
# and behaves identically on every platform we build for.
#
# ─── Lifetime ──────────────────────────────────────────────────────────────
# TEMPORARY BRIDGE. The permanent fix is to re-bake opus-rade-prepared.tar.gz
# with both patches, which is best done when RADE V1 is removed and the
# snapshot has exactly one consumer. Raising the ceiling is V1-safe — a larger
# bound changes no V1 behaviour — so one shared snapshot can serve both, and
# this hook can then simply be deleted.

if(NOT DEFINED OPUS_SRC)
    message(FATAL_ERROR "PatchOpusForV2: OPUS_SRC is required")
endif()

set(_nnet "${OPUS_SRC}/dnn/nnet.c")
if(NOT EXISTS "${_nnet}")
    message(FATAL_ERROR
        "PatchOpusForV2: ${_nnet} not found — the Opus tree layout changed. "
        "Do NOT skip this silently: an unpatched build aborts at the first "
        "rade_tx(), or corrupts the stack under NDEBUG.")
endif()

file(READ "${_nnet}" _contents)

# Idempotent: ExternalProject may re-run PATCH_COMMAND, and re-extraction is
# not guaranteed to have restored the original file.
if(_contents MATCHES "MAX_CONV_INPUTS_ALL 2048")
    message(STATUS "PatchOpusForV2: already patched")
    return()
endif()

string(REPLACE
    "#define MAX_CONV_INPUTS_ALL DRED_MAX_CONV_INPUTS"
    "#define MAX_CONV_INPUTS_ALL 2048 /* AetherSDR: RADE V2 decoder needs 1792 */"
    _patched "${_contents}")

# Fail loudly if the anchor moved. A no-op replace would leave the build
# looking healthy right up to the runtime abort — exactly the silent-success
# shape this project keeps getting bitten by.
if(_patched STREQUAL _contents)
    message(FATAL_ERROR
        "PatchOpusForV2: could not find '#define MAX_CONV_INPUTS_ALL "
        "DRED_MAX_CONV_INPUTS' in ${_nnet}. The vendored Opus snapshot has "
        "changed; re-derive this patch from "
        "third_party/radae_v2/src/opus-nnet.c.diff before continuing.")
endif()

file(WRITE "${_nnet}" "${_patched}")
message(STATUS "PatchOpusForV2: raised MAX_CONV_INPUTS_ALL to 2048 for RADE V2")
