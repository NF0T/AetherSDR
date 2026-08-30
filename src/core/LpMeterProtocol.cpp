#include "LpMeterProtocol.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QList>
#include <QStringList>

namespace AetherSDR {
namespace LpMeter {

namespace {

// Canonical separator offsets within a kRecordLength body, derived from the
// captured layout (widths 7,5,5,1,6,1,1,4,4 with eight single-char commas).
// Checked only when the body is exactly kRecordLength long -- see
// looksLikeRecord().
const QList<int>& canonicalCommaOffsets()
{
    static const QList<int> offsets = {7, 13, 19, 21, 28, 30, 32, 37};
    return offsets;
}

bool parseDouble(const QByteArray& field, double* out)
{
    bool ok = false;
    const double v = QString::fromLatin1(field).trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

// Enum fields are one character on the wire. Requiring a single ASCII digit
// is the corruption check; whether the VALUE is one we recognise is left to
// the name lookups, so a firmware with a fourth mode still decodes.
bool parseDigit(const QByteArray& field, int* out)
{
    const QByteArray t = field.trimmed();
    if (t.size() != 1 || t.at(0) < '0' || t.at(0) > '9') {
        return false;
    }
    *out = t.at(0) - '0';
    return true;
}

}  // namespace

// ---- Enum names ----------------------------------------------------------

QString alarmSetPointName(int value)
{
    switch (value) {
        case 0: return QStringLiteral("Off");
        case 1: return QStringLiteral("1.5");
        case 2: return QStringLiteral("2.0");
        case 3: return QStringLiteral("2.5");
        case 4: return QStringLiteral("3.0");
        case 5: return QStringLiteral("User");   // confirmed on hardware
        default: break;
    }
    return QStringLiteral("? (%1)").arg(value);
}

QString powerRangeName(int value)
{
    switch (value) {
        case 0: return QStringLiteral("High");
        case 1: return QStringLiteral("Mid");
        case 2: return QStringLiteral("Low");
        default: break;
    }
    return QStringLiteral("? (%1)").arg(value);
}

QString peakHoldModeName(int value)
{
    switch (value) {
        case 0: return QStringLiteral("Avg");
        case 1: return QStringLiteral("Peak");
        case 2: return QStringLiteral("Fast");  // confirmed on hardware
        default: break;
    }
    return QStringLiteral("? (%1)").arg(value);
}

// ---- Validation ----------------------------------------------------------

bool looksLikeRecord(const QByteArray& body)
{
    if (body.isEmpty()) {
        return false;
    }

    const QList<QByteArray> fields = body.split(',');
    if (fields.size() != FieldCount) {
        return false;
    }

    // Exact separator placement, but only for the known-good length. A body
    // of some other length may still be a valid record from a firmware whose
    // dBm field needed a fifth character, and rejecting it here would reject
    // every record that unit ever sends.
    if (body.size() == kRecordLength) {
        int offset = 0;
        for (int i = 0; i < canonicalCommaOffsets().size(); ++i) {
            offset += fields.at(i).size();
            if (offset != canonicalCommaOffsets().at(i)) {
                return false;
            }
            offset += 1;  // the comma itself
        }
    }

    double powerW = 0.0;
    double zOhms = 0.0;
    double phaseDeg = 0.0;
    double dBm = 0.0;
    double swr = 0.0;
    if (!parseDouble(fields.at(FieldPower), &powerW)
        || !parseDouble(fields.at(FieldZ), &zOhms)
        || !parseDouble(fields.at(FieldPhase), &phaseDeg)
        || !parseDouble(fields.at(FieldDbm), &dBm)
        || !parseDouble(fields.at(FieldSwr), &swr)) {
        return false;
    }

    int alarm = 0;
    int range = 0;
    int peak = 0;
    if (!parseDigit(fields.at(FieldAlarm), &alarm)
        || !parseDigit(fields.at(FieldRange), &range)
        || !parseDigit(fields.at(FieldPeakHold), &peak)) {
        return false;
    }

    // Physical plausibility. Deliberately generous -- this is here to catch
    // corruption, not to second-guess the meter.
    if (powerW < 0.0 || zOhms < 0.0) {
        return false;
    }
    // Phase is a magnitude, so it can never be negative on the wire, and a
    // reactance angle cannot exceed a quarter turn.
    if (phaseDeg < 0.0 || phaseDeg > 90.0) {
        return false;
    }
    // SWR is a ratio and cannot be below unity. A small tolerance absorbs a
    // meter that rounds 0.999 into its 4-character field.
    if (swr < 0.99) {
        return false;
    }
    return true;
}

std::optional<Reading> decodeReading(const QByteArray& body)
{
    if (!looksLikeRecord(body)) {
        return std::nullopt;
    }

    const QList<QByteArray> fields = body.split(',');
    Reading r;
    parseDouble(fields.at(FieldPower), &r.powerW);
    parseDouble(fields.at(FieldZ), &r.zOhms);
    parseDouble(fields.at(FieldPhase), &r.phaseDeg);
    parseDouble(fields.at(FieldDbm), &r.dBm);
    parseDouble(fields.at(FieldSwr), &r.swr);
    parseDigit(fields.at(FieldAlarm), &r.alarmSetPoint);
    parseDigit(fields.at(FieldRange), &r.powerRange);
    parseDigit(fields.at(FieldPeakHold), &r.peakHoldMode);
    r.callsign = QString::fromLatin1(fields.at(FieldCallsign)).trimmed();

    // Only meaningful under drive. With no RF the impedance bridge has
    // nothing to measure, so the idle values (SWR 1.00 against a phase near
    // 90 degrees) are not physically consistent and would read as incoherent
    // for the entire time the operator is receiving.
    if (r.powerW > 0.5) {
        const double implied = swrFromImpedance(r.zOhms, r.phaseDeg);
        r.coherent = std::fabs(implied - r.swr) <= kCoherenceTolerance;
    }
    return r;
}

// ---- Streaming parser ----------------------------------------------------

// Longest body still treated as a possible record while waiting for the next
// marker. Four records' worth, matching the cap on the no-marker path.
static constexpr int kMaxBodyLength = kRecordLength * 4;

void ResponseParser::feed(const QByteArray& bytes)
{
    m_buf += bytes;

    // Drop anything before the first marker. This is what silently absorbs
    // the ser2net connect banner as well as any mid-stream garbage.
    const int firstMarker = m_buf.indexOf(kRecordMarker);
    if (firstMarker < 0) {
        // No marker at all yet. Keep only enough to recognise one arriving
        // split across feeds; without this an endless non-record stream would
        // grow the buffer without bound.
        if (m_buf.size() > kRecordLength * 4) {
            m_buf = m_buf.right(kRecordLength);
        }
        return;
    }
    if (firstMarker > 0) {
        m_buf.remove(0, firstMarker);
    }

    // m_buf now starts at a marker.
    while (!m_buf.isEmpty() && m_buf.at(0) == kRecordMarker) {
        QByteArray body;
        int consume = 0;

        const int next = m_buf.indexOf(kRecordMarker, 1);
        if (next >= 0) {
            // The following record has begun, so this one is complete
            // whatever its length. This is the path that tolerates a firmware
            // whose fields are wider than the captured layout.
            body = m_buf.mid(1, next - 1);
            consume = next;
        } else if (m_buf.size() - 1 >= kRecordLength
                   && looksLikeRecord(m_buf.mid(1, kRecordLength))) {
            // Records are fixed-length, so a complete and valid one can be
            // emitted WITHOUT waiting for the next marker. Waiting would hold
            // every record until the following poll -- imperceptible at 10 Hz,
            // but a 5 s delay when riding along behind a slow foreign poller
            // (PollGate), which is exactly when latency is least affordable.
            body = m_buf.mid(1, kRecordLength);
            consume = 1 + kRecordLength;
        } else {
            // Incomplete; wait for more bytes -- but not without limit. The
            // cap on the no-marker path above does not cover this one: once
            // m_buf starts with ';' that indexOf always succeeds, so a stream
            // that delivers one marker and then never another (and never a
            // body that validates) would append here forever. Needs a device
            // that goes malformed AFTER talking properly, which is exactly
            // the case the other cap was written for.
            //
            // A body longer than kMaxBodyLength cannot become a record, so
            // drop the marker and resync at the next one. Bounded at four
            // records' worth for the same reason as above: wide enough that
            // no plausible firmware variant is truncated, narrow enough that
            // the buffer cannot grow.
            if (m_buf.size() - 1 > kMaxBodyLength) {
                m_buf.remove(0, 1);       // discard this marker
                const int resync = m_buf.indexOf(kRecordMarker);
                if (resync < 0) {
                    m_buf.clear();
                    return;
                }
                m_buf.remove(0, resync);
                continue;                 // retry from the next marker
            }
            break;
        }

        m_buf.remove(0, consume);

        // Tolerate a terminator even though this meter emits none: the
        // capture says so for this firmware, and one rstrip is cheaper than
        // finding out the hard way that another one differs.
        while (!body.isEmpty()
               && (body.endsWith('\r') || body.endsWith('\n'))) {
            body.chop(1);
        }

        if (m_onReading) {
            if (const auto reading = decodeReading(body)) {
                m_onReading(*reading);
            }
        }

        // Re-establish marker alignment: the fixed-length path above may have
        // consumed a record without landing exactly on the next marker.
        const int resync = m_buf.indexOf(kRecordMarker);
        if (resync < 0) {
            m_buf.clear();
            break;
        }
        if (resync > 0) {
            m_buf.remove(0, resync);
        }
    }
}

// ---- Derived values ------------------------------------------------------

double returnLossDb(double swr)
{
    if (!(swr > 1.0)) {
        return 0.0;  // perfect match; the log below would diverge
    }
    const double gamma = (swr - 1.0) / (swr + 1.0);
    return -20.0 * std::log10(gamma);
}

double swrFromImpedance(double zOhms, double phaseDeg)
{
    const double theta = phaseDeg * M_PI / 180.0;
    const double re = zOhms * std::cos(theta);
    const double im = zOhms * std::sin(theta);
    const double numer = std::hypot(re - 50.0, im);
    const double denom = std::hypot(re + 50.0, im);
    if (denom <= 0.0) {
        return 1.0;
    }
    const double gamma = numer / denom;
    if (gamma >= 1.0) {
        return std::numeric_limits<double>::infinity();
    }
    return (1.0 + gamma) / (1.0 - gamma);
}

double reflectedWattsFromSwr(double forwardW, double swr)
{
    if (forwardW <= 0.0 || !(swr > 1.0)) {
        return 0.0;
    }
    const double gamma = (swr - 1.0) / (swr + 1.0);
    return forwardW * gamma * gamma;
}

// ---- PollGate ------------------------------------------------------------

void PollGate::reset()
{
    m_lastPollMs = -1;
    m_ownReplyPending = false;
    m_lastForeignMs = -1;
    m_foreignIntervalMs = -1;
    m_ridingAlong = false;
}

qint64 PollGate::quietThresholdMs() const
{
    if (m_foreignIntervalMs < 0) {
        return kMinQuietMs;
    }
    // 2x the observed cadence. The multiplier has to cover the TAIL of the
    // foreign client's jitter, not its mean: at a measured mean of 100.5 ms a
    // 1.3x threshold is 131 ms, and a sample already contained gaps of 121 ms,
    // so occasional gaps exceed it and the gate flaps between riding along and
    // polling. Observed live before this was widened. The cost of 2x is that a
    // departed foreign client is noticed one extra cadence later -- 200 ms at
    // the measured rate, which nothing can perceive.
    const qint64 scaled = m_foreignIntervalMs * 2;
    return std::clamp<qint64>(scaled, kMinQuietMs, kMaxQuietMs);
}

void PollGate::onRecord(qint64 nowMs)
{
    // At most ONE reply per poll: a second record inside the same window is
    // necessarily another client's, however well the phases happen to line up.
    const bool ours = m_ownReplyPending
                      && m_lastPollMs >= 0
                      && (nowMs - m_lastPollMs) <= kOwnReplyWindowMs;
    if (ours) {
        m_ownReplyPending = false;
        return;
    }

    // Estimate the foreign cadence only from CONSECUTIVE foreign records, so
    // one own-reply misclassified as foreign (a reply delayed past the window)
    // cannot poison the estimate with a spuriously short interval.
    if (m_lastForeignMs >= 0) {
        const qint64 gap = nowMs - m_lastForeignMs;
        if (gap > 0 && gap <= kMaxQuietMs * 4) {
            m_foreignIntervalMs = (m_foreignIntervalMs < 0)
                                      ? gap
                                      : (m_foreignIntervalMs * 3 + gap) / 4;
        }
    }
    m_lastForeignMs = nowMs;   // read above before being overwritten here
}

bool PollGate::shouldPoll(qint64 nowMs)
{
    if (m_lastForeignMs >= 0
        && (nowMs - m_lastForeignMs) < quietThresholdMs()) {
        m_ridingAlong = true;
        return false;
    }
    m_ridingAlong = false;
    m_lastPollMs = nowMs;
    m_ownReplyPending = true;
    return true;
}

// ---- RangeCeilings / RangeTracker ---------------------------------------

double RangeCeilings::forRange(int rangeIndex) const
{
    switch (rangeIndex) {
        case 0: return highW;
        case 1: return midW;
        case 2: return lowW;
        default: break;
    }
    // An unrecognised range index must not silently scale the gauge to zero.
    // The widest configured ceiling is the safe fallback: it under-reads the
    // needle rather than pinning it.
    return std::max({highW, midW, lowW});
}

void RangeTracker::setCeilings(const RangeCeilings& ceilings, CeilingSource source)
{
    // Captured BEFORE the assignment: whether the edit touched the range
    // currently on screen is the question the OperatorEdit branch turns on.
    const double previous = m_ceilings.forRange(m_displayedRange);
    m_ceilings = ceilings;
    const double configured = m_ceilings.forRange(m_displayedRange);

    // An operator edit is authoritative by definition -- it is a person
    // telling us what their meter is actually set to, which is the one thing
    // this protocol never puts on the wire. Take it even when it lowers an
    // auto-expanded ceiling; if observed power really does exceed it, the
    // kCeilingExpandRecords run re-expands within two records anyway, so the
    // worst case is self-correcting and visible.
    //
    // But only when the edit actually moved THIS range. The menu edits one
    // range at a time while the gauge shows whichever the meter reports, so
    // editing Low while High is displayed used to discard High's expansion:
    // `configured` was unchanged, yet m_ceilingW was overwritten with it and
    // the flag cleared. Self-correcting, but a scale that visibly jumps when
    // the operator edited a different range is a bug in its own right.
    if (source == CeilingSource::OperatorEdit) {
        if (std::fabs(configured - previous) > 1e-9) {
            m_ceilingW = configured;
            m_autoExpanded = false;
        }
        return;
    }

    // A re-read must never SHRINK one that observed power has already expanded
    // within this session -- the same up-only guard AcomConnection applies to
    // a late SystemConfig reply that would otherwise snap the tier below what
    // auto-ranging had established.
    if (!m_autoExpanded || configured > m_ceilingW) {
        m_ceilingW = configured;
        m_autoExpanded = false;
    }
}

void RangeTracker::reset()
{
    m_displayedRange = 0;
    m_ceilingW = m_ceilings.forRange(0);
    m_autoExpanded = false;
    m_candidateRange = -1;
    m_candidateSinceMs = -1;
    m_overCeilingRun = 0;
}

void RangeTracker::adoptRange(int range)
{
    m_displayedRange = range;
    m_ceilingW = m_ceilings.forRange(range);
    m_autoExpanded = false;
    m_candidateRange = -1;
    m_candidateSinceMs = -1;
    m_overCeilingRun = 0;
}

void RangeTracker::onReading(int reportedRange, double watts, qint64 nowMs)
{
    // Range indices run 0=High .. 2=Low, so a SMALLER index is a bigger
    // range. Expand immediately and unconditionally -- deliberately with no
    // "power is present" gate, because such a gate defers an expansion
    // arriving on the first record of a transmission to the second, which is
    // exactly the record where the wide scale is most wanted.
    if (reportedRange < m_displayedRange) {
        adoptRange(reportedRange);
    } else if (reportedRange == m_displayedRange) {
        m_candidateRange = -1;
        m_candidateSinceMs = -1;
    } else {
        // Contract only after the meter has held the SAME smaller range for
        // kContractHoldMs of wall-clock time. Any disagreeing record restarts
        // the candidate, so a meter hunting between ranges never contracts.
        if (reportedRange != m_candidateRange) {
            m_candidateRange = reportedRange;
            m_candidateSinceMs = nowMs;
        } else if (m_candidateSinceMs >= 0
                   && nowMs - m_candidateSinceMs >= kContractHoldMs) {
            adoptRange(reportedRange);
        }
    }

    // Session-scoped, up-only ceiling expansion, so a ceiling configured too
    // low reads as a generous axis rather than a pinned needle. Never
    // persisted: reset() restores the configured value.
    if (m_ceilingW > 0.0 && watts > m_ceilingW) {
        if (++m_overCeilingRun >= kCeilingExpandRecords) {
            m_ceilingW = watts;
            m_autoExpanded = true;
            m_overCeilingRun = 0;
        }
    } else {
        m_overCeilingRun = 0;
    }
}

}  // namespace LpMeter
}  // namespace AetherSDR
