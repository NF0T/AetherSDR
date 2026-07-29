#pragma once

// Mode-family classification, shared by RxApplet and VfoWidget.
//
// Both widgets decide the same things about a slice's mode — is it digital, do
// its filter presets hang off `digu_offset` — and both did it with inline
// string comparisons repeated across ten sites. That was survivable while the
// set of digital modes was a fixed literal list. It stops being survivable the
// moment a mode is added, because every site missed is a *silent* misbehaviour:
// squelch that auto-arms and gates decode, an adaptive notch chewing OFDM
// carriers, a filter preset that clips half the modem away. None of them report
// anything; they present as "the codec doesn't work".
//
// RFC docs/architecture/rade-v2-waveform-design.md §10.4 found exactly that set
// (G1–G4) against a stock client, before any RAD2 code existed. These functions
// are where those decisions live now, so a new mode joins the families once
// rather than ten times.
//
// ─── Why the digital-voice half is data-driven ─────────────────────────────
// The registry already knows each digital-voice mode's UNDERLYING mode, and
// that is precisely what decides the family: RAD2 runs on DIGU and belongs with
// the DIG modes; D-STAR runs on DFM and belongs with the FM ones. Reading it
// from the descriptor means the answer cannot drift away from the registration.

#include <QString>

#include "core/DigitalVoiceModeRegistry.h"

namespace AetherSDR::ModeFamily {

// A registered digital-voice mode whose underlying mode is in the USB family.
//
// True for RAD2 (underlying DIGU). False for D-STAR (underlying DFM), and
// false for every non-digital-voice mode — so callers still have to name DIGU /
// DIGL / NT themselves; this only adds the modes the registry knows about.
inline bool isUsbFamilyDigitalVoice(const QString& mode)
{
    const std::optional<DigitalVoiceModeId> id =
        DigitalVoiceModeRegistry::modeForRadioMode(mode);
    if (!id.has_value()) {
        return false;
    }
    const QString& underlying =
        DigitalVoiceModeRegistry::descriptor(id.value()).underlyingMode;
    return underlying == QLatin1String("DIGU")
        || underlying == QLatin1String("USB");
}

// The "DIG" family: squelch disabled, adaptive notch hidden, DIG controls
// shown, digital filter presets.
//
// §10.4 G1/G2 — squelch auto-arms and gates decode; ANF/ANFL/ANFT are
// destructive on OFDM. Both are silent.
inline bool isDigital(const QString& mode)
{
    return mode == QLatin1String("DIGU")
        || mode == QLatin1String("DIGL")
        || mode == QLatin1String("NT")
        || isUsbFamilyDigitalVoice(mode);
}

// Filter presets specified as a WIDTH are built around `digu_offset` for these.
//
// §10.4 G3, and the highest-value item in the set: a mode that misses this
// falls through to `lo = 95; hi = width`, which for a 1500 Hz preset yields
// 95–1500 Hz and clips the top half of V2's 1000–1937.5 Hz occupancy. It
// happens in response to an ordinary operator action, and the only symptom is
// that decode stops.
//
// §10.3 item 7 / §10.4 G4: `digu_offset` is the CENTRE the filter is built
// around, not a passband shift — 1500 Hz wide on a slice with offset 1500 means
// 750–2250 Hz.
inline bool usesDiguOffsetPresets(const QString& mode)
{
    return mode == QLatin1String("DIGU") || isUsbFamilyDigitalVoice(mode);
}

}  // namespace AetherSDR::ModeFamily
