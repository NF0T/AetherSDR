// Regression guard for the RADE V2 transport, and above all for §7.1 T3.
//
// T1: there is no free-running transmit clock anywhere in the system — the only
//     timebase is inbound `tx_stream_in`.
// T2: that stream is started AND STOPPED by the radio, at key and unkey.
// T3: therefore the ~120 ms EOO tail cannot go out on the normal path. By the
//     time it is due, nothing is driving the pump. Holding PTT longer does not
//     help; the radio is not asking us for anything.
//
// ─── Why this file is written the way it is ────────────────────────────────
// A test that keys, waits out the tail duration, and unkeys PASSES WITH THE
// ENTIRE DRAIN MECHANISM DELETED — because while PTT is held the real clock is
// still running, so ticks keep arriving and nothing looks wrong. That is the
// exact shape of the bug T3 warns about, reproduced in the test that was
// supposed to catch it.
//
// So the assertions here are specifically that ticks continue while INBOUND HAS
// STOPPED, and `tailIsNotDrivenByInbound` proves the ticks were synthesized by
// checking the stream received no datagrams at all during the drain. That is
// the difference between testing the tail and testing the transmitter.

#include <vector>

#include <QSignalSpy>
#include <QUdpSocket>
#include <QtEndian>
#include <QtTest>

#include "core/backends/flex/FlexWaveformStream.h"
#include "core/backends/flex/FlexWaveformTransport.h"

using AetherSDR::FlexWaveformProvider;
using AetherSDR::FlexWaveformStream;
using AetherSDR::FlexWaveformTransport;

namespace {

// Verbatim from the live FLEX-8400 (fw 4.2.20.41343).
const char* kCreateReply =
    "tx_stream_in_id=0X81000005 rx_stream_in_id=0X81000004 "
    "tx_stream_out_id=0X1000005 rx_stream_out_id=0X1000004 "
    "byte_stream_in_id=0X80100002 byte_stream_out_id=0X100002";

constexpr quint32 kTxStreamIn = 0x81000005u;

// A command sink that answers like the radio: the create reply above, code 0
// for everything else. Records what was asked, so ordering can be checked.
FlexWaveformProvider::CommandSink makeSink(QStringList* log) {
    return [log](const QString& cmd, FlexWaveformProvider::ReplyFn reply) {
        if (log) log->append(cmd);
        if (!reply) return;
        if (cmd.startsWith(QLatin1String("waveform create")))
            reply(0, QString::fromLatin1(kCreateReply));
        else
            reply(0, QString());
    };
}

// One tx_stream_in datagram, as the radio would send it: type 3, 128 samples.
QByteArray txClockDatagram(int count) {
    QByteArray d;
    d.resize(28 + 128 * 8);
    d.fill('\0');
    char* p = d.data();
    const quint32 word0 = 0x30000000u | (static_cast<quint32>(count & 0x0F) << 16);
    qToBigEndian(word0, p);
    qToBigEndian(kTxStreamIn, p + 4);
    qToBigEndian(quint32(0x001C2Du), p + 8);
    qToBigEndian(quint32(FlexWaveformStream::kPccIfNarrow), p + 12);
    return d;
}

}  // namespace

class FlexWaveformTransportTest : public QObject {
    Q_OBJECT

private slots:
    void bindsBeforeRegisteringAndUdpPortIsLast();
    void wiresOnlyInboundStreamIds();
    void realClockDrivesTicksWhileKeyed();
    void tailIsEmittedAfterInboundStops();   // §7.1 T3 — the load-bearing one
    void tailIsNotDrivenByInbound();         // proves the above is real
    void tailTimeoutReleasesPtt();
    void cancelReleasesPttAndStopsEmitting();// §7.1 T8
    void unkeyWithoutKeyStillAnswers();
};

void FlexWaveformTransportTest::bindsBeforeRegisteringAndUdpPortIsLast() {
    QStringList log;
    FlexWaveformTransport t;
    t.setCommandSink(makeSink(&log));
    QSignalSpy readySpy(&t, &FlexWaveformTransport::ready);

    QVERIFY(t.start(0));
    QCOMPARE(readySpy.count(), 1);

    // §7.1 R1 — the port must exist before registration, because `udpport` is
    // the LAST thing sent. That ordering is why the provider and the stream are
    // separate objects.
    const quint16 port = t.stream()->boundPort();
    QVERIFY(port != 0);

    // Note: QStringList::indexOf(QRegularExpression) matches the WHOLE string,
    // not a substring, so find by predicate instead.
    const auto findFirst = [&log](const QString& needle) {
        for (int i = 0; i < log.size(); ++i)
            if (log.at(i).contains(needle)) return i;
        return -1;
    };
    const int createAt = findFirst(QStringLiteral("waveform create"));
    const int udpAt    = findFirst(QStringLiteral("udpport="));
    QVERIFY(createAt >= 0);
    QVERIFY(udpAt >= 0);
    QVERIFY2(udpAt > createAt, "udpport must follow create");
    QCOMPARE(udpAt, log.size() - 1);          // and be last
    QVERIFY(log.last().contains(QString::number(port)));

    // §7.1 R4 — tx_filter is accepted and ignored, so it is never sent.
    for (const QString& c : log)
        QVERIFY2(!c.contains(QLatin1String("tx_filter")),
                 "tx_filter is accepted-and-ignored (§7.1 R4); sending it implies "
                 "a control we do not have");
}

void FlexWaveformTransportTest::wiresOnlyInboundStreamIds() {
    // §7.1 E1 — emit on the INBOUND ids; the *_out ids are measured silent.
    FlexWaveformTransport t;
    t.setCommandSink(makeSink(nullptr));
    QVERIFY(t.start(0));

    QCOMPARE(t.stream()->rxStreamId(), 0x81000004u);
    QCOMPARE(t.stream()->txStreamId(), kTxStreamIn);
    // Both carry the inbound high bit; neither is an *_out value.
    QVERIFY(t.stream()->rxStreamId() & 0x80000000u);
    QVERIFY(t.stream()->txStreamId() & 0x80000000u);
    QVERIFY(t.stream()->rxStreamId() != 0x01000004u);
    QVERIFY(t.stream()->txStreamId() != 0x01000005u);
}

void FlexWaveformTransportTest::realClockDrivesTicksWhileKeyed() {
    // §7.1 T1 — inbound tx_stream_in is the clock. Establishes the positive
    // control: without this, a null result in the T3 tests would say nothing.
    FlexWaveformTransport t;
    t.setCommandSink(makeSink(nullptr));
    QVERIFY(t.start(0));
    const quint16 port = t.stream()->boundPort();

    QSignalSpy tickSpy(&t, &FlexWaveformTransport::txClockTick);
    t.beginTx();
    QCOMPARE(t.txState(), FlexWaveformTransport::TxState::Voice);

    QUdpSocket radio;
    for (int i = 0; i < 5; ++i)
        radio.writeDatagram(txClockDatagram(i), QHostAddress::LocalHost, port);
    QTRY_COMPARE(tickSpy.count(), 5);

    // Real ticks are flagged as such — the codec has to be able to tell a voice
    // frame from a synthesized tail tick.
    for (const auto& args : tickSpy) {
        QCOMPARE(args.at(0).toInt(), 128);
        QCOMPARE(args.at(1).toBool(), false);
    }
}

void FlexWaveformTransportTest::tailIsEmittedAfterInboundStops() {
    // ─────────────────────────────────────────────────────────────────────
    // §7.1 T3. The whole point of the transport.
    //
    // Key, let the radio drive a few frames, then STOP SENDING ENTIRELY — which
    // is what the radio does at unkey (T2) — and unkey. The tail must still be
    // clocked out. If the drain mechanism is missing, tick production stops
    // dead here and this fails.
    // ─────────────────────────────────────────────────────────────────────
    FlexWaveformTransport t;
    t.setCommandSink(makeSink(nullptr));
    QVERIFY(t.start(0));
    const quint16 port = t.stream()->boundPort();

    QUdpSocket radio;
    t.beginTx();
    for (int i = 0; i < 4; ++i)
        radio.writeDatagram(txClockDatagram(i), QHostAddress::LocalHost, port);

    QSignalSpy tickSpy(&t, &FlexWaveformTransport::txClockTick);
    QTRY_COMPARE(tickSpy.count(), 4);
    tickSpy.clear();

    // From here the radio sends NOTHING. Only the transport can produce ticks.
    QSignalSpy unkeySpy(&t, &FlexWaveformTransport::readyToUnkey);
    t.requestEndTx();
    QCOMPARE(t.txState(), FlexWaveformTransport::TxState::Draining);

    // The EOO is ~120 ms at 128 samples / 24 kHz ≈ 5.33 ms per tick, so ~22
    // ticks. Assert a clear majority of that rather than an exact count, since
    // timer granularity is not the thing under test.
    QTRY_VERIFY_WITH_TIMEOUT(tickSpy.count() >= 15, 400);
    QVERIFY2(tickSpy.count() > 0,
             "no ticks after inbound stopped — the EOO tail would be silently "
             "truncated (§7.1 T3); holding PTT alone emits nothing");

    // Every one of them must be flagged synthesized.
    for (const auto& args : tickSpy)
        QCOMPARE(args.at(1).toBool(), true);

    // And the codec ending the tail must release PTT.
    QCOMPARE(unkeySpy.count(), 0);
    t.notifyTxTailComplete();
    QCOMPARE(unkeySpy.count(), 1);
    QCOMPARE(t.txState(), FlexWaveformTransport::TxState::Idle);
}

void FlexWaveformTransportTest::tailIsNotDrivenByInbound() {
    // Proves the previous test is not measuring the transmitter.
    //
    // The naive version of that test keeps PTT held and therefore keeps the
    // REAL clock running, so it passes with the drain mechanism deleted. Here
    // we assert the stream received no datagrams whatsoever during the drain —
    // so every tick counted above was necessarily synthesized.
    FlexWaveformTransport t;
    t.setCommandSink(makeSink(nullptr));
    QVERIFY(t.start(0));

    t.beginTx();
    const quint64 datagramsAtUnkey = t.stream()->stats().datagrams;

    QSignalSpy tickSpy(&t, &FlexWaveformTransport::txClockTick);
    t.requestEndTx();
    QTRY_VERIFY_WITH_TIMEOUT(tickSpy.count() >= 10, 400);

    QCOMPARE(t.stream()->stats().datagrams, datagramsAtUnkey);
    QVERIFY2(tickSpy.count() >= 10,
             "ticks must come from the drain timer, with zero inbound packets");
}

void FlexWaveformTransportTest::tailTimeoutReleasesPtt() {
    // Safety backstop. A codec that never reports completion must not hold the
    // transmitter keyed: a truncated EOO is a decode problem, a stuck carrier
    // is an interference problem.
    FlexWaveformTransport t;
    t.setCommandSink(makeSink(nullptr));
    QVERIFY(t.start(0));
    t.setTailTimeoutMs(60);

    QSignalSpy unkeySpy(&t, &FlexWaveformTransport::readyToUnkey);
    t.beginTx();
    t.requestEndTx();

    // notifyTxTailComplete() is deliberately never called.
    QTRY_VERIFY_WITH_TIMEOUT(unkeySpy.count() == 1, 1000);
    QCOMPARE(t.txState(), FlexWaveformTransport::TxState::Idle);
}

void FlexWaveformTransportTest::cancelReleasesPttAndStopsEmitting() {
    // §7.1 T8 — TX_FAULT / TIMEOUT / STUCK_INPUT all drive to CANCEL. A cancel
    // does not try to flush a tail: the chain is already wrong. But PTT is
    // still held on our say-so, so it must be released on this path too.
    FlexWaveformTransport t;
    t.setCommandSink(makeSink(nullptr));
    QVERIFY(t.start(0));
    t.setRadioAddress(QHostAddress::LocalHost);

    QSignalSpy cancelSpy(&t, &FlexWaveformTransport::txCancelled);
    QSignalSpy unkeySpy(&t, &FlexWaveformTransport::readyToUnkey);

    t.beginTx();
    t.requestEndTx();
    QCOMPARE(t.txState(), FlexWaveformTransport::TxState::Draining);

    t.cancelTx(QStringLiteral("STUCK_INPUT"));
    QCOMPARE(cancelSpy.count(), 1);
    QCOMPARE(cancelSpy.first().at(0).toString(), QStringLiteral("STUCK_INPUT"));
    QCOMPARE(unkeySpy.count(), 1);

    // Drain must have stopped: no further ticks.
    QSignalSpy tickSpy(&t, &FlexWaveformTransport::txClockTick);
    QTest::qWait(60);
    QCOMPARE(tickSpy.count(), 0);
}

void FlexWaveformTransportTest::unkeyWithoutKeyStillAnswers() {
    // The caller's contract is "wait for readyToUnkey() before dropping PTT".
    // That has to hold on every path, including the uninteresting one, or a
    // caller that waits will hang.
    FlexWaveformTransport t;
    t.setCommandSink(makeSink(nullptr));
    QVERIFY(t.start(0));

    QSignalSpy unkeySpy(&t, &FlexWaveformTransport::readyToUnkey);
    t.requestEndTx();
    QCOMPARE(unkeySpy.count(), 1);
    QCOMPARE(t.txState(), FlexWaveformTransport::TxState::Idle);
}

QTEST_MAIN(FlexWaveformTransportTest)
#include "flex_waveform_transport_test.moc"
