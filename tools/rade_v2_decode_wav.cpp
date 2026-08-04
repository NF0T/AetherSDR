// Decode a RADE V2 recording from a WAV file.
//
// The V2 analogue of third_party/radae/src/rade_demod_wav.c, which does the
// same job for V1 and is what the V1 OTA validation campaign was built on.
// Offline decode is the only way to check a transmission that was received
// somewhere else — a websdr, a second station, a recording made months ago —
// and it is the difference between "it looked right on a waterfall" and "it
// decodes".
//
// ─── What it reports, and why each line is there ───────────────────────────
//   sync        the receiver acquired. NOT evidence the sideband sense or the
//               tuning is right — §7.1b measured a conjugated signal acquiring
//               happily at −4.7 dB. Read it together with the SNR, never alone.
//   SNR         the discriminator. Good V2 copy is tens of dB; a decode that
//               syncs at a negative SNR is a signal the receiver is chewing on
//               without understanding.
//   EOO         the end-of-over frame, detected by correlating against the
//               known template (rade_rx_v2.c:311). This is the direct check on
//               §7.1 T9: if the transmit resampler ate the tail, the far end
//               sees no EOO here, and everything else can still look fine.
//   TEXT        callsigns recovered from the inline data channel (§9.2), each
//               one CRC-16 validated. The strongest single indicator in this
//               tool — see the note on the sweep below.
//   freq offset how far off the receiver thinks the signal is. V2 acquires
//               over ±31.25 Hz only, so a recording made on a receiver tuned
//               even slightly differently can fail to acquire for a reason
//               that has nothing to do with the transmission.
//
// ─── The frequency sweep, and its figure of merit ──────────────────────────
// Because of that ±31.25 Hz window, "did not decode" is ambiguous on a
// third-party recording: it can mean the transmission was bad, or it can mean
// the receiver was tuned 50 Hz off. --sweep resolves the ambiguity by shifting
// the input and retrying.
//
// **The sweep ranks offsets by validated TEXT frames first, and only falls
// back to sync count.** Sync count is actively misleading here and §10.13
// measured it being so: on a real off-air recording, an offset 30–140 Hz WRONG
// produced MORE synced blocks than the correct one — 489 against 394 — while
// mean SNR collapsed from 12.3 dB to ~7 dB. A CRC-16-validated callsign cannot
// be a false positive at better than 1/65536, so when text is present it is a
// far better oracle for "this offset is right" than anything else available.
//
// ─── Voice ─────────────────────────────────────────────────────────────────
// --out-wav runs the recovered features through FARGAN and writes 16 kHz mono.
// The warm-up sequence mirrors RADEV2Engine exactly rather than being
// re-derived: skipping it is a silent quality bug, audible only as a slightly
// wrong first fraction of a second that nobody attributes to the tool.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include <QString>

#include "core/RadeV2TextChannel.h"

extern "C" {
#include "rade_api.h"
#include "lpcnet.h"
#include "fargan.h"
}

using AetherSDR::RadeV2TextChannel;

namespace {

constexpr int kV2Flags = RADE_USE_C_ENCODER | RADE_USE_C_DECODER
                       | RADE_MODE_V2 | RADE_VERBOSE_0;

// Mirrors RADEV2Engine: FARGAN wants five frames of (zero) context before the
// first real synthesis.
constexpr int kFarganWarmupFrames = 5;
constexpr int kFeaturesPerFrame   = NB_TOTAL_FEATURES;

struct Wav {
    std::vector<float> samples;   // mono, normalised to ±1
    int rate = 0;
};

uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

// Chunk-walking rather than fixed offsets: WAV writers interleave LIST/fact
// chunks freely, and a parser that assumes `data` at byte 36 reads garbage
// from perfectly valid files.
bool readWav(const char* path, Wav& out, std::string& err) {
    FILE* f = fopen(path, "rb");
    if (!f) { err = "cannot open"; return false; }
    std::vector<uint8_t> buf;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 44) { fclose(f); err = "too short to be a WAV"; return false; }
    buf.resize(size_t(n));
    const size_t got = fread(buf.data(), 1, size_t(n), f);
    fclose(f);
    if (got != size_t(n)) { err = "short read"; return false; }

    if (memcmp(buf.data(), "RIFF", 4) != 0 || memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    int channels = 0, bits = 0, format = 0;
    size_t pos = 12;
    const uint8_t* data = nullptr;
    uint32_t dataBytes = 0;

    while (pos + 8 <= buf.size()) {
        const char* id = reinterpret_cast<const char*>(buf.data() + pos);
        const uint32_t sz = rd32(buf.data() + pos + 4);
        const size_t body = pos + 8;
        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= buf.size()) {
            format   = rd16(buf.data() + body);
            channels = rd16(buf.data() + body + 2);
            out.rate = int(rd32(buf.data() + body + 4));
            bits     = rd16(buf.data() + body + 14);
        } else if (memcmp(id, "data", 4) == 0) {
            data = buf.data() + body;
            dataBytes = uint32_t(std::min<size_t>(sz, buf.size() - body));
        }
        pos = body + sz + (sz & 1);   // chunks are word-aligned
    }

    if (!data || channels < 1 || out.rate <= 0) { err = "no usable fmt/data chunk"; return false; }
    if (format != 1 && format != 3) { err = "unsupported WAV format (want PCM or float)"; return false; }

    if (format == 1 && bits == 16) {
        const uint32_t frames = dataBytes / uint32_t(2 * channels);
        out.samples.resize(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            // Downmix by taking channel 0: a websdr recording is mono anyway,
            // and averaging channels of a stereo I/Q file would destroy it.
            const int16_t s = int16_t(rd16(data + size_t(i) * 2 * size_t(channels)));
            out.samples[i] = float(s) / 32768.0f;
        }
    } else if (format == 3 && bits == 32) {
        const uint32_t frames = dataBytes / uint32_t(4 * channels);
        out.samples.resize(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            float s;
            memcpy(&s, data + size_t(i) * 4 * size_t(channels), 4);
            out.samples[i] = s;
        }
    } else {
        err = "unsupported bit depth";
        return false;
    }
    return true;
}

bool writeWavMono16(const char* path, const std::vector<float>& x, int rate) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t dataBytes = uint32_t(x.size()) * 2;
    auto w32 = [&](uint32_t v) {
        const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        fwrite(b, 1, 4, f);
    };
    auto w16 = [&](uint16_t v) {
        const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        fwrite(b, 1, 2, f);
    };
    fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(1);
    w32(uint32_t(rate)); w32(uint32_t(rate) * 2); w16(2); w16(16);
    fwrite("data", 1, 4, f); w32(dataBytes);
    for (float s : x) {
        const long v = std::lround(double(s) * 32767.0);
        w16(uint16_t(int16_t(std::max(-32768L, std::min(32767L, v)))));
    }
    fclose(f);
    return true;
}

struct TextHit {
    double  t = 0.0;
    QString text;
    float   confidence = 0.0f;
    bool    inverted = false;
};

struct Result {
    int   syncedBlocks = 0;
    int   decodedFrames = 0;
    int   eooEvents = 0;
    float bestSnr = -999.0f;
    float snrSum = 0.0f;
    int   snrCount = 0;
    float lastFreqOffset = 0.0f;
    double firstSyncSec = -1.0;
    double firstEooSec = -1.0;

    int rawSymbols = 0;                       // inline-channel symbols seen
    std::vector<TextHit> texts;               // CRC-validated frames
    std::vector<float>   speech16k;           // only when --out-wav
    std::vector<std::pair<double, float>> symbolDump;   // only when --dump-symbols

    // Raw decoder-output feature vectors, in the layout the upstream
    // verification `loss.py` consumes: float32, reshaped (1, -1, 36).
    // Exists so a capture that will not decode can be compared against one
    // that does ON THE OFFICIAL METRIC, rather than by inferring from spectra
    // what the decoder is or is not receiving — which is how §10.19 went wrong.
    std::vector<float>   featuresRx;          // only when --features-rx
};

// Shift the whole spectrum by `shiftHz`. The input is a real passband
// recording, so this produces a complex signal whose positive-frequency image
// has moved — which is exactly what is wanted to hunt for a mistuned receiver.
// At shiftHz == 0 the imaginary part is zero, i.e. plain real input, which is
// how V1 has always fed this modem and which §7.1b measured costing 0.3 dB
// against a true analytic pair.
Result decode(const std::vector<float>& x, int rate, double shiftHz, bool verbose,
              bool wantSpeech, bool wantSymbolDump, bool wantFeatures) {
    Result r;
    struct rade* rade = rade_open(const_cast<char*>("dummy"), kV2Flags);
    if (!rade) return r;

    const int nFeat = rade_n_features_in_out(rade);
    std::vector<float> features(static_cast<size_t>(nFeat));
    std::vector<RADE_COMP> in(size_t(rade_nin_max(rade)));

    RadeV2TextChannel textChannel;

    FARGANState fargan;
    bool farganWarmedUp = false;
    std::vector<float> featAccum;
    if (wantSpeech) fargan_init(&fargan);

    size_t pos = 0;
    const double w = -2.0 * M_PI * shiftHz / double(rate);
    bool wasSynced = false;

    while (pos + size_t(rade_nin(rade)) <= x.size()) {
        // Re-read EVERY iteration. rade_nin() changes as the receiver tracks
        // timing, and hoisting it walks the input off the symbol boundary —
        // the receiver then never acquires, with nothing to say why.
        const int nin = rade_nin(rade);
        for (int k = 0; k < nin; ++k) {
            const double t = double(pos + size_t(k));
            const float s = x[pos + size_t(k)];
            if (shiftHz == 0.0) {
                in[size_t(k)].real = s;
                in[size_t(k)].imag = 0.0f;
            } else {
                in[size_t(k)].real = float(s * std::cos(w * t));
                in[size_t(k)].imag = float(s * std::sin(w * t));
            }
        }
        const double tSec = double(pos) / double(rate);
        pos += size_t(nin);

        int hasEoo = 0;
        const int nOut = rade_rx(rade, features.data(), &hasEoo, nullptr, in.data());
        if (nOut > 0) {
            r.decodedFrames++;

            // §7.1 D2 — the data symbol is valid only after a step that
            // produced features, and exactly once per such step.
            // Whole vectors only. rade_rx returns nOut floats, a multiple of
            // the 36-float frame; appending anything else would silently
            // misalign every later frame in the file.
            if (wantFeatures)
                r.featuresRx.insert(r.featuresRx.end(),
                                    features.begin(), features.begin() + nOut);

            const float symbol = rade_rx_get_data_symbol(rade);
            r.rawSymbols++;
            if (wantSymbolDump) r.symbolDump.emplace_back(tSec, symbol);
            if (const auto d = textChannel.pushSymbol(symbol)) {
                r.texts.push_back(TextHit{tSec, d->text, d->confidence, d->inverted});
                if (verbose)
                    printf("  %7.2f s  TEXT \"%s\"  conf %.2f%s\n", tSec,
                           d->text.toUtf8().constData(), double(d->confidence),
                           d->inverted ? "  [inverted]" : "");
            }

            if (wantSpeech) {
                featAccum.insert(featAccum.end(), features.begin(), features.begin() + nOut);
                while (int(featAccum.size()) >= kFeaturesPerFrame) {
                    if (!farganWarmedUp) {
                        float zeros[2 * LPCNET_FRAME_SIZE] = {0};
                        float warmup[kFarganWarmupFrames * kFeaturesPerFrame] = {0};
                        fargan_cont(&fargan, zeros, warmup);
                        farganWarmedUp = true;
                    }
                    float pcm[LPCNET_FRAME_SIZE];
                    fargan_synthesize(&fargan, pcm, featAccum.data());
                    r.speech16k.insert(r.speech16k.end(), pcm, pcm + LPCNET_FRAME_SIZE);
                    featAccum.erase(featAccum.begin(), featAccum.begin() + kFeaturesPerFrame);
                }
            }
        }

        const bool synced = rade_sync(rade) != 0;
        if (synced) {
            r.syncedBlocks++;
            const float snr = float(rade_snrdB_3k_est(rade));
            if (snr > r.bestSnr) r.bestSnr = snr;
            r.snrSum += snr;
            r.snrCount++;
            r.lastFreqOffset = float(rade_freq_offset(rade));
            if (!wasSynced) {
                if (r.firstSyncSec < 0) r.firstSyncSec = tSec;
                if (verbose)
                    printf("  %7.2f s  SYNC acquired   snr %6.1f dB  foff %6.1f Hz\n",
                           tSec, snr, r.lastFreqOffset);
            }
        } else if (wasSynced && verbose) {
            printf("  %7.2f s  sync lost\n", tSec);
        }
        wasSynced = synced;

        if (hasEoo) {
            r.eooEvents++;
            if (r.firstEooSec < 0) r.firstEooSec = tSec;
            if (verbose) printf("  %7.2f s  *** END-OF-OVER detected ***\n", tSec);
        }
    }

    rade_close(rade);
    return r;
}

void report(const char* label, const Result& r) {
    const float meanSnr = r.snrCount ? r.snrSum / float(r.snrCount) : 0.0f;
    printf("  %-14s sync %5d blk  frames %5d  EOO %2d  TEXT %2d  SNR best %6.1f mean %6.1f  foff %6.1f Hz\n",
           label, r.syncedBlocks, r.decodedFrames, r.eooEvents, int(r.texts.size()),
           r.snrCount ? r.bestSnr : 0.0f, meanSnr, r.lastFreqOffset);
}

// Distinct callsigns, in first-seen order, with how many copies validated.
void reportTexts(const Result& r) {
    if (r.rawSymbols == 0) {
        printf("  inline data: no symbols (nothing decoded to carry them)\n");
        return;
    }
    printf("  inline data: %d symbols, %d frames validated\n",
           r.rawSymbols, int(r.texts.size()));
    if (r.texts.empty()) {
        // Worth distinguishing loudly: symbols arrived but nothing framed.
        // Either the sender used different framing (very likely, if it was not
        // us), or the threshold is wrong — which is what --dump-symbols is for.
        printf("      symbols present but NO frame validated — different framing,\n"
               "      or the sync threshold needs re-deriving (--dump-symbols)\n");
        return;
    }
    std::vector<std::pair<QString, int>> seen;
    for (const auto& h : r.texts) {
        auto it = std::find_if(seen.begin(), seen.end(),
                               [&](const auto& p) { return p.first == h.text; });
        if (it == seen.end()) seen.emplace_back(h.text, 1);
        else it->second++;
    }
    for (const auto& [text, count] : seen)
        printf("      \"%s\"  ×%d\n", text.toUtf8().constData(), count);
    printf("      first at %.2f s, confidence %.2f%s\n",
           r.texts.front().t, double(r.texts.front().confidence),
           r.texts.front().inverted ? " (polarity inverted)" : "");
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    const char* outWav = nullptr;
    const char* featPath = nullptr;
    const char* dumpPath = nullptr;
    double shift = 0.0;
    bool sweep = false, verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sweep") sweep = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "--shift" && i + 1 < argc) shift = atof(argv[++i]);
        else if (a == "--out-wav" && i + 1 < argc) outWav = argv[++i];
        else if (a == "--features-rx" && i + 1 < argc) featPath = argv[++i];
        else if (a == "--dump-symbols" && i + 1 < argc) dumpPath = argv[++i];
        else if (!path) path = argv[i];
    }
    if (!path) {
        fprintf(stderr,
                "usage: rade_v2_decode_wav <file.wav> [options]\n"
                "  --shift Hz          shift the input before decoding\n"
                "  --sweep             hunt a mistuned receiver (V2 acquires over\n"
                "                      +/-31.25 Hz only); ranks offsets by validated\n"
                "                      TEXT frames, then by sync count\n"
                "  --out-wav <f>       write decoded speech, 16 kHz mono\n"
                "  --features-rx <f>   write decoder-output feature vectors as\n"
                "                      float32 (1,-1,36), for the upstream loss.py\n"
                "  --dump-symbols <f>  write raw inline-channel soft symbols as CSV\n"
                "  -v                  per-event timeline\n");
        return 2;
    }

    Wav wav;
    std::string err;
    if (!readWav(path, wav, err)) {
        fprintf(stderr, "%s: %s\n", path, err.c_str());
        return 1;
    }

    printf("%s\n", path);
    printf("  %zu samples at %d Hz = %.2f s\n",
           wav.samples.size(), wav.rate,
           double(wav.samples.size()) / double(wav.rate));

    if (wav.rate != RADE_MODEM_SAMPLE_RATE) {
        fprintf(stderr, "  ERROR: need %d Hz input; this file is %d Hz.\n",
                RADE_MODEM_SAMPLE_RATE, wav.rate);
        return 1;
    }

    double peak = 0.0, rms = 0.0;
    for (float s : wav.samples) { peak = std::max(peak, std::fabs(double(s))); rms += double(s) * s; }
    rms = wav.samples.empty() ? 0.0 : std::sqrt(rms / double(wav.samples.size()));
    printf("  peak %.3f  rms %.4f\n\n", peak, rms);

    if (!sweep) {
        if (verbose) printf("  timeline:\n");
        const Result r = decode(wav.samples, wav.rate, shift, verbose,
                                outWav != nullptr, dumpPath != nullptr,
                                featPath != nullptr);
        printf("\n");
        report(shift == 0.0 ? "as recorded" : "shifted", r);
        if (r.firstSyncSec >= 0) printf("  first sync at %.2f s\n", r.firstSyncSec);
        if (r.firstEooSec >= 0)  printf("  first EOO  at %.2f s\n", r.firstEooSec);
        reportTexts(r);

        if (featPath) {
            FILE* f = fopen(featPath, "wb");
            if (f) {
                fwrite(r.featuresRx.data(), sizeof(float), r.featuresRx.size(), f);
                fclose(f);
                printf("  wrote %s — %zu floats = %zu frames of 36\n", featPath,
                       r.featuresRx.size(), r.featuresRx.size() / 36);
            } else {
                fprintf(stderr, "  ERROR: cannot write %s\n", featPath);
            }
        }

        if (outWav) {
            if (writeWavMono16(outWav, r.speech16k, RADE_SPEECH_SAMPLE_RATE))
                printf("  wrote %s — %zu samples, %.2f s at %d Hz\n", outWav,
                       r.speech16k.size(),
                       double(r.speech16k.size()) / double(RADE_SPEECH_SAMPLE_RATE),
                       RADE_SPEECH_SAMPLE_RATE);
            else
                fprintf(stderr, "  ERROR: cannot write %s\n", outWav);
        }
        if (dumpPath) {
            FILE* f = fopen(dumpPath, "wb");
            if (f) {
                fprintf(f, "time_s,soft\n");
                for (const auto& [t, s] : r.symbolDump) fprintf(f, "%.4f,%.6f\n", t, double(s));
                fclose(f);
                printf("  wrote %s — %zu raw symbols\n", dumpPath, r.symbolDump.size());
            } else {
                fprintf(stderr, "  ERROR: cannot write %s\n", dumpPath);
            }
        }
        return r.syncedBlocks > 0 ? 0 : 3;
    }

    printf("  sweeping frequency offset (V2 acquires over +/-31.25 Hz only)\n");
    printf("  ranked by validated TEXT frames, then sync — §10.13 measured sync\n"
           "  count peaking at the WRONG offset (489 vs 394), so it cannot rank alone\n\n");
    double bestShift = 0.0;
    Result best;
    for (double s = -200.0; s <= 200.0; s += 10.0) {
        const Result r = decode(wav.samples, wav.rate, s, false, false, false, false);
        char label[32];
        snprintf(label, sizeof(label), "%+7.1f Hz", s);
        if (r.syncedBlocks > 0) report(label, r);
        // Text first, sync only as a tie-break: a CRC-16-validated callsign is
        // better than 1/65536 to be a false positive, while a sync count is
        // demonstrably not evidence of anything on its own.
        const auto rank  = std::make_tuple(r.texts.size(), r.syncedBlocks);
        const auto bestR = std::make_tuple(best.texts.size(), best.syncedBlocks);
        if (rank > bestR) { best = r; bestShift = s; }
    }
    printf("\n");
    if (best.syncedBlocks > 0) {
        printf("  BEST: shift %+.1f Hz%s\n", bestShift,
               best.texts.empty() ? "  (by sync count only — no text validated anywhere)"
                                  : "  (by validated text)");
        report("best", best);
        reportTexts(best);
    } else {
        printf("  no acquisition at any offset in +/-200 Hz\n");
    }
    return best.syncedBlocks > 0 ? 0 : 3;
}
