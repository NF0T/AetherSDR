#pragma once

// FlexWaveformProvider — RADE V2 as an in-process Flex *waveform* (RFC:
// docs/architecture/rade-v2-waveform-design.md). Registers a real slice mode on
// the radio, receives the demodulated modem passband, and hands decoded audio
// back — so the radio routes the slice natively and there is nothing to mute.
//
// Gated behind ENABLE_RADE_V2 (OFF by default) until the release phase. Lives in
// the backend tree because it speaks the Flex wire protocol directly, which EB3
// permits only below the radio seam.
//
// Clean-room: implemented against the public waveform protocol and AetherSDR's
// own VITA-49 stack. The GPL-3.0 `third_party/smartsdr-dsp` provider glue is
// read as protocol authority (Principle I) and never linked (RFC §11).
//
// NOT to be confused with the pre-existing `FlexWaveformModel` (src/models/),
// which mirrors FlexLib's Waveform record — the list of waveforms *installed on
// the radio*. That is the consumer side. This is the provider side: we register
// and host one. The two never interact.
//
// ─── The constraints this class exists to honour ───────────────────────────
// RFC §7.1 lists 32 measured constraints, 15 of which fail SILENTLY — no error,
// no non-zero reply, no crash, just a wrong result. The ones that bite here are
// cited inline by their §7.1 id. Do not "simplify" a cited line without reading
// the section: several of these look like mistakes and are not.

#ifdef AETHER_ENABLE_RADE_V2

#include <functional>

#include <QObject>
#include <QString>
#include <QtGlobal>

namespace AetherSDR {

// The six stream ids returned by `waveform create`.
//
// §7.1 R7 — fw 4.2.20 returns SIX, not the four the vendored D-Star parser
// reads. §7.1 R6 — ids are RECYCLED across create/remove, so never treat one as
// a session identity; re-read them on every registration.
struct WaveformStreams {
    // Inbound: radio → us. These are also the ids we EMIT on (§7.1 E1).
    quint32 txStreamIn  = 0;   // mic audio, only while keyed (§7.1 T2)
    quint32 rxStreamIn  = 0;   // demodulated passband, conjugate I/Q (§7.1 X2)

    // Outbound ids as returned by the radio. INFORMATIONAL ONLY — emitting on
    // these was measured to produce exactly nothing, twice, including a retry
    // after the path was proven live (§7.1 E1, RFC §10.6/§10.9). They are kept
    // so the parse is complete and so a future reader can see they were
    // considered and rejected, not overlooked.
    quint32 txStreamOut = 0;
    quint32 rxStreamOut = 0;

    // Local radio↔client data channel, not over-the-air (RFC §9.2). Out of
    // baseline scope; parsed so it is not silently dropped.
    quint32 byteStreamIn  = 0;
    quint32 byteStreamOut = 0;

    bool valid = false;
};

class FlexWaveformProvider : public QObject {
    Q_OBJECT

public:
    explicit FlexWaveformProvider(QObject* parent = nullptr);
    ~FlexWaveformProvider() override;

    // Where registration commands are written. Same funnel FlexBackend uses, so
    // the provider reuses the one wire-write path rather than opening a second
    // socket — the in-process, single-connection result from RFC §8 (OQ#1).
    void setCommandSink(std::function<void(const QString&)> sink);

    // Registration lifecycle.
    //
    // §7.1 R1 — order is fixed: create → tx=1 → filters → udpport LAST.
    // §7.1 R2 — each filter field is its own command; they do not combine.
    // §7.1 R3 — rx_filter is latched at registration/mode entry. It MUST be set
    //           before the slice enters the mode; changing it later returns
    //           code 0 and does nothing. To change it, re-register.
    // §7.1 R4 — tx_filter is accepted and entirely ignored. It is not sent and
    //           must never be surfaced as an operator control.
    // §7.1 R8 — needs no slice and no panadapter.
    bool registerWaveform();
    void removeWaveform();

    bool isRegistered() const;
    const WaveformStreams& streams() const;

signals:
    // Registration outcome, for the engine to surface. Emitted on the provider's
    // thread; connect queued if the receiver lives elsewhere.
    void registered(const WaveformStreams& streams);
    void unregistered();
    void failed(const QString& reason);

private:
    struct Private;
    Private* d;
};

}  // namespace AetherSDR

#endif  // AETHER_ENABLE_RADE_V2
