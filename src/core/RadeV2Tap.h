#pragma once

// RadeV2Tap — per-stage WAV taps along the RAD2 audio chain, for bench work.
//
// The V2 counterpart of RADEEngine's `RADE_WAV_TAP` (which produced the
// rade_tap_A…F files the V1 EOO investigation was built on). Not carried over
// when RADEV2Engine was written, and its absence cost real time: the first OTA
// transmission carrying speech decoded as voice-shaped babble, and there was no
// way to ask "was there ever speech in what we sent?" without inventing one.
//
// ─── Compile-time, like V1 ─────────────────────────────────────────────────
//   -DRADE_V2_WAV_TAP            enable
//   -DRADE_V2_TAP_DIR=\"C:/tmp\" where files land (default ".")
// With the flag off every macro below expands to `((void)0)` and no member is
// declared, so the shipped build carries nothing — not a branch, not a buffer.
//
// The directory can also be overridden at run time with AETHER_RADE_V2_TAP_DIR,
// because moving output between runs should not need a rebuild.
//
// ─── Why it prints a summary as well as writing files ──────────────────────
// The question that motivated this — "is our transmitted level right?" — is
// answered by peak and RMS per stage, not by opening nine WAV files. A stage
// that is silent, clipped, or 20 dB down is visible in the summary line.
//
// ─── What it deliberately does NOT do ──────────────────────────────────────
// No timestamping or rotation: files are overwritten each over. Taps are for a
// bench session where you key once and look, and a directory full of
// tap_tx5_out_24k_mono_00017.wav is worse than one file you can point a decoder
// at. If you need to keep an over, copy it before the next one.

#ifdef RADE_V2_WAV_TAP

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
    // Accumulate interleaved float frames. `frames` counts FRAMES, not floats,
    // so a stereo block of 128 frames is 256 floats — the same convention the
    // engine's own buffers use, to remove one conversion from the call sites.
    void add(const char* name, int rateHz, int channels,
             const float* interleaved, int frames) {
        if (!interleaved || frames <= 0) return;
        Buf* b = find(name);
        if (!b) {
            m_bufs.push_back(Buf{QString::fromLatin1(name), rateHz, channels, {}, false});
            b = &m_bufs.last();
        }
        // Bounded, because the RX taps run for as long as the mode is selected
        // rather than for one over. Unbounded they would consume ~190 kB/s per
        // 24 kHz stereo tap and turn a long listening session into an
        // out-of-memory bug that only appears with taps enabled — the worst
        // possible place to put one.
        if (b->data.size() >= kMaxBytesPerTap) {
            if (!b->capped) {
                b->capped = true;
                qWarning("RADE_V2_TAP: %s hit the %lld MB cap — further audio dropped",
                         qPrintable(b->name), qint64(kMaxBytesPerTap >> 20));
            }
            return;
        }
        b->data.append(reinterpret_cast<const char*>(interleaved),
                       qsizetype(frames) * channels * qsizetype(sizeof(float)));
    }

    // Convenience for the complex stages: writes I and Q as a stereo pair, so
    // the file opens in any editor and the two components stay aligned.
    template <typename Complex>
    void addComplex(const char* name, int rateHz, const Complex* z, int frames) {
        if (!z || frames <= 0) return;
        QVector<float> il(qsizetype(frames) * 2);
        for (int i = 0; i < frames; ++i) {
            il[2 * i]     = float(z[i].real());
            il[2 * i + 1] = float(z[i].imag());
        }
        add(name, rateHz, 2, il.constData(), frames);
    }

    // Same, for the codec's own RADE_COMP (plain struct, .real/.imag fields).
    void addRadeComp(const char* name, int rateHz, const float* reals,
                     const float* imags, int frames) {
        if (frames <= 0) return;
        QVector<float> il(qsizetype(frames) * 2);
        for (int i = 0; i < frames; ++i) { il[2*i] = reals[i]; il[2*i+1] = imags[i]; }
        add(name, rateHz, 2, il.constData(), frames);
    }

    void reset() { m_bufs.clear(); }

    // Write every accumulated stage and print one summary line each. Called at
    // end of over so a tap file is exactly one transmission.
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
    struct Buf { QString name; int rate; int ch; QByteArray data; bool capped; };
    QVector<Buf> m_bufs;

    // ~175 s of 24 kHz stereo float, or ~350 s mono. Bench overs are seconds.
    static constexpr qsizetype kMaxBytesPerTap = qsizetype(32) << 20;

    Buf* find(const char* name) {
        for (Buf& b : m_bufs)
            if (b.name == QLatin1String(name)) return &b;
        return nullptr;
    }

    static void writeWav(const QString& path, const Buf& b) {
        const auto* s = reinterpret_cast<const float*>(b.data.constData());
        const qsizetype n = b.data.size() / qsizetype(sizeof(float));
        double peak = 0.0, sum = 0.0;
        for (qsizetype i = 0; i < n; ++i) {
            const double v = std::fabs(double(s[i]));
            if (v > peak) peak = v;
            sum += double(s[i]) * s[i];
        }
        const double rms = n ? std::sqrt(sum / double(n)) : 0.0;
        const double secs = b.ch && b.rate
                          ? double(n) / double(b.ch) / double(b.rate) : 0.0;

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            qWarning("RADE_V2_TAP: cannot write %s", qPrintable(path));
            return;
        }
        const quint32 dataSize = quint32(b.data.size());
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
        f.write(b.data);
        f.close();

        // The line that answers "is the level right?" without opening anything.
        // dBFS against 1.0 full scale, which is what every stage here uses.
        qDebug("RADE_V2_TAP: %-22s %5d Hz %dch %7.2f s  peak %7.4f (%+6.1f dBFS)  rms %7.4f (%+6.1f dBFS)  -> %s",
               qPrintable(b.name), b.rate, b.ch, secs,
               peak, peak > 0 ? 20.0 * std::log10(peak) : -999.0,
               rms,  rms  > 0 ? 20.0 * std::log10(rms)  : -999.0,
               qPrintable(path));
    }
};

}  // namespace AetherSDR

#  define RADE_V2_TAP_MEMBER          AetherSDR::RadeV2Tap m_tap;
#  define RADE_V2_TAP_RESET()         m_tap.reset()
#  define RADE_V2_TAP_FLUSH(prefix)   m_tap.flush(prefix)
#  define RADE_V2_TAP(name, rate, ch, ptr, frames) \
       m_tap.add(name, rate, ch, ptr, frames)
#  define RADE_V2_TAP_IQ(name, rate, re, im, frames) \
       m_tap.addRadeComp(name, rate, re, im, frames)

#else   // !RADE_V2_WAV_TAP — everything compiles away

#  define RADE_V2_TAP_MEMBER
#  define RADE_V2_TAP_RESET()                       ((void)0)
#  define RADE_V2_TAP_FLUSH(prefix)                 ((void)0)
#  define RADE_V2_TAP(name, rate, ch, ptr, frames)  ((void)0)
#  define RADE_V2_TAP_IQ(name, rate, re, im, frames) ((void)0)

#endif  // RADE_V2_WAV_TAP
