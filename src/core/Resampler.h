#pragma once

#include <QByteArray>
#include <memory>
#include <vector>
#include <cstdint>

namespace r8b { class CDSPResampler24; }

namespace AetherSDR {

// High-quality sample rate converter using r8brain-free-src (MIT).
// Wraps r8b::CDSPResampler24 with float32 ↔ double conversion.
//
// Each instance handles one fixed rate ratio. Create separate instances
// for upsample and downsample paths.
//
// Thread safety: NOT thread-safe. Use one instance per thread/path.

class Resampler {
public:
    // maxBlockSamples: max mono samples per process() call
    // reqTransBand: transition band in percent of Nyquist (default 2.0 = ~950 taps; 45.0 = ~42 taps)
    Resampler(double srcRate, double dstRate, int maxBlockSamples = 4096, double reqTransBand = 2.0);
    ~Resampler();

    // Resample mono float32 PCM. Returns resampled mono float32.
    QByteArray process(const float* in, int numSamples);

    // Reuse caller-owned storage. The returned count is mono float samples.
    // Reserving output before the first call keeps steady-state conversion
    // allocation-free for blocks no larger than maxBlockSamples.
    int process(const float* in, int numSamples, QByteArray& output);

    // ─── End of stream ─────────────────────────────────────────────────────
    //
    // Push the filter's remaining memory out and return it.
    //
    // The constructor already calls prewarm() to absorb the startup latency at
    // the HEAD of a stream. This is the symmetric operation at the TAIL: after
    // the last real sample, roughly `latency` samples are still inside the FIR
    // and will never appear unless zeros are pushed through behind them.
    //
    // For continuous audio that loss is inaudible and nobody noticed. For a
    // signal whose LAST samples are the payload it is fatal — RADE's
    // end-of-over frame is exactly that, and RFC §7.1 T9 records V1 having to
    // loosen its TX resampler to ~42 taps to keep the EOO inside a 60 ms
    // window. This is the direct fix for that class of loss.
    //
    // TERMINAL. Zeros are now inside the filter, so feeding more real audio
    // would splice silence into the middle of the stream. After flush() the
    // instance is unusable until reset(); process() will refuse and warn rather
    // than quietly produce a glitch. That refusal is deliberate — "usable again
    // but subtly wrong" is worse than a hard stop.
    QByteArray flush();

    // Return to a clean, prewarmed state. Discards all filter memory, so any
    // un-flushed tail is lost by definition. Call between overs, or after
    // flush() to reuse the instance.
    void reset();

    // True once flush() has been called and reset() has not.
    bool isFlushed() const { return m_flushed; }

    // Filter latency in INPUT samples — how much signal is in flight inside the
    // FIR at any moment, and therefore how long a drain must over-run to let a
    // tail escape (RFC §7.1 T9). 0 when source and destination rates match.
    int latencySamples() const;

    // Convenience: stereo float32 → mono downsample → resampled mono float32
    QByteArray processStereoToMono(const float* stereoIn, int numStereoFrames);

    // Convenience: mono float32 → resampled → duplicated to stereo float32
    QByteArray processMonoToStereo(const float* monoIn, int numSamples);

    // Convenience: stereo float32 → downmix to mono → resample → duplicate to stereo float32
    QByteArray processStereoToStereo(const float* stereoIn, int numStereoFrames);

    // Discard streaming state and consume startup latency again. Call between
    // independent streams, never from the realtime process callback.
    void reset();

    double srcRate() const { return m_srcRate; }
    double dstRate() const { return m_dstRate; }

    // Linear-phase signal delay expressed in source-rate samples. prewarm()
    // consumes r8brain's no-output startup interval, but it does not remove
    // this acoustic delay from the converted waveform.
    int groupDelayInputFrames() const noexcept { return m_groupDelayInputFrames; }

private:
    void prewarm();

    double m_srcRate;
    double m_dstRate;
    int    m_maxBlockSamples;
    double m_reqTransBand;         // retained so reset() can rebuild identically
    std::unique_ptr<r8b::CDSPResampler24> m_resampler;
    int m_groupDelayInputFrames{0};
    std::vector<double> m_inBuf;   // float32 → double conversion buffer
    bool m_flushed = false;        // set by flush(), cleared by reset()
};

} // namespace AetherSDR
