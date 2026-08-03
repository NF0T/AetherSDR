#include "core/RADEV2Engine.h"

#ifdef AETHER_ENABLE_RADE_V2

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QLoggingCategory>

#include "core/Resampler.h"

#ifdef HAVE_RADE_V2
extern "C" {
#include "rade_api.h"
#include "lpcnet.h"
#include "fargan.h"
}
#endif

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcRadeV2Codec, "aether.radev2.codec")

#ifdef HAVE_RADE_V2

namespace {

// V2's flag set. RADE_MODE_V2 (0x10) selects the V2 encoder/decoder inside the
// unified library; without it the same call opens a V1 codec that links, runs,
// and interoperates with nobody.
constexpr int kOpenFlags = RADE_USE_C_ENCODER | RADE_USE_C_DECODER
                         | RADE_MODE_V2 | RADE_VERBOSE_0;

// One lpcnet analysis frame: 36 floats describing 10 ms of 16 kHz speech.
// rade_n_features_in_out() is 144 for V2 — four of these, ≈40 ms per step.
constexpr int kFeaturesPerFrame = NB_TOTAL_FEATURES;

// FARGAN's warm-up wants five frames' worth of (zero) context before the first
// real synthesis, exactly as V1 does it.
constexpr int kFarganWarmupFrames = 5;

int msAt24k(int samples) { return samples * 1000 / RADEV2Engine::kWaveformRateHz; }

}  // namespace

RADEV2Engine::RADEV2Engine(QObject* parent) : QObject(parent) {}

RADEV2Engine::~RADEV2Engine() { stop(); }

bool RADEV2Engine::start() {
    if (m_rade) return true;

    rade_initialize();

    // `model_file` is ignored in V2 — the weights are compiled in from
    // rade_enc_v2_data.c / rade_dec_v2_data.c. Passing a path that does not
    // exist is correct, not an oversight.
    m_rade = rade_open(const_cast<char*>("dummy"), kOpenFlags);
    if (!m_rade) {
        qCWarning(lcRadeV2Codec) << "rade_open() failed for RADE_MODE_V2";
        rade_finalize();
        return false;
    }

    m_lpcnetEnc = lpcnet_encoder_create();
    if (!m_lpcnetEnc) {
        qCWarning(lcRadeV2Codec) << "lpcnet_encoder_create() failed";
        rade_close(m_rade);
        m_rade = nullptr;
        rade_finalize();
        return false;
    }

    auto* fargan = new FARGANState;
    fargan_init(fargan);
    m_fargan = fargan;
    m_farganWarmedUp = false;

    // RX runs the default transition band. Filter latency is a fidelity
    // question on this side, not a correctness one — nothing on RX depends on a
    // tail escaping before the stream stops.
    m_rxDownI     = std::make_unique<Resampler>(kWaveformRateHz, kModemRateHz);
    m_rxDownQ     = std::make_unique<Resampler>(kWaveformRateHz, kModemRateHz);
    m_rxUp16to24  = std::make_unique<Resampler>(kSpeechRateHz, kWaveformRateHz);
    m_txDown24to16 = std::make_unique<Resampler>(kWaveformRateHz, kSpeechRateHz);

    // §7.1 T9 obligation 1 — NOT the default transition band.
    //
    // Measured through Resampler::latencySamples(): the default holds 2376
    // input samples (297 ms) of signal inside the FIR, against a 120 ms EOO
    // frame. `ReqTransBand=45` holds 69 (8.6 ms). The far end detects the
    // end-of-over by correlating against a known template, and a filter three
    // times longer than the frame both delays and smears it out of recognition.
    // Nothing errors; the tail simply stops being detectable.
    m_txUp8to24 = std::make_unique<Resampler>(kModemRateHz, kWaveformRateHz,
                                              4096, kTxUpsampleTransBand);

    // Symbols buffered from a previous session cannot align with this one's
    // bit stream, and a frame straddling the two would be pure fabrication.
    m_textChannel.reset();

    m_rxAccumI.clear();
    m_rxAccumQ.clear();
    m_rxFeatAccum.clear();
    m_rxOut24k.clear();
    m_txSpeech16k.clear();
    m_txFeatAccum.clear();
    m_txOut24k.clear();
    m_synced = false;
    m_snrDb  = 0.0f;
    m_stats  = Stats{};

    qCDebug(lcRadeV2Codec) << "started — n_features=" << rade_n_features_in_out(m_rade)
                      << "n_tx_out=" << rade_n_tx_out(m_rade)
                      << "nin=" << rade_nin(m_rade)
                      << "nin_max=" << rade_nin_max(m_rade)
                      << "tx upsampler latency=" << m_txUp8to24->latencySamples()
                      << "input samples";
    return true;
}

void RADEV2Engine::stop() {
    if (!m_rade) return;

    if (m_lpcnetEnc) {
        lpcnet_encoder_destroy(m_lpcnetEnc);
        m_lpcnetEnc = nullptr;
    }
    if (m_fargan) {
        delete static_cast<FARGANState*>(m_fargan);
        m_fargan = nullptr;
    }
    rade_close(m_rade);
    m_rade = nullptr;
    rade_finalize();

    m_rxDownI.reset();
    m_rxDownQ.reset();
    m_rxUp16to24.reset();
    m_txDown24to16.reset();
    m_txUp8to24.reset();

    m_txActive = false;
    m_tailQueued = false;
    m_tailDone = false;
    m_farganWarmedUp = false;
    m_synced = false;
    m_textChannel.reset();

    qCDebug(lcRadeV2Codec) << "stopped";
}

bool  RADEV2Engine::isActive() const { return m_rade != nullptr; }
bool  RADEV2Engine::isSynced() const { return m_synced; }
float RADEV2Engine::snrDb() const    { return m_snrDb; }

RADEV2Engine::Stats RADEV2Engine::stats() const { return m_stats; }

int RADEV2Engine::queuedTxSamples() const {
    return static_cast<int>(m_txOut24k.size());
}

QString RADEV2Engine::versionString() {
    return QStringLiteral("RADE V2 v%1.%2").arg(rade_version()).arg(rade_version_minor());
}

// ───────────────────────────────────────────────────────────────────────────
// RX
// ───────────────────────────────────────────────────────────────────────────

void RADEV2Engine::onRxPassband(const std::vector<std::complex<float>>& block) {
    if (!m_rade || !m_fargan || block.empty()) return;
    auto* fargan = static_cast<FARGANState*>(m_fargan);
    m_stats.rxPacketsIn++;

    // Deinterleave and resample the two components SEPARATELY through
    // identically-configured filters. The block is a genuine analytic signal —
    // §10.3 item 5 measured it one-sided to better than 70 dB, and the stream
    // has already undone the radio's conjugation (§7.1 X2). So there is no
    // Hilbert transform to do here; there is also no real-only shortcut, since
    // discarding Q would re-introduce the mirror image the radio has already
    // removed for us.
    const int n = static_cast<int>(block.size());
    std::vector<float> i(n), q(n);
    for (int k = 0; k < n; ++k) {
        i[size_t(k)] = block[size_t(k)].real();
        q[size_t(k)] = block[size_t(k)].imag();
    }

    const QByteArray i8k = m_rxDownI->process(i.data(), n);
    const QByteArray q8k = m_rxDownQ->process(q.data(), n);
    const int ni = int(i8k.size() / qsizetype(sizeof(float)));
    const int nq = int(q8k.size() / qsizetype(sizeof(float)));
    const int nc = std::min(ni, nq);
    if (nc <= 0) return;
    if (ni != nq) {
        // Identical filters fed identical lengths cannot diverge; if they ever
        // do, the I/Q pairing is off by a sample and everything downstream is
        // quietly wrong rather than broken.
        qCWarning(lcRadeV2Codec) << "I/Q resampler length mismatch" << ni << nq
                            << "— the analytic pairing has slipped";
    }
    const auto* ip = reinterpret_cast<const float*>(i8k.constData());
    const auto* qp = reinterpret_cast<const float*>(q8k.constData());
    m_rxAccumI.insert(m_rxAccumI.end(), ip, ip + nc);
    m_rxAccumQ.insert(m_rxAccumQ.end(), qp, qp + nc);

    const int nFeat = rade_n_features_in_out(m_rade);
    std::vector<float>    featOut(static_cast<size_t>(nFeat));
    std::vector<RADE_COMP> rxIn(size_t(rade_nin_max(m_rade)));
    std::vector<float>    speech16k;

    // rade_nin() is RE-READ EVERY ITERATION and must never be hoisted out of
    // this loop. It changes as the receiver tracks timing, and a stale value
    // walks the input pointer off the symbol boundary — the receiver then
    // simply never acquires, with no error and no clue as to why.
    while (int(m_rxAccumI.size()) >= rade_nin(m_rade)) {
        const int nin = rade_nin(m_rade);
        for (int k = 0; k < nin; ++k) {
            rxIn[size_t(k)].real = m_rxAccumI[size_t(k)];
            rxIn[size_t(k)].imag = m_rxAccumQ[size_t(k)];
        }
        m_rxAccumI.erase(m_rxAccumI.begin(), m_rxAccumI.begin() + nin);
        m_rxAccumQ.erase(m_rxAccumQ.begin(), m_rxAccumQ.begin() + nin);

        int hasEoo = 0;
        // eoo_out is V1's LDPC text payload; V2 carries its callsign inline
        // instead (§9.2), so there is nothing to collect from the EOO frame.
        const int nOut = rade_rx(m_rade, featOut.data(), &hasEoo, nullptr, rxIn.data());
        if (nOut > 0) {
            m_stats.rxFramesDecoded++;
            m_rxFeatAccum.insert(m_rxFeatAccum.end(),
                                 featOut.begin(), featOut.begin() + nOut);
            // One symbol per successful step, and ONLY on a successful step —
            // rade_api.h documents the value as valid after rade_rx() returns
            // > 0. Sampling it unconditionally would feed the framer stale
            // repeats and desynchronise the bit stream against the sender's.
            const float symbol = rade_rx_get_data_symbol(m_rade);
            emit dataSymbolReceived(symbol);
            if (const auto decoded = m_textChannel.pushSymbol(symbol)) {
                qCDebug(lcRadeV2Codec) << "rx text" << decoded->text
                                  << "confidence" << decoded->confidence
                                  << (decoded->inverted ? "(inverted)" : "");
                emit textDecoded(decoded->text, decoded->confidence);
            }
        }

        while (int(m_rxFeatAccum.size()) >= kFeaturesPerFrame) {
            if (!m_farganWarmedUp) {
                float zeros[2 * LPCNET_FRAME_SIZE] = {0};
                float warmup[kFarganWarmupFrames * kFeaturesPerFrame] = {0};
                fargan_cont(fargan, zeros, warmup);
                m_farganWarmedUp = true;
            }
            float pcm[LPCNET_FRAME_SIZE];
            fargan_synthesize(fargan, pcm, m_rxFeatAccum.data());
            speech16k.insert(speech16k.end(), pcm, pcm + LPCNET_FRAME_SIZE);
            m_rxFeatAccum.erase(m_rxFeatAccum.begin(),
                                m_rxFeatAccum.begin() + kFeaturesPerFrame);
        }
    }

    if (!speech16k.empty()) {
        const QByteArray up = m_rxUp16to24->process(speech16k.data(),
                                                    int(speech16k.size()));
        const auto* up24 = reinterpret_cast<const float*>(up.constData());
        const int nUp = int(up.size() / qsizetype(sizeof(float)));
        m_rxOut24k.insert(m_rxOut24k.end(), up24, up24 + nUp);
    }

    // Hand it back in the radio's own packet size (§7.1 X1). Anything left over
    // stays queued rather than going out as a short packet, so the return
    // stream keeps one steady cadence.
    while (int(m_rxOut24k.size()) >= kEmitBlockSamples) {
        std::vector<float> out(m_rxOut24k.begin(),
                               m_rxOut24k.begin() + kEmitBlockSamples);
        m_rxOut24k.erase(m_rxOut24k.begin(), m_rxOut24k.begin() + kEmitBlockSamples);
        m_stats.rxSpeechSamples += quint64(out.size());
        emit decodedAudioReady(out);
    }

    const bool synced = rade_sync(m_rade) != 0;
    if (synced != m_synced) {
        m_synced = synced;
        emit syncChanged(synced);
    }
    if (synced) {
        const float snr = float(rade_snrdB_3k_est(m_rade));
        if (std::abs(snr - m_snrDb) >= 0.1f) {
            m_snrDb = snr;
            emit snrChanged(snr);
        }
        emit freqOffsetChanged(float(rade_freq_offset(m_rade)));
    }
}

// ───────────────────────────────────────────────────────────────────────────
// TX
// ───────────────────────────────────────────────────────────────────────────

void RADEV2Engine::beginTx() {
    if (!m_rade) return;

    m_txActive   = true;
    m_tailQueued = false;
    m_tailDone   = false;
    m_txSpeech16k.clear();
    m_txFeatAccum.clear();
    m_txOut24k.clear();
    m_txDataBitPos = 0;
    m_stats.txTailSamples = 0;

    // Mandatory, not hygiene: flush() is TERMINAL, so the upsampler used for
    // the previous over refuses to process anything until it is reset. Skipping
    // this makes the *second* transmission silent while the first was fine —
    // the worst possible shape for a bug to have.
    m_txDown24to16->reset();
    m_txUp8to24->reset();

    qCDebug(lcRadeV2Codec) << "tx begin";
}

void RADEV2Engine::feedTxAudio(const QByteArray& stereo24k) {
    if (!m_rade || !m_lpcnetEnc || !m_txActive || m_tailQueued) return;
    if (stereo24k.isEmpty()) return;

    const auto* src = reinterpret_cast<const float*>(stereo24k.constData());
    const int frames = int(stereo24k.size() / qsizetype(2 * sizeof(float)));
    if (frames <= 0) return;

    const QByteArray mono16k = m_txDown24to16->processStereoToMono(src, frames);
    const auto* mf = reinterpret_cast<const float*>(mono16k.constData());
    const int nMono = int(mono16k.size() / qsizetype(sizeof(float)));
    m_txSpeech16k.reserve(m_txSpeech16k.size() + size_t(nMono));
    for (int k = 0; k < nMono; ++k)
        m_txSpeech16k.push_back(
            qint16(std::clamp(mf[k] * 32768.0f, -32768.0f, 32767.0f)));

    encodePendingFeatures(/*drain=*/false);
}

void RADEV2Engine::encodePendingFeatures(bool drain) {
    // Speech → lpcnet features, one 10 ms frame at a time.
    while (int(m_txSpeech16k.size()) >= LPCNET_FRAME_SIZE
           || (drain && !m_txSpeech16k.empty())) {
        std::vector<qint16> frame(LPCNET_FRAME_SIZE, 0);
        const int take = std::min<int>(LPCNET_FRAME_SIZE, int(m_txSpeech16k.size()));
        std::copy(m_txSpeech16k.begin(), m_txSpeech16k.begin() + take, frame.begin());
        m_txSpeech16k.erase(m_txSpeech16k.begin(), m_txSpeech16k.begin() + take);

        float features[kFeaturesPerFrame];
        lpcnet_compute_single_frame_features(
            m_lpcnetEnc, reinterpret_cast<opus_int16*>(frame.data()), features, 0);
        m_txFeatAccum.insert(m_txFeatAccum.end(), features, features + kFeaturesPerFrame);
    }

    // Features → modem. One step = rade_n_features_in_out() floats ≈ 40 ms.
    const int nFeat  = rade_n_features_in_out(m_rade);
    const int nTxOut = rade_n_tx_out(m_rade);
    std::vector<RADE_COMP> txOut(static_cast<size_t>(nTxOut));
    std::vector<float>     modem(static_cast<size_t>(nTxOut));

    while (int(m_txFeatAccum.size()) >= nFeat
           || (drain && !m_txFeatAccum.empty())) {
        std::vector<float> featIn(size_t(nFeat), 0.0f);
        const int take = std::min<int>(nFeat, int(m_txFeatAccum.size()));
        std::copy(m_txFeatAccum.begin(), m_txFeatAccum.begin() + take, featIn.begin());
        m_txFeatAccum.erase(m_txFeatAccum.begin(), m_txFeatAccum.begin() + take);

        // §9.2 — one inline BPSK bit per step, cycled. The PLACEMENT is the
        // part that matters and is fixed: per step, immediately before the
        // encode. The bit-to-symbol mapping and the framing above it are
        // provisional until the V2 ecosystem's convention is settled.
        if (!m_txDataBits.empty()) {
            const bool bit = m_txDataBits[m_txDataBitPos];
            m_txDataBitPos = (m_txDataBitPos + 1) % m_txDataBits.size();
            rade_tx_set_data_symbol(m_rade, bit ? 1.0f : -1.0f);
        }

        const int n = rade_tx(m_rade, txOut.data(), featIn.data());
        if (n <= 0) {
            qCWarning(lcRadeV2Codec) << "rade_tx() produced" << n << "samples";
            continue;
        }
        m_stats.txSteps++;
        for (int k = 0; k < n; ++k) modem[size_t(k)] = txOut[size_t(k)].real;
        queueModem(modem.data(), n);
    }
}

void RADEV2Engine::queueModem(const float* modem8k, int count) {
    if (count <= 0) return;

    // Real part only. The modem's output is already a passband signal centred
    // in the audio band, and what goes on the air is audio (§7.1 E2) — the
    // imaginary component here is the analytic partner, not a second channel.
    const QByteArray up = m_txUp8to24->process(modem8k, count);
    const auto* p = reinterpret_cast<const float*>(up.constData());
    const int n = int(up.size() / qsizetype(sizeof(float)));
    m_txOut24k.insert(m_txOut24k.end(), p, p + n);
}

void RADEV2Engine::onTxClockTick(int samples, bool synthesized) {
    if (!m_rade) return;
    if (!m_txActive) return;

    if (synthesized && !m_tailQueued) {
        // The transport only synthesizes during its drain, which it enters on
        // requestEndTx(). Seeing one before endTx() means the two lifecycles
        // have been wired to different events, and the tail is about to be
        // clocked out before it has been generated.
        qCWarning(lcRadeV2Codec) << "synthesized tick before endTx() — the transport "
                               "is draining a tail the codec has not queued";
    }
    m_stats.txTicks++;
    serveTick(samples > 0 ? samples : kEmitBlockSamples);
}

void RADEV2Engine::serveTick(int samples) {
    const int have = int(m_txOut24k.size());
    const int take = std::min(samples, have);

    if (take > 0) {
        std::vector<float> out(m_txOut24k.begin(), m_txOut24k.begin() + take);
        m_txOut24k.erase(m_txOut24k.begin(), m_txOut24k.begin() + take);

        if (!m_tailQueued && take < samples) {
            // Mid-over starvation. Pad rather than short-change the packet: the
            // radio holds TX through an underrun (§7.1 T4), and keeping the
            // sample clock honest costs one glitch instead of a permanent
            // offset between our stream and the radio's.
            out.resize(size_t(samples), 0.0f);
            m_stats.txUnderruns++;
        }
        emit modulatedAudioReady(out);
    } else if (!m_tailQueued) {
        m_stats.txUnderruns++;
        emit modulatedAudioReady(std::vector<float>(size_t(samples), 0.0f));
    }

    // The tail is done when the queue it was appended to has actually gone out
    // — not when it was generated. §7.1 T9 obligation 3: because flush() put
    // the filter's remaining memory INTO this queue, draining it is exactly
    // "EOO plus resampler latency emitted", with nothing left to estimate.
    if (m_tailQueued && !m_tailDone && m_txOut24k.empty()) {
        m_tailDone = true;
        m_txActive = false;
        qCDebug(lcRadeV2Codec) << "tx tail complete —" << m_stats.txTailSamples
                          << "tail samples ≈" << msAt24k(int(m_stats.txTailSamples)) << "ms";
        emit txTailComplete();
    }
}

void RADEV2Engine::endTx() {
    // Every exit path answers, so a caller holding PTT until txTailComplete()
    // can never be left holding it.
    if (!m_rade || !m_txActive || m_tailQueued) {
        if (!m_tailDone) {
            m_tailDone = true;
            // Both, and in this order. The transport has to be told to drain
            // even when there is nothing to drain, or a caller holding PTT
            // until readyToUnkey() waits for a signal that never comes.
            emit txTailQueued();
            emit txTailComplete();
        }
        return;
    }

    // Drain the voice pipeline first: a partial lpcnet frame and a partial
    // feature step are zero-padded out, so the EOO follows the last real speech
    // rather than being spliced in front of it.
    encodePendingFeatures(/*drain=*/true);

    const int before = int(m_txOut24k.size());

    const int nEoo = rade_n_tx_eoo_out(m_rade);
    if (nEoo > 0) {
        // rade_api.h marks this "V1 only" and the header is WRONG —
        // rade_api.c:207 dispatches on RADE_MODE_V2 to rade_tx_v2_eoo().
        // Pinned by rade_v2_codec_test::eooApiWorksForV2(), because a reader
        // trusting the comment would build around an EOO that does not exist.
        std::vector<RADE_COMP> eoo(static_cast<size_t>(nEoo));
        const int n = rade_tx_eoo(m_rade, eoo.data());
        if (n > 0) {
            std::vector<float> mono(static_cast<size_t>(n));
            for (int k = 0; k < n; ++k) mono[size_t(k)] = eoo[size_t(k)].real;
            queueModem(mono.data(), n);
        } else {
            qCWarning(lcRadeV2Codec) << "rade_tx_eoo() returned" << n
                                << "— the far end will not see an end-of-over";
        }
    }

    // §7.1 T9 obligation 2. Without this the last ~8.6 ms of the EOO stays
    // inside the FIR and is never transmitted: the frame goes out truncated,
    // the correlation at the far end weakens, and nothing anywhere reports a
    // problem. The recovered samples land in the same queue as everything else,
    // which is what makes obligation 3 a queue-empty check instead of a guess.
    const QByteArray flushed = m_txUp8to24->flush();
    if (!flushed.isEmpty()) {
        const auto* p = reinterpret_cast<const float*>(flushed.constData());
        const int n = int(flushed.size() / qsizetype(sizeof(float)));
        m_txOut24k.insert(m_txOut24k.end(), p, p + n);
    }

    m_stats.txTailSamples = quint64(int(m_txOut24k.size()) - before);
    m_tailQueued = true;
    qCDebug(lcRadeV2Codec) << "tx end — tail queued:" << m_stats.txTailSamples
                      << "samples ≈" << msAt24k(int(m_stats.txTailSamples)) << "ms";

    // Starts the transport's synthesized clock (§7.1 T3) — without which the
    // tail is never transmitted and nothing ever releases PTT. After the queue
    // is filled rather than before, which closes a (microsecond-wide) window
    // for free; see the signal's declaration for how little that buys.
    emit txTailQueued();

    // An over with nothing queued at all still has to answer.
    if (m_txOut24k.empty()) {
        m_tailDone = true;
        m_txActive = false;
        emit txTailComplete();
    }
}

void RADEV2Engine::cancelTx() {
    // §7.1 T8 — the chain is already wrong, so there is no tail worth flushing;
    // stop rather than keep feeding it. PTT is still held on our say-so, so the
    // answer goes out on this path too.
    m_txOut24k.clear();
    m_txSpeech16k.clear();
    m_txFeatAccum.clear();
    m_txActive   = false;
    m_tailQueued = false;
    if (!m_tailDone) {
        m_tailDone = true;
        // Same pair, same order, as every other exit. A cancel that skipped
        // txTailQueued() would leave a transport still in Voice with nothing
        // able to move it, and the caller holding PTT until the tail timeout
        // rescued it 500 ms later.
        emit txTailQueued();
        emit txTailComplete();
    }
}

void RADEV2Engine::setTxCallsign(const QString& callsign) {
    m_txDataBitPos = 0;
    m_txDataBits   = RadeV2TextChannel::encode(callsign);

    // The cycling in encodePendingFeatures() is unchanged: it walks these bits
    // and wraps, so consecutive copies are back-to-back with no gap, which is
    // what lets a receiver joining mid-over pick up the next one.
    qCDebug(lcRadeV2Codec) << "tx text" << RadeV2TextChannel::sanitise(callsign)
                      << "—" << m_txDataBits.size() << "bits ="
                      << double(m_txDataBits.size()) / 25.0
                      << "s per copy at 25 bit/s (framing provisional, §9.2)";
}

#else   // !HAVE_RADE_V2 — feature enabled, codec not vendored

// ENABLE_RADE_V2 defines AETHER_ENABLE_RADE_V2 whether or not
// third_party/radae_v2 is present, so the transport and its guards still build
// in a sparse checkout. The engine cannot: it IS the codec. Fail at start()
// with a reason rather than at link with a missing symbol.

RADEV2Engine::RADEV2Engine(QObject* parent) : QObject(parent) {}
RADEV2Engine::~RADEV2Engine() = default;

bool RADEV2Engine::start() {
    qCWarning(lcRadeV2Codec) << "built without the RADE V2 codec (HAVE_RADE_V2 undefined) "
                           "— see third_party/radae_v2/AETHER_VENDORING.md";
    return false;
}

void  RADEV2Engine::stop() {}
bool  RADEV2Engine::isActive() const { return false; }
bool  RADEV2Engine::isSynced() const { return false; }
float RADEV2Engine::snrDb() const    { return 0.0f; }
RADEV2Engine::Stats RADEV2Engine::stats() const { return m_stats; }
int   RADEV2Engine::queuedTxSamples() const { return 0; }
QString RADEV2Engine::versionString() { return {}; }

void RADEV2Engine::onRxPassband(const std::vector<std::complex<float>>&) {}
void RADEV2Engine::onTxClockTick(int, bool) {}
void RADEV2Engine::feedTxAudio(const QByteArray&) {}
void RADEV2Engine::beginTx() {}
void RADEV2Engine::endTx() { emit txTailQueued(); emit txTailComplete(); }
void RADEV2Engine::cancelTx() { emit txTailQueued(); emit txTailComplete(); }
void RADEV2Engine::setTxCallsign(const QString&) {}
void RADEV2Engine::encodePendingFeatures(bool) {}
void RADEV2Engine::queueModem(const float*, int) {}
void RADEV2Engine::serveTick(int) {}

#endif  // HAVE_RADE_V2

}  // namespace AetherSDR

#endif  // AETHER_ENABLE_RADE_V2
