#include "core/backends/flex/FlexWaveformProvider.h"

#ifdef AETHER_ENABLE_RADE_V2

#include <QLoggingCategory>

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
constexpr int  kRxFilterLowHz  = 800;
constexpr int  kRxFilterHighHz = 2200;
constexpr int  kFilterDepth    = 256;
}  // namespace

struct FlexWaveformProvider::Private {
    std::function<void(const QString&)> commandSink;
    WaveformStreams streams;
    bool registered = false;
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

void FlexWaveformProvider::setCommandSink(std::function<void(const QString&)> sink) {
    d->commandSink = std::move(sink);
}

bool FlexWaveformProvider::registerWaveform() {
    // Stage 2 fills this in: the R1-ordered command sequence, then parse the six
    // stream ids out of the create reply (§7.1 R6/R7).
    qCDebug(lcRadeV2) << "registerWaveform: not implemented yet (stage 1 skeleton)";
    return false;
}

void FlexWaveformProvider::removeWaveform() {
    if (!d->registered) return;
    qCDebug(lcRadeV2) << "removeWaveform: not implemented yet (stage 1 skeleton)";
    d->registered = false;
    d->streams = WaveformStreams{};
}

bool FlexWaveformProvider::isRegistered() const {
    return d->registered;
}

const WaveformStreams& FlexWaveformProvider::streams() const {
    return d->streams;
}

}  // namespace AetherSDR

#endif  // AETHER_ENABLE_RADE_V2
