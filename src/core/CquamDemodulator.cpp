#include "core/CquamDemodulator.h"
#include "core/CquamDsp.h"
#include "core/LogManager.h"
#include "models/DaxIqModel.h"

namespace AetherSDR {

CquamDemodulator::CquamDemodulator(QObject* parent) : QObject(parent) {}

CquamDemodulator::~CquamDemodulator() { stop(); }

void CquamDemodulator::start(DaxIqModel* daxIq, const QString& panId)
{
    if (m_active) stop();

    m_daxIq = daxIq;
    m_panId = panId;
    m_panSent = false;
    m_dsp.reset();
    m_lastPilotLocked = false;

    connect(m_daxIq, &DaxIqModel::iqSamplesReady, this, &CquamDemodulator::onIqSamples);
    connect(m_daxIq, &DaxIqModel::streamChanged,  this, &CquamDemodulator::onStreamChanged);
    m_daxIq->createStream(DAX_CHANNEL);

    m_active = true;
    qCDebug(lcAudio) << "CquamDemodulator: started on panId=" << m_panId;
}

void CquamDemodulator::stop()
{
    if (!m_active) return;
    m_active = false;

    if (m_daxIq) {
        m_daxIq->removeStream(DAX_CHANNEL);
        disconnect(m_daxIq, nullptr, this, nullptr);
        m_daxIq = nullptr;
    }
    m_dsp.reset();
}

void CquamDemodulator::setFreqOffsetHz(float offsetHz)
{
    m_freqOffsetHz = offsetHz;
    if (m_dsp) m_dsp->setFreqOffsetHz(offsetHz);
}

float CquamDemodulator::maxFreqOffsetHz() const
{
    return m_dsp ? m_dsp->maxFreqOffsetHz() : 14800.0f;
}

void CquamDemodulator::ensureDsp(int iqRateHz)
{
    if (m_dsp && m_dsp->iqRateHz() == iqRateHz) return;
    m_dsp = std::make_unique<CquamDsp>(iqRateHz);
    m_dsp->setFreqOffsetHz(m_freqOffsetHz);
    qCInfo(lcAudio) << "CquamDemodulator: DSP chain created at" << iqRateHz << "Hz";
}

void CquamDemodulator::onStreamChanged(int channel)
{
    const auto& s = m_daxIq->stream(DAX_CHANNEL);
    if (channel != DAX_CHANNEL || m_panSent || m_panId.isEmpty()) return;
    if (!s.exists || s.streamId == 0) return;
    
    // Bind DAX IQ channel to the slice's panadapter
    const QString cmd = QString("stream set 0x%1 pan=%2").arg(s.streamId, 0, 16).arg(m_panId);
    emit commandReady(cmd);
    m_panSent = true;
}

void CquamDemodulator::onIqSamples(int channel, QVector<float> iq, int sampleRate)
{
    if (channel != DAX_CHANNEL || !m_active) return;
    ensureDsp(sampleRate > 0 ? sampleRate : AUDIO_RATE);
    processIqFloat(iq.constData(), iq.size() / 2);
}

void CquamDemodulator::processIqFloat(const float* iq, int frames)
{
    if (!m_dsp || frames <= 0) return;

    m_dsp->process(iq, frames, m_audioBuf);
    
    // Notify UI if the pilot lock state changes
    bool locked = m_dsp->isPilotLocked();
    if (locked != m_lastPilotLocked) {
        m_lastPilotLocked = locked;
        emit pilotLocked(locked);
    }

    const int n = static_cast<int>(m_audioBuf.size());
    if (n <= 0) return;

    QByteArray pcm(n * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* out = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < n; ++i) {
        out[i] = std::clamp(m_audioBuf[static_cast<size_t>(i)], -1.0f, 1.0f) * m_volume;
    }

    emit cquamAudioReady(pcm);
}

} // namespace AetherSDR
