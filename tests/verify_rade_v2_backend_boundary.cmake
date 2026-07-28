# RFC §7.2 — the RADE V2 / radio-backend dependency must point ONE way.
#
#     RADEV2Engine → FlexWaveformTransport → command sink
#
# AetherSDR has three IRadioBackend implementors now (Flex, HL2, Sim) and will
# gain more. IRadioBackend is a *radio* abstraction; RADE is a *codec* with one
# transport. If a backend ever learns what RADE is, every backend added after it
# inherits the question "and does this one do RADE?" — which is not a question
# about radios. §7.2 decided it does not, and this is the check that keeps it
# true after the decision has scrolled out of everyone's memory.
#
# Two rules, both greps:
#
#   1. No file under src/core/backends/ may reference RADE. The Flex waveform
#      transport lives in that tree (it speaks the Flex wire protocol, and EB3
#      permits that only below the seam) — but it is named for the WAVEFORM API,
#      not for its one consumer, and it must stay that way.
#
#   2. FlexBackend specifically may not name the provider, the stream, or the
#      transport. It is the file most likely to acquire the coupling, because
#      "just hand it the sink from in here" is a two-line change that reads as
#      convenience and quietly inverts the architecture.
#
# Runs unconditionally — NOT gated on ENABLE_RADE_V2. A guard that only runs
# when the feature is on cannot catch the commit that turns the feature off and
# leaves the coupling behind.

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(backend_dir "${SOURCE_DIR}/src/core/backends")
if(NOT IS_DIRECTORY "${backend_dir}")
    message(FATAL_ERROR "backend tree not found: ${backend_dir}")
endif()

file(GLOB_RECURSE backend_files "${backend_dir}/*.h" "${backend_dir}/*.cpp")
if(backend_files STREQUAL "")
    message(FATAL_ERROR "no backend sources scanned — the guard would pass vacuously")
endif()

# The transport trio is allowed to exist in this tree; it is what §7.2 places
# there. What is forbidden is any of it naming RADE, or FlexBackend naming any
# of it.
set(transport_names "FlexWaveformProvider" "FlexWaveformStream" "FlexWaveformTransport")

set(violations "")
set(scanned 0)

foreach(file IN LISTS backend_files)
    file(READ "${file}" contents)
    file(RELATIVE_PATH rel "${SOURCE_DIR}" "${file}")
    math(EXPR scanned "${scanned} + 1")

    # Strip comments before matching anything.
    #
    # Prose is not coupling, and this guard is worthless if it cannot tell the
    # difference. The transport's own header legitimately draws the dependency
    # arrow "RADEV2Engine -> FlexWaveformTransport -> command sink" in a comment
    # — documenting the rule is the opposite of breaking it, and a check that
    # punishes the documentation trains people to delete the documentation.
    #
    # This also matters for the guard's OWN mutation test: without stripping, a
    # commented-out `// FlexWaveformProvider* m_rade;` trips it, so the guard
    # looks like it works while actually only proving it can find a string.
    # Mutations must be real code.
    #
    # Block comments first (CMake regex has no non-greedy, hence the classic
    # /*-consuming form), then line comments.
    string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" " " code "${contents}")
    string(REGEX REPLACE "//[^\n]*" " " code "${code}")

    # Rule 1 — nothing in the backend tree references RADE in code.
    # Case-insensitive on the stem so RADEV2Engine, rade_api.h, radae/ and
    # RadeV2 are all caught.
    string(TOLOWER "${code}" lowered)
    if(lowered MATCHES "rade")
        if(code MATCHES "#[ \t]*include[^\n]*[Rr][Aa][Dd][AaEe]"
           OR code MATCHES "RADEV2Engine"
           OR code MATCHES "RadeV2Engine"
           OR code MATCHES "rade_[a-z_]*\\(")
            list(APPEND violations
                 "${rel}: references RADE in code — §7.2 forbids the backend tree knowing the codec exists")
        endif()
    endif()

    # Rule 2 — FlexBackend names none of the transport trio, in code.
    get_filename_component(stem "${file}" NAME_WE)
    if(stem STREQUAL "FlexBackend")
        foreach(name IN LISTS transport_names)
            if(code MATCHES "${name}")
                list(APPEND violations
                     "${rel}: names ${name} in code — §7.2 says FlexBackend is not modified and does not own the transport")
            endif()
        endforeach()
    endif()
endforeach()

if(NOT violations STREQUAL "")
    string(REPLACE ";" "\n  " pretty "${violations}")
    message(FATAL_ERROR
        "RADE V2 backend-boundary violations (RFC §7.2):\n  ${pretty}\n\n"
        "The dependency points one way: RADEV2Engine -> FlexWaveformTransport -> command sink.\n"
        "If the transport needs something from the radio, give it a sink; do not give the radio\n"
        "a reference to the transport.")
endif()

message(STATUS "RADE V2 backend boundary: ${scanned} file(s) scanned, clean (RFC §7.2)")
