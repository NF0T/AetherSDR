#pragma once
#include <QObject>
#include <QVector>
#include <QByteArray>
#include <memory>
#include <vector>

namespace AetherSDR {

class DaxIqModel;
class CquamDsp;

// ---------------------------------------------------------------------------
// CquamDemodulator — device plumbing around CquamDsp.
//
// Hooks DAX IQ, centers it on the slice (Doppler tracks via NCO),
// feeds the C-QUAM DSP, and emits stereo audio to be injected into
// the main AudioEngine.
// ---------------------------------------------------------------------------
class CquamDemodulator : public QObject
{
    Q_OBJECT
public:
    static constexpr int DAX_CHANNEL = 1;
    static constexpr int AUDIO_RATE  = 48000;
    static constexpr int FILTER_HZ   = 15000;

    explicit CquamDemodulator(QObject* parent = nullptr);
    ~CquamDemodulator() override;

    void start(DaxIqModel* daxIq, const QString& panId = QString());
    void stop();

    bool isActive() const { return m_active; }

    void setFreqOffsetHz(float offsetHz);
    float maxFreqOffsetHz() const;

    void setVolume(int pct) { m_volume = std::clamp(pct / 100.0f, 0.0f, 1.0f); }

signals:
    void commandReady(const QString& cmd);
    void cquamAudioReady(const QByteArray& pcm);
    void pilotLocked(bool locked);

public slots:
    void onIqSamples(int channel, QVector<float> iqInterleaved, int sampleRate);
    void onStreamChanged(int channel);

private:
    void ensureDsp(int iqRateHz);
    void processIqFloat(const float* iq, int frames);

    DaxIqModel*         m_daxIq{nullptr};
    bool                m_active{false};
    bool                m_lastPilotLocked{false};
    std::unique_ptr<CquamDsp> m_dsp;
    std::vector<float>        m_audioBuf;
    float                     m_freqOffsetHz{0.0f};
    float                     m_volume{1.0f};

    QString m_panId;
    bool    m_panSent{false};
};

} // namespace AetherSDR
