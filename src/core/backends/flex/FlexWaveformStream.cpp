#include "core/backends/flex/FlexWaveformStream.h"

#ifdef AETHER_ENABLE_RADE_V2

#include <bit>
#include <chrono>
#include <cstring>
#include <map>

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtEndian>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcRadeV2Rx, "aether.radev2.rx")

namespace {

// One big-endian IEEE-754 float32 out of the payload.
//
// Read as an integer and bit_cast rather than byte-swapping a float in place:
// the integer path has no chance of tripping over a signalling NaN, and the
// radio does occasionally send denormals in the noise floor.
inline float beFloat(const char* p) {
    quint32 bits = 0;
    std::memcpy(&bits, p, sizeof(bits));
    return std::bit_cast<float>(qFromBigEndian<quint32>(bits));
}

// Append one big-endian float32.
inline void putBeFloat(char* p, float f) {
    const quint32 bits = qToBigEndian<quint32>(std::bit_cast<quint32>(f));
    std::memcpy(p, &bits, sizeof(bits));
}

inline void putBeU32(char* p, quint32 v) {
    const quint32 bits = qToBigEndian<quint32>(v);
    std::memcpy(p, &bits, sizeof(bits));
}

}  // namespace

struct FlexWaveformStream::Private {
    QUdpSocket* socket = nullptr;
    quint16 port = 0;
    quint32 rxStreamId = 0;
    quint32 txStreamId = 0;
    QHostAddress radioAddr;

    // §7.1 E3 — per-STREAM 4-bit rolling count. A single shared counter
    // desyncs the moment two streams run, which is the normal case the instant
    // TX starts while RX is still flowing.
    std::map<quint32, int> emitCount;

    int settleMs = 1000;          // §7.1 X3, measured
    QElapsedTimer settleClock;    // starts at the first packet of a stream
    bool sawFirstPacket = false;
    bool announcedLive = false;

    int lastCount = -1;           // §7.1 E3, per-stream 4-bit rolling
    Stats stats;

    std::vector<std::complex<float>> block;   // reused, not reallocated

    void resetStreamState() {
        sawFirstPacket = false;
        announcedLive = false;
        lastCount = -1;
        settleClock.invalidate();
    }
};

FlexWaveformStream::FlexWaveformStream(QObject* parent)
    : QObject(parent), d(new Private) {
    // Same registration Hl2RxDsp does — the signal carries a std::vector across
    // what will be a thread boundary once this moves onto the network thread.
    qRegisterMetaType<std::vector<std::complex<float>>>(
        "std::vector<std::complex<float>>");
    d->block.reserve(kRxSamplesPerPkt * 2);
}

FlexWaveformStream::~FlexWaveformStream() {
    close();
    delete d;
}

quint16 FlexWaveformStream::open(quint16 preferredPort) {
    if (d->socket) return d->port;

    d->socket = new QUdpSocket(this);
    // AnyIPv4, not Any: the radio addresses us by the v4 address it saw the TCP
    // control connection arrive from, and a dual-stack bind on Windows has been
    // observed elsewhere in this codebase to deliver to the v6 side instead.
    if (!d->socket->bind(QHostAddress(QHostAddress::AnyIPv4), preferredPort,
                         QUdpSocket::DefaultForPlatform)) {
        qCWarning(lcRadeV2Rx) << "bind failed on port" << preferredPort << ":"
                              << d->socket->errorString();
        delete d->socket;
        d->socket = nullptr;
        return 0;
    }
    d->port = d->socket->localPort();
    connect(d->socket, &QUdpSocket::readyRead,
            this, &FlexWaveformStream::readPending);
    qCDebug(lcRadeV2Rx) << "bound udp port" << d->port;
    return d->port;
}

void FlexWaveformStream::close() {
    if (!d->socket) return;
    d->socket->close();
    delete d->socket;
    d->socket = nullptr;
    d->port = 0;
    d->resetStreamState();
    qCDebug(lcRadeV2Rx) << "closed";
}

bool FlexWaveformStream::isOpen() const { return d->socket != nullptr; }
quint16 FlexWaveformStream::boundPort() const { return d->port; }

void FlexWaveformStream::setRxStreamId(quint32 id) {
    if (d->rxStreamId == id) return;
    d->rxStreamId = id;
    // §7.1 R6 — ids are recycled, so a new id is a genuinely new stream even if
    // the number looks familiar. Everything per-stream restarts.
    d->resetStreamState();
    qCDebug(lcRadeV2Rx).nospace() << "rx stream id = 0x" << Qt::hex << id;
}

quint32 FlexWaveformStream::rxStreamId() const { return d->rxStreamId; }

void FlexWaveformStream::setTxStreamId(quint32 id) {
    if (d->txStreamId == id) return;
    d->txStreamId = id;
    d->emitCount.erase(id);   // §7.1 R6 + E3 — new stream, count restarts at 0
    qCDebug(lcRadeV2Rx).nospace() << "tx stream id = 0x" << Qt::hex << id;
}

quint32 FlexWaveformStream::txStreamId() const { return d->txStreamId; }

void FlexWaveformStream::setRadioAddress(const QHostAddress& addr) {
    d->radioAddr = addr;
}

QHostAddress FlexWaveformStream::radioAddress() const { return d->radioAddr; }

void FlexWaveformStream::setSettleMs(int ms) { d->settleMs = ms < 0 ? 0 : ms; }
int FlexWaveformStream::settleMs() const { return d->settleMs; }

bool FlexWaveformStream::parseHeader(const char* data, int size, VitaHeader& out) {
    out = VitaHeader{};
    if (!data || size < kVitaHeaderBytes) return false;

    const auto* raw = reinterpret_cast<const uchar*>(data);
    const quint32 word0 = qFromBigEndian<quint32>(raw);

    out.streamId    = qFromBigEndian<quint32>(raw + 4);
    // PacketClassCode is the LOW 16 bits of word 3; the high half is the
    // InformationClassCode, which we do not filter on.
    out.packetClass = static_cast<quint16>(qFromBigEndian<quint32>(raw + 12) & 0xFFFFu);
    out.packetCount = static_cast<int>((word0 >> 16) & 0x0Fu);
    out.hasTrailer  = (word0 & kTrailerFlag) != 0;

    out.payloadOffset = kVitaHeaderBytes;
    out.payloadBytes  = size - kVitaHeaderBytes - (out.hasTrailer ? 4 : 0);
    if (out.payloadBytes < 0) return false;

    out.valid = true;
    return true;
}

int FlexWaveformStream::decodeRxPayload(const char* payload, int payloadBytes,
                                        std::vector<std::complex<float>>& out) {
    if (!payload || payloadBytes < 0) return -1;
    // §7.1 X1 — interleaved big-endian float32 PAIRS. A payload that is not a
    // whole number of pairs means the layout is not what we measured, and
    // silently truncating it would hide exactly that.
    if (payloadBytes % 8 != 0) return -1;

    const int pairs = payloadBytes / 8;
    out.reserve(out.size() + static_cast<size_t>(pairs));
    for (int i = 0; i < pairs; ++i) {
        const char* p = payload + i * 8;
        const float a = beFloat(p);
        const float b = beFloat(p + 4);
        // ─────────────────────────────────────────────────────────────────
        // §7.1 X2 — CONJUGATE. The minus sign is not a typo.
        //
        // The radio delivers the conjugate of the analytic signal; the
        // positive-frequency passband is A − jB. Writing `+b` here mirrors the
        // spectrum about DC, and because a mirrored OFDM burst still has
        // perfectly normal power, level and occupancy, absolutely nothing
        // downstream complains — it just never decodes. Measured on the live
        // FLEX-8400 (fw 4.2.20.41343): −freq/+freq energy ratio well past the
        // 10 dB decision threshold. RFC §10.3.
        // ─────────────────────────────────────────────────────────────────
        out.emplace_back(a, -b);
    }
    return pairs;
}

QByteArray FlexWaveformStream::buildAudioPacket(quint32 streamId, int packetCount,
                                                quint64 utcSec, quint64 fracPicos,
                                                std::span<const float> mono) {
    const int n = static_cast<int>(mono.size());
    QByteArray pkt;
    pkt.resize(kVitaHeaderBytes + n * 8);
    char* p = pkt.data();

    // §7.1 E4 — the measured header. Packet size counts 32-bit WORDS: seven
    // header words plus two per sample, because each sample goes out twice.
    //
    // Note kHdrIfDataStream (type 1), NOT the type 3 the radio sends us. The
    // asymmetry is real; matching the inbound type does not work.
    const quint32 word0 = kHdrIfDataStream | kHdrClassPresent
                        | kHdrTsiUtc | kHdrTsfRealTime
                        | (static_cast<quint32>(packetCount & 0x0F) << 16)
                        | static_cast<quint32>(7 + n * 2);

    putBeU32(p +  0, word0);
    putBeU32(p +  4, streamId);
    putBeU32(p +  8, kFlexOui);
    putBeU32(p + 12, kAudioClassId);
    putBeU32(p + 16, static_cast<quint32>(utcSec));
    // Fractional timestamp is 64-bit PICOseconds, high word first.
    putBeU32(p + 20, static_cast<quint32>((fracPicos >> 32) & 0xFFFFFFFFu));
    putBeU32(p + 24, static_cast<quint32>(fracPicos & 0xFFFFFFFFu));

    // ─────────────────────────────────────────────────────────────────────
    // §7.1 E2 — STEREO AUDIO. The same sample in both slots, unmodified.
    //
    // Do NOT reach for decodeRxPayload()'s conjugation here, however symmetric
    // it looks. Inbound is I/Q and needs `-b`; outbound is a pair of audio
    // channels and needs neither an imaginary part nor a sign. Conjugating
    // inverts the sideband — and an inverted sideband is normal power, normal
    // occupancy, normal on every meter the radio has, and decodable by nobody.
    // Measured upper-sideband-correct as written (RFC §10.11).
    // ─────────────────────────────────────────────────────────────────────
    char* pay = p + kVitaHeaderBytes;
    for (int i = 0; i < n; ++i) {
        putBeFloat(pay + i * 8,     mono[static_cast<size_t>(i)]);
        putBeFloat(pay + i * 8 + 4, mono[static_cast<size_t>(i)]);
    }
    return pkt;
}

bool FlexWaveformStream::emitOn(quint32 streamId, std::span<const float> mono) {
    if (!d->socket || streamId == 0 || d->radioAddr.isNull() || mono.empty()) {
        d->stats.emitFailed++;
        return false;
    }

    // §7.1 E3 — per-stream count, post-increment so the first packet is 0.
    int& c = d->emitCount[streamId];
    const int count = c;
    c = (c + 1) & 0x0F;

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto rem  = now - secs;
    const quint64 picos = static_cast<quint64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(rem).count()) * 1000ull;

    const QByteArray pkt = buildAudioPacket(streamId, count,
                                            static_cast<quint64>(secs.count()),
                                            picos, mono);
    const qint64 sent = d->socket->writeDatagram(pkt, d->radioAddr, kVitaOutPort);
    if (sent != pkt.size()) {
        d->stats.emitFailed++;
        qCWarning(lcRadeV2Rx) << "emit short write" << sent << "of" << pkt.size();
        return false;
    }
    d->stats.emitted++;
    return true;
}

bool FlexWaveformStream::emitRxReturn(std::span<const float> mono) {
    // §7.1 E1 — rx_stream_IN. Not rx_stream_out; that one is measured silent.
    return emitOn(d->rxStreamId, mono);
}

bool FlexWaveformStream::emitTxReturn(std::span<const float> mono) {
    // §7.1 E1 — tx_stream_IN, same reasoning.
    return emitOn(d->txStreamId, mono);
}

void FlexWaveformStream::readPending() {
    if (!d->socket) return;

    while (d->socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = d->socket->receiveDatagram();
        const QByteArray data = dg.data();
        d->stats.datagrams++;

        VitaHeader hdr;
        if (!parseHeader(data.constData(), data.size(), hdr)) {
            d->stats.malformed++;
            continue;
        }
        // §7.1 T1 — the transmit clock. Checked BEFORE the rx filter and never
        // subject to the X3 settle window: tx_stream_in is PTT-gated (T2), so
        // discarding its first second would eat the start of every over.
        if (d->txStreamId != 0 && hdr.streamId == d->txStreamId) {
            emit txClockPacket(hdr.payloadBytes / 8);
            continue;
        }

        if (d->rxStreamId == 0 || hdr.streamId != d->rxStreamId) {
            // Not ours. Expected in normal operation: the radio will send other
            // streams here if anything else was ever pointed at this port.
            d->stats.wrongStream++;
            continue;
        }

        // §7.1 X3 — drop the startup burst. The clock starts at the first
        // packet of the stream, not at bind, because registration and first
        // flow are seconds apart and a bind-relative window would expire
        // before the burst even arrived.
        if (!d->sawFirstPacket) {
            d->sawFirstPacket = true;
            d->settleClock.start();
        }
        if (d->settleMs > 0 && d->settleClock.elapsed() < d->settleMs) {
            d->stats.settleDropped++;
            continue;
        }

        // §7.1 E3 — per-stream 4-bit rolling count. Tracked because a gap here
        // is the difference between "the modem is failing" and "the network is
        // dropping packets", and those have nothing to do with each other.
        if (d->lastCount >= 0) {
            const int expected = (d->lastCount + 1) & 0x0F;
            if (hdr.packetCount != expected) {
                d->stats.sequenceGaps++;
                qCDebug(lcRadeV2Rx) << "sequence gap: expected" << expected
                                    << "got" << hdr.packetCount;
            }
        }
        d->lastCount = hdr.packetCount;

        d->block.clear();
        const int n = decodeRxPayload(data.constData() + hdr.payloadOffset,
                                      hdr.payloadBytes, d->block);
        if (n <= 0) {
            d->stats.malformed++;
            continue;
        }

        d->stats.accepted++;
        d->stats.samples += static_cast<quint64>(n);

        if (!d->announcedLive) {
            d->announcedLive = true;
            qCDebug(lcRadeV2Rx) << "stream live," << n << "samples/packet";
            emit rxStreamLive();
        }
        emit rxPassband(d->block);
    }
}

FlexWaveformStream::Stats FlexWaveformStream::stats() const { return d->stats; }
void FlexWaveformStream::resetStats() { d->stats = Stats{}; }

}  // namespace AetherSDR

#endif  // AETHER_ENABLE_RADE_V2
