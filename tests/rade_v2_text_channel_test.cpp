// Regression guard for RadeV2TextChannel — the interim framing on RADE V2's
// inline 25 bit/s data channel (RFC §9.2).
//
// Every failure this class can have is silent. A wrong callsign looks exactly
// like a right one to everything downstream, and a channel that never syncs
// looks exactly like a channel nobody transmitted on. So each test below is
// paired with the specific mutation it must catch, and each was confirmed to
// FAIL under that mutation before being trusted — the discipline this branch
// adopted after the §7.2 boundary guard turned out to be matching comments.
//
//   test                        mutation that must break it
//   ─────────────────────────── ──────────────────────────────────────────────
//   joinsMidStream              consume the buffer on a failed candidate
//   survivesAFalseSyncWord      ditto, and `break` instead of `continue`
//   decodesInvertedPolarity     use |corr| without applying sign(corr)
//   rejectsCorruptedFrames      drop the CRC check
//   rejectsUnusedCodePoints     drop the alphabet range check
//   rejectsAWrongVersion        drop the version check
//   pinsTheWireFormat           any change to the frame layout at all
//
// Note what is NOT asserted anywhere: that a corrupted frame produces no
// output. It must produce no output *and* no wrong text, and those are
// different claims — the second is the one that matters, so it is the one
// written down.

#include <cmath>
#include <optional>
#include <vector>

#include <QtTest>

#include "core/RadeV2TextChannel.h"

using AetherSDR::RadeV2TextChannel;

namespace {

// Push a bit vector as ideal ±1 soft symbols, returning every frame decoded.
std::vector<RadeV2TextChannel::Decoded>
pushBits(RadeV2TextChannel& ch, const std::vector<bool>& bits,
         float amplitude = 1.0f, bool invert = false) {
    std::vector<RadeV2TextChannel::Decoded> out;
    for (bool b : bits) {
        float s = (b ? amplitude : -amplitude);
        if (invert) s = -s;
        if (auto d = ch.pushSymbol(s)) out.push_back(*d);
    }
    return out;
}

// Deterministic pseudo-noise, so a failure is reproducible rather than a
// once-a-week surprise on CI.
std::vector<bool> noiseBits(int n, uint32_t seed) {
    std::vector<bool> out;
    uint32_t x = seed;
    for (int i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        out.push_back(((x >> 16) & 1u) != 0u);
    }
    return out;
}

std::vector<bool> concat(std::vector<bool> a, const std::vector<bool>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}  // namespace

class RadeV2TextChannelTest : public QObject {
    Q_OBJECT

private slots:

    // ─── Wire format ───────────────────────────────────────────────────────

    void pinsTheWireFormat() {
        // 36 + 6N. These numbers ARE the protocol: if they change, every other
        // implementation of it stops interoperating, so the change should have
        // to be deliberate enough to edit a test.
        QCOMPARE(int(RadeV2TextChannel::encode("").size()), 36);
        QCOMPARE(int(RadeV2TextChannel::encode("NF0T").size()), 60);
        QCOMPARE(int(RadeV2TextChannel::encode("3DA/VK2ABC/QRP").size()), 120);

        // 2.4 s and 4.8 s at 25 bit/s (§9.2) — the figures the RFC quotes.
        QCOMPARE(RadeV2TextChannel::encode("NF0T").size() / 25.0, 2.4);
        QCOMPARE(RadeV2TextChannel::encode("3DA/VK2ABC/QRP").size() / 25.0, 4.8);
    }

    void sanitisesInput() {
        QCOMPARE(RadeV2TextChannel::sanitise("nf0t"), QStringLiteral("NF0T"));
        QCOMPARE(RadeV2TextChannel::sanitise("  NF0T  "), QStringLiteral("NF0T"));
        // Dropped, not substituted: a substituted character is a wrong
        // callsign, a dropped one is merely a short one.
        QCOMPARE(RadeV2TextChannel::sanitise("NF0T!@#"), QStringLiteral("NF0T"));
        QCOMPARE(RadeV2TextChannel::sanitise("AAAAAAAAAAAAAAAAAAAA").size(),
                 RadeV2TextChannel::kMaxChars);
    }

    // ─── The happy path ────────────────────────────────────────────────────

    void roundTripsACallsign() {
        RadeV2TextChannel ch;
        const auto got = pushBits(ch, RadeV2TextChannel::encode("NF0T"));
        QCOMPARE(int(got.size()), 1);
        QCOMPARE(got[0].text, QStringLiteral("NF0T"));
        QVERIFY(!got[0].inverted);
        QVERIFY(got[0].complete);
        QVERIFY(got[0].confidence > 0.9f);
    }

    void roundTripsTheWorstCaseCallsign() {
        RadeV2TextChannel ch;
        const auto got = pushBits(ch, RadeV2TextChannel::encode("3DA/VK2ABC/QRP"));
        QCOMPARE(int(got.size()), 1);
        QCOMPARE(got[0].text, QStringLiteral("3DA/VK2ABC/QRP"));
    }

    void reportsEveryRepeatedCopy() {
        // Frames repeat back-to-back; each copy is genuine evidence, and
        // de-duplication is the consumer's policy rather than the protocol's.
        RadeV2TextChannel ch;
        const auto one = RadeV2TextChannel::encode("NF0T");
        const auto got = pushBits(ch, concat(concat(one, one), one));
        QCOMPARE(int(got.size()), 3);
        for (const auto& d : got) QCOMPARE(d.text, QStringLiteral("NF0T"));
    }

    // ─── Joining an over already in progress ───────────────────────────────

    void joinsMidStream() {
        // The operational case: we start listening partway through somebody
        // else's transmission. MUTATION: consume/clear the buffer on a failed
        // candidate and the leading partial frame poisons the real one.
        const auto frame = RadeV2TextChannel::encode("NF0T");
        std::vector<bool> partial(frame.begin() + 31, frame.end());

        RadeV2TextChannel ch;
        const auto got = pushBits(ch, concat(partial, frame));
        QVERIFY(!got.empty());
        QCOMPARE(got.back().text, QStringLiteral("NF0T"));
    }

    void joinsAfterArbitraryNoise() {
        const auto frame = RadeV2TextChannel::encode("NF0T");
        RadeV2TextChannel ch;
        const auto got = pushBits(ch, concat(noiseBits(97, 0xC0FFEEu), frame));
        QVERIFY(!got.empty());
        QCOMPARE(got.back().text, QStringLiteral("NF0T"));
    }

    void survivesAFalseSyncWord() {
        // A bare Barker-13 with garbage behind it, immediately before a real
        // frame. A reader that syncs and then blindly consumes a frame's worth
        // of bits desynchronises permanently here. MUTATION: `break` instead of
        // `continue` on a failed candidate, or consuming on failure.
        std::vector<bool> decoy;
        for (bool b : {1,1,1,1,1,0,0,1,1,0,1,0,1}) decoy.push_back(b != 0);
        decoy = concat(decoy, noiseBits(23, 0xBADF00Du));

        RadeV2TextChannel ch;
        const auto got = pushBits(ch, concat(decoy, RadeV2TextChannel::encode("NF0T")));
        QVERIFY(!got.empty());
        QCOMPARE(got.back().text, QStringLiteral("NF0T"));
    }

    // ─── Polarity ──────────────────────────────────────────────────────────

    void decodesInvertedPolarity() {
        // BPSK carries a 180° ambiguity. MUTATION: detect on |corr| but never
        // apply sign(corr) — the frame then slices to the complement of itself
        // and the CRC rejects it, so this reports zero frames instead of one.
        RadeV2TextChannel ch;
        const auto got = pushBits(ch, RadeV2TextChannel::encode("NF0T"),
                                  /*amplitude=*/1.0f, /*invert=*/true);
        QCOMPARE(int(got.size()), 1);
        QCOMPARE(got[0].text, QStringLiteral("NF0T"));
        QVERIFY(got[0].inverted);
    }

    // ─── Integrity: the tests that matter most ─────────────────────────────

    void rejectsCorruptedFrames() {
        // MUTATION: drop the CRC check. The assertion that matters is not
        // "nothing decoded" but "nothing WRONG decoded" — a placeholder that
        // publishes bit errors as somebody's callsign is the exact harm §9.3
        // defers FreeDV Reporter over.
        int accepted = 0, wrong = 0;
        for (int bit = RadeV2TextChannel::kSyncBits; bit < 60; ++bit) {
            auto frame = RadeV2TextChannel::encode("NF0T");
            frame[size_t(bit)] = !frame[size_t(bit)];

            RadeV2TextChannel ch;
            for (const auto& d : pushBits(ch, frame)) {
                ++accepted;
                if (d.text != QStringLiteral("NF0T")) ++wrong;
            }
        }
        QCOMPARE(wrong, 0);
        QCOMPARE(accepted, 0);
    }

    void rejectsUnusedCodePoints() {
        // Code points 39–63 are unused, so they are invalid on receive — a free
        // integrity check under the CRC. Built by hand because encode() cannot
        // produce one. MUTATION: drop the range check and this decodes as some
        // arbitrary character.
        std::vector<bool> fields;
        auto put = [&fields](uint32_t v, int n) {
            for (int b = n - 1; b >= 0; --b) fields.push_back(((v >> b) & 1u) != 0u);
        };
        put(RadeV2TextChannel::kVersion, RadeV2TextChannel::kVersionBits);
        put(1, RadeV2TextChannel::kLengthBits);
        put(40, RadeV2TextChannel::kBitsPerChar);        // ← out of range

        uint16_t crc = 0xFFFFu;                          // CRC-16/CCITT-FALSE
        for (bool b : fields) {
            const bool msb = ((crc >> 15) & 1u) != 0u;
            crc = uint16_t(crc << 1);
            if (msb != b) crc = uint16_t(crc ^ 0x1021u);
        }

        std::vector<bool> frame;
        for (bool b : {1,1,1,1,1,0,0,1,1,0,1,0,1}) frame.push_back(b != 0);
        frame = concat(frame, fields);
        for (int b = 15; b >= 0; --b) frame.push_back(((crc >> b) & 1u) != 0u);

        RadeV2TextChannel ch;
        QVERIFY(pushBits(ch, frame).empty());   // CRC is valid; the range check must reject
    }

    void rejectsAWrongVersion() {
        // The version field is what keeps our frames and upstream's eventual
        // ones distinguishable rather than mutually corrupting, so a version we
        // do not implement must be REJECTED, never best-effort parsed.
        std::vector<bool> fields;
        auto put = [&fields](uint32_t v, int n) {
            for (int b = n - 1; b >= 0; --b) fields.push_back(((v >> b) & 1u) != 0u);
        };
        put(RadeV2TextChannel::kVersion + 1, RadeV2TextChannel::kVersionBits);
        put(0, RadeV2TextChannel::kLengthBits);

        uint16_t crc = 0xFFFFu;
        for (bool b : fields) {
            const bool msb = ((crc >> 15) & 1u) != 0u;
            crc = uint16_t(crc << 1);
            if (msb != b) crc = uint16_t(crc ^ 0x1021u);
        }

        std::vector<bool> frame;
        for (bool b : {1,1,1,1,1,0,0,1,1,0,1,0,1}) frame.push_back(b != 0);
        frame = concat(frame, fields);
        for (int b = 15; b >= 0; --b) frame.push_back(((crc >> b) & 1u) != 0u);

        RadeV2TextChannel ch;
        QVERIFY(pushBits(ch, frame).empty());
    }

    // ─── Soft decisions ────────────────────────────────────────────────────

    void detectionIsScaleFree() {
        // The decoder's output is a learned reconstruction, not a ±1 signal, so
        // a uniformly scaled stream must still sync. MUTATION: compare the raw
        // correlation against the threshold without normalising by the mean
        // magnitude, and this decodes nothing.
        RadeV2TextChannel ch;
        const auto got = pushBits(ch, RadeV2TextChannel::encode("NF0T"), 0.15f);
        QCOMPARE(int(got.size()), 1);
        QCOMPARE(got[0].text, QStringLiteral("NF0T"));
    }

    void confidenceTracksSoftMagnitude() {
        // …while confidence is deliberately NOT normalised, so it stays an
        // absolute quality measure against the transmitted ±1.0.
        RadeV2TextChannel strong, weak;
        const auto s = pushBits(strong, RadeV2TextChannel::encode("NF0T"), 1.0f);
        const auto w = pushBits(weak,   RadeV2TextChannel::encode("NF0T"), 0.15f);
        QCOMPARE(int(s.size()), 1);
        QCOMPARE(int(w.size()), 1);
        QVERIFY(w[0].confidence < s[0].confidence);
        QVERIFY(s[0].confidence > 0.9f);
        QVERIFY(w[0].confidence < 0.5f);
    }

    // ─── Housekeeping ──────────────────────────────────────────────────────

    void resetDropsPartialState() {
        const auto frame = RadeV2TextChannel::encode("NF0T");
        RadeV2TextChannel ch;
        for (size_t i = 0; i < frame.size() - 1; ++i)
            ch.pushSymbol(frame[i] ? 1.0f : -1.0f);
        ch.reset();
        // The final bit alone must not complete the pre-reset frame.
        QVERIFY(!ch.pushSymbol(frame.back() ? 1.0f : -1.0f).has_value());
    }

    void emptyPayloadIsAValidIdleFrame() {
        RadeV2TextChannel ch;
        const auto got = pushBits(ch, RadeV2TextChannel::encode(""));
        QCOMPARE(int(got.size()), 1);
        QVERIFY(got[0].text.isEmpty());
    }
};

QTEST_APPLESS_MAIN(RadeV2TextChannelTest)
#include "rade_v2_text_channel_test.moc"
