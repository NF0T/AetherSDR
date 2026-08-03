#pragma once

// RadeV2Tap — per-stage WAV taps along the RAD2 audio chain, for bench work.
//
// The V2 counterpart of RADEEngine's `RADE_WAV_TAP` (which produced the
// rade_tap_A…F files the V1 EOO investigation was built on). Not carried over
// when RADEV2Engine was written, and its absence cost real time: the first OTA
// transmission carrying speech decoded as voice-shaped babble, and there was no
// way to ask "was there ever speech in what we sent, and at what level?"
// without inventing an answer.
//
// ─── Two independent flags ─────────────────────────────────────────────────
//   -DRADE_V2_WAV_TAP            TX taps (tx1…tx5). The V1-equivalent set.
//   -DRADE_V2_WAV_TAP_RX         RX taps (rx1…rx4). Separate ON PURPOSE — V1
//                                has no RX tap at all, and RX is the direction
//                                that runs unbounded, so it should not come
//                                along for free with a TX bench session.
//   -DRADE_V2_TAP_DIR=\"C:/tmp\" where files land (default ".")
//
// Either flag pulls in the class; neither declares a member for the direction
// it does not enable, so a TX-only build carries no RX buffer at all. With both
// off every macro is `((void)0)`: no branch, no buffer, no member.
//
// The directory is also overridable at run time with AETHER_RADE_V2_TAP_DIR,
// because moving output between runs should not need a rebuild.
//
// ─── Lifetimes differ by direction, which is why there are two objects ─────
// A TX tap set is exactly one over: reset at key, written when the tail is out,
// so the file includes the EOO. RX has no natural end — it runs for as long as
// the mode is selected — so it is written at stop().
//
// ─── The RX buffer is a RING, and keeps the MOST RECENT audio ─────────────
// The obvious guard is to stop accumulating at a cap. That is the wrong shape
// here: you notice a problem, deselect the mode, and discover the tap holds the
// first three minutes of a session whose interesting part was at minute ten.
// Discarding the OLDEST instead means the tap always holds the last ~175 s, so
// "something just went wrong, let me look" works. Storage is chunked so that
// dropping the front is O(1) rather than a memmove of tens of megabytes at
// 187 packets/second.
//
// ─── Why it prints a summary as well as writing files ──────────────────────
// The question that motivated this — "is our transmitted level right?" — is
// answered by peak and RMS per stage, not by opening nine WAV files. A stage
// that is silent, clipped, or 20 dB down is visible in the summary line.
//
// ─── What it deliberately does NOT do ──────────────────────────────────────
// No timestamping or rotation: files are overwritten each over. A directory
// full of rade_v2_tx_tx5_out_24k_mono_00017.wav is worse than one file you can
// point a decoder at. If you need to keep an over, copy it before the next one.

#if defined(RADE_V2_WAV_TAP) || defined(RADE_V2_WAV_TAP_RX)

#include <cmath>

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QString>
#include <QVector>

#ifndef RADE_V2_TAP_DIR
#  define RADE_V2_TAP_DIR "."
#endif

namespace AetherSDR {

class RadeV2Tap {
public:
    // Public so the test asserts against the same numbers the production path
    // uses, rather than against copies that can drift apart silently.
    //
    // Chunk granularity for the ring. Only the discard cost depends on this;
    // smaller chunks mean a tighter fit to the cap and more bookkeeping.
    //
    // It does NOT have to be a multiple of the frame size, and an earlier
    // comment here claimed it did. A mutation test disproved that: setting it
    // to 1 MB + 4 broke nothing. Frame alignment across a discard is
    // structural — a chunk is only ever grown by whole add() calls and every
    // add() appends a whole number of frames, so a chunk boundary is always a
    // frame boundary whatever this value is. **The property to protect is that
    // invariant**: if add() ever starts splitting one append across two chunks,
    // alignment breaks and this constant still will not save it.
    static constexpr qsizetype kChunkBytes     = qsizetype(1) << 20;

    // ~175 s of 24 kHz stereo float, ~350 s mono — sized for a listening
    // session, which is what RX needs.
    static constexpr qsizetype kMaxBytesPerTap = qsizetype(32) << 20;

    // Accumulate interleaved float frames. `frames` counts FRAMES, not floats,
    // so a stereo block of 128 frames is 256 floats — the same convention the
    // engine's own buffers use, to remove one conversion from the call sites.
    void add(const char* name, int rateHz, int channels,
             const float* interleaved, int frames) {
        if (!interleaved || frames <= 0) return;
        Buf* b = find(name);
        if (!b) {
            m_bufs.push_back(Buf{QString::fromLatin1(name), rateHz, channels, {}, 0, false});
            b = &m_bufs.last();
        }
        const qsizetype len = qsizetype(frames) * channels * qsizetype(sizeof(float));
        const char* src = reinterpret_cast<const char*>(interleaved);

        // One append goes into ONE chunk, never split. This is what keeps a
        // chunk boundary on a frame boundary (see kChunkBytes) — it is the
        // invariant, not the constant. A chunk overshoots kChunkBytes by at
        // most one block, which is what "size() >= kChunkBytes" allows for.
        if (b->chunks.isEmpty() || b->chunks.last().size() >= kChunkBytes)
            b->chunks.push_back(QByteArray());
        b->chunks.last().append(src, len);
        b->bytes += len;

        // Discard oldest whole chunks. kChunkBytes is a multiple of 8, so a
        // chunk boundary is also a frame boundary for both mono and stereo
        // float32 — dropping one cannot leave a half sample behind.
        while (b->bytes > kMaxBytesPerTap && b->chunks.size() > 1) {
            b->bytes -= b->chunks.first().size();
            b->chunks.removeFirst();
            if (!b->recycled) {
                b->recycled = true;
                qDebug("RADE_V2_TAP: %s reached %lld MB — now keeping the most "
                       "recent audio and discarding the oldest",
                       qPrintable(b->name), qint64(kMaxBytesPerTap >> 20));
            }
        }
    }

    // Convenience for the complex stages: writes I and Q as a stereo pair, so
    // the file opens in any editor and the two components stay aligned.
    void addRadeComp(const char* name, int rateHz, const float* reals,
                     const float* imags, int frames) {
        if (!reals || !imags || frames <= 0) return;
        QVector<float> il(qsizetype(frames) * 2);
        for (int i = 0; i < frames; ++i) { il[2*i] = reals[i]; il[2*i+1] = imags[i]; }
        add(name, rateHz, 2, il.constData(), frames);
    }

    void reset() { m_bufs.clear(); }

    // Write every accumulated stage and print one summary line each.
    void flush(const char* prefix) {
        if (m_bufs.isEmpty()) return;
        const QString dir = qEnvironmentVariableIsSet("AETHER_RADE_V2_TAP_DIR")
                          ? qEnvironmentVariable("AETHER_RADE_V2_TAP_DIR")
                          : QString::fromLatin1(RADE_V2_TAP_DIR);
        for (const Buf& b : m_bufs) {
            const QString path = QString("%1/rade_v2_%2_%3.wav")
                                     .arg(dir, QString::fromLatin1(prefix), b.name);
            writeWav(path, b);
        }
        m_bufs.clear();
    }

private:
    struct Buf {
        QString name; int rate; int ch;
        QVector<QByteArray> chunks;   // oldest first
        qsizetype bytes;
        bool recycled;                // has it wrapped at least once?
    };
    QVector<Buf> m_bufs;

    Buf* find(const char* name) {
        for (Buf& b : m_bufs)
            if (b.name == QLatin1String(name)) return &b;
        return nullptr;
    }

    static void writeWav(const QString& path, const Buf& b) {
        QByteArray all;
        all.reserve(b.bytes);
        for (const QByteArray& c : b.chunks) all.append(c);

        const auto* s = reinterpret_cast<const float*>(all.constData());
        const qsizetype n = all.size() / qsizetype(sizeof(float));
        double peak = 0.0, sum = 0.0;
        for (qsizetype i = 0; i < n; ++i) {
            const double v = std::fabs(double(s[i]));
            if (v > peak) peak = v;
            sum += double(s[i]) * s[i];
        }
        const double rms = n ? std::sqrt(sum / double(n)) : 0.0;
        const double secs = (b.ch && b.rate)
                          ? double(n) / double(b.ch) / double(b.rate) : 0.0;

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            qWarning("RADE_V2_TAP: cannot write %s", qPrintable(path));
            return;
        }
        const quint32 dataSize = quint32(all.size());
        const quint32 riffSize = 36 + dataSize;
        const quint32 fmtSize  = 16;
        const quint16 fmtFloat = 3;                       // IEEE float
        const quint16 ch       = quint16(b.ch);
        const quint32 rate     = quint32(b.rate);
        const quint32 byteRate = quint32(b.rate * b.ch * 4);
        const quint16 align    = quint16(b.ch * 4);
        const quint16 bits     = 32;
        f.write("RIFF", 4);
        f.write(reinterpret_cast<const char*>(&riffSize), 4);
        f.write("WAVE", 4);
        f.write("fmt ", 4);
        f.write(reinterpret_cast<const char*>(&fmtSize), 4);
        f.write(reinterpret_cast<const char*>(&fmtFloat), 2);
        f.write(reinterpret_cast<const char*>(&ch), 2);
        f.write(reinterpret_cast<const char*>(&rate), 4);
        f.write(reinterpret_cast<const char*>(&byteRate), 4);
        f.write(reinterpret_cast<const char*>(&align), 2);
        f.write(reinterpret_cast<const char*>(&bits), 2);
        f.write("data", 4);
        f.write(reinterpret_cast<const char*>(&dataSize), 4);
        f.write(all);
        f.close();

        // The line that answers "is the level right?" without opening anything.
        // dBFS against 1.0 full scale, which is what every stage here uses.
        qDebug("RADE_V2_TAP: %-22s %5d Hz %dch %7.2f s%s  peak %7.4f (%+6.1f dBFS)  rms %7.4f (%+6.1f dBFS)  -> %s",
               qPrintable(b.name), b.rate, b.ch, secs,
               b.recycled ? " [most recent, older discarded]" : "",
               peak, peak > 0 ? 20.0 * std::log10(peak) : -999.0,
               rms,  rms  > 0 ? 20.0 * std::log10(rms)  : -999.0,
               qPrintable(path));
    }
};

}  // namespace AetherSDR

#endif  // RADE_V2_WAV_TAP || RADE_V2_WAV_TAP_RX

// ─── TX macros ─────────────────────────────────────────────────────────────
#ifdef RADE_V2_WAV_TAP
#  define RADE_V2_TAP_TX_MEMBER   AetherSDR::RadeV2Tap m_tapTx;
#  define RADE_V2_TAP_TX_RESET()  m_tapTx.reset()
#  define RADE_V2_TAP_TX_FLUSH()  m_tapTx.flush("tx")
#  define RADE_V2_TAP_TX(name, rate, ch, ptr, frames) \
       m_tapTx.add(name, rate, ch, ptr, frames)
#  define RADE_V2_TAP_TX_IQ(name, rate, re, im, frames) \
       m_tapTx.addRadeComp(name, rate, re, im, frames)
#else
#  define RADE_V2_TAP_TX_MEMBER
#  define RADE_V2_TAP_TX_RESET()                        ((void)0)
#  define RADE_V2_TAP_TX_FLUSH()                        ((void)0)
#  define RADE_V2_TAP_TX(name, rate, ch, ptr, frames)   ((void)0)
#  define RADE_V2_TAP_TX_IQ(name, rate, re, im, frames) ((void)0)
#endif

// ─── RX macros ─────────────────────────────────────────────────────────────
#ifdef RADE_V2_WAV_TAP_RX
#  define RADE_V2_TAP_RX_MEMBER   AetherSDR::RadeV2Tap m_tapRx;
#  define RADE_V2_TAP_RX_FLUSH()  m_tapRx.flush("rx")
#  define RADE_V2_TAP_RX(name, rate, ch, ptr, frames) \
       m_tapRx.add(name, rate, ch, ptr, frames)
#  define RADE_V2_TAP_RX_IQ(name, rate, re, im, frames) \
       m_tapRx.addRadeComp(name, rate, re, im, frames)
#else
#  define RADE_V2_TAP_RX_MEMBER
#  define RADE_V2_TAP_RX_FLUSH()                        ((void)0)
#  define RADE_V2_TAP_RX(name, rate, ch, ptr, frames)   ((void)0)
#  define RADE_V2_TAP_RX_IQ(name, rate, re, im, frames) ((void)0)
#endif

#define RADE_V2_TAP_MEMBERS RADE_V2_TAP_TX_MEMBER RADE_V2_TAP_RX_MEMBER
