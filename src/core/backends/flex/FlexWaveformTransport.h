#pragma once

// FlexWaveformTransport — the object the codec talks to, and the only one it
// ever sees (RFC: docs/architecture/rade-v2-waveform-design.md §6 decision 7,
// §7.2, §12).
//
// Owns the control plane (FlexWaveformProvider) and the data plane
// (FlexWaveformStream) and presents the transport half of the codec↔transport
// seam:  *passband PCM in ↔ modem PCM out + control*.
//
// ─── Why this class exists at all ──────────────────────────────────────────
// §7.2: `FlexBackend` is not modified and does not know RADE exists.
// `IRadioBackend` is a *radio* abstraction with three implementors already;
// RADE is a *codec* with one transport. So the transport is handed a command
// sink from outside and the dependency points one way:
//
//     RADEV2Engine  →  FlexWaveformTransport  →  command sink
//
// Enforced by rade_v2_backend_boundary_test, not by good intentions.
//
// ─── Why the seam is signals, not an interface pointer ─────────────────────
// The codec is a worker-thread QObject. Signal/slot connections auto-queue
// across that boundary; a raw interface pointer would need every caller to get
// the threading right, and getting it wrong produces data races rather than
// compile errors. So there is no `IWaveformCodec` — connect to the signals.
//
// ─── The constraint that shapes the whole TX side ──────────────────────────
// §7.1 T1: **there is no free-running transmit clock anywhere.** The radio's
// scheduler is purely reactive, and the only timebase is inbound
// `tx_stream_in`. §7.1 T2: that stream is started *and stopped* by the radio,
// at key and unkey.
//
// Together those two mean §7.1 T3, which is the trap: **the 120 ms EOO tail
// cannot be emitted by the normal path,** because by the time it is due the
// radio has already stopped driving the pump. Holding PTT longer does not
// help — the radio is not asking us for anything, so nothing is sent. The tail
// needs SYNTHESIZED clock ticks, and this class is where they come from.
//
// A test that keys, waits 120 ms, and unkeys passes straight through that bug,
// because while PTT is held the real clock is still running. The guard has to
// assert that ticks continue *with inbound stopped*.

#ifdef AETHER_ENABLE_RADE_V2

#include <complex>
#include <vector>

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include "core/backends/flex/FlexWaveformProvider.h"

namespace AetherSDR {

class FlexWaveformStream;

class FlexWaveformTransport : public QObject {
    Q_OBJECT

public:
    explicit FlexWaveformTransport(QObject* parent = nullptr);
    ~FlexWaveformTransport() override;

    // §7.2 — the transport is GIVEN its way onto the wire. It does not reach
    // for a backend, and no backend reaches for it.
    void setCommandSink(FlexWaveformProvider::CommandSink sink);
    void setRadioAddress(const QHostAddress& addr);

    // Bind the UDP port, then register. Order matters: §7.1 R1 sends `udpport`
    // last, so the port must exist before registration begins. Returns false if
    // the bind failed; registration success arrives asynchronously as ready().
    bool start(quint16 preferredUdpPort = 0);
    void stop();

    // ─── TX lifecycle ──────────────────────────────────────────────────────
    enum class TxState {
        Idle,        // not transmitting
        Voice,       // keyed; the radio is driving the pump (§7.1 T1)
        Draining,    // operator unkeyed; WE are driving the pump (§7.1 T3)
        Cancelled,   // TX_FAULT / TIMEOUT / STUCK_INPUT (§7.1 T8)
    };
    Q_ENUM(TxState)
    TxState txState() const;

    // Operator keyed. PTT itself is not ours to assert — this only tells the
    // transport to expect the clock to start.
    void beginTx();

    // Operator unkeyed. **Does not mean "drop PTT now."** The tail still has to
    // be fed (§7.1 T3), so the transport enters Draining, synthesizes clock
    // ticks, and emits readyToUnkey() when the codec reports the tail flushed
    // or the timeout expires. The caller must wait for that signal before
    // releasing PTT, or the EOO is truncated and the far end never sees an
    // end-of-over.
    void requestEndTx();

    // §7.1 T8 — TX_FAULT / TIMEOUT / STUCK_INPUT all drive the gate to CANCEL.
    // An explicit path, because a happy-path-only design has no way back.
    void cancelTx(const QString& reason);

    // Upper bound on Draining. The EOO is ~120 ms; this is the backstop for a
    // codec that never reports completion.
    //
    // Safety-relevant, not a tuning knob: without it a stuck codec holds the
    // transmitter keyed indefinitely. On expiry the transport gives up on the
    // tail and emits readyToUnkey() anyway — a truncated EOO is a decode
    // problem, a stuck carrier is an interference problem.
    void setTailTimeoutMs(int ms);
    int tailTimeoutMs() const;

    // Owned; non-owning access for wiring and diagnostics.
    FlexWaveformProvider* provider() const;
    FlexWaveformStream*   stream() const;

public slots:
    // ─── codec → transport ─────────────────────────────────────────────────
    //
    // Decoded speech, to be heard on the slice. Goes out on rx_stream_IN
    // (§7.1 E1) as duplicated stereo audio (§7.1 E2).
    void submitDecodedAudio(const std::vector<float>& mono);

    // Modulated passband, to be transmitted. Out on tx_stream_IN, same rules.
    //
    // §7.1 T6 — band-limit BEFORE calling this. The V2 modulator applies no
    // shaping at all and `tx_filter` is accepted-and-ignored (§7.1 R4), so the
    // transport is the last place that could suppress sidelobes and it does not
    // know how; that belongs in the codec's TX chain (§10.10 item 3a).
    void submitModulatedAudio(const std::vector<float>& mono);

    // The codec has emitted the last of the EOO tail. Ends Draining.
    void notifyTxTailComplete();

signals:
    // Registered, streams bound, ready to carry traffic.
    void ready();
    void failed(const QString& reason);

    // ─── transport → codec ─────────────────────────────────────────────────
    // Demodulated passband, already deconjugated (§7.1 X2).
    void rxPassband(const std::vector<std::complex<float>>& block);

    // §7.1 T1/T3 — drive one TX step. `synthesized` is true when this tick came
    // from the drain timer rather than from the radio, which is the difference
    // between "a normal frame" and "the EOO tail". The codec needs to know:
    // during a synthesized tick there is no mic audio to consume.
    void txClockTick(int samples, bool synthesized);

    // The tail is out. **Only now is it safe to release PTT.**
    void readyToUnkey();

    void txCancelled(const QString& reason);
    void txStateChanged(TxState state);

private:
    void onRegistered(const WaveformStreams& streams);
    void onTxClockPacket(int samples);
    void driveDrain();
    void setTxState(TxState next);
    void finishDrain(const char* why);

    struct Private;
    Private* d;
};

}  // namespace AetherSDR

#endif  // AETHER_ENABLE_RADE_V2
