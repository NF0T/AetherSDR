#pragma once

// RX automatic gain control for RADE V2 — RFC §10.20.
//
// `rade_rx()` is LEVEL-DEPENDENT and the vendored C library does not
// normalise. Acquisition and the SNR estimate are scale-invariant, but the
// autoencoder that produces the feature vectors — and with them the inline
// data symbol — is a neural network. Fed ~23 dB below its training level it
// returns wrong features while `rade_sync()` and `rade_snrdB_3k_est()` both
// still read healthy, which is precisely why this cost a day to find: every
// modem-domain indicator says the signal is fine.
//
// Measured threshold on a real off-air capture is about -11 dBFS peak. The
// capture arrived at -23.6 dBFS and decoded as unintelligible babble with the
// callsign channel at chance; the SAME samples scaled up decoded 5 valid
// callsign frames and copyable speech. `rade_api.h` documents no level
// requirement anywhere.
//
// The constants are the reference receiver's, not ours — `radae_v2.py`,
// `RADEv2Receiver._compute_gain`:
//
//     self.agc_target = 1.0 * 10 ** (-3 / 20)   # "PAPR (~3 dB) below peak of 1.0"
//     gain = self.agc_target / (sqrt(mean(abs(rx_in)**2)) + 1e-6)
//     return float(np.clip(gain, 0.1, 10.0))
//
// recomputed per input block and applied before the receive buffer.
//
// This lives in a header shared by the engine and the offline decode tool on
// purpose. A bench tool whose front end differs from the product's is a tool
// that cannot predict the product, and the whole value of the WAV taps is that
// what decodes offline is what would decode live.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace AetherSDR::RadeV2RxAgc {

// Upstream feeds raw int16 MAGNITUDES scaled by 1.22e-4 (see radev2_rx_wav.sh),
// so a full-scale signal reaches the AGC at ~4.0. Our samples are already
// ±1.0 full scale. Without this factor we would sit ~12 dB below the reference
// operating point and the [kMinGain, kMaxGain] clip — which is the AGC's
// entire authority — would engage at the wrong input levels.
inline constexpr float kInputScale = 4.0f;

inline constexpr float kTargetRms  = 0.70794578f;   // 10^(-3/20)
inline constexpr float kMinGain    = 0.1f;          // ±20 dB of authority,
inline constexpr float kMaxGain    = 10.0f;         // exactly as upstream

// Gain for one block of complex samples, in the caller's ±1.0 domain.
// Returns a factor to multiply I and Q by. `n` must be > 0.
inline float blockGain(const float* i, const float* q, int n)
{
    if (n <= 0) return kInputScale;
    double sumSq = 0.0;
    for (int k = 0; k < n; ++k) {
        const double a = double(i[k]) * kInputScale;
        const double b = double(q[k]) * kInputScale;
        sumSq += a * a + b * b;
    }
    const double rms = std::sqrt(sumSq / double(n));
    // +1e-6 guards silence, and matches upstream rather than special-casing it.
    const float g = float(double(kTargetRms) / (rms + 1e-6));
    return kInputScale * std::clamp(g, kMinGain, kMaxGain);
}

// Same, for INTERLEAVED {re, im} pairs — which is exactly the layout of an
// array of RADE_COMP. Lets a caller holding RADE_COMP avoid deinterleaving
// into scratch buffers on the receive path. `n` is the number of PAIRS.
//
// The caller is responsible for asserting sizeof(RADE_COMP) == 2*sizeof(float)
// before casting; this header deliberately does not include rade_api.h.
inline float blockGainInterleaved(const float* iq, int n)
{
    if (n <= 0) return kInputScale;
    double sumSq = 0.0;
    for (int k = 0; k < n; ++k) {
        const double a = double(iq[2 * k])     * kInputScale;
        const double b = double(iq[2 * k + 1]) * kInputScale;
        sumSq += a * a + b * b;
    }
    const double rms = std::sqrt(sumSq / double(n));
    const float  g   = float(double(kTargetRms) / (rms + 1e-6));
    return kInputScale * std::clamp(g, kMinGain, kMaxGain);
}

}  // namespace AetherSDR::RadeV2RxAgc
