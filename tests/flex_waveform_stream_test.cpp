// Regression guard for RADE V2 waveform RX ingest.
//
// RFC §7.1 X1/X2/E3. The one that matters is X2: `rx_stream_in` carries the
// CONJUGATE of the analytic signal, so the positive-frequency passband is
// A − jB. Feeding the modem A + jB mirrors the spectrum about DC — and a
// mirrored OFDM burst has entirely normal power, level, occupancy and packet
// rate, so nothing anywhere reports a problem. It simply never decodes.
//
// That is not a defect code review can catch; `imag = b` looks correct. So the
// conjugation is tested here against a synthesized packet whose true content is
// known, and the test is written so that FLIPPING THE SIGN IN THE PRODUCTION
// CODE FAILS IT. A test that passes either way would be worse than none.
//
// Wire layout below is the measured one (live FLEX-8400, fw 4.2.20.41343):
// 28-byte VITA-49 ExtDataWithStream header, then interleaved big-endian
// float32 pairs, 128 pairs per packet at 24 kHz.

#include <bit>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

#include <QtEndian>
#include <QtTest>

#include "core/backends/flex/FlexWaveformStream.h"

using AetherSDR::FlexWaveformStream;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSamplesPerPacket = FlexWaveformStream::kRxSamplesPerPkt;  // 128
constexpr double kFs = FlexWaveformStream::kRxSampleRateHz;             // 24000

void appendBeU32(QByteArray& out, quint32 v) {
    char buf[4];
    qToBigEndian(v, buf);
    out.append(buf, 4);
}

void appendBeFloat(QByteArray& out, float f) {
    appendBeU32(out, std::bit_cast<quint32>(f));
}

// A VITA-49 ExtDataWithStream header shaped like the radio's.
QByteArray header(quint32 streamId, int count, bool trailer = false) {
    QByteArray h;
    quint32 word0 = 0x30000000u | (static_cast<quint32>(count & 0x0F) << 16);
    if (trailer) word0 |= FlexWaveformStream::kTrailerFlag;
    appendBeU32(h, word0);
    appendBeU32(h, streamId);
    appendBeU32(h, 0x001C2Du);                                   // class OUI
    appendBeU32(h, FlexWaveformStream::kPccIfNarrow);            // ICC|PCC
    appendBeU32(h, 0);                                           // ts integer
    appendBeU32(h, 0);                                           // ts frac hi
    appendBeU32(h, 0);                                           // ts frac lo
    Q_ASSERT(h.size() == FlexWaveformStream::kVitaHeaderBytes);
    return h;
}

// Encode a known positive-frequency tone THE WAY THE RADIO DOES.
//
// The true signal is z(t) = exp(+j·2πft). The radio sends the conjugate, so
// what goes on the wire is A = Re(z̄) = cos(2πft) and B = Im(z̄) = −sin(2πft).
// A correct decoder must undo that and hand back exp(+j·2πft).
QByteArray conjugateToneWire(double freqHz, int samples, double phase0 = 0.0) {
    QByteArray p;
    for (int n = 0; n < samples; ++n) {
        const double t = phase0 + 2.0 * kPi * freqHz * n / kFs;
        appendBeFloat(p, static_cast<float>(std::cos(t)));
        appendBeFloat(p, static_cast<float>(-std::sin(t)));
    }
    return p;
}

// Energy of `sig` at +f relative to −f, in dB. The probe's decisive test:
// a correctly-deconjugated tone puts everything at +f, a sign error puts
// everything at −f, and the two verdicts are ~100 dB apart.
double posOverNegDb(const std::vector<std::complex<float>>& sig, double freqHz) {
    std::complex<double> pos{}, neg{};
    for (size_t n = 0; n < sig.size(); ++n) {
        const double w = 2.0 * kPi * freqHz * static_cast<double>(n) / kFs;
        const std::complex<double> s(sig[n].real(), sig[n].imag());
        pos += s * std::complex<double>(std::cos(-w), std::sin(-w));
        neg += s * std::complex<double>(std::cos(w), std::sin(w));
    }
    const double p = std::norm(pos), q = std::norm(neg);
    return 10.0 * std::log10((p + 1e-30) / (q + 1e-30));
}

}  // namespace

class FlexWaveformStreamTest : public QObject {
    Q_OBJECT

private slots:
    void parsesMeasuredHeader();
    void honoursTrailerFlag();
    void rejectsShortDatagram();
    void conjugatesInboundIq();          // §7.1 X2 — the load-bearing one
    void conjugationSignIsDetectable();  // proves the above can fail
    void rejectsPartialFloatPair();
    void appendsRatherThanReplaces();
};

void FlexWaveformStreamTest::parsesMeasuredHeader() {
    const QByteArray dg = header(0x81000004u, 5)
                          + conjugateToneWire(1500.0, kSamplesPerPacket);
    QCOMPARE(dg.size(), 28 + kSamplesPerPacket * 8);

    FlexWaveformStream::VitaHeader h;
    QVERIFY(FlexWaveformStream::parseHeader(dg.constData(), dg.size(), h));
    QVERIFY(h.valid);
    QCOMPARE(h.streamId, 0x81000004u);
    QCOMPARE(h.packetClass, FlexWaveformStream::kPccIfNarrow);
    QCOMPARE(h.packetCount, 5);
    QVERIFY(!h.hasTrailer);
    QCOMPARE(h.payloadOffset, 28);
    // §7.1 X1 — 128 complex samples per packet.
    QCOMPARE(h.payloadBytes, kSamplesPerPacket * 8);
}

void FlexWaveformStreamTest::honoursTrailerFlag() {
    // A trailer occupies the last 4 bytes and is NOT payload. Counting it as
    // payload would push a stray sample pair into the modem on every packet.
    QByteArray dg = header(0x81000004u, 0, /*trailer=*/true)
                    + conjugateToneWire(1500.0, kSamplesPerPacket);
    appendBeU32(dg, 0xDEADBEEFu);

    FlexWaveformStream::VitaHeader h;
    QVERIFY(FlexWaveformStream::parseHeader(dg.constData(), dg.size(), h));
    QVERIFY(h.hasTrailer);
    QCOMPARE(h.payloadBytes, kSamplesPerPacket * 8);
}

void FlexWaveformStreamTest::rejectsShortDatagram() {
    FlexWaveformStream::VitaHeader h;
    QVERIFY(!FlexWaveformStream::parseHeader(nullptr, 0, h));
    const QByteArray runt(27, '\0');
    QVERIFY(!FlexWaveformStream::parseHeader(runt.constData(), runt.size(), h));
    QVERIFY(!h.valid);
}

void FlexWaveformStreamTest::conjugatesInboundIq() {
    // §7.1 X2. Wire carries the conjugate of a +1500 Hz tone; the decoder must
    // hand back the +1500 Hz tone itself.
    constexpr double kTone = 1500.0;
    const QByteArray payload = conjugateToneWire(kTone, 1024);

    std::vector<std::complex<float>> out;
    const int n = FlexWaveformStream::decodeRxPayload(payload.constData(),
                                                      payload.size(), out);
    QCOMPARE(n, 1024);
    QCOMPARE(out.size(), size_t(1024));

    // Sample-level: at n=0 the tone is real, and by a quarter cycle it must
    // have rotated the POSITIVE way (+j), not the negative way.
    QVERIFY(std::abs(out[0].real() - 1.0f) < 1e-5f);
    QVERIFY(std::abs(out[0].imag()) < 1e-5f);
    const int quarter = static_cast<int>(kFs / kTone / 4.0);   // 4 samples
    QVERIFY2(out[quarter].imag() > 0.5f,
             "quarter-cycle rotation is negative — the conjugation was dropped");

    // Spectral: essentially all energy at +f.
    const double ratio = posOverNegDb(out, kTone);
    QVERIFY2(ratio > 20.0,
             qPrintable(QStringLiteral("+f/-f = %1 dB, expected > 20").arg(ratio)));
}

void FlexWaveformStreamTest::conjugationSignIsDetectable() {
    // Mutation check on the test itself. If decodeRxPayload were written with
    // `+b` instead of `-b`, its output would be exactly the wire pair read as
    // A + jB — which is what this builds. The assertion below is the inverse
    // of the one above, so the two cannot both pass. That is what makes
    // conjugatesInboundIq() a real guard rather than a tautology.
    constexpr double kTone = 1500.0;
    const QByteArray payload = conjugateToneWire(kTone, 1024);

    std::vector<std::complex<float>> asWritten;   // the buggy interpretation
    for (int i = 0; i < 1024; ++i) {
        const char* p = payload.constData() + i * 8;
        quint32 a = 0, b = 0;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        asWritten.emplace_back(std::bit_cast<float>(qFromBigEndian<quint32>(a)),
                               std::bit_cast<float>(qFromBigEndian<quint32>(b)));
    }
    const double ratio = posOverNegDb(asWritten, kTone);
    QVERIFY2(ratio < -20.0,
             qPrintable(QStringLiteral("uncorrected +f/-f = %1 dB, expected < -20")
                            .arg(ratio)));
}

void FlexWaveformStreamTest::rejectsPartialFloatPair() {
    // §7.1 X1 — payload is whole float32 PAIRS. Anything else means the layout
    // is not what was measured; truncating would hide precisely that.
    std::vector<std::complex<float>> out;
    QByteArray odd = conjugateToneWire(1500.0, 4);
    odd.chop(4);                                    // one float short of a pair
    QCOMPARE(FlexWaveformStream::decodeRxPayload(odd.constData(), odd.size(), out), -1);
    QCOMPARE(FlexWaveformStream::decodeRxPayload(nullptr, 8, out), -1);
    QVERIFY(out.empty());
}

void FlexWaveformStreamTest::appendsRatherThanReplaces() {
    // Callers accumulate several packets into one decoder block, so a decode
    // must not clobber what is already there.
    const QByteArray a = conjugateToneWire(1500.0, 16);
    std::vector<std::complex<float>> out;
    QCOMPARE(FlexWaveformStream::decodeRxPayload(a.constData(), a.size(), out), 16);
    QCOMPARE(FlexWaveformStream::decodeRxPayload(a.constData(), a.size(), out), 16);
    QCOMPARE(out.size(), size_t(32));
    QCOMPARE(out[16], out[0]);   // second packet starts where the first did
}

QTEST_APPLESS_MAIN(FlexWaveformStreamTest)
#include "flex_waveform_stream_test.moc"
