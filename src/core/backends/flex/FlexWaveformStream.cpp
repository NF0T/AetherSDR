#include "core/backends/flex/FlexWaveformStream.h"

#ifdef AETHER_ENABLE_RADE_V2

#include <bit>
#include <cstring>

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

}  // namespace

struct FlexWaveformStream::Private {
    QUdpSocket* socket = nullptr;
    quint16 port = 0;
    quint32 rxStreamId = 0;

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
