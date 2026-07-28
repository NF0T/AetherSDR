// Does the vendored RADE V2 codec actually RUN in our build?
//
// Stage 6a proved third_party/radae_v2 compiles and links. That is a weaker
// claim than it sounds: the V2 weights are 62 MB of generated C arrays, the
// Win32 export macro was locally patched, and Opus comes from AetherSDR's
// vendored snapshot rather than the version upstream fetches. Any of those
// could produce a library that builds perfectly and does nothing.
//
// So this exercises the real modem path — rade_tx() → rade_rx() — before
// RADEV2Engine is built on top of it. Finding out here costs minutes; finding
// out after the engine, the resamplers and the transport wiring are in place
// costs a day of bisecting someone else's neural net.
//
// It also pins two facts the public header states INCORRECTLY, both of which
// the RFC's TX design depends on. See eooApiWorksForV2().

#include <cmath>
#include <vector>

#include <QtTest>

extern "C" {
#include "rade_api.h"
}

namespace {
constexpr int kV2Flags = RADE_USE_C_ENCODER | RADE_USE_C_DECODER
                       | RADE_MODE_V2 | RADE_VERBOSE_0;
}

class RadeV2CodecTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void opensInV2Mode();
    void reportsCoherentBufferSizes();
    void eooApiWorksForV2();          // header says "V1 only" — it is wrong
    void modulatesAndDemodulates();   // the one that matters

private:
    struct rade* m_rade = nullptr;
};

void RadeV2CodecTest::initTestCase() {
    rade_initialize();
    m_rade = rade_open(const_cast<char*>("dummy"), kV2Flags);
    QVERIFY2(m_rade != nullptr, "rade_open() with RADE_MODE_V2 returned NULL");
}

void RadeV2CodecTest::cleanupTestCase() {
    if (m_rade) rade_close(m_rade);
    rade_finalize();
}

void RadeV2CodecTest::opensInV2Mode() {
    // model_file is ignored — V2 weights are compiled in from
    // rade_enc_v2_data.c / rade_dec_v2_data.c (rade_api.c:72).
    QVERIFY(m_rade != nullptr);
    QVERIFY(rade_version() > 0);
    // Added upstream in 5a49f19 specifically so callers can detect
    // non-breaking additions. We rely on it to notice API drift.
    QVERIFY(rade_version_minor() >= 0);
}

void RadeV2CodecTest::reportsCoherentBufferSizes() {
    // These drive every buffer RADEV2Engine will allocate, so a zero or a
    // negative here would surface later as a heap bug rather than a clear
    // failure.
    const int nFeatures = rade_n_features_in_out(m_rade);
    const int nTxOut    = rade_n_tx_out(m_rade);
    const int nNinMax   = rade_nin_max(m_rade);
    const int nNin      = rade_nin(m_rade);

    QVERIFY2(nFeatures > 0, "n_features_in_out must be positive");
    QVERIFY2(nTxOut    > 0, "n_tx_out must be positive");
    QVERIFY2(nNinMax   > 0, "nin_max must be positive");
    QVERIFY2(nNin      > 0, "nin must be positive");
    QVERIFY2(nNin <= nNinMax, "nin must never exceed nin_max — the RX input "
                              "buffer is sized on nin_max");

    // Feature vectors are the lpcnet_demo format: 36 floats per 10 ms frame.
    QVERIFY2(nFeatures % 36 == 0,
             qPrintable(QStringLiteral("n_features_in_out=%1 is not a multiple "
                                       "of the 36-float frame").arg(nFeatures)));

    qInfo("V2: n_features=%d n_tx_out=%d nin=%d nin_max=%d",
          nFeatures, nTxOut, nNin, nNinMax);
}

void RadeV2CodecTest::eooApiWorksForV2() {
    // ── A correction to the vendored public header ──────────────────────
    // rade_api.h marks rade_n_tx_eoo_out() and rade_tx_eoo() "// V1 only".
    // That is wrong, and it is wrong in the direction that would damage our
    // design: RFC §7.1 T3 requires emitting V2's ~120 ms EOO frame after the
    // last voice frame, and a reader trusting the header would conclude V2
    // has no EOO API and build around its absence.
    //
    // rade_api.c:155 and :207 both dispatch on RADE_MODE_V2 to
    // rade_tx_v2_n_eoo_out() / rade_tx_v2_eoo(). The "V1 only" marking
    // belongs to the aux TEXT channel carried in V1's EOO bits — not to the
    // EOO frame itself. Asserted here so the header can never quietly become
    // right without us noticing.
    const int nEoo = rade_n_tx_eoo_out(m_rade);
    QVERIFY2(nEoo > 0,
             "rade_n_tx_eoo_out() returned 0 for V2 — if this ever fires, the "
             "header's 'V1 only' has become true and RFC §7.1 T3's tail needs "
             "a different source");

    std::vector<RADE_COMP> eoo(static_cast<size_t>(nEoo) + 16);
    const int written = rade_tx_eoo(m_rade, eoo.data());
    QCOMPARE(written, nEoo);

    // The tail must actually carry signal — an all-zero "EOO" would emit
    // silence and read as a working tail everywhere upstream.
    double energy = 0.0;
    for (int i = 0; i < written; ++i)
        energy += double(eoo[i].real) * eoo[i].real + double(eoo[i].imag) * eoo[i].imag;
    QVERIFY2(energy > 0.0, "EOO frame is all zeros — it would transmit silence");

    // ~120 ms at the 8 kHz modem rate ≈ 960 samples. Assert the order of
    // magnitude, not the exact count: the point is that the tail is long
    // enough to matter, which is why it cannot ride out on the normal pump.
    const double ms = 1000.0 * written / RADE_MODEM_SAMPLE_RATE;
    qInfo("V2 EOO: %d samples = %.1f ms at %d Hz", written, ms, RADE_MODEM_SAMPLE_RATE);
    QVERIFY2(ms > 20.0, "EOO shorter than expected — re-check §7.1 T3 sizing");
}

void RadeV2CodecTest::modulatesAndDemodulates() {
    // The decisive test: push features through rade_tx(), feed the modem
    // samples straight back into rade_rx(), and require the receiver to
    // ACQUIRE. No RF, no resampling, no transport — if this fails, nothing
    // built on top of it can work.
    const int nFeatures = rade_n_features_in_out(m_rade);
    const int nTxOut    = rade_n_tx_out(m_rade);
    QVERIFY(nFeatures > 0 && nTxOut > 0);

    // Synthetic but non-degenerate features. Silence or a constant can make a
    // vocoder's output collapse, which would be an ambiguous failure; a slow
    // varying pattern keeps every frame distinct.
    std::vector<float> features(static_cast<size_t>(nFeatures));
    auto fillFeatures = [&](int frame) {
        for (int i = 0; i < nFeatures; ++i)
            features[size_t(i)] = 0.35f * std::sin(0.05f * float(i + frame * 7));
    };

    // Modulate a few seconds' worth so the receiver has time to acquire —
    // V2's acquisition needs several frames, and one frame proves nothing.
    constexpr int kFrames = 120;
    std::vector<RADE_COMP> modem;
    modem.reserve(size_t(kFrames) * size_t(nTxOut));

    std::vector<RADE_COMP> txOut(static_cast<size_t>(nTxOut));
    for (int f = 0; f < kFrames; ++f) {
        fillFeatures(f);
        const int n = rade_tx(m_rade, txOut.data(), features.data());
        QVERIFY2(n > 0, "rade_tx() produced no samples");
        QCOMPARE(n, nTxOut);
        modem.insert(modem.end(), txOut.begin(), txOut.begin() + n);
    }

    // Non-silent output, or the demodulate half would be meaningless.
    double txEnergy = 0.0;
    for (const RADE_COMP& c : modem)
        txEnergy += double(c.real) * c.real + double(c.imag) * c.imag;
    QVERIFY2(txEnergy > 0.0, "rade_tx() produced all-zero modem samples");

    // Demodulate. rade_nin() changes between calls (timing tracking), so it
    // must be re-read every iteration rather than hoisted — hoisting it is a
    // classic way to desync a RADE receiver.
    std::vector<float> featOut(static_cast<size_t>(nFeatures));
    std::vector<RADE_COMP> rxIn(static_cast<size_t>(rade_nin_max(m_rade)));
    size_t pos = 0;
    int syncedFrames = 0, decodedFrames = 0;

    while (pos + size_t(rade_nin(m_rade)) <= modem.size()) {
        const int nin = rade_nin(m_rade);
        std::copy(modem.begin() + qsizetype(pos),
                  modem.begin() + qsizetype(pos) + nin, rxIn.begin());
        pos += size_t(nin);

        int hasEoo = 0;
        const int nOut = rade_rx(m_rade, featOut.data(), &hasEoo, nullptr, rxIn.data());
        if (nOut > 0) decodedFrames++;
        if (rade_sync(m_rade)) syncedFrames++;
    }

    qInfo("V2 loopback: %zu modem samples, %d synced, %d decoded, SNR %.1f dB, foff %.1f Hz",
          modem.size(), syncedFrames, decodedFrames,
          double(rade_snrdB_3k_est(m_rade)), double(rade_freq_offset(m_rade)));

    QVERIFY2(syncedFrames > 0,
             "receiver never acquired on a clean noise-free loopback — the "
             "vendored V2 codec does not work in this build");
    QVERIFY2(decodedFrames > 0,
             "receiver synced but produced no feature frames");
}

QTEST_APPLESS_MAIN(RadeV2CodecTest)
#include "rade_v2_codec_test.moc"
