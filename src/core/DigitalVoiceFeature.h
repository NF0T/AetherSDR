#pragma once

#include "DigitalVoiceModeRegistry.h"

#include <QString>
#include <QStringList>

namespace AetherSDR {

#ifdef AETHER_ENABLE_DIGITAL_VOICE_HELPER
inline constexpr bool kLocalDigitalVoiceWaveformAvailable = true;
#else
inline constexpr bool kLocalDigitalVoiceWaveformAvailable = false;
#endif

inline QStringList filterUnavailableDigitalVoiceModes(QStringList modes)
{
    QStringList filteredModes;
    for (const QString& mode : modes) {
        if (!filteredModes.contains(mode)) {
            filteredModes.append(mode);
        }
    }
    modes = filteredModes;

    if constexpr (!kLocalDigitalVoiceWaveformAvailable) {
        // Only the modes that actually NEED the helper. The test used to be
        // "is it a digital-voice mode", which was the same thing while D-STAR
        // was the only one — and would silently strip RAD2, whose codec is
        // in-process, out of every mode list on a build without the helper.
        for (const DigitalVoiceModeDescriptor& mode
             : DigitalVoiceModeRegistry::supportedModes()) {
            if (mode.requiresLocalHelper) {
                modes.removeAll(mode.radioMode);
            }
        }
    }
    return modes;
}

} // namespace AetherSDR
