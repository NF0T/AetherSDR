#pragma once

#include <array>
#include <memory>
#include <vector>

namespace AetherSDR {

class Resampler;

// ---------------------------------------------------------------------------
// CquamDsp — pure DSP core of the C-QUAM (AM Stereo) decoder.
//
// No Qt audio, no radio I/O, fully unit-testable.
//
// Pipeline:
//   IQ @ native device rate
//     → [1] NCO mix-down (phase-continuous offset correction)
//     → [2] identical twin resamplers → exactly 48 kHz
//     → [3] Carrier PLL (locks onto AM carrier)
//     → [4] Envelope detect (L+R) and Quadrature demod (L-R) with C-QUAM correction
//     → [5] 25 Hz Pilot detection via Goertzel filter
//     → [6] Stereo Matrix (auto mono-fallback)
//     → stereo float audio @ 48 kHz
// ---------------------------------------------------------------------------
class CquamDsp
{
public:
    static constexpr int kAudioRate = 24000;
    static constexpr float kAudioGain = 0.112f; // Baseline volume matching radio's native AM audio level (~ -26dB)

    explicit CquamDsp(int iqRateHz);
    ~CquamDsp();

    int iqRateHz() const { return m_iqRate; }

    float maxFreqOffsetHz() const;
    void setFreqOffsetHz(float offsetHz);

    // Demodulate `frames` interleaved IQ pairs at iqRateHz().
    // audioOut will contain stereo (L,R interleaved) 48 kHz audio.
    void process(const float* iqInterleaved, int frames, std::vector<float>& audioOut);

    bool isPilotLocked() const { return m_pilotLocked; }

private:
    const int m_iqRate;

    // [1] NCO
    double m_ncoPhase{0.0};
    double m_ncoStep{0.0};

    // [2] Twin resamplers
    std::unique_ptr<Resampler> m_resampleI;
    std::unique_ptr<Resampler> m_resampleQ;
    std::vector<float> m_workI, m_workQ;

    // [3] PLL State
    float m_pllPhase{0.0f};
    float m_pllFreq{0.0f};
    float m_alpha;
    float m_beta;

    // DC Trackers for AC coupling
    float m_dcEnv{0.0f};

    // [5] 25 Hz Pilot Detector (Goertzel at 24kHz, N=960)
    static constexpr int kGoertzelN = 960; 
    float m_goertzelCoeff;
    float m_q1{0.0f};
    float m_q2{0.0f};
    int   m_gCount{0};
    bool  m_pilotLocked{false};
    float m_pilotEnergy{0.0f};
};

} // namespace AetherSDR
