#include "core/backends/flex/FlexWaveformTransport.h"

#ifdef AETHER_ENABLE_RADE_V2

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QTimer>

#include "core/backends/flex/FlexWaveformStream.h"

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcRadeV2Tx, "aether.radev2.tx")

namespace {
// One packet's worth of clock, at the measured rate: 128 samples / 24 kHz.
// The drain timer fires faster than this and emits however many periods are
// actually owed, so the tail does not drift with timer jitter.
constexpr int    kSamplesPerTick = FlexWaveformStream::kTxSamplesPerPkt;
constexpr double kTickMs         = 1000.0 * kSamplesPerTick
                                 / FlexWaveformStream::kRxSampleRateHz;   // 5.333
constexpr int    kDrainTimerMs   = 2;
constexpr int    kDefaultTailTimeoutMs = 500;   // EOO is ~120 ms; backstop only
}  // namespace

struct FlexWaveformTransport::Private {
    FlexWaveformProvider* provider = nullptr;
    FlexWaveformStream*   stream   = nullptr;

    TxState txState = TxState::Idle;
    QHostAddress radioAddr;

    QTimer*       drainTimer = nullptr;
    QElapsedTimer drainClock;
    qint64        ticksEmitted = 0;
    int           tailTimeoutMs = kDefaultTailTimeoutMs;
};

FlexWaveformTransport::FlexWaveformTransport(QObject* parent)
    : QObject(parent), d(new Private) {
    d->provider = new FlexWaveformProvider(this);
    d->stream   = new FlexWaveformStream(this);

    connect(d->provider, &FlexWaveformProvider::registered,
            this, &FlexWaveformTransport::onRegistered);
    connect(d->provider, &FlexWaveformProvider::failed,
            this, &FlexWaveformTransport::failed);

    // RX is a straight relay: the stream has already handled framing and the
    // X2 conjugation, so there is nothing left for the transport to do to it.
    connect(d->stream, &FlexWaveformStream::rxPassband,
            this, &FlexWaveformTransport::rxPassband);

    // §7.1 T1 — the radio's packets ARE the transmit clock.
    connect(d->stream, &FlexWaveformStream::txClockPacket,
            this, &FlexWaveformTransport::onTxClockPacket);

    d->drainTimer = new QTimer(this);
    d->drainTimer->setTimerType(Qt::PreciseTimer);
    d->drainTimer->setInterval(kDrainTimerMs);
    connect(d->drainTimer, &QTimer::timeout, this, &FlexWaveformTransport::driveDrain);
}

FlexWaveformTransport::~FlexWaveformTransport() {
    stop();
    delete d;
}

void FlexWaveformTransport::setCommandSink(FlexWaveformProvider::CommandSink sink) {
    d->provider->setCommandSink(std::move(sink));
}

void FlexWaveformTransport::setRadioAddress(const QHostAddress& addr) {
    d->radioAddr = addr;
    d->stream->setRadioAddress(addr);
}

bool FlexWaveformTransport::start(quint16 preferredUdpPort) {
    // §7.1 R1 — bind FIRST. `udpport` is the last thing registration sends, so
    // the number has to exist before the conversation starts. This ordering is
    // the reason the provider and the stream are separate objects at all.
    const quint16 port = d->stream->open(preferredUdpPort);
    if (port == 0) {
        emit failed(QStringLiteral("could not bind a UDP port for the waveform streams"));
        return false;
    }
    qCDebug(lcRadeV2Tx) << "bound" << port << "— registering";
    d->provider->registerWaveform(port);
    return true;
}

void FlexWaveformTransport::stop() {
    d->drainTimer->stop();
    d->provider->removeWaveform();
    d->stream->close();
    setTxState(TxState::Idle);
}

void FlexWaveformTransport::onRegistered(const WaveformStreams& streams) {
    // §7.1 E1 — the INBOUND ids, and only those. The stream's emit API takes no
    // id precisely so the *_out values cannot be wired here by accident.
    // §7.1 R6 — re-read on every registration; they are recycled.
    d->stream->setRxStreamId(streams.rxStreamIn);
    d->stream->setTxStreamId(streams.txStreamIn);
    if (!d->radioAddr.isNull())
        d->stream->setRadioAddress(d->radioAddr);
    qCDebug(lcRadeV2Tx) << "ready";
    emit ready();
}

FlexWaveformTransport::TxState FlexWaveformTransport::txState() const {
    return d->txState;
}

void FlexWaveformTransport::setTxState(TxState next) {
    if (d->txState == next) return;
    d->txState = next;
    emit txStateChanged(next);
}

void FlexWaveformTransport::setTailTimeoutMs(int ms) {
    d->tailTimeoutMs = ms < 0 ? 0 : ms;
}

int FlexWaveformTransport::tailTimeoutMs() const { return d->tailTimeoutMs; }

FlexWaveformProvider* FlexWaveformTransport::provider() const { return d->provider; }
FlexWaveformStream*   FlexWaveformTransport::stream() const   { return d->stream; }

void FlexWaveformTransport::beginTx() {
    d->drainTimer->stop();
    // §7.1 T2 — nothing flows until the radio starts it. Entering Voice here is
    // a statement of intent; the first real tick still comes from the radio.
    setTxState(TxState::Voice);
    qCDebug(lcRadeV2Tx) << "tx begin";
}

void FlexWaveformTransport::onTxClockPacket(int samples) {
    if (d->txState == TxState::Cancelled) return;

    // A real tick arriving during Draining means the radio is still keyed and
    // still asking — so prefer it over our synthesized one and reset the drain
    // clock. This is not expected (T2 says the stream stops at unkey) but if it
    // happens, the radio's cadence is authoritative and ours is a fallback.
    if (d->txState == TxState::Draining)
        d->drainClock.restart();

    emit txClockTick(samples > 0 ? samples : kSamplesPerTick, /*synthesized=*/false);
}

void FlexWaveformTransport::requestEndTx() {
    if (d->txState != TxState::Voice) {
        // Nothing keyed — there is no tail to flush, so unkeying is immediately
        // safe. Still answer, so the caller's wait-for-readyToUnkey() contract
        // holds in every path rather than only the interesting one.
        emit readyToUnkey();
        return;
    }

    // ─────────────────────────────────────────────────────────────────────
    // §7.1 T3 — THE TRAP.
    //
    // The radio has stopped, or is about to stop, delivering tx_stream_in.
    // With no free-running clock (T1), the ~120 ms EOO tail would simply never
    // be emitted: the pump has nothing driving it. Holding PTT does not help —
    // the radio is not asking us for anything, so nothing goes out. The far end
    // then never sees an end-of-over.
    //
    // So from here the transport synthesizes the clock itself, exactly as the
    // vendored D-Star provider does when it stamps its own buffer descriptors
    // during drain. That mechanism is the design we reuse (§10.5 item 4); the
    // code is reimplemented, not linked (§11).
    //
    // A test that keys, waits out the tail duration, then unkeys will pass even
    // with this entire branch deleted — because while PTT is held the REAL
    // clock is still running. The guard has to prove ticks continue with
    // inbound stopped.
    // ─────────────────────────────────────────────────────────────────────
    setTxState(TxState::Draining);
    d->ticksEmitted = 0;
    d->drainClock.start();
    d->drainTimer->start();
    qCDebug(lcRadeV2Tx) << "tx end requested — draining tail (synthesized clock)";
}

void FlexWaveformTransport::driveDrain() {
    if (d->txState != TxState::Draining) {
        d->drainTimer->stop();
        return;
    }

    const qint64 elapsed = d->drainClock.elapsed();

    // Safety backstop before anything else. A codec that never reports
    // completion must not hold the transmitter keyed: a truncated EOO is a
    // decode problem, a stuck carrier is an interference problem.
    if (d->tailTimeoutMs > 0 && elapsed > d->tailTimeoutMs) {
        qCWarning(lcRadeV2Tx) << "tail timeout after" << elapsed
                              << "ms — unkeying with the EOO incomplete";
        finishDrain("timeout");
        return;
    }

    // Emit however many ticks are owed rather than one per timer fire, so the
    // tail keeps the modem's real cadence under timer jitter instead of
    // drifting slow.
    const qint64 owed = static_cast<qint64>(elapsed / kTickMs) - d->ticksEmitted;
    for (qint64 i = 0; i < owed; ++i) {
        d->ticksEmitted++;
        emit txClockTick(kSamplesPerTick, /*synthesized=*/true);
        if (d->txState != TxState::Draining) return;   // codec finished mid-burst
    }
}

void FlexWaveformTransport::notifyTxTailComplete() {
    if (d->txState != TxState::Draining) return;
    finishDrain("codec reported tail complete");
}

void FlexWaveformTransport::finishDrain(const char* why) {
    d->drainTimer->stop();
    setTxState(TxState::Idle);
    qCDebug(lcRadeV2Tx) << "drain finished:" << why << "— safe to unkey";

    // §10.19 — what we EMIT decodes and what comes off the air does not, so the
    // damage is downstream of this socket. `emitFailed` counts sends that never
    // left, and a lost packet is a hole in the radio's playout that nothing on
    // our side would otherwise reveal. It has been counted since the first over
    // and read by nobody, which is exactly how txUnderruns hid.
    //
    // Logged as a warning only when non-zero: a clean over should not add noise
    // to the log, but a dirty one must not stay quiet.
    const auto s = d->stream->stats();
    if (s.emitFailed > 0) {
        qCWarning(lcRadeV2Tx).nospace()
            << "tx emit failures " << s.emitFailed << " of " << s.emitted
            << " packets — each is a gap in the radio's playout";
    } else {
        qCInfo(lcRadeV2Tx) << "tx emitted" << s.emitted << "packets, 0 send failures";
    }

    emit readyToUnkey();
}

void FlexWaveformTransport::cancelTx(const QString& reason) {
    // §7.1 T8 — TX_FAULT / TIMEOUT / STUCK_INPUT. Unlike the drain path this
    // does NOT try to flush a tail: a cancel means the transmit chain is
    // already wrong, and the right move is to stop rather than to keep feeding
    // it. readyToUnkey() still fires, because whoever is holding PTT is holding
    // it on our say-so and must be released on every exit path.
    d->drainTimer->stop();
    setTxState(TxState::Cancelled);
    qCWarning(lcRadeV2Tx) << "tx cancelled:" << reason;
    emit txCancelled(reason);
    emit readyToUnkey();
    setTxState(TxState::Idle);
}

void FlexWaveformTransport::submitDecodedAudio(const std::vector<float>& mono) {
    if (mono.empty()) return;
    // §7.1 E1/E2 — inbound id, stereo audio, no conjugation. All of that lives
    // in the stream; the transport must not "help" by pre-converting.
    d->stream->emitRxReturn(mono);
}

void FlexWaveformTransport::submitModulatedAudio(const std::vector<float>& mono) {
    if (mono.empty()) return;
    if (d->txState == TxState::Cancelled) return;   // §7.1 T8
    d->stream->emitTxReturn(mono);
}

}  // namespace AetherSDR

#endif  // AETHER_ENABLE_RADE_V2
