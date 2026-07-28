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

    # Rule 1 — nothing in the backend tree mentions RADE. Case-insensitive so
    # RADEV2Engine, rade_api.h, radae/ and RadeV2 are all caught.
    string(TOLOWER "${contents}" lowered)
    if(lowered MATCHES "rade")
        # Prose in a comment is not coupling. Only flag a real code reference:
        # an #include, or a RADE-named identifier being used.
        if(contents MATCHES "#[ \t]*include[^\n]*[Rr][Aa][Dd][AaEe]"
           OR contents MATCHES "RADEV2Engine"
           OR contents MATCHES "RadeV2Engine"
           OR contents MATCHES "rade_[a-z_]*\\(")
            list(APPEND violations
                 "${rel}: references RADE — §7.2 forbids the backend tree knowing the codec exists")
        endif()
    endif()

    # Rule 2 — FlexBackend names none of the transport trio.
    get_filename_component(stem "${file}" NAME_WE)
    if(stem STREQUAL "FlexBackend")
        foreach(name IN LISTS transport_names)
            if(contents MATCHES "${name}")
                list(APPEND violations
                     "${rel}: names ${name} — §7.2 says FlexBackend is not modified and does not own the transport")
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
