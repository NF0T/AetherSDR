#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QList>

#include <optional>

namespace AetherSDR {

enum class DigitalVoiceModeId {
    DStar,
#ifdef AETHER_ENABLE_RADE_V2
    // Gated with the descriptor rather than left permanently visible: with the
    // feature off there is no RAD2 entry to resolve, and descriptor() would
    // silently hand back D-STAR's. A compile error at the call site is the
    // better failure.
    RadeV2,
#endif
};

struct DigitalVoiceModeDescriptor {
    DigitalVoiceModeId id;
    QString settingsId;
    QString displayName;
    QString radioMode;
    QString underlyingMode;
    QString waveformName;

    // Does this mode need the out-of-process digital-voice waveform helper
    // (AETHER_ENABLE_DIGITAL_VOICE_HELPER) to work?
    //
    // D-STAR does: the ThumbDV vocoder lives behind that helper, so without it
    // the mode is offered and cannot function. RADE V2 does not — its codec is
    // in-process. Before this field existed the availability test was "is it a
    // digital-voice mode", which would have stripped RAD2 out of every mode
    // list on any build without the helper, with no error anywhere.
    bool requiresLocalHelper = true;
};

struct DigitalVoiceSliceClaim {
    DigitalVoiceModeId mode{DigitalVoiceModeId::DStar};
    int sliceId{-1};
    QString previousMode;
};

class DigitalVoiceModeRegistry : public QObject
{
    Q_OBJECT

public:
    static DigitalVoiceModeRegistry& instance();

    static const QList<DigitalVoiceModeDescriptor>& supportedModes();
    static const DigitalVoiceModeDescriptor& descriptor(DigitalVoiceModeId id);
    static std::optional<DigitalVoiceModeId> modeForRadioMode(const QString& radioMode);

    bool activateMode(DigitalVoiceModeId id, QString* error = nullptr);
    std::optional<DigitalVoiceSliceClaim> deactivateMode(DigitalVoiceModeId id);
    bool claimSlice(DigitalVoiceModeId id, int sliceId, QString* error = nullptr);
    bool claimSlice(DigitalVoiceModeId id,
                    int sliceId,
                    const QString& previousMode,
                    QString* error = nullptr);
    bool transferSlice(DigitalVoiceModeId id,
                       int sliceId,
                       const QString& previousMode,
                       std::optional<DigitalVoiceSliceClaim>* displaced,
                       QString* error = nullptr);
    void releaseSlice(int sliceId);

    std::optional<DigitalVoiceModeId> activeMode() const;
    int activeSliceId() const;
    std::optional<DigitalVoiceSliceClaim> activeClaim() const;

signals:
    void activeSliceChanged(int sliceId);

private:
    DigitalVoiceModeRegistry() = default;

    mutable QMutex m_mutex;
    std::optional<DigitalVoiceModeId> m_activeMode;
    int m_activeSliceId{-1};
    QString m_previousMode;
};

} // namespace AetherSDR
