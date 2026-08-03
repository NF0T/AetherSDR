#include "core/RadeV2TextChannel.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// Barker-13. Optimal aperiodic autocorrelation for its length — peak 13,
// sidelobes ≤ 1 — which is what makes a threshold on the *normalised*
// correlation meaningful rather than arbitrary.
constexpr float kBarker[RadeV2TextChannel::kSyncBits] = {
    +1, +1, +1, +1, +1, -1, -1, +1, +1, -1, +1, -1, +1
};

constexpr char kAlphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/-";
static_assert(sizeof(kAlphabet) - 1 == RadeV2TextChannel::kAlphabetSize,
              "alphabet size must match the declared constant");
static_assert(RadeV2TextChannel::kAlphabetSize <= (1 << RadeV2TextChannel::kBitsPerChar),
              "alphabet must fit the per-character bit width");

int alphabetIndex(QChar c) {
    for (int i = 0; i < RadeV2TextChannel::kAlphabetSize; ++i)
        if (kAlphabet[i] == c.toLatin1()) return i;
    return -1;
}

void appendBits(std::vector<bool>& out, uint32_t value, int bits) {
    for (int b = bits - 1; b >= 0; --b)          // MSB first
        out.push_back(((value >> b) & 1u) != 0u);
}

// CRC-16/CCITT-FALSE — poly 0x1021, init 0xFFFF, no reflection, no final XOR.
//
// Computed BITWISE over the field bit-string, NOT over bytes. The frame is
// 3 + 4 + 6N bits and is therefore not byte-aligned; a byte-oriented CRC would
// need a padding convention, and an undocumented padding convention is exactly
// what makes a second implementation disagree with the first. One bit at a
// time has no such ambiguity and, at 25 bit/s, no cost worth measuring.
uint16_t crc16(const std::vector<bool>& bits) {
    uint16_t crc = 0xFFFFu;
    for (bool b : bits) {
        const bool msb = ((crc >> 15) & 1u) != 0u;
        crc = uint16_t(crc << 1);
        if (msb != b) crc = uint16_t(crc ^ 0x1021u);
    }
    return crc;
}

}  // namespace

const char* RadeV2TextChannel::alphabet() { return kAlphabet; }

QString RadeV2TextChannel::sanitise(const QString& text) {
    QString out;
    for (QChar c : text.trimmed().toUpper()) {
        if (alphabetIndex(c) >= 0) out.append(c);
        if (out.size() >= kMaxChars) break;
    }
    return out;
}

std::vector<bool> RadeV2TextChannel::encode(const QString& text) {
    const QString payload = sanitise(text);

    // Everything the CRC covers: version, length, payload. The sync word is
    // excluded deliberately — a frame is only ever CRC-checked once it has
    // already correlated, so covering it would protect a field whose integrity
    // has been established by other means.
    std::vector<bool> fields;
    fields.reserve(size_t(kVersionBits + kLengthBits + kBitsPerChar * payload.size()));
    appendBits(fields, uint32_t(kVersion), kVersionBits);
    appendBits(fields, uint32_t(payload.size()), kLengthBits);
    for (QChar c : payload)
        appendBits(fields, uint32_t(alphabetIndex(c)), kBitsPerChar);

    std::vector<bool> frame;
    frame.reserve(size_t(frameBits(int(payload.size()))));
    for (float s : kBarker) frame.push_back(s > 0.0f);
    frame.insert(frame.end(), fields.begin(), fields.end());
    appendBits(frame, crc16(fields), kCrcBits);
    return frame;
}

void RadeV2TextChannel::reset() {
    m_buf.clear();
    m_absSum = 0.0;
}

void RadeV2TextChannel::setSyncThreshold(float threshold) {
    m_syncThreshold = std::clamp(threshold, 0.0f, 1.0f);
}

std::optional<RadeV2TextChannel::Decoded> RadeV2TextChannel::pushSymbol(float soft) {
    ++m_symbolsSeen;

    m_buf.push_back(soft);
    m_absSum += std::fabs(double(soft));
    while (int(m_buf.size()) > frameBits(kMaxChars)) {
        m_absSum -= std::fabs(double(m_buf.front()));
        m_buf.pop_front();
    }

    // Try every frame length that could END at this symbol — 16 of them. The
    // length field must then agree with the length assumed, which rejects
    // almost everything before the CRC is even consulted.
    //
    // **Nothing is consumed on failure, and that is load-bearing.** A false
    // Barker hit inside payload data must not be able to swallow the real
    // frame that follows it, which is exactly what a "sync, then take the next
    // N bits, then resynchronise" reader would do. Here a bad candidate simply
    // does not validate, and the real frame is still tried on its own terms.
    for (int chars = 0; chars <= kMaxChars; ++chars) {
        if (frameBits(chars) > int(m_buf.size())) break;   // monotonic in chars
        if (auto decoded = tryFrameEndingNow(chars)) {
            ++m_framesAccepted;
            return decoded;
        }
    }
    return std::nullopt;
}

std::optional<RadeV2TextChannel::Decoded>
RadeV2TextChannel::tryFrameEndingNow(int chars) const {
    const int    len   = frameBits(chars);
    const size_t start = m_buf.size() - size_t(len);

    // Scale-free detection. The decoder's output is a learned reconstruction of
    // a ±1 input, not a ±1 signal, so a fixed correlation threshold would track
    // the decoder's output level rather than the sync quality.
    const double mean = m_absSum / double(m_buf.size());
    const float  mag  = mean > 1e-6 ? float(mean) : 1.0f;

    float corr = 0.0f;
    for (int i = 0; i < kSyncBits; ++i)
        corr += kBarker[i] * m_buf[start + size_t(i)];
    const float norm = corr / (float(kSyncBits) * mag);

    if (std::fabs(norm) < m_syncThreshold) return std::nullopt;

    // BPSK carries a 180° ambiguity, so the sign of the correlation is data,
    // not noise: it says which polarity the stream arrived in. Taking |corr| to
    // detect and sign(corr) to correct means an inverted stream decodes instead
    // of silently producing garbage that the CRC then rejects.
    const bool inverted = norm < 0.0f;

    auto bitAt = [&](int k) {
        const float v = m_buf[start + size_t(k)];
        return (inverted ? -v : v) > 0.0f;
    };
    auto readUInt = [&](int at, int bits) {
        uint32_t v = 0;
        for (int b = 0; b < bits; ++b) v = (v << 1) | uint32_t(bitAt(at + b) ? 1 : 0);
        return v;
    };

    int pos = kSyncBits;
    if (int(readUInt(pos, kVersionBits)) != kVersion) return std::nullopt;
    pos += kVersionBits;
    if (int(readUInt(pos, kLengthBits)) != chars) return std::nullopt;
    pos += kLengthBits;

    QString text;
    for (int c = 0; c < chars; ++c) {
        const uint32_t idx = readUInt(pos + c * kBitsPerChar, kBitsPerChar);
        if (idx >= uint32_t(kAlphabetSize)) return std::nullopt;   // unused code point
        text.append(QChar(kAlphabet[idx]));
    }
    pos += chars * kBitsPerChar;

    std::vector<bool> fields;
    fields.reserve(size_t(kVersionBits + kLengthBits + kBitsPerChar * chars));
    for (int k = kSyncBits; k < pos; ++k) fields.push_back(bitAt(k));
    if (crc16(fields) != uint16_t(readUInt(pos, kCrcBits))) return std::nullopt;

    // Confidence is deliberately NOT normalised by `mag`, unlike detection
    // above. The transmitted symbol is exactly ±1.0, so the reconstruction's
    // raw magnitude is an absolute quality measure — dividing it by the mean of
    // the same stream would report ~1.0 for every frame and say nothing.
    double sum = 0.0;
    for (int k = 0; k < len; ++k) sum += std::fabs(double(m_buf[start + size_t(k)]));

    Decoded d;
    d.text       = text;
    d.confidence = float(std::clamp(sum / double(len), 0.0, 1.0));
    d.complete   = true;
    d.inverted   = inverted;
    return d;
}

}  // namespace AetherSDR
