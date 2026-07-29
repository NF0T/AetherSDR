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
//   freq offset how far off the receiver thinks the signal is. V2 acquires
//               over ±31.25 Hz only, so a recording made on a receiver tuned
//               even slightly differently can fail to acquire for a reason
//               that has nothing to do with the transmission.
//
// ─── The frequency sweep ───────────────────────────────────────────────────
// Because of that ±31.25 Hz window, "did not decode" is ambiguous on a
// third-party recording: it can mean the transmission was bad, or it can mean
// the receiver was tuned 50 Hz off. --sweep resolves the ambiguity by shifting
// the input and retrying, turning a null result into either "bad transmission"
// or "good transmission, receiver offset by N Hz".

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "rade_api.h"
}

namespace {

constexpr int kV2Flags = RADE_USE_C_ENCODER | RADE_USE_C_DECODER
                       | RADE_MODE_V2 | RADE_VERBOSE_0;

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
};

// Shift the whole spectrum by `shiftHz`. The input is a real passband
// recording, so this produces a complex signal whose positive-frequency image
// has moved — which is exactly what is wanted to hunt for a mistuned receiver.
// At shiftHz == 0 the imaginary part is zero, i.e. plain real input, which is
// how V1 has always fed this modem and which §7.1b measured costing 0.3 dB
// against a true analytic pair.
Result decode(const std::vector<float>& x, int rate, double shiftHz, bool verbose) {
    Result r;
    struct rade* rade = rade_open(const_cast<char*>("dummy"), kV2Flags);
    if (!rade) return r;

    const int nFeat = rade_n_features_in_out(rade);
    std::vector<float> features(static_cast<size_t>(nFeat));
    std::vector<RADE_COMP> in(size_t(rade_nin_max(rade)));

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
        if (nOut > 0) r.decodedFrames++;

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
    printf("  %-14s sync %5d blk  frames %5d  EOO %2d  SNR best %6.1f mean %6.1f  foff %6.1f Hz\n",
           label, r.syncedBlocks, r.decodedFrames, r.eooEvents,
           r.snrCount ? r.bestSnr : 0.0f, meanSnr, r.lastFreqOffset);
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    double shift = 0.0;
    bool sweep = false, verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sweep") sweep = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "--shift" && i + 1 < argc) shift = atof(argv[++i]);
        else if (!path) path = argv[i];
    }
    if (!path) {
        fprintf(stderr,
                "usage: rade_v2_decode_wav <file.wav> [--shift Hz] [--sweep] [-v]\n"
                "  Decodes a RADE V2 recording. --sweep hunts a mistuned receiver\n"
                "  (V2 acquires over +/-31.25 Hz only).\n");
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
        const Result r = decode(wav.samples, wav.rate, shift, verbose);
        printf("\n");
        report(shift == 0.0 ? "as recorded" : "shifted", r);
        if (r.firstSyncSec >= 0) printf("  first sync at %.2f s\n", r.firstSyncSec);
        if (r.firstEooSec >= 0)  printf("  first EOO  at %.2f s\n", r.firstEooSec);
        return r.syncedBlocks > 0 ? 0 : 3;
    }

    printf("  sweeping frequency offset (V2 acquires over +/-31.25 Hz only)\n\n");
    double bestShift = 0.0;
    Result best;
    for (double s = -200.0; s <= 200.0; s += 10.0) {
        const Result r = decode(wav.samples, wav.rate, s, false);
        char label[32];
        snprintf(label, sizeof(label), "%+7.1f Hz", s);
        if (r.syncedBlocks > 0) report(label, r);
        if (r.syncedBlocks > best.syncedBlocks) { best = r; bestShift = s; }
    }
    printf("\n");
    if (best.syncedBlocks > 0) {
        printf("  BEST: shift %+.1f Hz\n", bestShift);
        report("best", best);
    } else {
        printf("  no acquisition at any offset in +/-200 Hz\n");
    }
    return best.syncedBlocks > 0 ? 0 : 3;
}
