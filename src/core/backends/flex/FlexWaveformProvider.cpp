#include "core/backends/flex/FlexWaveformProvider.h"

#ifdef AETHER_ENABLE_RADE_V2

#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcRadeV2, "aether.radev2")

// Registration parameters — RFC §10.1, derived from the V2 OFDM source rather
// than from V1 (V1's modem is wider and carries pilots, so it is the WRONG
// proxy for any of this).
//
//   Nc=14 carriers, 62.5 Hz spacing, indices 17–30 = 1062.5–1875.0 Hz
//   null-to-null occupancy ~1000–1937.5 Hz, centroid 1468.75 Hz (not 1500)
//   receiver acquisition range ±31.25 Hz
//
// rx_filter [800, 2200] leaves ~181 Hz of guard below the modem's own internal
// BPF edge and ~244 Hz above — both far beyond what the receiver can acquire
// over, so nothing decodable is clipped. Measured flat 1000–2000 Hz with a
// brick-wall transition (RFC §10.3).
namespace {
constexpr auto kWaveformName   = "AetherRADEv2";
constexpr auto kRadioMode      = "RAD2";
constexpr auto kUnderlyingMode = "DIGU";   // §10.1 — the data variant, per
                                           // D-Star's DFM-not-FM precedent;
                                           // DIGU on every band, no DIGL.
constexpr auto kVersion        = "0.0.1";
constexpr int  kRxFilterLowHz  = 800;
constexpr int  kRxFilterHighHz = 2200;
constexpr int  kFilterDepth    = 256;

// Pull "<key>=<hex>" out of a create reply. The radio emits an uppercase "0X"
// prefix on some fields and none on others, so normalise before converting
// rather than trusting a base-16 parse to cope with both.
bool takeHexField(const QStringList& tokens, const QString& key, quint32& out) {
    const QString prefix = key + QLatin1Char('=');
    for (const QString& tok : tokens) {
        if (!tok.startsWith(prefix, Qt::CaseInsensitive)) continue;
        QString value = tok.mid(prefix.size()).trimmed();
        if (value.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
            value = value.mid(2);
        bool ok = false;
        const quint32 parsed = value.toUInt(&ok, 16);
        if (!ok) return false;
        out = parsed;
        return true;
    }
    return false;
}
}  // namespace

struct FlexWaveformProvider::Private {
    FlexWaveformProvider::CommandSink commandSink;
    WaveformStreams streams;
    bool registered = false;

    // The R1-ordered tail of the registration sequence, sent one at a time
    // AFTER `waveform create` has returned the stream ids. A queue rather than
    // nested callbacks because §7.1 R1 fixes the order and R2 forbids combining
    // fields onto one line — so the ordering is a protocol requirement, and it
    // should be visible as a list rather than buried in call nesting.
    QVector<QString> pendingSteps;
    bool inFlight = false;
};

FlexWaveformProvider::FlexWaveformProvider(QObject* parent)
    : QObject(parent), d(new Private) {}

FlexWaveformProvider::~FlexWaveformProvider() {
    // §7.1 N1 — the radio reverts the slice cleanly on `waveform remove`, on
    // disconnect, and even on an abrupt kill, so teardown is belt-and-braces
    // rather than load-bearing. Unlike RADE V1, nothing can be stranded here.
    removeWaveform();
    delete d;
}

void FlexWaveformProvider::setCommandSink(CommandSink sink) {
    d->commandSink = std::move(sink);
}

bool FlexWaveformProvider::parseCreateReply(const QString& body, WaveformStreams& out) {
    out = WaveformStreams{};
    const QStringList tokens = body.split(QRegularExpression(QStringLiteral("\\s+")),
                                          Qt::SkipEmptyParts);
    if (tokens.isEmpty()) return false;

    // §7.1 R7 — SIX ids, not four. The two *_out ids are never emitted on
    // (§7.1 E1) but are parsed anyway: they are protocol-bearing whether or not
    // we use them, and a partial parse would hide a firmware change that
    // reshaped the reply.
    const bool ok =
        takeHexField(tokens, QStringLiteral("tx_stream_in_id"),   out.txStreamIn)   &&
        takeHexField(tokens, QStringLiteral("rx_stream_in_id"),   out.rxStreamIn)   &&
        takeHexField(tokens, QStringLiteral("tx_stream_out_id"),  out.txStreamOut)  &&
        takeHexField(tokens, QStringLiteral("rx_stream_out_id"),  out.rxStreamOut)  &&
        takeHexField(tokens, QStringLiteral("byte_stream_in_id"), out.byteStreamIn) &&
        takeHexField(tokens, QStringLiteral("byte_stream_out_id"), out.byteStreamOut);

    if (!ok) {
        out = WaveformStreams{};
        return false;
    }
    out.valid = true;
    return true;
}

void FlexWaveformProvider::registerWaveform(quint16 udpPort) {
    if (!d->commandSink) {
        emit failed(QStringLiteral("no command sink"));
        return;
    }
    if (d->inFlight) {
        emit failed(QStringLiteral("registration already in flight"));
        return;
    }
    d->inFlight = true;

    // Defensive pre-clean. The radio auto-removes a waveform when its client
    // disconnects (§7.1 N1), so a stale registration should be impossible — but
    // this is idempotent and costs one command, and a create over a surviving
    // name would fail in a way that reads like a protocol error.
    d->commandSink(QStringLiteral("waveform remove %1").arg(kWaveformName), nullptr);

    // §7.1 R1 — the tail, in order. udpport LAST.
    // §7.1 R2 — one field per command; these do NOT combine onto one line.
    // §7.1 R3 — rx_filter is latched at registration/mode entry, so it must be
    //           set here and never "adjusted" live; a live set returns 0 and
    //           does nothing.
    // §7.1 R4 — tx_filter is accepted and entirely ignored, so it is NOT sent.
    //           Sending it would imply a control we do not have. Band-limiting
    //           happens in our own TX chain instead (§7.1 T6).
    d->pendingSteps = {
        QStringLiteral("waveform set %1 tx=1").arg(kWaveformName),
        QStringLiteral("waveform set %1 rx_filter low_cut=%2")
            .arg(kWaveformName).arg(kRxFilterLowHz),
        QStringLiteral("waveform set %1 rx_filter high_cut=%2")
            .arg(kWaveformName).arg(kRxFilterHighHz),
        QStringLiteral("waveform set %1 rx_filter depth=%2")
            .arg(kWaveformName).arg(kFilterDepth),
        QStringLiteral("waveform set %1 udpport=%2")
            .arg(kWaveformName).arg(udpPort),
    };

    const QString create =
        QStringLiteral("waveform create name=%1 mode=%2 underlying_mode=%3 version=%4")
            .arg(kWaveformName, kRadioMode, kUnderlyingMode, kVersion);

    qCDebug(lcRadeV2) << "registering" << kWaveformName << "udpport" << udpPort;

    d->commandSink(create, [this](int code, const QString& body) {
        if (code != 0) {
            d->inFlight = false;
            d->pendingSteps.clear();
            emit failed(QStringLiteral("waveform create rejected: 0x%1")
                            .arg(code, 8, 16, QLatin1Char('0')));
            return;
        }
        if (!parseCreateReply(body, d->streams)) {
            d->inFlight = false;
            d->pendingSteps.clear();
            emit failed(QStringLiteral("waveform create reply unparsable: %1").arg(body));
            return;
        }
        qCDebug(lcRadeV2).nospace()
            << "streams rx_in=0x" << Qt::hex << d->streams.rxStreamIn
            << " tx_in=0x" << d->streams.txStreamIn
            << " byte_in=0x" << d->streams.byteStreamIn;
        sendNextStep();
    });
}

void FlexWaveformProvider::sendNextStep() {
    if (d->pendingSteps.isEmpty()) {
        d->inFlight = false;
        d->registered = true;
        qCDebug(lcRadeV2) << "registered";
        emit registered(d->streams);
        return;
    }
    const QString step = d->pendingSteps.takeFirst();
    d->commandSink(step, [this, step](int code, const QString&) {
        if (code != 0) {
            d->inFlight = false;
            d->pendingSteps.clear();
            emit failed(QStringLiteral("'%1' rejected: 0x%2")
                            .arg(step).arg(code, 8, 16, QLatin1Char('0')));
            return;
        }
        sendNextStep();
    });
}

void FlexWaveformProvider::removeWaveform() {
    if (!d->registered && !d->inFlight) return;
    if (d->commandSink)
        d->commandSink(QStringLiteral("waveform remove %1").arg(kWaveformName), nullptr);
    d->inFlight = false;
    d->pendingSteps.clear();
    d->registered = false;
    // §7.1 R6 — ids are RECYCLED across create/remove. Clearing them here means
    // a stale id can never be mistaken for a live one after teardown.
    d->streams = WaveformStreams{};
    qCDebug(lcRadeV2) << "removed";
    emit unregistered();
}

bool FlexWaveformProvider::isRegistered() const {
    return d->registered;
}

const WaveformStreams& FlexWaveformProvider::streams() const {
    return d->streams;
}

}  // namespace AetherSDR

#endif  // AETHER_ENABLE_RADE_V2
