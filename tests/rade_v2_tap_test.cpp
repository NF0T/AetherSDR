// Regression guard for RadeV2Tap — the bench WAV taps along the RAD2 chain.
//
// Worth a real test rather than the one-off check that first validated it,
// because every property here fails QUIETLY. A tap that silently keeps the
// oldest audio, or slips half a sample at a discard boundary, still produces a
// perfectly valid WAV file of about the right size — and it will be believed,
// because the whole point of a tap is that you trust it when the thing it is
// measuring is already suspect.
//
// The ring is the part that most needs pinning. RX taps run for as long as the
// mode is selected, so the buffer WILL wrap in any real session, and which end
// it discards decides whether "something just went wrong, let me look" works or
// hands you three minutes of silence from before the problem.
//
//   test                          mutation that must break it        confirmed
//   ───────────────────────────── ────────────────────────────────── ─────────
//   keepsTheNewestWhenItWraps     discard from the back, not front   ✔ fails
//   keepsTheNewestWhenItWraps     remove the discard loop entirely   ✔ fails
//   neverExceedsTheCap            remove the discard loop entirely   ✔ fails
//   underTheCapNothingIsDropped   discard unconditionally            ✔ fails
//   staysFrameAlignedAcrossWrap   (see the note on that test)        ✘ NOT proven
//   wavHeaderMatchesTheData       any header field wrong             — by inspection
//   interleavesIQAsStereo         swap I and Q                       — by inspection
//
// The ✔ rows were each applied to RadeV2Tap.h, rebuilt, and confirmed to fail
// exactly the tests named and nothing else. The ✘ row is honest: see the
// comment on that test for the two mutations that did NOT break it and why.
//
// Built with -DRADE_V2_WAV_TAP because the header is otherwise empty. It needs
// no codec and no aethercore — it is header-only over Qt — so unlike the rest
// of RADE V2 it compiles and runs in the existing cross-platform CI jobs.

#include <cstring>
#include <vector>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "core/RadeV2Tap.h"

using AetherSDR::RadeV2Tap;

namespace {

struct Wav {
    int rate = 0, channels = 0, bits = 0, format = 0;
    QByteArray data;
    qsizetype frames() const {
        return channels ? data.size() / qsizetype(channels * 4) : 0;
    }
    float sample(qsizetype frame, int ch) const {
        float v = 0.0f;
        memcpy(&v, data.constData() + (frame * channels + ch) * 4, 4);
        return v;
    }
};

// Deliberately parses the file rather than trusting what was written: the
// header is part of the contract, and a decoder on the other end will believe
// it. Little-endian reads, matching the writer.
bool readWav(const QString& path, Wav& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray all = f.readAll();
    f.close();
    if (all.size() < 44) return false;
    if (memcmp(all.constData(), "RIFF", 4) != 0) return false;
    if (memcmp(all.constData() + 8, "WAVE", 4) != 0) return false;
    auto rd16 = [&](int o) { return quint16(quint8(all[o])) | (quint16(quint8(all[o+1])) << 8); };
    auto rd32 = [&](int o) { return quint32(quint8(all[o])) | (quint32(quint8(all[o+1])) << 8)
                                  | (quint32(quint8(all[o+2])) << 16) | (quint32(quint8(all[o+3])) << 24); };
    out.format   = rd16(20);
    out.channels = rd16(22);
    out.rate     = int(rd32(24));
    out.bits     = rd16(34);
    const quint32 dataSize = rd32(40);
    if (memcmp(all.constData() + 36, "data", 4) != 0) return false;
    if (qsizetype(dataSize) != all.size() - 44) return false;   // header agrees with reality
    out.data = all.mid(44);
    return true;
}

// A block whose every sample carries its own frame index, so the test can say
// exactly WHICH audio survived a discard rather than only how much.
std::vector<float> markedBlock(int frames, int channels, qint64& counter) {
    std::vector<float> b(size_t(frames) * size_t(channels));
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c)
            b[size_t(i) * size_t(channels) + size_t(c)] = float(counter);
        ++counter;
    }
    return b;
}

}  // namespace

class RadeV2TapTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        // Exercises the runtime override as a side effect: if that stopped
        // working, every test here would write to the source tree instead.
        qputenv("AETHER_RADE_V2_TAP_DIR", m_dir.path().toLocal8Bit());
    }

    // ─── The ring ──────────────────────────────────────────────────────────

    void keepsTheNewestWhenItWraps() {
        // MUTATION: discard from the back (removeLast) instead of the front.
        // The file is then the same size and equally valid, and holds exactly
        // the wrong audio.
        RadeV2Tap tap;
        qint64 counter = 0;
        const int blockFrames = 4096;
        const qint64 targetFrames = (RadeV2Tap::kMaxBytesPerTap / 8) * 5 / 4;  // 1.25x cap
        while (counter < targetFrames) {
            const auto b = markedBlock(blockFrames, 2, counter);
            tap.add("ring", 24000, 2, b.data(), blockFrames);
        }
        tap.flush("t");

        Wav w;
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_ring.wav"), w));
        QVERIFY(w.frames() > 0);

        // The last frame written must be the last frame present.
        QCOMPARE(w.sample(w.frames() - 1, 0), float(counter - 1));
        // …and the oldest must be gone, or nothing was discarded at all.
        QVERIFY2(w.sample(0, 0) > 0.0f,
                 "frame 0 survived — the buffer never wrapped, so this test "
                 "proved nothing about which end is discarded");
        // Contiguous: a ring must drop a prefix, not punch a hole.
        const float first = w.sample(0, 0);
        QCOMPARE(w.sample(w.frames() - 1, 0) - first, float(w.frames() - 1));
    }

    void neverExceedsTheCap() {
        // MUTATION: remove the discard loop. Nothing else changes — the file is
        // just quietly enormous, which on a long RX session is an OOM.
        RadeV2Tap tap;
        qint64 counter = 0;
        const int blockFrames = 4096;
        const qint64 targetFrames = (RadeV2Tap::kMaxBytesPerTap / 8) * 2;      // 2x cap
        while (counter < targetFrames) {
            const auto b = markedBlock(blockFrames, 2, counter);
            tap.add("cap", 24000, 2, b.data(), blockFrames);
        }
        tap.flush("t");

        Wav w;
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_cap.wav"), w));
        QVERIFY2(w.data.size() <= RadeV2Tap::kMaxBytesPerTap,
                 qPrintable(QString("retained %1 bytes against a %2 cap")
                                .arg(w.data.size()).arg(RadeV2Tap::kMaxBytesPerTap)));
        // And it should be near the cap, not a token amount — a "delete
        // everything and start over" strategy also satisfies the bound above
        // while leaving you almost nothing to look at.
        QVERIFY2(w.data.size() > RadeV2Tap::kMaxBytesPerTap / 2,
                 "retained far less than the cap — the buffer is being cleared "
                 "rather than recycled, so a wrap loses nearly all the history");
    }

    void staysFrameAlignedAcrossWrap_data() {
        QTest::addColumn<int>("channels");
        QTest::newRow("mono")   << 1;
        QTest::newRow("stereo") << 2;
    }

    void staysFrameAlignedAcrossWrap() {
        // ⚠ WEAKER THAN THE OTHERS, AND THAT IS RECORDED DELIBERATELY.
        //
        // This asserts an invariant it cannot reliably force to fail. Two
        // mutations were tried and NEITHER broke it: making kChunkBytes not a
        // multiple of the frame size, and additionally splitting one append
        // across two chunks. The first does nothing because alignment does not
        // depend on that constant (see RadeV2Tap.h). The second does misalign
        // the buffer, but each misaligned chunk shifts by only 4 bytes, so
        // discarding an EVEN number of them restores alignment — and this
        // workload happens to discard eight.
        //
        // Kept anyway, because it would catch a gross regression and it
        // documents the contract. But it is NOT in the same class as the ring
        // tests below: treat it as a statement of intent, not a proven guard.
        // The real protection is structural — add() appends whole frames and
        // never splits one across chunks.
        QFETCH(int, channels);
        RadeV2Tap tap;
        qint64 counter = 0;
        const int blockFrames = 4096;
        const qint64 bytesPerFrame = channels * 4;
        const qint64 targetFrames = (RadeV2Tap::kMaxBytesPerTap / bytesPerFrame) * 5 / 4;
        while (counter < targetFrames) {
            const auto b = markedBlock(blockFrames, channels, counter);
            tap.add("align", 16000, channels, b.data(), blockFrames);
        }
        tap.flush("t");

        Wav w;
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_align.wav"), w));
        QCOMPARE(w.channels, channels);
        QCOMPARE(w.data.size() % bytesPerFrame, qsizetype(0));

        // Every channel of a frame carries the same marker, so a half-frame
        // slip shows up as channels disagreeing. Checked at the START, which is
        // where a discard boundary lands.
        for (int c = 1; c < channels; ++c)
            QCOMPARE(w.sample(0, c), w.sample(0, 0));
        QCOMPARE(w.sample(w.frames() - 1, 0), float(counter - 1));
    }

    void underTheCapNothingIsDropped() {
        // MUTATION: discard unconditionally rather than only when over the cap.
        RadeV2Tap tap;
        qint64 counter = 0;
        const int blockFrames = 1000;
        for (int i = 0; i < 10; ++i) {
            const auto b = markedBlock(blockFrames, 1, counter);
            tap.add("small", 8000, 1, b.data(), blockFrames);
        }
        tap.flush("t");

        Wav w;
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_small.wav"), w));
        QCOMPARE(w.frames(), qsizetype(10 * blockFrames));
        QCOMPARE(w.sample(0, 0), 0.0f);
        QCOMPARE(w.sample(w.frames() - 1, 0), float(counter - 1));
    }

    // ─── The file itself ───────────────────────────────────────────────────

    void wavHeaderMatchesTheData() {
        RadeV2Tap tap;
        qint64 counter = 0;
        const auto b = markedBlock(256, 2, counter);
        tap.add("hdr", 24000, 2, b.data(), 256);
        tap.flush("t");

        Wav w;
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_hdr.wav"), w));
        QCOMPARE(w.format, 3);        // IEEE float — not PCM
        QCOMPARE(w.bits, 32);
        QCOMPARE(w.channels, 2);
        QCOMPARE(w.rate, 24000);
        QCOMPARE(w.frames(), qsizetype(256));
    }

    void interleavesIQAsStereo() {
        // MUTATION: swap I and Q, or write them as separate runs. Both produce
        // a plausible stereo file that decodes to nonsense.
        RadeV2Tap tap;
        std::vector<float> re(64), im(64);
        for (int i = 0; i < 64; ++i) { re[size_t(i)] = float(i); im[size_t(i)] = float(-i); }
        tap.addRadeComp("iq", 8000, re.data(), im.data(), 64);
        tap.flush("t");

        Wav w;
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_iq.wav"), w));
        QCOMPARE(w.channels, 2);
        QCOMPARE(w.frames(), qsizetype(64));
        for (int i = 0; i < 64; ++i) {
            QCOMPARE(w.sample(i, 0), float(i));    // I in left
            QCOMPARE(w.sample(i, 1), float(-i));   // Q in right
        }
    }

    // ─── Bookkeeping ───────────────────────────────────────────────────────

    void separateTapsDoNotMix() {
        RadeV2Tap tap;
        std::vector<float> a(16, 1.0f), b(16, 2.0f);
        tap.add("one", 8000, 1, a.data(), 16);
        tap.add("two", 8000, 1, b.data(), 16);
        tap.flush("t");

        Wav w1, w2;
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_one.wav"), w1));
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_two.wav"), w2));
        QCOMPARE(w1.sample(0, 0), 1.0f);
        QCOMPARE(w2.sample(0, 0), 2.0f);
        QCOMPARE(w1.frames(), qsizetype(16));
        QCOMPARE(w2.frames(), qsizetype(16));
    }

    void resetDiscardsEverything() {
        RadeV2Tap tap;
        std::vector<float> a(16, 1.0f);
        tap.add("gone", 8000, 1, a.data(), 16);
        tap.reset();
        tap.flush("t");
        QVERIFY2(!QFile::exists(m_dir.filePath("rade_v2_t_gone.wav")),
                 "reset() left data behind, so a tap from a previous over can "
                 "leak into the next one");
    }

    void flushClearsSoOversDoNotAccumulate() {
        RadeV2Tap tap;
        std::vector<float> a(16, 1.0f);
        tap.add("over", 8000, 1, a.data(), 16);
        tap.flush("t");
        Wav first;
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_over.wav"), first));
        QCOMPARE(first.frames(), qsizetype(16));

        // A second over of the same length must produce the same length, not
        // double it.
        tap.add("over", 8000, 1, a.data(), 16);
        tap.flush("t");
        Wav second;
        QVERIFY(readWav(m_dir.filePath("rade_v2_t_over.wav"), second));
        QCOMPARE(second.frames(), qsizetype(16));
    }

    void ignoresEmptyAndNullInput() {
        RadeV2Tap tap;
        std::vector<float> a(16, 1.0f);
        tap.add("guard", 8000, 1, nullptr, 16);
        tap.add("guard", 8000, 1, a.data(), 0);
        tap.add("guard", 8000, 1, a.data(), -1);
        tap.flush("t");
        QVERIFY(!QFile::exists(m_dir.filePath("rade_v2_t_guard.wav")));
    }

private:
    QTemporaryDir m_dir;
};

QTEST_APPLESS_MAIN(RadeV2TapTest)
#include "rade_v2_tap_test.moc"
