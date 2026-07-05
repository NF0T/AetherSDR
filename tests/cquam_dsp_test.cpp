// Standalone tests for the C-QUAM AM Stereo DSP chain (CquamDsp).
// Run: ./build/cquam_dsp_test
//
// Synthesizes a mathematically valid C-QUAM composite signal (envelope
// carries L+R, quadrature phase carries the pre-distorted L-R per the
// Motorola C-QUAM standard the decoder itself documents) and verifies the
// decoder against it — no over-the-air signal or radio needed. This is the
// regression harness for two real bugs found in review:
//   1. An erroneous extra /32768 scale on the input IQ made every absolute
//      epsilon gate in the decoder (L-R gate, pilot-lock threshold) require
//      signal levels ~90dB above what real full-scale IQ ever reaches —
//      pilot lock was unreachable for any real signal. Case 2 pins lock.
//   2. Level-matching work (kAudioGain, AGC) had to survive that fix
//      unchanged. Case 4 pins the output level.

#include "core/CquamDsp.h"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

using AetherSDR::CquamDsp;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-68s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

constexpr double kPilotHz  = 25.0;
constexpr double kPilotAmp = 0.05;   // small vs L/R tone amplitude, as on air

// Synthesize a C-QUAM composite signal at `rate` Hz for `frames` samples,
// carrying tone `lAmp*sin(2*pi*lHz*t)` on L and `rAmp*sin(2*pi*rHz*t)` on R,
// at IQ offset `offsetHz` from centre. Inverts the decoder's own documented
// model: envelope = 1+L+R, quadrature phase = atan2(L-R [+pilot], envelope).
std::vector<float> makeCquam(int rate, int frames, double offsetHz,
                             double lHz, double lAmp,
                             double rHz, double rAmp,
                             bool withPilot)
{
    std::vector<float> iq(static_cast<size_t>(frames) * 2);
    double carrierPhase = 0.0;
    const double twoPi = 2.0 * std::numbers::pi;
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double L = lAmp * std::sin(twoPi * lHz * t);
        const double R = rAmp * std::sin(twoPi * rHz * t);
        double diffLR = L - R;
        if (withPilot) diffLR += kPilotAmp * std::sin(twoPi * kPilotHz * t);
        const double envelope = 1.0 + L + R;
        const double phi = std::atan2(diffLR, envelope);

        carrierPhase += twoPi * offsetHz / rate;
        if (carrierPhase > std::numbers::pi)       carrierPhase -= twoPi;
        else if (carrierPhase < -std::numbers::pi) carrierPhase += twoPi;

        const double total = phi + carrierPhase;
        iq[2 * static_cast<size_t>(i)]     = static_cast<float>(envelope * std::cos(total));
        iq[2 * static_cast<size_t>(i) + 1] = static_cast<float>(envelope * std::sin(total));
    }
    return iq;
}

// Single-bin DFT magnitude at toneHz, over `count` samples at CquamDsp::kAudioRate.
double toneAmplitude(const std::vector<float>& x, size_t from, size_t count,
                     double toneHz, int channelStride, int channelOffset)
{
    double re = 0.0, im = 0.0;
    const double w = 2.0 * std::numbers::pi * toneHz / CquamDsp::kAudioRate;
    for (size_t i = 0; i < count; ++i) {
        const double v = x[(from + i) * static_cast<size_t>(channelStride)
                           + static_cast<size_t>(channelOffset)];
        re += v * std::cos(w * static_cast<double>(i));
        im += v * std::sin(w * static_cast<double>(i));
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

// RMS over interleaved-stereo frames [fromFrame, fromFrame+frameCount), i.e.
// skipping the DC-tracker's startup transient (dcEnv starts at 0 and takes
// ~1000 samples to settle) instead of biasing on it like the full buffer would.
double rmsOfFrameRange(const std::vector<float>& x, size_t fromFrame, size_t frameCount)
{
    double sum = 0.0;
    const size_t from = fromFrame * 2, count = frameCount * 2;
    for (size_t i = from; i < from + count && i < x.size(); ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(count));
}

constexpr double kToneHzL = 1000.0;
constexpr double kToneHzR = 1200.0;
constexpr double kAmpL    = 0.30;
constexpr double kAmpR    = 0.10;

// Steady-state window: skip PLL/Goertzel acquisition transient, then take an
// integer number of cycles of both test tones (LCM-ish window: 20 cycles of
// 1000/1200 Hz at 24 kHz = 480 samples/cycle-set → use 24000 for headroom).
constexpr size_t kSkip = 12000;   // 0.5s @ 24kHz — well past PLL/Goertzel lock-in
constexpr size_t kSpan = 24000;   // 1s window, integer cycles of both tones

} // namespace

int main()
{
    // ── 1. Full-scale carrier, no modulation: locks to unity envelope,
    //       output stays silent (no clipping, no bogus stereo) ────────────
    {
        CquamDsp dsp(24000);
        std::vector<float> out;
        const auto iq = makeCquam(24000, 24000, 0.0, kToneHzL, 0.0, kToneHzR, 0.0, false);
        dsp.process(iq.data(), 24000, out);
        report("unmodulated carrier: no pilot lock",
               !dsp.isPilotLocked());
        // Skip the DC-tracker's startup transient (dcEnv ramps 0→1 over the
        // first ~1000 samples) — measure the settled window, like every
        // other case's kSkip/kSpan analysis window.
        const double rms = rmsOfFrameRange(out, kSkip, kSpan);
        report("unmodulated carrier: near-silent output (no spurious stereo)",
               rms < 0.01, "rms=" + std::to_string(rms));
    }

    // ── 2. Pilot lock is achieved on a real C-QUAM signal (regression test
    //       for the /32768 scale bug that made lock unreachable) ─────────
    {
        CquamDsp dsp(24000);
        std::vector<float> out;
        const auto iq = makeCquam(24000, 24000, 0.0, kToneHzL, kAmpL, kToneHzR, kAmpR, true);
        dsp.process(iq.data(), 24000, out);
        report("C-QUAM signal: pilot lock achieved within 1s",
               dsp.isPilotLocked());
    }

    // ── 3. No pilot present (plain mono AM, e.g. non-stereo station):
    //       decoder must not force stereo separation ──────────────────────
    {
        CquamDsp dsp(24000);
        std::vector<float> out;
        // Mono AM: only L+R is meaningful, feed identical L==R so a broken
        // decoder that fabricates separation would be caught by amplitude
        // mismatch below even without checking isPilotLocked() directly.
        const auto iq = makeCquam(24000, 24000, 0.0, kToneHzL, kAmpL, kToneHzL, kAmpL, false);
        dsp.process(iq.data(), 24000, out);
        report("mono AM (no pilot): stays unlocked", !dsp.isPilotLocked());

        const size_t n = std::min(kSpan, out.size() / 2 - kSkip);
        const double ampL = toneAmplitude(out, kSkip, n, kToneHzL, 2, 0);
        const double ampR = toneAmplitude(out, kSkip, n, kToneHzL, 2, 1);
        report("mono AM: L and R channels match (no fabricated stereo)",
               std::abs(ampL - ampR) < 0.05 * std::max(ampL, ampR),
               "ampL=" + std::to_string(ampL) + " ampR=" + std::to_string(ampR));
    }

    // ── 4. Locked stereo: recovered L/R amplitude ratio matches the
    //       injected ratio, and output level matches the documented
    //       kAudioGain baseline (regression test for the leveling work —
    //       must survive the /32768 fix unchanged) ────────────────────────
    {
        CquamDsp dsp(24000);
        std::vector<float> out;
        // Run twice: PLL/Goertzel state must be warmed up before measuring.
        const auto warm = makeCquam(24000, 24000, 0.0, kToneHzL, kAmpL, kToneHzR, kAmpR, true);
        dsp.process(warm.data(), 24000, out);
        const auto iq = makeCquam(24000, 24000, 0.0, kToneHzL, kAmpL, kToneHzR, kAmpR, true);
        dsp.process(iq.data(), 24000, out);

        report("locked stereo: pilot lock holds on second block", dsp.isPilotLocked());

        const size_t n = std::min(kSpan, out.size() / 2 - kSkip);
        const double ampL = toneAmplitude(out, kSkip, n, kToneHzL, 2, 0);
        const double ampR = toneAmplitude(out, kSkip, n, kToneHzR, 2, 1);
        const double ratio = ampR > 1e-9 ? ampL / ampR : 0.0;
        const double expectedRatio = kAmpL / kAmpR;   // 3.0
        report("locked stereo: L/R amplitude ratio matches injected ratio (~3:1)",
               std::abs(ratio - expectedRatio) < 0.25 * expectedRatio,
               "ratio=" + std::to_string(ratio) + " expected=" + std::to_string(expectedRatio));

        // Baseline level check: a full-scale carrier's recovered audio must
        // land near kAudioGain (~-19dBFS per the corrected doc comment), and
        // never clip (the original reported bug was -0.9dBFS clipping).
        double peak = 0.0;
        for (size_t i = kSkip; i < kSkip + n; ++i) {
            peak = std::max(peak, static_cast<double>(std::abs(out[2 * i])));
            peak = std::max(peak, static_cast<double>(std::abs(out[2 * i + 1])));
        }
        report("locked stereo: output never clips (peak < 1.0)",
               peak < 0.999, "peak=" + std::to_string(peak));
        report("locked stereo: output level near kAudioGain baseline (0.05-0.3 peak)",
               peak > 0.05 && peak < 0.3, "peak=" + std::to_string(peak));
    }

    // ── 5. Chunked processing matches single-shot (state continuity) ─────
    {
        const auto iq = makeCquam(24000, 24000, 0.0, kToneHzL, kAmpL, kToneHzR, kAmpR, true);

        CquamDsp whole(24000);
        std::vector<float> warmWhole;
        whole.process(iq.data(), 24000, warmWhole);   // warm up lock
        std::vector<float> outWhole;
        whole.process(iq.data(), 24000, outWhole);

        CquamDsp chunked(24000);
        std::vector<float> outChunked, block;
        constexpr int kChunk = 1000;   // not a divisor-friendly size on purpose
        for (int pass = 0; pass < 2; ++pass) {
            for (int off = 0; off < 24000; off += kChunk) {
                const int n = std::min(kChunk, 24000 - off);
                chunked.process(iq.data() + 2 * static_cast<size_t>(off), n, block);
                if (pass == 1) outChunked.insert(outChunked.end(), block.begin(), block.end());
            }
        }

        const size_t n = std::min(outWhole.size(), outChunked.size());
        double maxDiff = 0.0;
        for (size_t i = 0; i < n; ++i)
            maxDiff = std::max(maxDiff,
                               static_cast<double>(std::abs(outWhole[i] - outChunked[i])));
        report("chunked == single-shot (no clicks at block edges)",
               n > 40000 && maxDiff < 0.05,
               "n=" + std::to_string(n) + " maxDiff=" + std::to_string(maxDiff));
    }

    if (g_failed) {
        std::printf("\n%d test(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("\nAll tests passed\n");
    return 0;
}
