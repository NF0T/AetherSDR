// Regression guard for RADE V2 waveform-create reply parsing.
//
// RFC §7.1 R7: `waveform create` returns SIX stream ids on fw 4.2.20, not the
// four the vendored D-Star parser reads. R6: the ids are RECYCLED across
// create/remove, so a stale id must never survive teardown.
//
// Both fail SILENTLY on the wire — a partial parse yields zeroed ids that look
// like ordinary defaults, and a stale id addresses a stream that no longer
// means what it did. This test exists because neither would produce an error.
//
// Reply strings below are verbatim captures from the live FLEX-8400
// (fw 4.2.20.41343), including the radio's uppercase "0X" hex prefix.

#include <QtTest>

#include "core/backends/flex/FlexWaveformProvider.h"

using AetherSDR::FlexWaveformProvider;
using AetherSDR::WaveformStreams;

namespace {
// Verbatim from the radio. Note the uppercase 0X prefix, and that the *_out and
// byte_stream ids carry NO leading zeros ("0X1000005" not "0X01000005") — a
// fixed-width assumption would break on exactly this.
const char* kRealReply =
    "tx_stream_in_id=0X81000005 rx_stream_in_id=0X81000004 "
    "tx_stream_out_id=0X1000005 rx_stream_out_id=0X1000004 "
    "byte_stream_in_id=0X80100002 byte_stream_out_id=0X100002";
}  // namespace

class FlexWaveformProviderTest : public QObject {
    Q_OBJECT

private slots:
    void parsesAllSixIds();
    void rejectsPartialReply();
    void rejectsGarbage();
    void toleratesLowercaseAndNoPrefix();
};

void FlexWaveformProviderTest::parsesAllSixIds() {
    WaveformStreams s;
    QVERIFY(FlexWaveformProvider::parseCreateReply(QString::fromLatin1(kRealReply), s));
    QVERIFY(s.valid);
    QCOMPARE(s.txStreamIn,    0x81000005u);
    QCOMPARE(s.rxStreamIn,    0x81000004u);
    QCOMPARE(s.txStreamOut,   0x01000005u);
    QCOMPARE(s.rxStreamOut,   0x01000004u);
    QCOMPARE(s.byteStreamIn,  0x80100002u);
    QCOMPARE(s.byteStreamOut, 0x00100002u);

    // The high bit marks the inbound (to-waveform) streams, and those are also
    // the ids we EMIT on (§7.1 E1). Assert the relationship rather than just the
    // literals, so a firmware change that reassigned them is caught.
    QVERIFY(s.rxStreamIn  & 0x80000000u);
    QVERIFY(s.txStreamIn  & 0x80000000u);
    QVERIFY(!(s.rxStreamOut & 0x80000000u));
    QVERIFY(!(s.txStreamOut & 0x80000000u));
}

void FlexWaveformProviderTest::rejectsPartialReply() {
    // Exactly the four-id subset the vendored D-Star parser reads. Accepting
    // this is the R7 bug: it looks successful and silently loses the byte
    // stream pair.
    WaveformStreams s;
    const QString fourOnly =
        QStringLiteral("tx_stream_in_id=0X81000005 rx_stream_in_id=0X81000004 "
                       "tx_stream_out_id=0X1000005 rx_stream_out_id=0X1000004");
    QVERIFY(!FlexWaveformProvider::parseCreateReply(fourOnly, s));
    QVERIFY(!s.valid);
    // A failed parse must leave nothing behind — a half-filled struct would be
    // worse than an empty one, because the populated fields would look valid.
    QCOMPARE(s.rxStreamIn, 0u);
    QCOMPARE(s.txStreamIn, 0u);
}

void FlexWaveformProviderTest::rejectsGarbage() {
    WaveformStreams s;
    QVERIFY(!FlexWaveformProvider::parseCreateReply(QString(), s));
    QVERIFY(!FlexWaveformProvider::parseCreateReply(QStringLiteral("0"), s));
    QVERIFY(!FlexWaveformProvider::parseCreateReply(
        QStringLiteral("rx_stream_in_id=notahexnumber"), s));
    QVERIFY(!s.valid);
}

void FlexWaveformProviderTest::toleratesLowercaseAndNoPrefix() {
    // The radio is not consistent about the 0X prefix across firmware and
    // fields, so the parser must not depend on it.
    WaveformStreams s;
    const QString mixed =
        QStringLiteral("tx_stream_in_id=0x81000005 rx_stream_in_id=81000004 "
                       "tx_stream_out_id=0X1000005 rx_stream_out_id=1000004 "
                       "byte_stream_in_id=0x80100002 byte_stream_out_id=100002");
    QVERIFY(FlexWaveformProvider::parseCreateReply(mixed, s));
    QCOMPARE(s.rxStreamIn,   0x81000004u);
    QCOMPARE(s.byteStreamOut, 0x00100002u);
}

QTEST_APPLESS_MAIN(FlexWaveformProviderTest)
#include "flex_waveform_provider_test.moc"
