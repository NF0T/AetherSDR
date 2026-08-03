#pragma once

// RadeV2TextChannel — the application-layer protocol for RADE V2's inline
// low-speed data channel (RFC: docs/architecture/rade-v2-waveform-design.md
// §9.2, §16 Q2).
//
// ─── This class is designed to be DELETED ──────────────────────────────────
// V2's data channel is RAW: `rade_tx_set_data_symbol` / `rade_rx_get_data_symbol`
// carry one BPSK symbol per modem step and nothing else — no sync, no framing,
// no CRC, no FEC. The *application* owns the protocol, and the convention that
// will matter for interoperability is the one the FreeDV ecosystem publishes,
// not this one. §9.2 commits us to adopting theirs at vendor time.
//
// So this exists to exercise the channel end-to-end until then, and every
// choice below optimises for being replaceable rather than for being right:
// the protocol lives in exactly one file, the `version` field means a
// mismatched frame is REJECTED rather than misread, and nothing outside
// RADEV2Engine knows the frame layout.
//
// ─── Deliberately NOT behind AETHER_ENABLE_RADE_V2 ─────────────────────────
// This is pure bit manipulation — no codec, no Flex protocol, no platform
// dependency. Compiling it unconditionally costs nothing and buys something
// the rest of RADE V2 does not have: it is built and tested by the existing
// cross-platform CI jobs, which never set ENABLE_RADE_V2. Its *caller* in
// RADEV2Engine stays gated.
//
// ─── The frame ─────────────────────────────────────────────────────────────
//
//   ┌────────────┬─────┬─────┬──────────────┬─────────┐
//   │  Barker-13 │ ver │ len │  N × 6 bits  │ CRC-16  │
//   │    sync    │  3  │  4  │   payload    │         │
//   └────────────┴─────┴─────┴──────────────┴─────────┘
//        13        3     4        6N            16      = 36 + 6N bits
//
// At 25 bit/s (§9.2) that is 2.4 s for `NF0T` and 4.8 s for a 14-character
// worst case. Frames repeat back-to-back with no gap, so a receiver joining
// mid-over picks up the next copy.
//
// **CRC-16, not CRC-8, and the 0.32 s it costs is the point.** CRC-8 admits
// ~1/256 of corrupted frames that clear the sync gate, and this field gets
// DISPLAYED — publishing a bit error as somebody else's callsign is precisely
// the harm §9.3 defers FreeDV Reporter over.
//
// ─── Two things the API shape is protecting ────────────────────────────────
// 1. It is named for TEXT, not callsign, and `Decoded::complete` exists even
//    though version 1 always sets it true. Upstream may carry a message field
//    as well (V1 has FreeDvMyMessage) and the operator expects
//    display-as-it-arrives, so the *file boundary* should survive that, not
//    just the filename.
// 2. It is a plain class, not a QObject with a `textDecoded` signal. It lives
//    inside a worker-thread QObject, so a QObject member would raise
//    thread-affinity questions for nothing; as a plain class it tests with no
//    Qt event loop and the offline decoder links it directly. RADEV2Engine
//    already owns all signal emission.

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include <QString>
#include <QtGlobal>

namespace AetherSDR {

class RadeV2TextChannel {
public:
    // ─── Wire format ───────────────────────────────────────────────────────
    static constexpr int kVersion     = 1;   // 0 and 7 reserved
    static constexpr int kSyncBits    = 13;  // Barker-13
    static constexpr int kVersionBits = 3;
    static constexpr int kLengthBits  = 4;   // ⇒ at most 15 characters
    static constexpr int kCrcBits     = 16;
    static constexpr int kBitsPerChar = 6;
    static constexpr int kMaxChars    = (1 << kLengthBits) - 1;   // 15

    static constexpr int kOverheadBits =
        kSyncBits + kVersionBits + kLengthBits + kCrcBits;         // 36

    static constexpr int frameBits(int chars) {
        return kOverheadBits + kBitsPerChar * chars;
    }

    // 39 values; indices 39–63 are unused and therefore INVALID on receive,
    // which is a free integrity check layered under the CRC.
    static const char* alphabet();          // " ABC…XYZ0123456789/-"
    static constexpr int kAlphabetSize = 39;

    // ─── Detection ─────────────────────────────────────────────────────────
    // Normalised |correlation| against Barker-13 required to consider a frame.
    //
    // **This number is a placeholder and must be re-derived from real captures
    // before it is trusted.** It cannot be computed from first principles: it
    // depends on how the neural decoder's reconstruction of the 21st feature
    // behaves under real channel conditions, which nothing has measured yet.
    // `rade_v2_decode_wav --dump-symbols` exists to supply that evidence.
    static constexpr float kDefaultSyncThreshold = 0.70f;

    struct Decoded {
        QString text;
        float   confidence = 0.0f;   // mean |soft| over the frame, clamped 0–1
        bool    complete   = true;   // always true in version 1
        bool    inverted   = false;  // the BPSK polarity that decoded
    };

    // Uppercases, drops any character outside `alphabet()`, truncates to
    // kMaxChars. Dropping rather than substituting is deliberate — a
    // substituted character is a wrong callsign, a dropped one is a short one.
    static QString sanitise(const QString& text);

    // A complete frame: sync, fields, payload and CRC. The caller cycles these
    // bits continuously; consecutive copies are back-to-back by construction.
    static std::vector<bool> encode(const QString& text);

    // One soft symbol from rade_rx_get_data_symbol(). Returns a frame whenever
    // one ENDS at this symbol.
    //
    // Emits on EVERY valid copy, including repeats of a callsign already seen —
    // de-duplication is the consumer's policy, not the protocol's, and a second
    // copy is genuine evidence rather than noise.
    std::optional<Decoded> pushSymbol(float soft);

    void reset();

    void  setSyncThreshold(float threshold);
    float syncThreshold() const { return m_syncThreshold; }

    // ─── Diagnostics ───────────────────────────────────────────────────────
    quint64 symbolsSeen()    const { return m_symbolsSeen; }
    quint64 framesAccepted() const { return m_framesAccepted; }

private:
    std::optional<Decoded> tryFrameEndingNow(int chars) const;

    std::deque<float> m_buf;
    double            m_absSum = 0.0;   // running Σ|soft| over m_buf

    float   m_syncThreshold = kDefaultSyncThreshold;
    quint64 m_symbolsSeen    = 0;
    quint64 m_framesAccepted = 0;
};

}  // namespace AetherSDR
