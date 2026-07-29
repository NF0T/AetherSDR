// Regression guard for RADEV2Engine — the codec core between the transport and
// the vendored V2 modem (RFC §7, §9).
//
// Three of the things this class does fail in complete silence, and all three
// are guarded here by assertions that were checked under mutation:
//
//   §7.1 T9 (1) — the 8→24 kHz TX upsampler must run at ReqTransBand=45.
//                 The default holds 297 ms of signal against a 120 ms EOO.
//   §7.1 T9 (2) — it must be flush()ed at end of over, or the last ~8.6 ms of
//                 that EOO never leaves the filter.
//   flush() is TERMINAL — so beginTx() must reset(). Skip it and the FIRST
//                 over transmits perfectly and every one after it is silent.
//
// The last is the nastiest of the three: nothing about the code looks wrong,
// the failure is in the second occurrence of a working path, and the class that
// refuses is doing so deliberately and correctly.
//
// The RX direction is driven from rade_tx() rather than from this engine's own
// TX output, and that is not laziness: what we transmit is REAL audio (§7.1
// E2), while what the radio hands back is an ANALYTIC pair (§10.3 item 5).
// There is no loopback between them without a Hilbert transform, so the test
// synthesizes what the radio actually sends.

#include <cmath>
#include <complex>
#include <tuple>
#include <vector>

#include <QSignalSpy>
#include <QtTest>

#include "core/RADEV2Engine.h"
#include "core/Resampler.h"

extern "C" {
#include "rade_api.h"
}

using AetherSDR::RADEV2Engine;
using AetherSDR::Resampler;

namespace {

constexpr int kV2Flags = RADE_USE_C_ENCODER | RADE_USE_C_DECODER
                       | RADE_MODE_V2 | RADE_VERBOSE_0;

// 24 kHz stereo float32, matching AudioEngine::txRawPcmReady exactly.
// Amplitude-modulated harmonics rather than a pure tone: lpcnet's analysis is
// pitch-driven, and a constant can collapse a vocoder's output in ways that
// would make a null result ambiguous.
QByteArray micAudio(int ms) {
    const int frames = RADEV2Engine::kWaveformRateHz * ms / 1000;
    QByteArray pcm(frames * 2 * int(sizeof(float)), Qt::Uninitialized);
    auto* p = reinterpret_cast<float*>(pcm.data());
    for (int n = 0; n < frames; ++n) {
        const float t = float(n) / RADEV2Engine::kWaveformRateHz;
        const float env = 0.4f * (1.0f + 0.5f * std::sin(2.0f * float(M_PI) * 3.0f * t));
        const float s = env * (std::sin(2.0f * float(M_PI) * 140.0f * t)
                             + 0.5f * std::sin(2.0f * float(M_PI) * 420.0f * t)
                             + 0.25f * std::sin(2.0f * float(M_PI) * 980.0f * t));
        p[2 * n]     = s;
        p[2 * n + 1] = s;
    }
    return pcm;
}

double energy(const std::vector<float>& v) {
    double e = 0.0;
    for (float s : v) e += double(s) * s;
    return e;
}

}  // namespace

class RadeV2EngineTest : public QObject {
    Q_OBJECT

private slots:
    void startsAndReportsItsGeometry();
    void txProducesModemAudioPacedByTheClock();
    void tailCarriesTheEooAndTheFlushedFilterMemory();   // §7.1 T9 (1) + (2)
    void tailCompletesOnlyWhenTheQueueHasGoneOut();      // §7.1 T9 (3)
    void aSecondOverStillTransmits();                    // flush() is terminal
    void cancelAnswersAndDropsTheTail();                 // §7.1 T8
    void rxDecodesTheAnalyticSignalTheRadioSends();
    void conjugatedInputWrecksSnrWithoutLosingSync();    // §7.1 X2 + a correction

private:
    // The modem signal the radio would hand us: rade_tx() output, upsampled to
    // 24 kHz as a complex pair. Built once — it is several seconds of neural
    // vocoder output and the cost is not the thing under test.
    const std::vector<std::complex<float>>& probe();
    std::vector<std::complex<float>> m_probe;
};

const std::vector<std::complex<float>>& RadeV2EngineTest::probe() {
    if (!m_probe.empty()) return m_probe;

    struct rade* tx = rade_open(const_cast<char*>("dummy"), kV2Flags);
    Q_ASSERT(tx);
    const int nFeat  = rade_n_features_in_out(tx);
    const int nTxOut = rade_n_tx_out(tx);

    std::vector<float> features(static_cast<size_t>(nFeat));
    std::vector<RADE_COMP> out(static_cast<size_t>(nTxOut));
    std::vector<float> i8k, q8k;

    // Enough steps for the receiver to acquire — V2 needs several, and one
    // proves nothing. 150 × 40 ms = 6 s.
    for (int f = 0; f < 150; ++f) {
        for (int k = 0; k < nFeat; ++k)
            features[size_t(k)] = 0.35f * std::sin(0.05f * float(k + f * 7));
        const int n = rade_tx(tx, out.data(), features.data());
        for (int k = 0; k < n; ++k) {
            i8k.push_back(out[size_t(k)].real);
            q8k.push_back(out[size_t(k)].imag);
        }
    }
    rade_close(tx);

    // 8 → 24 kHz on both components with identical filters, which is what makes
    // the pair still analytic at the other end.
    Resampler upI(RADEV2Engine::kModemRateHz, RADEV2Engine::kWaveformRateHz);
    Resampler upQ(RADEV2Engine::kModemRateHz, RADEV2Engine::kWaveformRateHz);
    const QByteArray bi = upI.process(i8k.data(), int(i8k.size()));
    const QByteArray bq = upQ.process(q8k.data(), int(q8k.size()));
    const auto* pi = reinterpret_cast<const float*>(bi.constData());
    const auto* pq = reinterpret_cast<const float*>(bq.constData());
    const int n = int(std::min(bi.size(), bq.size()) / qsizetype(sizeof(float)));
    m_probe.reserve(size_t(n));
    for (int k = 0; k < n; ++k)
        m_probe.emplace_back(pi[k], pq[k]);
    return m_probe;
}

void RadeV2EngineTest::startsAndReportsItsGeometry() {
    RADEV2Engine e;
    QVERIFY2(e.start(), "engine failed to start — the vendored V2 codec is the "
                        "first suspect (see rade_v2_codec_test)");
    QVERIFY(e.isActive());
    QVERIFY(!e.versionString().isEmpty());
    QVERIFY(!e.isSynced());

    // The three rates are fixed by the radio and by rade_api.h; a change in any
    // of them invalidates every resampler in the class.
    QCOMPARE(RADEV2Engine::kModemRateHz,  RADE_MODEM_SAMPLE_RATE);
    QCOMPARE(RADEV2Engine::kSpeechRateHz, RADE_SPEECH_SAMPLE_RATE);
    QCOMPARE(RADEV2Engine::kWaveformRateHz, 24000);

    e.stop();
    QVERIFY(!e.isActive());
}

void RadeV2EngineTest::txProducesModemAudioPacedByTheClock() {
    RADEV2Engine e;
    QVERIFY(e.start());

    std::vector<std::vector<float>> blocks;
    connect(&e, &RADEV2Engine::modulatedAudioReady, &e,
            [&blocks](const std::vector<float>& b) { blocks.push_back(b); });

    e.beginTx();
    e.feedTxAudio(micAudio(480));

    // Encoding is driven by mic audio arriving, not by the clock: the queue has
    // to be standing ready when the radio asks, because §7.1 T1 means the radio
    // asks on its own schedule and does not wait.
    QVERIFY2(e.queuedTxSamples() > 0,
             "no modulated audio queued after 480 ms of mic input");

    const int queued = e.queuedTxSamples();
    int served = 0;
    while (e.queuedTxSamples() > 0 && served < 1000) {
        e.onTxClockTick(RADEV2Engine::kEmitBlockSamples, /*synthesized=*/false);
        served++;
    }

    QVERIFY(!blocks.empty());
    for (const auto& b : blocks)
        QCOMPARE(int(b.size()), RADEV2Engine::kEmitBlockSamples);

    double total = 0.0;
    for (const auto& b : blocks) total += energy(b);
    QVERIFY2(total > 0.0, "the modem produced only zeros");

    // Roughly what was queued came back out; the clock does not invent or eat
    // samples.
    const int emitted = int(blocks.size()) * RADEV2Engine::kEmitBlockSamples;
    QVERIFY(emitted >= queued);
    QVERIFY(e.stats().txSteps >= 10);
}

void RadeV2EngineTest::tailCarriesTheEooAndTheFlushedFilterMemory() {
    // ─────────────────────────────────────────────────────────────────────
    // §7.1 T9 (1) and (2), stated as one measurement.
    //
    // The tail queued by endTx() is the EOO frame plus whatever flush() pulls
    // back out of the FIR. Both obligations show up in its LENGTH:
    //
    //   no flush()          → exactly the EOO, and the last 8.6 ms of it is
    //                         still sitting in the filter, untransmitted
    //   default trans-band  → the EOO smeared across 297 ms of filter memory
    //
    // Asserted against Resampler::latencySamples() rather than against
    // constants, so the test states the reason instead of the numbers.
    // ─────────────────────────────────────────────────────────────────────
    RADEV2Engine e;
    QVERIFY(e.start());
    e.beginTx();
    e.feedTxAudio(micAudio(200));
    while (e.queuedTxSamples() > 0)
        e.onTxClockTick(RADEV2Engine::kEmitBlockSamples, false);

    e.endTx();
    const int tail = int(e.stats().txTailSamples);

    const int ratio = RADEV2Engine::kWaveformRateHz / RADEV2Engine::kModemRateHz;
    Resampler shaped(RADEV2Engine::kModemRateHz, RADEV2Engine::kWaveformRateHz,
                     4096, RADEV2Engine::kTxUpsampleTransBand);
    Resampler dflt(RADEV2Engine::kModemRateHz, RADEV2Engine::kWaveformRateHz);
    const int shapedLat = shaped.latencySamples();
    const int dfltLat   = dflt.latencySamples();
    QVERIFY2(dfltLat > 10 * shapedLat,
             "the two transition bands no longer differ materially — re-derive "
             "§7.1 T9 before trusting anything below");

    struct rade* r = rade_open(const_cast<char*>("dummy"), kV2Flags);
    QVERIFY(r);
    const int eooSamples = rade_n_tx_eoo_out(r) * ratio;
    rade_close(r);
    QVERIFY(eooSamples > 0);

    qInfo("tail=%d samples (%.1f ms); EOO alone=%d; latency shaped=%d default=%d",
          tail, 1000.0 * tail / RADEV2Engine::kWaveformRateHz,
          eooSamples, shapedLat, dfltLat);

    QVERIFY2(tail > eooSamples,
             "the tail is exactly the EOO — flush() did not run, so the last "
             "~8.6 ms of the end-of-over is stranded in the FIR and never "
             "transmitted (§7.1 T9 obligation 2). Nothing errors; the far end "
             "just stops detecting end-of-over");

    QVERIFY2(tail < eooSamples + 4 * ratio * shapedLat,
             "the tail is far longer than the EOO plus a short filter's memory "
             "— the TX upsampler is running the DEFAULT transition band (297 ms "
             "against a 120 ms frame), not ReqTransBand=45 (§7.1 T9 "
             "obligation 1)");
}

void RadeV2EngineTest::tailCompletesOnlyWhenTheQueueHasGoneOut() {
    // §7.1 T9 (3). The tail is complete when it has been EMITTED, not when it
    // was generated — that is the whole reason the transport keeps synthesizing
    // clock ticks after the radio has stopped. Announcing completion at endTx()
    // would release PTT with the EOO still sitting in a queue.
    RADEV2Engine e;
    QVERIFY(e.start());

    int emittedAfterEnd = 0;
    bool ended = false;
    connect(&e, &RADEV2Engine::modulatedAudioReady, &e,
            [&](const std::vector<float>& b) {
                if (ended) emittedAfterEnd += int(b.size());
            });
    QSignalSpy tailSpy(&e, &RADEV2Engine::txTailComplete);

    e.beginTx();
    e.feedTxAudio(micAudio(200));
    while (e.queuedTxSamples() > 0)
        e.onTxClockTick(RADEV2Engine::kEmitBlockSamples, false);

    ended = true;
    e.endTx();
    QCOMPARE(tailSpy.count(), 0);
    QVERIFY2(e.queuedTxSamples() > 0, "endTx() queued nothing at all");

    const int tail = int(e.stats().txTailSamples);
    int ticks = 0;
    while (tailSpy.isEmpty() && ticks < 1000) {
        // Synthesized, because by now the radio has stopped asking (§7.1 T2).
        e.onTxClockTick(RADEV2Engine::kEmitBlockSamples, /*synthesized=*/true);
        ticks++;
    }

    QCOMPARE(tailSpy.count(), 1);
    QVERIFY2(emittedAfterEnd >= tail,
             "txTailComplete() fired before the whole tail had been emitted — "
             "PTT would drop mid-EOO");
    QCOMPARE(e.queuedTxSamples(), 0);
}

void RadeV2EngineTest::aSecondOverStillTransmits() {
    // Resampler::flush() is TERMINAL: it leaves zeros in the filter and refuses
    // process() until reset(). So beginTx() must reset the TX upsampler, and if
    // it does not, the first over is perfect and every subsequent one is
    // silent — a failure that cannot be seen in a single-transmission test.
    RADEV2Engine e;
    QVERIFY(e.start());

    for (int over = 0; over < 2; ++over) {
        e.beginTx();
        e.feedTxAudio(micAudio(200));
        QVERIFY2(e.queuedTxSamples() > 0,
                 qPrintable(QStringLiteral("over %1 produced no modulated audio "
                                           "— beginTx() did not reset() the "
                                           "flushed TX upsampler").arg(over)));
        while (e.queuedTxSamples() > 0)
            e.onTxClockTick(RADEV2Engine::kEmitBlockSamples, false);

        QSignalSpy tailSpy(&e, &RADEV2Engine::txTailComplete);
        e.endTx();
        QVERIFY(e.stats().txTailSamples > 0);
        int ticks = 0;
        while (tailSpy.isEmpty() && ticks++ < 1000)
            e.onTxClockTick(RADEV2Engine::kEmitBlockSamples, true);
        QCOMPARE(tailSpy.count(), 1);
    }
}

void RadeV2EngineTest::cancelAnswersAndDropsTheTail() {
    // §7.1 T8. A cancel means the transmit chain is already wrong, so there is
    // no tail worth flushing — but PTT is held on our say-so, so the answer
    // still has to come.
    RADEV2Engine e;
    QVERIFY(e.start());
    QSignalSpy tailSpy(&e, &RADEV2Engine::txTailComplete);

    e.beginTx();
    e.feedTxAudio(micAudio(200));
    QVERIFY(e.queuedTxSamples() > 0);

    e.cancelTx();
    QCOMPARE(tailSpy.count(), 1);
    QCOMPARE(e.queuedTxSamples(), 0);

    // And an endTx() with nothing keyed still answers, so a caller that always
    // waits cannot hang.
    RADEV2Engine idle;
    QVERIFY(idle.start());
    QSignalSpy idleSpy(&idle, &RADEV2Engine::txTailComplete);
    idle.endTx();
    QCOMPARE(idleSpy.count(), 1);
}

void RadeV2EngineTest::rxDecodesTheAnalyticSignalTheRadioSends() {
    RADEV2Engine e;
    QVERIFY(e.start());

    QSignalSpy syncSpy(&e, &RADEV2Engine::syncChanged);
    double speechEnergy = 0.0;
    int speechSamples = 0;
    connect(&e, &RADEV2Engine::decodedAudioReady, &e,
            [&](const std::vector<float>& b) {
                speechEnergy += energy(b);
                speechSamples += int(b.size());
                QCOMPARE(int(b.size()), RADEV2Engine::kEmitBlockSamples);
            });

    // Fed in the radio's own packet size, so the engine sees the same block
    // boundaries it will see on air (§7.1 X1).
    const auto& sig = probe();
    for (size_t pos = 0; pos + 128 <= sig.size(); pos += 128)
        e.onRxPassband(std::vector<std::complex<float>>(sig.begin() + qsizetype(pos),
                                                        sig.begin() + qsizetype(pos) + 128));

    qInfo("RX: %llu blocks in, %llu frames decoded, %d speech samples out, SNR %.1f dB",
          (unsigned long long)e.stats().rxPacketsIn,
          (unsigned long long)e.stats().rxFramesDecoded,
          speechSamples, double(e.snrDb()));

    QVERIFY2(e.isSynced(),
             "the receiver never acquired on a clean synthetic signal — the "
             "24→8 kHz stage or the I/Q pairing is wrong, not the codec "
             "(rade_v2_codec_test covers the codec itself)");
    QVERIFY(!syncSpy.isEmpty());
    QVERIFY2(e.stats().rxFramesDecoded > 0, "synced but produced no features");
    QVERIFY2(speechSamples > 0, "features decoded but no speech reached the transport");
    QVERIFY2(speechEnergy > 0.0, "the vocoder produced only silence");
}

void RadeV2EngineTest::conjugatedInputWrecksSnrWithoutLosingSync() {
    // ─────────────────────────────────────────────────────────────────────
    // §7.1 X2, and a MEASURED CORRECTION to how it presents.
    //
    // Two things are being pinned here at once.
    //
    // (a) The imaginary component is actually USED. If a later
    //     "simplification" dropped Q, or replaced the pair with its real part,
    //     then A+jB and A−jB would be the same input and would score
    //     identically. The gap below is what makes that impossible to do
    //     silently.
    //
    // (b) **A conjugation error does NOT show up as a loss of sync.** X2 says
    //     feeding A+jB "mirrors the spectrum and never decodes", which is true
    //     of the audio — but the receiver still ACQUIRES, and it produces MORE
    //     feature frames than the correct input, not fewer. Measured on this
    //     machine: correct 128 frames at +24.0 dB; conjugated 165 frames at
    //     −4.7 dB.
    //
    //     The reason is structural rather than incidental: V2 acquires on the
    //     cyclic-prefix autocorrelation (rade_rx_v2.c:374, compute_autocorr →
    //     detect_signal), and that detector is magnitude-based. Conjugating the
    //     input conjugates Ry and leaves |Ry| untouched, so the sync machine
    //     cannot tell the difference; only the estimated frequency offset flips
    //     sign, and the tracker absorbs that.
    //
    //     So a sync indicator is NOT a check on this. Anyone debugging silent
    //     RAD2 audio by looking at sync will conclude the modem is fine. The
    //     SNR estimate is the tell, and it is a ~29 dB tell.
    //
    //     Measured alongside: discarding Q ENTIRELY costs 0.3 dB here (23.7 vs
    //     24.0). The failure is the WRONG sign, not the absence of one — real
    //     input splits its energy between both images and the receiver copes,
    //     while conjugated input puts all of it in the wrong one. Full write-up
    //     in RFC §7.1b.
    // ─────────────────────────────────────────────────────────────────────
    const auto& sig = probe();

    const auto run = [&](bool conjugate) {
        RADEV2Engine e;
        if (!e.start()) return std::tuple<bool, quint64, float>{false, 0, 0.0f};
        for (size_t pos = 0; pos + 128 <= sig.size(); pos += 128) {
            std::vector<std::complex<float>> block;
            block.reserve(128);
            for (size_t k = pos; k < pos + 128; ++k)
                block.emplace_back(sig[k].real(),
                                   conjugate ? -sig[k].imag() : sig[k].imag());
            e.onRxPassband(block);
        }
        return std::tuple<bool, quint64, float>{e.isSynced(),
                                                e.stats().rxFramesDecoded,
                                                e.snrDb()};
    };

    const auto [okSync, okFrames, okSnr]   = run(false);
    const auto [cjSync, cjFrames, cjSnr]   = run(true);

    qInfo("correct:    synced=%d frames=%llu SNR=%.1f dB", int(okSync),
          (unsigned long long)okFrames, double(okSnr));
    qInfo("conjugated: synced=%d frames=%llu SNR=%.1f dB", int(cjSync),
          (unsigned long long)cjFrames, double(cjSnr));

    QVERIFY(okSync);
    QVERIFY2(okSnr - cjSnr > 15.0f,
             "conjugated input scored the same as correct input — the "
             "imaginary component is being ignored somewhere in the RX chain, "
             "so the §7.1 X2 sideband error would pass every check we have");

    // Deliberately NOT asserted, because it is upstream codec behaviour rather
    // than ours and may change: cjSync is currently TRUE. If a future vendor
    // bump makes conjugated input fail to acquire, that is an improvement and
    // this comment is what should be updated — not a reason to relax the SNR
    // assertion above, which is the part that guards our own code.
}

QTEST_APPLESS_MAIN(RadeV2EngineTest)
#include "rade_v2_engine_test.moc"
