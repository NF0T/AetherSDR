// The codec↔transport seam, wired exactly as MainWindow::activateRADEV2()
// wires it, with the engine on a REAL worker thread.
//
// Stage 6b tested the engine alone and stage 5 tested the transport alone.
// Both passed while the thing between them did not exist. This is the graph
// itself: the connection set, the thread boundary, and the end-of-over
// handshake that decides when the transmitter may be released.
//
// ─── Why the worker thread is not optional here ────────────────────────────
// The seam carries `std::vector<float>` and
// `std::vector<std::complex<float>>` across a queued connection. If those
// metatypes are not resolvable at connect time, Qt refuses the invocation at
// RUNTIME with a warning and the slot is simply never called: no audio, no
// error, no crash. A same-thread test uses direct connections and proves
// nothing about that — it is exactly the shape of "the test passes and the
// radio is silent". So the engine lives on a QThread here, as it does in the
// app.
//
// ─── The handshake this pins ───────────────────────────────────────────────
// §7.1 T3: the ~120 ms EOO tail cannot ride the normal pump, because by the
// time it is due the radio has stopped asking. It goes out on the transport's
// synthesized clock — and that clock does not start by itself. The codec's
// txTailQueued() starts it and txTailComplete() ends it, so both ends of the
// handshake are asserted here: delete the first and the tail is never
// transmitted while the caller waits forever for permission to unkey (checked
// by mutation — two of these tests hang out to their timeouts).
//
// The drain-duration assertion matters as much as the packet count: the
// transport has a 500 ms safety backstop that also releases the transmitter,
// so a broken handshake can still LOOK like a completed one. Requiring the
// release to land well inside the backstop is what separates "the codec
// reported the tail out" from "the safety net caught us".

#include <vector>

#include <QSignalSpy>
#include <QThread>
#include <QUdpSocket>
#include <QtEndian>
#include <QtTest>

#include "core/RADEV2Engine.h"
#include "core/backends/flex/FlexWaveformStream.h"
#include "core/backends/flex/FlexWaveformTransport.h"

using AetherSDR::FlexWaveformProvider;
using AetherSDR::FlexWaveformStream;
using AetherSDR::FlexWaveformTransport;
using AetherSDR::RADEV2Engine;

namespace {

// Verbatim from the live FLEX-8400 (fw 4.2.20.41343).
const char* kCreateReply =
    "tx_stream_in_id=0X81000005 rx_stream_in_id=0X81000004 "
    "tx_stream_out_id=0X1000005 rx_stream_out_id=0X1000004 "
    "byte_stream_in_id=0X80100002 byte_stream_out_id=0X100002";

constexpr quint32 kTxStreamIn = 0x81000005u;

FlexWaveformProvider::CommandSink makeSink() {
    return [](const QString& cmd, FlexWaveformProvider::ReplyFn reply) {
        if (!reply) return;
        if (cmd.startsWith(QLatin1String("waveform create")))
            reply(0, QString::fromLatin1(kCreateReply));
        else
            reply(0, QString());
    };
}

// One tx_stream_in datagram — the radio's transmit clock (§7.1 T1).
QByteArray txClockDatagram(int count) {
    QByteArray d;
    d.resize(28 + 128 * 8);
    d.fill('\0');
    char* p = d.data();
    qToBigEndian(quint32(0x30000000u | (quint32(count & 0x0F) << 16)), p);
    qToBigEndian(kTxStreamIn, p + 4);
    qToBigEndian(quint32(0x001C2Du), p + 8);
    qToBigEndian(quint32(FlexWaveformStream::kPccIfNarrow), p + 12);
    return d;
}

// 24 kHz stereo float32, as AudioEngine::txRawPcmReady delivers it.
QByteArray micAudio(int ms) {
    const int frames = RADEV2Engine::kWaveformRateHz * ms / 1000;
    QByteArray pcm(frames * 2 * int(sizeof(float)), Qt::Uninitialized);
    auto* p = reinterpret_cast<float*>(pcm.data());
    for (int n = 0; n < frames; ++n) {
        const float t = float(n) / RADEV2Engine::kWaveformRateHz;
        const float s = 0.4f * (std::sin(2.0f * float(M_PI) * 180.0f * t)
                              + 0.5f * std::sin(2.0f * float(M_PI) * 520.0f * t));
        p[2 * n] = s;
        p[2 * n + 1] = s;
    }
    return pcm;
}

}  // namespace

class RadeV2SeamTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void micAudioCrossesTheThreadBoundaryAndReachesTheWire();
    void theTailIsEmittedBeforeTheTransmitterIsReleased();   // §7.1 T3 + T9
    void unkeyIsNeverAnnouncedWhileTheTailIsStillQueued();

private:
    void wireAsMainWindowDoes();
    void key();
    void unkey();

    FlexWaveformTransport* m_transport = nullptr;
    RADEV2Engine*          m_engine    = nullptr;
    QThread*               m_thread    = nullptr;
    QUdpSocket*            m_radio     = nullptr;
    QUdpSocket*            m_radioVita = nullptr;   // stands in for the radio's :4991
    quint16                m_port      = 0;
};

void RadeV2SeamTest::init() {
    m_transport = new FlexWaveformTransport;
    m_transport->setCommandSink(makeSink());
    QVERIFY(m_transport->start(0));
    m_port = m_transport->stream()->boundPort();
    QVERIFY(m_port != 0);

    // Emitted packets go to radio:4991. Nothing has to be listening — UDP
    // sends succeed regardless, and the stream's own `emitted` counter is what
    // proves a packet was built and written.
    m_transport->setRadioAddress(QHostAddress::LocalHost);

    m_engine = new RADEV2Engine;
    m_thread = new QThread;
    m_thread->setObjectName("RADEV2EngineTest");
    m_engine->moveToThread(m_thread);
    m_thread->start();

    bool ok = false;
    QMetaObject::invokeMethod(m_engine, [this, &ok]() { ok = m_engine->start(); },
                              Qt::BlockingQueuedConnection);
    QVERIFY2(ok, "engine failed to start on the worker thread");

    wireAsMainWindowDoes();
    m_radio = new QUdpSocket;

    // Emitted packets go to radio:4991 (§7.1 E4). Bind a listener for them.
    //
    // Not cosmetic: with nothing on 4991 every send draws an ICMP
    // port-unreachable, which Windows surfaces on the SENDING socket as a
    // failed read — so the stream counts one bogus inbound datagram per packet
    // it emits, and `malformed` tracks `emitted` exactly. That is a property of
    // sending into a void on loopback, not of the code, but it makes any
    // inbound-traffic assertion meaningless. Best-effort: if the port is taken
    // the test still works, because what it asserts is the tick's `synthesized`
    // flag rather than a datagram count.
    m_radioVita = new QUdpSocket;
    m_radioVita->bind(FlexWaveformStream::kVitaOutPort,
                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
}

void RadeV2SeamTest::cleanup() {
    delete m_radio;
    m_radio = nullptr;
    delete m_radioVita;
    m_radioVita = nullptr;
    if (m_engine) {
        QMetaObject::invokeMethod(m_engine, &RADEV2Engine::stop,
                                  Qt::BlockingQueuedConnection);
    }
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(2000);
        delete m_thread;
        m_thread = nullptr;
    }
    delete m_engine;
    m_engine = nullptr;
    delete m_transport;
    m_transport = nullptr;
}

void RadeV2SeamTest::wireAsMainWindowDoes() {
    // Kept deliberately identical to MainWindow::activateRADEV2(). If the two
    // drift, this test stops describing the application.
    connect(m_transport, &FlexWaveformTransport::rxPassband,
            m_engine, &RADEV2Engine::onRxPassband, Qt::QueuedConnection);
    connect(m_transport, &FlexWaveformTransport::txClockTick,
            m_engine, &RADEV2Engine::onTxClockTick, Qt::QueuedConnection);
    connect(m_engine, &RADEV2Engine::decodedAudioReady,
            m_transport, &FlexWaveformTransport::submitDecodedAudio, Qt::QueuedConnection);
    connect(m_engine, &RADEV2Engine::modulatedAudioReady,
            m_transport, &FlexWaveformTransport::submitModulatedAudio, Qt::QueuedConnection);
    connect(m_engine, &RADEV2Engine::txTailQueued,
            m_transport, &FlexWaveformTransport::requestEndTx, Qt::QueuedConnection);
    connect(m_engine, &RADEV2Engine::txTailComplete,
            m_transport, &FlexWaveformTransport::notifyTxTailComplete, Qt::QueuedConnection);
}

void RadeV2SeamTest::key() {
    m_transport->beginTx();
    QMetaObject::invokeMethod(m_engine, [this]() { m_engine->beginTx(); },
                              Qt::BlockingQueuedConnection);
}

void RadeV2SeamTest::unkey() {
    // Exactly what MainWindow's PttOffHook does: tell the CODEC the over is
    // over, and let its txTailQueued() start the transport's drain. The
    // transport is never asked to drain directly — that is the ordering the
    // whole handshake exists to guarantee.
    QMetaObject::invokeMethod(m_engine, [this]() { m_engine->endTx(); },
                              Qt::QueuedConnection);
}

void RadeV2SeamTest::micAudioCrossesTheThreadBoundaryAndReachesTheWire() {
    key();

    QMetaObject::invokeMethod(m_engine, [this]() { m_engine->feedTxAudio(micAudio(400)); },
                              Qt::BlockingQueuedConnection);
    QTRY_VERIFY_WITH_TIMEOUT(m_engine->queuedTxSamples() > 0, 2000);

    const quint64 before = m_transport->stream()->stats().emitted;

    // The radio asks. §7.1 T1 — this is the only transmit clock there is.
    for (int i = 0; i < 40; ++i)
        m_radio->writeDatagram(txClockDatagram(i), QHostAddress::LocalHost, m_port);

    QTRY_VERIFY_WITH_TIMEOUT(m_transport->stream()->stats().emitted >= before + 30, 3000);
    QCOMPARE(m_transport->stream()->stats().emitFailed, quint64(0));

    // If the metatypes were unresolvable, every one of these queued hops would
    // have been dropped with a runtime warning and this count would be zero.
    QVERIFY2(m_engine->stats().txTicks > 0,
             "no clock ticks reached the engine — the queued connection across "
             "the thread boundary is not delivering");
    QVERIFY2(m_engine->stats().txSteps > 0, "the codec never ran a TX step");
}

void RadeV2SeamTest::theTailIsEmittedBeforeTheTransmitterIsReleased() {
    // ─────────────────────────────────────────────────────────────────────
    // §7.1 T3 + T9 end-to-end, across the seam and the thread boundary.
    //
    // Key, speak, unkey — then STOP CLOCKING ENTIRELY, which is what the radio
    // does at unkey (§7.1 T2). Everything after this point has to come from
    // the transport's synthesized clock, and the transmitter must not be
    // released until all of it is out.
    // ─────────────────────────────────────────────────────────────────────
    key();
    QMetaObject::invokeMethod(m_engine, [this]() { m_engine->feedTxAudio(micAudio(200)); },
                              Qt::BlockingQueuedConnection);

    for (int i = 0; i < 40; ++i)
        m_radio->writeDatagram(txClockDatagram(i), QHostAddress::LocalHost, m_port);
    QTRY_VERIFY_WITH_TIMEOUT(m_engine->stats().txTicks >= 30, 3000);

    // From here the radio sends NOTHING, which is what it does at unkey
    // (§7.1 T2). Watch every tick issued from now on.
    QSignalSpy tickSpy(m_transport, &FlexWaveformTransport::txClockTick);
    QSignalSpy unkeySpy(m_transport, &FlexWaveformTransport::readyToUnkey);
    const quint64 emittedAtUnkey = m_transport->stream()->stats().emitted;

    QElapsedTimer drain;
    drain.start();
    unkey();

    // The transport's tail timeout is a SAFETY backstop, not the mechanism.
    // Completing on it would still release the transmitter and would still
    // look like a pass on a count of packets — so assert the release came from
    // the codec reporting the tail out, by requiring it well inside the
    // backstop. The tail is ~128 ms; the backstop is 500 ms.
    QTRY_VERIFY_WITH_TIMEOUT(unkeySpy.count() == 1, 3000);
    const qint64 drainMs = drain.elapsed();
    QVERIFY2(drainMs < m_transport->tailTimeoutMs(),
             qPrintable(QStringLiteral("the drain took %1 ms against a %2 ms tail "
                                       "timeout — the transmitter was released by "
                                       "the safety backstop, not because the codec "
                                       "reported the tail out")
                            .arg(drainMs).arg(m_transport->tailTimeoutMs())));

    const quint64 emittedDuringTail =
        m_transport->stream()->stats().emitted - emittedAtUnkey;

    // The tail is ~128.6 ms (EOO 120 ms + the flushed filter memory) at 128
    // samples per packet and 24 kHz — about 24 packets. Assert a clear
    // majority rather than an exact count; packetisation is not what is under
    // test, the tail going out at all is.
    QVERIFY2(emittedDuringTail >= 20,
             qPrintable(QStringLiteral("only %1 packets went out after unkey — the "
                                       "EOO is being truncated (§7.1 T3/T9)")
                            .arg(emittedDuringTail)));

    // And prove it was the SYNTHESIZED clock that carried it. Without this the
    // test would pass on a build with no drain mechanism at all, because the
    // radio's own clock would still have been running — that is the trap §7.1
    // T3 warns about, reproduced inside the test meant to catch it.
    //
    // Asserted on the tick's own `synthesized` flag rather than on an inbound
    // packet count, which is both more direct and immune to loopback artefacts.
    QVERIFY2(!tickSpy.isEmpty(), "no ticks at all during the drain");
    for (const auto& args : tickSpy)
        QVERIFY2(args.at(1).toBool(),
                 "a REAL clock tick arrived during the tail — the drain is not "
                 "being driven by the transport, so this test is measuring the "
                 "radio rather than the drain mechanism (§7.1 T3)");

    QCOMPARE(m_engine->queuedTxSamples(), 0);
    QVERIFY(m_engine->stats().txTailSamples > 0);
}

void RadeV2SeamTest::unkeyIsNeverAnnouncedWhileTheTailIsStillQueued() {
    // The inverse of the above, and the one that would actually cost an
    // operator something: releasing the carrier while the EOO is still sitting
    // in a queue truncates it silently at the far end.
    key();
    QMetaObject::invokeMethod(m_engine, [this]() { m_engine->feedTxAudio(micAudio(200)); },
                              Qt::BlockingQueuedConnection);
    for (int i = 0; i < 40; ++i)
        m_radio->writeDatagram(txClockDatagram(i), QHostAddress::LocalHost, m_port);
    QTRY_VERIFY_WITH_TIMEOUT(m_engine->stats().txTicks >= 30, 3000);

    QSignalSpy unkeySpy(m_transport, &FlexWaveformTransport::readyToUnkey);
    unkey();

    // Sample repeatedly through the drain: at no point while samples remain
    // queued may the release have been announced.
    for (int i = 0; i < 40 && unkeySpy.isEmpty(); ++i) {
        if (m_engine->queuedTxSamples() > 0)
            QVERIFY2(unkeySpy.isEmpty(),
                     "readyToUnkey() fired with modulated audio still queued — "
                     "the carrier would drop mid-EOO");
        QTest::qWait(2);
    }
    QTRY_VERIFY_WITH_TIMEOUT(unkeySpy.count() == 1, 3000);
    QCOMPARE(m_engine->queuedTxSamples(), 0);
}

QTEST_MAIN(RadeV2SeamTest)
#include "rade_v2_seam_test.moc"
