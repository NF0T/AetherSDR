// rade_v2_loopback - can OUR chain reproduce speech at all, with no radio?
//
//   speech16k.wav -> lpcnet analysis -> rade_tx -> rade_rx -> FARGAN -> out.wav
//
// By default nothing sits between rade_tx and rade_rx: no resampler, no RF, no
// path. That is the point. If speech comes out garbled here, the fault is in
// our codec wiring; if it comes out clean, the wiring is fine and an OTA
// failure is somewhere we cannot reach from a bench.
//
// The FARGAN warm-up and feature accumulation are copied from RADEV2Engine
// deliberately, NOT re-derived, so a bug in the shipped path reproduces here.
//
// Written 2026-08-02 after the first OTA transmission carrying real speech
// decoded as voice-shaped babble. It cleared, in one run, the things that
// looked most likely: the FARGAN warm-up (written that day, never exposed to
// real speech), the section 7.1 T9 wide transition band (chosen for EOO
// detectability, never validated for signal quality - it costs 0.1 dB), and
// the offline decoder's own new --out-wav path.
//
// Judge the output by ENVELOPE CORRELATION against the input rather than by
// ear: r ~ 0.95 is the same speech, r < 0.15 is unrelated. The codec adds
// 200-240 ms, so any comparison needs a lag search.
//
//   --tx-chain matters more than it looks. rade_rx normally gets an analytic
//   pair; a websdr recording is REAL audio, so decoding one throws away image
//   rejection. Measured: complex input holds r ~ 0.95 down to an estimated
//   6.7 dB, while the real-only path falls off a cliff between an estimated
//   3.7 dB (r = 0.86) and 0.5 dB (r = 0.11). A websdr decode therefore
//   understates the transmitter by several dB.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <QByteArray>
#include "core/Resampler.h"
#include "core/RadeV2RxAgc.h"

extern "C" {
#include "rade_api.h"
#include "lpcnet.h"
#include "fargan.h"
}

static bool readWav16(const char* p, std::vector<short>& out, int& rate) {
    FILE* f = fopen(p, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b; b.resize(size_t(n));
    if (fread(b.data(),1,size_t(n),f)!=size_t(n)) { fclose(f); return false; }
    fclose(f);
    auto r32=[&](size_t o){ return uint32_t(b[o])|(uint32_t(b[o+1])<<8)|(uint32_t(b[o+2])<<16)|(uint32_t(b[o+3])<<24); };
    auto r16=[&](size_t o){ return uint16_t(b[o])|(uint16_t(b[o+1])<<8); };
    size_t pos=12; int ch=1, bits=16; const uint8_t* d=nullptr; uint32_t db=0;
    while (pos+8<=b.size()) {
        uint32_t sz=r32(pos+4); size_t body=pos+8;
        if (!memcmp(&b[pos],"fmt ",4)) { ch=r16(body+2); rate=int(r32(body+4)); bits=r16(body+14); }
        else if (!memcmp(&b[pos],"data",4)) { d=&b[body]; db=uint32_t(std::min<size_t>(sz,b.size()-body)); }
        pos=body+sz+(sz&1);
    }
    if(!d||bits!=16) return false;
    uint32_t fr=db/uint32_t(2*ch); out.resize(fr);
    for(uint32_t i=0;i<fr;i++) out[i]=short(r16(size_t(d-b.data())+size_t(i)*2*size_t(ch)));
    return true;
}

static void writeWav16(const char* p, const std::vector<short>& x, int rate) {
    FILE* f=fopen(p,"wb"); if(!f) return;
    uint32_t db=uint32_t(x.size())*2;
    auto w32=[&](uint32_t v){ uint8_t b[4]={uint8_t(v),uint8_t(v>>8),uint8_t(v>>16),uint8_t(v>>24)}; fwrite(b,1,4,f); };
    auto w16=[&](uint16_t v){ uint8_t b[2]={uint8_t(v),uint8_t(v>>8)}; fwrite(b,1,2,f); };
    fwrite("RIFF",1,4,f); w32(36+db); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); w32(16); w16(1); w16(1); w32(uint32_t(rate)); w32(uint32_t(rate)*2); w16(2); w16(16);
    fwrite("data",1,4,f); w32(db);
    fwrite(x.data(),2,x.size(),f); fclose(f);
}

int main(int argc, char** argv) {
    const char* inPath=nullptr; const char* outPath=nullptr; const char* refPath=nullptr;
    const char* featTxPath=nullptr; const char* featRxPath=nullptr;
    bool txChain=false; double snrDb=-1000.0; bool rxAgc=false;
    for (int i=1;i<argc;++i) {
        const std::string a=argv[i];
        if (a=="--tx-chain") txChain=true;
        else if (a=="--snr" && i+1<argc) snrDb=atof(argv[++i]);
        else if (a=="--rx-agc") rxAgc=true;
        else if (a=="--vocoder-ref" && i+1<argc) refPath=argv[++i];
        else if (a=="--features-tx" && i+1<argc) featTxPath=argv[++i];
        else if (a=="--features-rx" && i+1<argc) featRxPath=argv[++i];
        else if (!inPath) inPath=argv[i];
        else if (!outPath) outPath=argv[i];
    }
    if (!inPath || !outPath) {
        fprintf(stderr,
          "usage: rade_v2_loopback <speech16k.wav> <out.wav> [options]\n"
          "  --tx-chain          insert OUR real TX chain: .real, 8->24k at\n"
          "                      ReqTransBand=45, back to 8k (models the radio path)\n"
          "  --snr <dB>          inject AWGN at this wideband SNR\n"
          "  --rx-agc            run the live path's RX AGC (RFC 10.20). OFF by\n"
          "                      default: Level 1 declares no AGC in the path\n"
          "  --vocoder-ref <f>   also write FARGAN's output from the ORIGINAL\n"
          "                      features, with no modem at all\n"
          "  --features-tx <f>   raw float32 feature vectors AT THE ENCODER INPUT\n"
          "  --features-rx <f>   raw float32 feature vectors AT THE DECODER OUTPUT\n"
          "\n"
          "The two --features files are what the RADE integration verification\n"
          "procedure measures (doc/verification/verification_procedure.md in\n"
          "drowe67/radae@dr-radev2). Level 1 is a software loopback like this one:\n"
          "\n"
          "  rade_v2_loopback wav/all.wav out.wav \\\n"
          "      --features-tx features_tx.f32 --features-rx features_rx.f32\n"
          "  python3 loss.py features_tx.f32 features_rx.f32 \\\n"
          "      --clip_start 100 --clip_end 300\n"
          "\n"
          "V2 software-loopback baseline is loss ~0.082; a pass is within 10%%.\n"
          "Feed their 16 kHz wav/all.wav directly so no resampler is in the path\n"
          "and the signal-path declaration on the report form is honestly true.\n");
        return 2;
    }
    std::vector<short> in; int rate=0;
    if (!readWav16(inPath, in, rate)) { fprintf(stderr,"cannot read %s\n", inPath); return 1; }
    if (rate != 16000) { fprintf(stderr,"need 16 kHz input, got %d\n", rate); return 1; }
    printf("in: %zu samples @ %d Hz = %.2f s\n", in.size(), rate, double(in.size())/rate);

    rade_initialize();
    struct rade* tx = rade_open(const_cast<char*>("dummy"),
        RADE_USE_C_ENCODER|RADE_USE_C_DECODER|RADE_MODE_V2|RADE_VERBOSE_0);
    struct rade* rx = rade_open(const_cast<char*>("dummy"),
        RADE_USE_C_ENCODER|RADE_USE_C_DECODER|RADE_MODE_V2|RADE_VERBOSE_0);
    LPCNetEncState* enc = lpcnet_encoder_create();
    FARGANState fargan; fargan_init(&fargan); bool warmed=false;

    const int nFeat = rade_n_features_in_out(tx);      // 144 = 4 x 36
    const int nTxOut = rade_n_tx_out(tx);
    printf("nFeat=%d nTxOut=%d nin=%d\n", nFeat, nTxOut, rade_nin(rx));

    // 1) analysis: 16 kHz speech -> 36 features per 10 ms frame
    std::vector<float> feats;
    for (size_t i=0; i+LPCNET_FRAME_SIZE<=in.size(); i+=LPCNET_FRAME_SIZE) {
        float f[NB_TOTAL_FEATURES];
        lpcnet_compute_single_frame_features(enc, &in[i], f, 0);
        feats.insert(feats.end(), f, f+NB_TOTAL_FEATURES);
    }
    printf("analysis: %zu feature frames (%.2f s)\n",
           feats.size()/NB_TOTAL_FEATURES, double(feats.size()/NB_TOTAL_FEATURES)*0.01);

    // Feature vectors for the RADE integration verification procedure. 36
    // floats per vector (NB_TOTAL_FEATURES); loss.py reads them raw and uses
    // the first 20. Held in memory rather than streamed because a 56 s
    // wav/all.wav run is only ~800 kB per side.
    std::vector<float> featTx, featRx;

    // 2) modulate, 3) demodulate with NOTHING in between
    std::vector<RADE_COMP> modem;
    std::vector<RADE_COMP> txOut; txOut.resize(size_t(nTxOut));
    for (size_t p=0; p+size_t(nFeat)<=feats.size(); p+=size_t(nFeat)) {
        // Encoder input, exactly as handed to rade_tx(). Captured here rather
        // than from `feats` wholesale because the loop consumes whole nFeat
        // blocks and may leave a partial one at the end — loss.py aligns the
        // two streams itself, but only if each is a whole number of 36-float
        // vectors.
        if (featTxPath) featTx.insert(featTx.end(), &feats[p], &feats[p] + nFeat);
        int n = rade_tx(tx, txOut.data(), &feats[p]);
        modem.insert(modem.end(), txOut.begin(), txOut.begin()+n);
    }
    printf("modem: %zu complex samples (%.2f s at 8 kHz)\n", modem.size(), double(modem.size())/8000.0);

    // Optional: run the REAL TX chain the radio sees, which the direct path
    // above skips entirely:
    //   take .real (7.1 E2) -> 8->24 kHz at ReqTransBand=45 (7.1 T9)
    //   -> 24->8 kHz (what the websdr recording gives us back)
    // If SNR collapses here, the wide transition band chosen for the EOO is
    // costing signal quality, and T9's tradeoff needs revisiting.
    if (txChain) {
        std::vector<float> real8k;
        real8k.reserve(modem.size());
        for (auto& c : modem) real8k.push_back(c.real);

        AetherSDR::Resampler up(8000, 24000, 4096, 45.0);
        AetherSDR::Resampler down(24000, 8000);
        const QByteArray b24 = up.process(real8k.data(), int(real8k.size()));
        const auto* p24 = reinterpret_cast<const float*>(b24.constData());
        const int n24 = int(b24.size()/qsizetype(sizeof(float)));
        const QByteArray b8 = down.process(const_cast<float*>(p24), n24);
        const auto* p8 = reinterpret_cast<const float*>(b8.constData());
        const int n8 = int(b8.size()/qsizetype(sizeof(float)));
        printf("TXCHAIN: %d -> %d (24k) -> %d samples; feeding REAL-only to rx\n",
               int(real8k.size()), n24, n8);
        modem.clear();
        for (int k=0;k<n8;k++) { RADE_COMP c; c.real=p8[k]; c.imag=0.0f; modem.push_back(c); }
    }


    // Inject AWGN at a target SNR (dB) measured in the 3 kHz sense the
    // receiver's own estimator uses, so the number is comparable to what
    // rade_snrdB_3k_est reports on air.
    if (snrDb > -900.0) {
        const double target = snrDb;
        double p=0; for (auto&c:modem) p += double(c.real)*c.real + double(c.imag)*c.imag;
        p /= double(modem.size());
        double npow = p / pow(10.0, target/10.0);
        double sigma = sqrt(npow/2.0);
        unsigned int seed = 12345;
        auto gauss=[&](){ double u1,u2; seed=seed*1103515245u+12345u; u1=((seed>>8)&0xFFFFFF)/16777216.0+1e-12;
                          seed=seed*1103515245u+12345u; u2=((seed>>8)&0xFFFFFF)/16777216.0;
                          return sqrt(-2*log(u1))*cos(6.283185307*u2); };
        for (auto&c:modem){ c.real += float(sigma*gauss()); c.imag += float(sigma*gauss()); }
        printf("injected AWGN for target SNR %.1f dB\n", target);
    }

    std::vector<float> featOut, featAccum; featOut.resize(size_t(nFeat));
    (void)featTx;   // filled above when --features-tx was given
    std::vector<short> out;
    size_t pos=0; int steps=0, decoded=0, syncBlocks=0; float snrSum=0; int snrN=0;
    // §10.20 — the RX AGC, OFF BY DEFAULT AND DELIBERATELY SO.
    //
    // The live path (RADEV2Engine) and the real-capture decode tool always run
    // it, because the level arriving from a radio is unknown and rade_rx() is
    // level-dependent. Level 1 is the opposite case: a file at a known level,
    // and the procedure's signal-path declaration explicitly bans "additional
    // signal processing (AGC, noise gate, resampler, EQ, compression)" in the
    // verification path. Upstream draws the same line — `--agc` lives in
    // radev2_rx_wav.sh, not in the loopback that defines the 0.082 baseline.
    //
    // It is not free either: recomputing gain per block puts small steps into
    // a signal that did not need them. Measured on wav/all.wav, enabling it
    // moves Level 1 from 0.081 (PASS) to 0.092 (FAIL). That is the right
    // trade on a real receive path — the same capture goes from 0 to 5
    // callsign frames — and the wrong one here.
    std::vector<RADE_COMP> rxBlk;
    static_assert(sizeof(RADE_COMP) == 2 * sizeof(float),
                  "RADE_COMP must be two packed floats to alias as interleaved I/Q");

    while (pos + size_t(rade_nin(rx)) <= modem.size()) {
        int nin = rade_nin(rx);
        int hasEoo=0;
        RADE_COMP* rxPtr = &modem[pos];
        if (rxAgc) {
            rxBlk.assign(modem.begin() + qsizetype(pos),
                         modem.begin() + qsizetype(pos) + nin);
            const float agc = AetherSDR::RadeV2RxAgc::blockGainInterleaved(
                reinterpret_cast<const float*>(rxBlk.data()), nin);
            for (int k = 0; k < nin; ++k) { rxBlk[size_t(k)].real *= agc;
                                            rxBlk[size_t(k)].imag *= agc; }
            rxPtr = rxBlk.data();
        }
        int nOut = rade_rx(rx, featOut.data(), &hasEoo, nullptr, rxPtr);
        pos += size_t(nin); steps++;
        if (rade_sync(rx)) { syncBlocks++; snrSum += float(rade_snrdB_3k_est(rx)); snrN++; }
        if (nOut > 0) {
            decoded++;
            // Decoder output, before FARGAN. This is the other half of what
            // loss.py compares — the vocoder is deliberately NOT in the loop.
            if (featRxPath) featRx.insert(featRx.end(), featOut.begin(), featOut.begin()+nOut);
            featAccum.insert(featAccum.end(), featOut.begin(), featOut.begin()+nOut);
            while (int(featAccum.size()) >= NB_TOTAL_FEATURES) {
                if (!warmed) {
                    float z[2*LPCNET_FRAME_SIZE]={0}, w[5*NB_TOTAL_FEATURES]={0};
                    fargan_cont(&fargan, z, w); warmed=true;
                }
                float pcm[LPCNET_FRAME_SIZE];
                fargan_synthesize(&fargan, pcm, featAccum.data());
                for (int k=0;k<LPCNET_FRAME_SIZE;k++) {
                    float v = pcm[k]*32767.0f;
                    out.push_back(short(v>32767?32767:(v<-32768?-32768:v)));
                }
                featAccum.erase(featAccum.begin(), featAccum.begin()+NB_TOTAL_FEATURES);
            }
        }
    }
    printf("rx: %d steps, %d decoded, %d synced, SNR %.1f dB\n",
           steps, decoded, syncBlocks, snrN? snrSum/snrN : 0.0f);
    printf("out: %zu samples = %.2f s\n", out.size(), double(out.size())/16000.0);
    writeWav16(outPath, out, 16000);

    auto writeF32 = [](const char* path, const std::vector<float>& v, const char* what) {
        if (!path) return;
        FILE* f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
        fwrite(v.data(), sizeof(float), v.size(), f);
        fclose(f);
        printf("%s: %zu vectors (%zu floats) -> %s\n",
               what, v.size() / 36, v.size(), path);
        if (v.size() % 36)
            fprintf(stderr, "  WARNING: %zu floats is not a whole number of 36-float "
                            "vectors — loss.py will misread this\n", v.size());
    };
    writeF32(featTxPath, featTx, "features TX (encoder input)");
    writeF32(featRxPath, featRx, "features RX (decoder output)");
    if (featTxPath && featRxPath) {
        printf("\nverification: python3 loss.py %s %s --clip_start 100 --clip_end 300\n",
               featTxPath, featRxPath);
        printf("  V2 software-loopback baseline ~0.082, pass within 10%%\n");
    }

    // Also write what the vocoder does with the ORIGINAL features, no modem at
    // all. This separates "the vocoder/analysis is wrong" from "the modem is".
    if (refPath) {
        FARGANState f2; fargan_init(&f2);
        float z[2*LPCNET_FRAME_SIZE]={0}, w[5*NB_TOTAL_FEATURES]={0};
        fargan_cont(&f2, z, w);
        std::vector<short> ref;
        for (size_t p=0; p+NB_TOTAL_FEATURES<=feats.size(); p+=NB_TOTAL_FEATURES) {
            float pcm[LPCNET_FRAME_SIZE];
            fargan_synthesize(&f2, pcm, &feats[p]);
            for (int k=0;k<LPCNET_FRAME_SIZE;k++) {
                float v=pcm[k]*32767.0f;
                ref.push_back(short(v>32767?32767:(v<-32768?-32768:v)));
            }
        }
        writeWav16(refPath, ref, 16000);
        printf("vocoder-only reference: %zu samples -> %s\n", ref.size(), refPath);
    }
    lpcnet_encoder_destroy(enc); rade_close(tx); rade_close(rx); rade_finalize();
    return 0;
}
