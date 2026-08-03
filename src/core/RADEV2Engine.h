#pragma once

// RADEV2Engine — the RADE V2 codec, and nothing else (RFC:
// docs/architecture/rade-v2-waveform-design.md §7, §9).
//
// The missing middle of stage 6. Both sides of it already exist and are
// verified: `FlexWaveformTransport` carries 24 kHz passband in both directions,
// and `rade_v2_codec_test` proves the vendored codec modulates and demodulates.
// This is the chain between them:
//
//   RX:  24 kHz complex → 8 kHz → rade_rx() → features → FARGAN
//        → 16 kHz speech → 24 kHz → decodedAudioReady()
//   TX:  mic 24 kHz → 16 kHz → lpcnet features → rade_tx()
//        → 8 kHz → 24 kHz → modulatedAudioReady()
//
// ─── Three sample rates, and none of them is negotiable ────────────────────
//   24 kHz  the waveform streams, both directions (§7.1 X1, E2)
//    8 kHz  RADE_MODEM_SAMPLE_RATE
//   16 kHz  RADE_SPEECH_SAMPLE_RATE — lpcnet in, FARGAN out
//
// ─── Why the seam is signals, not an interface ─────────────────────────────
// This is a worker-thread QObject. Signal/slot connections auto-queue across
// that boundary; an interface pointer would make every caller responsible for
// getting the threading right, and getting it wrong produces data races rather
// than compile errors. So there is deliberately no `IWaveformCodec`, and this
// class names no transport — the wiring connects the two from outside.
//
// ─── §7.1 T9: the TX resampler eats the end-of-over ────────────────────────
// The single most expensive thing to get wrong here, because it fails in
// silence. Three obligations, all of them load-bearing:
//
//   1. The 8→24 kHz upsampler runs at `ReqTransBand=45`, not the default.
//      Measured latency: default **2376 input samples = 297 ms**; band 45
//      **69 samples = 8.6 ms**. A 297 ms filter smears a 120 ms EOO frame
//      across nearly three times its own duration, and the far end's
//      correlator — which matches against a known template — stops seeing it.
//   2. `flush()` the upsampler after the EOO, or the last ~8.6 ms of the tail
//      never leaves the filter at all.
//   3. Keep the transport's drain running until everything queued here has
//      actually gone out. Obligation 2 is what makes that finite: with flush()
//      the stranded samples are IN the queue, so "queue empty" is exactly
//      "EOO + filter latency emitted".
//
// flush() makes the loss recoverable; it does not make a long filter
// advisable. Correlation detection cares about the SHAPE of the tail, not
// merely its presence — both obligations are required, neither substitutes.

#ifdef AETHER_ENABLE_RADE_V2

#include <complex>
#include <memory>
#include <vector>

#include <QByteArray>
#include <QObject>
#include <QString>

#include "core/RadeV2TextChannel.h"
#include "core/RadeV2Tap.h"

// Opaque here on purpose — the codec headers are C, and pulling opus/lpcnet
// into every translation unit that merely wants to connect a signal is how a
// vendored dependency leaks into the whole tree. Mirrors RADEEngine.h.
struct rade;
struct LPCNetEncState;

namespace AetherSDR {

class Resampler;

class RADEV2Engine : public QObject {
    Q_OBJECT

public:
    explicit RADEV2Engine(QObject* parent = nullptr);
    ~RADEV2Engine() override;

    // ─── Rates (§7.1 X1; rade_api.h) ───────────────────────────────────────
    static constexpr int kWaveformRateHz = 24000;  // both waveform streams
    static constexpr int kModemRateHz    = 8000;   // RADE_MODEM_SAMPLE_RATE
    static constexpr int kSpeechRateHz   = 16000;  // RADE_SPEECH_SAMPLE_RATE

    // §7.1 X1 — the radio sends 128 samples per packet, so we return the same.
    // Not known to be required; it is what the radio itself does.
    static constexpr int kEmitBlockSamples = 128;

    // §7.1 T9 obligation 1. Named rather than inlined so the test can assert
    // against the same number the production path uses.
    static constexpr double kTxUpsampleTransBand = 45.0;

    bool start();
    void stop();
    bool isActive() const;

    bool  isSynced() const;
    float snrDb() const;

    // "RADE V2 vN", or empty when the codec is not compiled in.
    static QString versionString();

    // ─── Diagnostics ───────────────────────────────────────────────────────
    //
    // Everything this class can get wrong, it gets wrong quietly: a desynced
    // receiver produces no audio, a starved transmit queue produces silence,
    // and a truncated tail produces a decode failure at the far end. Counting
    // them separately is what tells the three apart.
    struct Stats {
        quint64 rxPacketsIn      = 0;   // passband blocks from the transport
        quint64 rxFramesDecoded  = 0;   // rade_rx() calls that yielded features
        quint64 rxSpeechSamples  = 0;   // 24 kHz samples handed back
        quint64 txSteps          = 0;   // rade_tx() calls
        quint64 txTicks          = 0;   // clock ticks served
        quint64 txUnderruns      = 0;   // ticks served from an empty queue
        quint64 txTailSamples    = 0;   // 24 kHz samples queued by the EOO+flush
    };
    Stats stats() const;

    // 24 kHz samples still waiting for a clock tick. The tail is complete when
    // this reaches zero after endTx().
    int queuedTxSamples() const;

public slots:
    // ─── transport → codec ─────────────────────────────────────────────────
    //
    // One block of demodulated passband, 24 kHz complex, ALREADY deconjugated
    // by the stream (§7.1 X2). Both components are used: the radio hands us a
    // genuine analytic signal, so no Hilbert transform is needed and dropping
    // the imaginary half would throw away half the receiver's advantage.
    void onRxPassband(const std::vector<std::complex<float>>& block);

    // §7.1 T1/T3 — drive one transmit step. `synthesized` marks a tick from the
    // transport's drain timer rather than from the radio; during those there is
    // no mic audio arriving, only the tail draining.
    void onTxClockTick(int samples, bool synthesized);

    // ─── audio → codec ─────────────────────────────────────────────────────
    //
    // Mic audio: 24 kHz **stereo float32**, matching AudioEngine::txRawPcmReady
    // verbatim. §9 takes the transmit CLOCK from `tx_stream_in` and the AUDIO
    // from AudioEngine, which is what makes "can tx_stream_in carry usable mic
    // audio" (§10.9 D — flowing, but silent) an open question rather than a
    // blocker.
    void feedTxAudio(const QByteArray& stereo24k);

    // ─── TX lifecycle ──────────────────────────────────────────────────────
    void beginTx();

    // The operator has unkeyed. **PTT must not drop here.** This drains the
    // voice pipeline, appends the 120 ms EOO frame and flushes the upsampler
    // (§7.1 T9), all into the transmit queue. txTailComplete() follows once the
    // queue has actually gone out on the clock — that is the signal to unkey.
    void endTx();

    // Abandon the over. The chain is already wrong (§7.1 T8), so the tail is
    // dropped rather than flushed; txTailComplete() still fires, because a
    // caller waiting on it must never be left hanging.
    void cancelTx();

    // The callsign for V2's inline BPSK data channel (§9.2): one bit per
    // rade_tx() step, ≈25 bit/s, cycled continuously.
    //
    // **The framing is PROVISIONAL.** The channel is raw — no sync, no CRC, no
    // FEC — so the application owns the protocol, and ours must match whatever
    // the RADE V2 ecosystem settles on rather than something we invented. What
    // is fixed here is only the placement: the symbol has to be set per step,
    // inside the encode loop.
    void setTxCallsign(const QString& callsign);

signals:
    // → FlexWaveformTransport::submitDecodedAudio. Mono 24 kHz; the transport
    // handles E1/E2 (inbound id, stereo duplication, no conjugation).
    void decodedAudioReady(const std::vector<float>& mono24k);

    // → FlexWaveformTransport::submitModulatedAudio. Mono 24 kHz.
    void modulatedAudioReady(const std::vector<float>& mono24k);

    // → FlexWaveformTransport::requestEndTx. The tail — EOO plus the flushed
    // filter memory — is now IN the queue and needs a clock to carry it out.
    //
    // **Something has to start the drain, and only the codec knows the over is
    // actually over.** §7.1 T3: at unkey the radio stops driving the pump, so
    // the ~120 ms tail goes out only on the transport's synthesized clock —
    // and that clock does not start by itself. If nothing emits this, the tail
    // is never transmitted AND the transport never reaches a state where
    // notifyTxTailComplete() means anything, so a caller holding PTT until
    // readyToUnkey() is never released. Verified by mutation: delete this one
    // emission and two seam tests hang out to their timeouts.
    //
    // Emitted on EVERY end-of-over path, including the degenerate one where
    // there was nothing to send, so that guarantee has no gaps.
    //
    // It is emitted after the queue is filled rather than before, which costs
    // nothing and closes the window where a drain tick could be served from an
    // empty queue. To be accurate about how much that buys: the queue is
    // filled synchronously a few microseconds earlier, and the drain timer
    // ticks every 2 ms, so this ordering is cheap insurance rather than a
    // demonstrated defect — do not read more into it than that.
    void txTailQueued();

    // → FlexWaveformTransport::notifyTxTailComplete. The EOO and the flushed
    // filter memory are out; only now is it safe to release PTT.
    //
    // Always follows txTailQueued(), never precedes it — both are emitted from
    // the same thread, so a queued connection delivers them in that order.
    void txTailComplete();

    void syncChanged(bool synced);
    void snrChanged(float snrDb);
    void freqOffsetChanged(float hz);

    // One raw inline-data symbol per decoded step (§9.2). Emitted undecoded and
    // unframed deliberately — see setTxCallsign().
    //
    // **Kept alongside textDecoded(), not replaced by it.** When the framing
    // misbehaves the raw soft symbols are the only thing that can tell a
    // transmit-side fault from a receive-side one, and re-deriving them means
    // another trip to the radio.
    void dataSymbolReceived(float symbol);

    // A complete, CRC-validated frame off the inline channel (§9.2), via
    // RadeV2TextChannel. `confidence` is the mean soft magnitude over the
    // frame — an absolute measure, since the transmitted symbol is exactly
    // ±1.0.
    //
    // Fires on EVERY valid copy, including repeats of a callsign already seen.
    // De-duplication is a display policy, and a second copy is corroboration
    // rather than noise.
    void textDecoded(const QString& text, float confidence);

private:
    void encodePendingFeatures(bool drain);
    void queueModem(const float* modem8k, int count);
    void serveTick(int samples);

    struct rade*    m_rade      = nullptr;
    LPCNetEncState* m_lpcnetEnc = nullptr;
    void*           m_fargan    = nullptr;   // FARGANState*, opaque here
    bool            m_farganWarmedUp = false;

    // Two resamplers for one complex stream, with IDENTICAL parameters. The I
    // and Q halves must stay phase-aligned, and each instance carries its own
    // filter memory — sharing one would interleave the two into nonsense.
    std::unique_ptr<Resampler> m_rxDownI;    // 24k → 8k
    std::unique_ptr<Resampler> m_rxDownQ;    // 24k → 8k
    std::unique_ptr<Resampler> m_rxUp16to24;
    std::unique_ptr<Resampler> m_txDown24to16;
    std::unique_ptr<Resampler> m_txUp8to24;  // §7.1 T9 — ReqTransBand=45

    // RX: complex modem samples in, features out, speech out.
    std::vector<float> m_rxAccumI;
    std::vector<float> m_rxAccumQ;
    std::vector<float> m_rxFeatAccum;
    std::vector<float> m_rxOut24k;

    // TX: speech in, features, modulated 24 kHz waiting on the clock.
    std::vector<qint16> m_txSpeech16k;
    std::vector<float>  m_txFeatAccum;
    std::vector<float>  m_txOut24k;

    bool m_txActive   = false;
    bool m_tailQueued = false;   // EOO + flush appended; queue is now finite
    bool m_tailDone   = false;   // txTailComplete() already emitted

    // TX carries a whole framed copy and cycles it; RX reassembles frames from
    // the soft symbol stream. Same protocol, opposite directions — see
    // RadeV2TextChannel, which owns all of it.
    std::vector<bool> m_txDataBits;
    size_t            m_txDataBitPos = 0;
    RadeV2TextChannel m_textChannel;

    bool  m_synced = false;
    float m_snrDb  = 0.0f;

    Stats m_stats;

    // Bench WAV taps along the chain; compiles to nothing without
    // -DRADE_V2_WAV_TAP (see RadeV2Tap.h).
    RADE_V2_TAP_MEMBER
};

}  // namespace AetherSDR

#endif  // AETHER_ENABLE_RADE_V2
