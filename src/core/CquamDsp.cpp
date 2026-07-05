#include "core/CquamDsp.h"
#include "core/Resampler.h"

#include <QByteArray>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR {

CquamDsp::CquamDsp(int iqRateHz)
    : m_iqRate(iqRateHz > 0 ? iqRateHz : kAudioRate)
{
    // [1] PLL Init
    float omega_n = 2.0f * static_cast<float>(std::numbers::pi) * 10.0f / kAudioRate; // 10 Hz PLL bandwidth
    float zeta = 0.707f;
    m_alpha = 2.0f * zeta * omega_n;
    m_beta = omega_n * omega_n;

    // [2] Goertzel Init (25 Hz)
    float omega = 2.0f * static_cast<float>(std::numbers::pi) * 25.0f / kAudioRate;
    m_goertzelCoeff = 2.0f * std::cos(omega);

    // [3] Resamplers (identical to WfmDsp)
    if (m_iqRate != kAudioRate) {
        constexpr int    kMaxBlock  = 8192;
        constexpr double kTransBand = 5.0;
        m_resampleI = std::make_unique<Resampler>(m_iqRate, kAudioRate, kMaxBlock, kTransBand);
        m_resampleQ = std::make_unique<Resampler>(m_iqRate, kAudioRate, kMaxBlock, kTransBand);
    }
}

CquamDsp::~CquamDsp() = default;

float CquamDsp::maxFreqOffsetHz() const
{
    const float usableHz = 0.95f * 0.5f * static_cast<float>(std::min(m_iqRate, kAudioRate));
    constexpr float kSignalGuardHz = 15000.0f; // Wide AM/C-QUAM requires ~15kHz half-bandwidth
    return std::max(0.0f, usableHz - kSignalGuardHz);
}

void CquamDsp::setFreqOffsetHz(float offsetHz)
{
    m_ncoStep = -2.0 * std::numbers::pi * static_cast<double>(offsetHz) / m_iqRate;
}

void CquamDsp::process(const float* iqInterleaved, int frames, std::vector<float>& audioOut)
{
    audioOut.clear();
    if (!iqInterleaved || frames <= 0) return;

    // [1] NCO mix-down
    m_workI.resize(static_cast<size_t>(frames));
    m_workQ.resize(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        // DAX IQ delivers already-normalized float32 in ±1.0 (same convention
        // WfmDemodulator consumes directly) — do not rescale here. The AGC
        // step below (agcGain = 1/dcEnv) makes final output level invariant
        // to whatever the input scale actually is; rescaling here only
        // matters for the absolute epsilon gates further down, which is why
        // those are now expressed relative to dcEnv instead.
        const float I = iqInterleaved[2 * i];
        const float Q = iqInterleaved[2 * i + 1];
        const float c = static_cast<float>(std::cos(m_ncoPhase));
        const float s = static_cast<float>(std::sin(m_ncoPhase));
        m_workI[static_cast<size_t>(i)] = I * c - Q * s;
        m_workQ[static_cast<size_t>(i)] = I * s + Q * c;
        m_ncoPhase += m_ncoStep;
        if (m_ncoPhase > std::numbers::pi)       m_ncoPhase -= 2.0 * std::numbers::pi;
        else if (m_ncoPhase < -std::numbers::pi) m_ncoPhase += 2.0 * std::numbers::pi;
    }

    // [2] Resample
    const float* pI = m_workI.data();
    const float* pQ = m_workQ.data();
    int n = frames;
    QByteArray bufI, bufQ;
    if (m_resampleI) {
        bufI = m_resampleI->process(pI, frames);
        bufQ = m_resampleQ->process(pQ, frames);
        n  = static_cast<int>(std::min(bufI.size(), bufQ.size()) / static_cast<qsizetype>(sizeof(float)));
        if (n <= 0) return;
        pI = reinterpret_cast<const float*>(bufI.constData());
        pQ = reinterpret_cast<const float*>(bufQ.constData());
    }

    audioOut.resize(static_cast<size_t>(n) * 2);

    float pllPhase = m_pllPhase;
    float pllFreq = m_pllFreq;
    float dcEnv = m_dcEnv;
    float q1 = m_q1;
    float q2 = m_q2;
    int gCount = m_gCount;

    for (int i = 0; i < n; ++i) {
        const float I = pI[i];
        const float Q = pQ[i];

        // 1. Synchronous Demodulation (mix down to baseband relative to carrier)
        const float c = std::cos(pllPhase);
        const float s = std::sin(pllPhase);
        const float I_sync = I * c + Q * s;
        const float Q_sync = -I * s + Q * c;

        // 2. PLL Update
        // The phase error is the angle between the recovered carrier and the I_sync axis.
        const float phase_err = std::atan2(Q_sync, I_sync);
        pllFreq += m_beta * phase_err;
        constexpr float maxFreq = 2.0f * static_cast<float>(std::numbers::pi) * 200.0f / kAudioRate;
        pllFreq = std::clamp(pllFreq, -maxFreq, maxFreq);
        
        pllPhase += pllFreq + m_alpha * phase_err;
        if (pllPhase > std::numbers::pi) pllPhase -= 2.0f * static_cast<float>(std::numbers::pi);
        if (pllPhase < -std::numbers::pi) pllPhase += 2.0f * static_cast<float>(std::numbers::pi);

        // 3. Envelope Extraction (L+R)
        // Cleanest AM demod is simply the magnitude, which is immune to phase fading.
        const float env = std::sqrt(I*I + Q*Q);
        
        // Very slow DC tracker to AC-couple the audio
        dcEnv = dcEnv * 0.999f + env * 0.001f;
        const float L_plus_R = env - dcEnv;

        // 4. C-QUAM Correction for Quadrature (L-R)
        // In Motorola C-QUAM, the transmitted signal is forced to have a mono-compatible
        // envelope (sqrt(I^2+Q^2) = 1+L+R). The quadrature channel is pre-distorted.
        // Unwrapping it requires dividing by cos(phi), which equals I_sync / env.
        float L_minus_R = 0.0f;
        // Gate relative to the tracked carrier envelope (dcEnv), not a fixed
        // absolute epsilon — the DAX IQ float scale is a full-scale ±1.0
        // convention today, but a relative gate stays correct regardless of
        // the input's absolute amplitude, and still avoids dividing by a
        // near-zero I_sync during a deep fade/null.
        const float minCarrier = std::max(dcEnv * 1e-3f, 1e-9f);
        if (std::abs(I_sync) > minCarrier && env > minCarrier) {
            L_minus_R = Q_sync * (env / I_sync);
            // Clamp to avoid extreme spikes on overmodulation or deep fades
            L_minus_R = std::clamp(L_minus_R, -dcEnv, dcEnv);
        }

        // 5. 25 Hz Pilot Detection (Goertzel Filter)
        // The 25Hz pilot tone is broadcast purely in the L-R channel.
        const float q0 = m_goertzelCoeff * q1 - q2 + L_minus_R;
        q2 = q1;
        q1 = q0;
        gCount++;
        
        if (gCount == kGoertzelN) {
            const float energy = q1*q1 + q2*q2 - m_goertzelCoeff * q1 * q2;
            // Normalize energy against the carrier power to set a relative threshold
            const float normEnergy = energy / (kGoertzelN * kGoertzelN * (dcEnv * dcEnv + 1e-9f));
            
            // Hysteresis to prevent the Stereo light from rapidly flickering
            if (m_pilotLocked) {
                if (normEnergy < 1e-4f) m_pilotLocked = false;
            } else {
                if (normEnergy > 2e-4f) m_pilotLocked = true;
            }
            
            q1 = 0.0f;
            q2 = 0.0f;
            gCount = 0;
        }

        // 6. Stereo Matrix
        float outL = L_plus_R;
        float outR = L_plus_R;
        if (m_pilotLocked) {
            outL = L_plus_R + L_minus_R;
            outR = L_plus_R - L_minus_R;
        }

        // 7. AGC: normalize volume relative to the carrier level (dcEnv)
        // This makes the modulation amplitude independent of signal strength.
        float agcGain = 1.0f;
        if (dcEnv > 1e-5f) {
            agcGain = 1.0f / dcEnv;
        }

        // kAudioGain sets the baseline level relative to full-scale (0dBFS),
        // tuned by ear to match native AM/SAM audio at the same slider gain.
        // Current value (0.112f) is ~-19dBFS, not the ~-26dBFS an earlier
        // version of this comment described — the constant below is the
        // source of truth; if the C-QUAM/AM level match needs re-tuning,
        // adjust this constant (not the AGC/dcEnv step above, which only
        // normalizes for input signal strength, not absolute output level).
        float sL = std::clamp(outL * agcGain * kAudioGain, -1.0f, 1.0f);
        float sR = std::clamp(outR * agcGain * kAudioGain, -1.0f, 1.0f);

        audioOut[2 * static_cast<size_t>(i)]     = sL;
        audioOut[2 * static_cast<size_t>(i) + 1] = sR;
    }

    m_pllPhase = pllPhase;
    m_pllFreq = pllFreq;
    m_dcEnv = dcEnv;
    m_q1 = q1;
    m_q2 = q2;
    m_gCount = gCount;
}

} // namespace AetherSDR
