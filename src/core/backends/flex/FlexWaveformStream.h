#pragma once

// FlexWaveformStream — the RADE V2 waveform DATA plane (RFC:
// docs/architecture/rade-v2-waveform-design.md).
//
// FlexWaveformProvider owns the control plane: it registers the waveform and
// learns the six stream ids. This class owns the UDP socket those streams
// actually flow over. The split is deliberate — registration is a short
// request/reply conversation on TCP 4992, while this runs at ~187 packets a
// second and will grow a TX pump in stage 5. Keeping them apart also means the
// port can be bound BEFORE registration, which matters: §7.1 R1 requires
// `udpport` to be the last thing sent, so the port number has to exist first.
//
// Gated behind ENABLE_RADE_V2 (OFF by default), same as the provider.
//
// ─── The constraints this class exists to honour ───────────────────────────
// X1  24 kHz Complex-float32, big-endian, 128 samples/packet   (inbound)
// X2  🔇 inbound is CONJUGATE I/Q — negate the imaginary component
// X3  discard the ~1 s startup burst
// E1  🔇 emit on the INBOUND id; the *_out ids are not honoured
// E2  🔇 outbound is STEREO AUDIO, not I/Q — duplicate mono, do NOT conjugate
// E3  packet count is a per-STREAM 4-bit rolling counter
// E4  header shape, class id, UTC + picosecond timestamp, → radio:4991
//
// ─── The trap ──────────────────────────────────────────────────────────────
// X2 and E2 are OPPOSITES, and both fail silently.
//
//   inbound  — conjugate I/Q      → negate imag to recover the passband
//   outbound — stereo audio       → duplicate mono, no conjugation at all
//
// The receive path's `-b` looks like it should be symmetric on transmit. It is
// not. Conjugating on the way out inverts the sideband, which reads as normal
// on every meter the radio has and decodes for nobody. Both directions
// therefore convert in static functions with mutation-checked tests, rather
// than sharing one "obviously reusable" helper — the reuse IS the bug.

#ifdef AETHER_ENABLE_RADE_V2

#include <complex>
#include <span>
#include <vector>

#include <QHostAddress>
#include <QObject>
#include <QtGlobal>

class QUdpSocket;

namespace AetherSDR {

class FlexWaveformStream : public QObject {
    Q_OBJECT

public:
    explicit FlexWaveformStream(QObject* parent = nullptr);
    ~FlexWaveformStream() override;

    // ─── Measured wire constants (§7.1 X1, E4) ─────────────────────────────
    static constexpr int      kVitaHeaderBytes = 28;
    static constexpr quint32  kTrailerFlag     = 0x04000000u;  // word0 bit 26
    static constexpr quint16  kPccIfNarrow     = 0x03E3u;      // float32 stereo BE
    static constexpr int      kRxSampleRateHz  = 24000;
    static constexpr int      kRxSamplesPerPkt = 128;

    // Emit side (§7.1 E4). Note the asymmetry, which is real and measured: the
    // radio SENDS us type 3 (ExtDataWithStream) and ACCEPTS type 1
    // (IFDataWithStream). Making the two match would be tidier and does not
    // work.
    static constexpr quint16  kVitaOutPort     = 4991;         // vita_output.c:121
    static constexpr quint32  kFlexOui         = 0x001C2Du;
    static constexpr quint32  kAudioClassId    = 0x534C03E3u;  // SL_VITA_SLICE_AUDIO
    static constexpr quint32  kHdrIfDataStream = 0x10000000u;
    static constexpr quint32  kHdrClassPresent = 0x08000000u;
    static constexpr quint32  kHdrTsiUtc       = 0x00400000u;
    static constexpr quint32  kHdrTsfRealTime  = 0x00200000u;
    static constexpr int      kTxSamplesPerPkt = 128;          // = HAL_TX_BUFFER_SIZE

    // ─── Socket lifecycle ──────────────────────────────────────────────────
    //
    // Bind and return the port the radio should send to — hand it straight to
    // FlexWaveformProvider::registerWaveform(). 0 asks the OS to choose, which
    // is the normal case; a fixed port is only useful for packet captures.
    // Returns 0 on failure.
    quint16 open(quint16 preferredPort = 0);
    void close();
    bool isOpen() const;
    quint16 boundPort() const;

    // The id from `waveform create`. Nothing is accepted until this is set, and
    // setting it resets the settle window and the sequence tracker.
    //
    // §7.1 R6 — ids are RECYCLED across create/remove, so this is re-set on
    // every registration and must never be cached elsewhere as an identity.
    void setRxStreamId(quint32 id);
    quint32 rxStreamId() const;

    // The tx_stream_in id, for the transmit direction (stage 5). Set from the
    // same create reply; same recycling caveat.
    void setTxStreamId(quint32 id);
    quint32 txStreamId() const;

    // Where emitted packets go — the radio's address, port 4991 (§7.1 E4).
    void setRadioAddress(const QHostAddress& addr);
    QHostAddress radioAddress() const;

    // §7.1 X3 — the radio front-loads roughly a second of buffered audio when a
    // stream starts; measured, it reads as a ~28 kHz rate rather than 24 kHz.
    // Packets are dropped for this long after the first one on a given stream.
    // Set to 0 to take everything (the probes do this deliberately, to see it).
    void setSettleMs(int ms);
    int settleMs() const;

    // ─── Pure parsing — no socket, no state, unit-testable ─────────────────
    struct VitaHeader {
        quint32 streamId       = 0;
        quint16 packetClass    = 0;
        int     packetCount    = -1;   // 4-bit rolling, per stream (§7.1 E3)
        bool    hasTrailer     = false;
        int     payloadOffset  = 0;
        int     payloadBytes   = 0;
        bool    valid          = false;
    };

    static bool parseHeader(const char* data, int size, VitaHeader& out);

    // Decode an rx_stream_in payload into complex baseband, APPENDING to `out`.
    //
    // Appends rather than assigns so a caller can accumulate several packets
    // into one decoder block without a copy per packet. Returns the number of
    // complex samples appended, or -1 if the payload is not a whole number of
    // float32 pairs.
    //
    // §7.1 X2 — the imaginary component is NEGATED here. The radio sends the
    // conjugate; the positive-frequency signal is A − jB. Measured on the live
    // FLEX-8400 as −freq/+freq energy at better than 10 dB (RFC §10.3).
    static int decodeRxPayload(const char* payload, int payloadBytes,
                               std::vector<std::complex<float>>& out);

    // Build one outbound audio packet: 28-byte header per §7.1 E4, then the
    // mono block duplicated into interleaved big-endian float32 stereo.
    //
    // §7.1 E2 — outbound is STEREO AUDIO, **not** I/Q, and is **not**
    // conjugated. This is the exact opposite of decodeRxPayload() above, and
    // the asymmetry is the whole reason both are separate static functions with
    // their own tests. Conjugating here inverts the sideband: every meter on
    // the radio reads normal and nobody can decode it.
    //
    // Static and clock-injected (utcSec / fracPicos passed in) so the byte
    // layout can be asserted exactly, with no wall clock in the test.
    static QByteArray buildAudioPacket(quint32 streamId, int packetCount,
                                       quint64 utcSec, quint64 fracPicos,
                                       std::span<const float> mono);

    // ─── Emit ──────────────────────────────────────────────────────────────
    //
    // §7.1 E1 — 🔇 EMIT ON THE INBOUND ID. The `*_out` ids are not honoured;
    // injecting on them was measured at exactly zero, twice, the second time
    // after the path had been proven live. There is no error anywhere — the
    // audio simply does not exist.
    //
    // That is why these take no stream id. The two directions are named methods
    // resolving to the stored INBOUND ids, so there is no parameter into which
    // an `*_out` id can be passed. E1 is enforced by the signature rather than
    // by a comment somebody has to read.
    //
    // Returns false if the socket is closed, the id is unset, or the send
    // failed. Timestamps come from the system clock; the counter advances per
    // stream (§7.1 E3).
    bool emitRxReturn(std::span<const float> mono);   // decoded audio → the slice
    bool emitTxReturn(std::span<const float> mono);   // modulated audio → the air

    // ─── Diagnostics ───────────────────────────────────────────────────────
    //
    // These exist because most of what can go wrong here goes wrong silently.
    // A conjugation error shows up as "it doesn't decode"; so does a stream-id
    // mismatch, and so does receiving nothing at all. Counting the three
    // separately is what tells them apart.
    struct Stats {
        quint64 datagrams      = 0;   // everything the socket handed us
        quint64 accepted       = 0;   // matched the stream id and parsed
        quint64 samples        = 0;   // complex samples emitted
        quint64 wrongStream    = 0;   // some other stream on our port
        quint64 malformed      = 0;   // too short, or a partial float pair
        quint64 settleDropped  = 0;   // discarded by the X3 window
        quint64 sequenceGaps   = 0;   // §7.1 E3 counter discontinuities
        quint64 emitted        = 0;   // outbound packets sent
        quint64 emitFailed     = 0;   // outbound sends that did not go out
    };
    Stats stats() const;
    void resetStats();

signals:
    // One packet's worth of demodulated passband, already conjugated (§7.1 X2)
    // and ready for the modem. 128 complex samples at 24 kHz in the measured
    // case, but the size is whatever arrived — nothing downstream should assume
    // a fixed block.
    void rxPassband(const std::vector<std::complex<float>>& block);

    // First accepted packet after a (re)registration. The point where the
    // radio is demonstrably feeding us, as distinct from merely having accepted
    // the registration — §7.1's "code 0 means accepted, never applied".
    void rxStreamLive();

private:
    void readPending();
    bool emitOn(quint32 streamId, std::span<const float> mono);

    struct Private;
    Private* d;
};

}  // namespace AetherSDR

#endif  // AETHER_ENABLE_RADE_V2
