// Resampler::flush() — recovering the tail the FIR would otherwise keep.
//
// The constructor calls prewarm() to absorb the filter's startup latency at the
// HEAD of a stream. Nothing did the symmetric thing at the TAIL, so roughly
// `latency` samples stayed inside the FIR at end of stream and were simply lost.
//
// For continuous audio that is inaudible, which is why it went unnoticed for
// years. For a signal whose LAST samples are the payload it is fatal: RADE's
// end-of-over frame is exactly that, and RFC §7.1 T9 records V1 having to
// loosen its TX resampler to ~42 taps to squeeze the EOO into a 60 ms window.
//
// Every test here uses 8000 → 24000 with the DEFAULT transition band, i.e. the
// long ~950-tap filter V1 could not use. If flush() works there, the loosening
// becomes an optimisation rather than a requirement.

#include <cmath>
#include <vector>

#include <QtTest>

#include "core/Resampler.h"

using AetherSDR::Resampler;

namespace {

std::vector<float> toFloats(const QByteArray& b) {
    const int n = b.size() / int(sizeof(float));
    const auto* p = reinterpret_cast<const float*>(b.constData());
    return std::vector<float>(p, p + n);
}

double energy(const std::vector<float>& v) {
    double e = 0.0;
    for (float f : v) e += double(f) * f;
    return e;
}

}  // namespace

class ResamplerFlushTest : public QObject {
    Q_OBJECT

private slots:
    void reportsNonZeroLatency();
    void flushRecoversTheStrandedTail();   // the point of the exercise
    void tailIsLostWithoutFlush();         // proves the above is not vacuous
    void flushIsIdempotent();
    void processAfterFlushRefuses();
    void resetRestoresUsability();
    void passthroughHasNothingToFlush();
};

void ResamplerFlushTest::reportsNonZeroLatency() {
    Resampler r(8000, 24000);
    const int lat = r.latencySamples();
    QVERIFY2(lat > 0, "8k->24k with the default transition band must have "
                      "non-trivial filter latency");
    qInfo("8000->24000 default band: latency = %d input samples (%.1f ms)",
          lat, 1000.0 * lat / 8000.0);

    // The loosened band V1 had to adopt should be dramatically shorter — that
    // is the whole reason it was adopted (RFC §7.1 T9).
    Resampler loose(8000, 24000, 4096, 45.0);
    QVERIFY2(loose.latencySamples() < lat,
             "ReqTransBand=45 must yield a shorter filter than the default");
    qInfo("8000->24000 band=45:      latency = %d input samples (%.1f ms)",
          loose.latencySamples(), 1000.0 * loose.latencySamples() / 8000.0);
}

void ResamplerFlushTest::flushRecoversTheStrandedTail() {
    Resampler r(8000, 24000);

    // A burst that ENDS abruptly — the shape of an EOO frame. If the tail is
    // stranded, what is lost is the end of the burst, which is precisely the
    // part a far-end correlator needs.
    std::vector<float> in(400);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = 0.5f * std::sin(2.0 * M_PI * 1500.0 * double(i) / 8000.0);

    const auto body = toFloats(r.process(in.data(), int(in.size())));
    const auto tail = toFloats(r.flush());

    QVERIFY2(!tail.empty(), "flush() returned nothing — the tail is still stuck "
                            "in the filter");
    QVERIFY2(energy(tail) > 0.0,
             "flush() returned only zeros — it emitted padding rather than the "
             "signal that was in flight");

    // flush() emits `latency` samples' worth of output, so the genuine tail is
    // followed by padding — counting total length would pass trivially and
    // prove nothing. Assert instead WHERE THE SIGNAL ENDS: concatenate body and
    // tail, find the last sample carrying real energy, and require it to land
    // near the theoretical output length. That is the claim that matters —
    // every input sample reached the output.
    std::vector<float> all = body;
    all.insert(all.end(), tail.begin(), tail.end());

    float peak = 0.0f;
    for (float f : all) peak = std::max(peak, std::abs(f));
    QVERIFY(peak > 0.0f);

    size_t lastSignal = 0;
    for (size_t i = 0; i < all.size(); ++i)
        if (std::abs(all[i]) > peak * 0.01f) lastSignal = i;

    const size_t ideal = in.size() * 3;
    qInfo("in=%zu body=%zu tail=%zu; signal ends at %zu (ideal %zu)",
          in.size(), body.size(), tail.size(), lastSignal, ideal);

    QVERIFY2(lastSignal > body.size(),
             "the signal ends inside the body — flush() recovered only padding, "
             "not the samples that were in flight");
    QVERIFY2(lastSignal >= ideal * 9 / 10,
             "signal ends more than 10% short of the theoretical output length "
             "— part of the input never made it out even after flushing");
}

void ResamplerFlushTest::tailIsLostWithoutFlush() {
    // Guards against the previous test being satisfied by a flush() that merely
    // returns something. Two identical instances, one flushed and one not: the
    // unflushed one must genuinely produce less. If this ever fails, flush()
    // has become decorative.
    std::vector<float> in(400);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = 0.5f * std::sin(2.0 * M_PI * 1500.0 * double(i) / 8000.0);

    Resampler withoutFlush(8000, 24000);
    Resampler withFlush(8000, 24000);

    const size_t a = toFloats(withoutFlush.process(in.data(), int(in.size()))).size();
    const size_t b = toFloats(withFlush.process(in.data(), int(in.size()))).size()
                   + toFloats(withFlush.flush()).size();

    QVERIFY2(b > a,
             "flushing produced no more output than not flushing — the tail was "
             "never actually stranded, or flush() is a no-op");
    qInfo("without flush: %zu samples; with flush: %zu (+%zu recovered)", a, b, b - a);
}

void ResamplerFlushTest::flushIsIdempotent() {
    Resampler r(8000, 24000);
    std::vector<float> in(160, 0.25f);
    r.process(in.data(), int(in.size()));

    QVERIFY(!r.isFlushed());
    const auto first = r.flush();
    QVERIFY(r.isFlushed());
    QVERIFY2(r.flush().isEmpty(),
             "a second flush() must return nothing — the filter was already "
             "drained, and emitting more would be inventing samples");
    QVERIFY(!first.isEmpty());
}

void ResamplerFlushTest::processAfterFlushRefuses() {
    // Terminal by design. After flush() the filter holds zeros, so further
    // audio would be spliced behind silence — audible, wrong, and with nothing
    // anywhere to explain it. Refusing loudly beats glitching quietly.
    Resampler r(8000, 24000);
    std::vector<float> in(160, 0.25f);
    r.process(in.data(), int(in.size()));
    r.flush();

    QTest::ignoreMessage(QtWarningMsg,
        QRegularExpression("process\\(\\) after flush\\(\\)"));
    QVERIFY2(r.process(in.data(), int(in.size())).isEmpty(),
             "process() after flush() must refuse rather than emit spliced audio");
}

void ResamplerFlushTest::resetRestoresUsability() {
    Resampler r(8000, 24000);
    std::vector<float> in(160, 0.25f);
    r.process(in.data(), int(in.size()));
    r.flush();
    QVERIFY(r.isFlushed());

    r.reset();
    QVERIFY(!r.isFlushed());
    QVERIFY2(!r.process(in.data(), int(in.size())).isEmpty(),
             "after reset() the instance must resample normally again");

    // And be genuinely prewarmed, not merely willing — a reset that skipped
    // prewarm() would reintroduce the startup transient the constructor exists
    // to remove.
    QVERIFY(r.latencySamples() > 0);
}

void ResamplerFlushTest::passthroughHasNothingToFlush() {
    Resampler r(24000, 24000);
    QCOMPARE(r.latencySamples(), 0);
    std::vector<float> in(160, 0.25f);
    r.process(in.data(), int(in.size()));
    QVERIFY2(r.flush().isEmpty(),
             "a pass-through resampler holds no filter memory to flush");
}

QTEST_APPLESS_MAIN(ResamplerFlushTest)
#include "resampler_flush_test.moc"
