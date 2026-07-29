// RAD2's identity as a mode, and the four GUI classifications that decide
// whether it works (RFC docs/architecture/rade-v2-waveform-design.md §10.4
// G1–G4, §13 "mode-family classification guard").
//
// ─── Why this file exists ──────────────────────────────────────────────────
// G1–G4 were found OTA against a STOCK client, before any RAD2 code existed:
// put a slice into a Python-registered waveform mode and the client renders the
// mode and filter correctly (which is the split-brain fix, demonstrated) — and
// then squelch auto-arms, the adaptive notch stays visible, and a width preset
// clips half the modem away. Every one of those presents as "the codec doesn't
// work". None of them logs anything.
//
// They are also the kind of defect code review cannot catch: the reviewer sees
// `mode == "DIGU" || mode == "DIGL" || mode == "NT"` and has no way to know
// which modes are missing from it.
//
// ─── What this guards, and what it does not ────────────────────────────────
// It guards the CLASSIFICATION FUNCTIONS the widgets now call — the ten sites
// in RxApplet and VfoWidget were changed to route through them, which is the
// only reason a test here means anything. It does NOT instantiate either
// widget, so it cannot prove a *future* site was wired up. That limit is real;
// stated here rather than implied by a passing test.

#include <QStringList>
#include <QtTest>

#include "core/DigitalVoiceFeature.h"
#include "core/DigitalVoiceModeRegistry.h"
#include "gui/ModeFamily.h"

using AetherSDR::DigitalVoiceModeId;
using AetherSDR::DigitalVoiceModeRegistry;
using AetherSDR::filterUnavailableDigitalVoiceModes;
namespace ModeFamily = AetherSDR::ModeFamily;

class RadeV2ModeIdentityTest : public QObject {
    Q_OBJECT

private slots:
    void rad2IsRegisteredOnDigu();
    void rad2JoinsTheDigitalFamily();          // §10.4 G1 + G2
    void rad2UsesDiguOffsetFilterPresets();    // §10.4 G3 + G4
    void dstarStaysOutOfTheDigitalFamily();    // the classification is not "any DV mode"
    void rad2SurvivesTheHelperAvailabilityFilter();
};

void RadeV2ModeIdentityTest::rad2IsRegisteredOnDigu() {
    const std::optional<DigitalVoiceModeId> id =
        DigitalVoiceModeRegistry::modeForRadioMode(QStringLiteral("RAD2"));
    QVERIFY2(id.has_value(),
             "RAD2 is not in the registry — SliceModel resolves digital-voice "
             "modes through it, so without this the slice has no mode identity "
             "and V1's split-brain is back");
    const auto& d = DigitalVoiceModeRegistry::descriptor(id.value());

    QCOMPARE(d.radioMode, QStringLiteral("RAD2"));

    // §10.1, corrected 2026-07-26: DIGU, not USB. Registration sends this as
    // `underlying_mode`, and §10.2 measured which values the radio accepts.
    QCOMPARE(d.underlyingMode, QStringLiteral("DIGU"));

    // No DIGL variant exists, deliberately (§10.11 — upper sideband is
    // correct). A lowercase or aliased lookup must still land on the same mode,
    // because the radio's own status strings are not case-normalised.
    QCOMPARE(DigitalVoiceModeRegistry::modeForRadioMode(QStringLiteral("rad2")), id);
    QVERIFY(!DigitalVoiceModeRegistry::modeForRadioMode(QStringLiteral("RAD2L"))
                 .has_value());
}

void RadeV2ModeIdentityTest::rad2JoinsTheDigitalFamily() {
    // §10.4 G1 — squelch. It auto-arms on a mode outside this family and then
    // GATES DECODE: the modem is not a signal the squelch detector understands,
    // so it closes on perfectly good OFDM. Two independent sites in RxApplet
    // (enable + the client-side override) and two in VfoWidget, all of which
    // now ask this function.
    //
    // §10.4 G2 — ANF/ANFL/ANFT visibility is `!isDig && ...`, so joining the
    // family is also what hides the adaptive notch. A notch that hunts tones is
    // destructive on a carrier-per-62.5-Hz OFDM signal — it eats the modem and
    // reports nothing.
    QVERIFY2(ModeFamily::isDigital(QStringLiteral("RAD2")),
             "RAD2 is not in the DIG family — squelch will auto-arm and gate "
             "decode (G1), and the adaptive notch will stay visible and chew "
             "the OFDM carriers (G2)");

    // The pre-existing members must not have been dropped on the way through.
    QVERIFY(ModeFamily::isDigital(QStringLiteral("DIGU")));
    QVERIFY(ModeFamily::isDigital(QStringLiteral("DIGL")));
    QVERIFY(ModeFamily::isDigital(QStringLiteral("NT")));
    QVERIFY(!ModeFamily::isDigital(QStringLiteral("USB")));
    QVERIFY(!ModeFamily::isDigital(QStringLiteral("LSB")));
    QVERIFY(!ModeFamily::isDigital(QStringLiteral("CW")));
    QVERIFY(!ModeFamily::isDigital(QStringLiteral("RTTY")));
    QVERIFY(!ModeFamily::isDigital(QStringLiteral("FM")));
}

void RadeV2ModeIdentityTest::rad2UsesDiguOffsetFilterPresets() {
    // §10.4 G3 — the highest-value item in the set, because it is the one
    // defect a normal operator action triggers. A width preset on a mode
    // outside this family falls through to `lo = 95; hi = width`.
    QVERIFY2(ModeFamily::usesDiguOffsetPresets(QStringLiteral("RAD2")),
             "RAD2 filter presets do not hang off digu_offset — a 1500 Hz "
             "preset would yield 95–1500 Hz and clip the top half of the "
             "modem's 1000–1937.5 Hz occupancy (G3)");
    QVERIFY(ModeFamily::usesDiguOffsetPresets(QStringLiteral("DIGU")));

    // DIGL is deliberately NOT here: it has its own offset (diglOffset) and its
    // own mirrored arithmetic. Folding the two together would silently move
    // every DIGL passband.
    QVERIFY(!ModeFamily::usesDiguOffsetPresets(QStringLiteral("DIGL")));
    QVERIFY(!ModeFamily::usesDiguOffsetPresets(QStringLiteral("USB")));
    QVERIFY(!ModeFamily::usesDiguOffsetPresets(QStringLiteral("NT")));

    // §10.4 G4 / §10.3 item 7 — restate the arithmetic the family selects, so
    // the test says what "uses digu_offset" MEANS. offset is the CENTRE a
    // width-specified filter is built around, not a passband shift.
    const int offset = 1500;
    const int width  = 1500;
    const int lo = offset - width / 2;
    const int hi = offset + width / 2;
    QCOMPARE(lo, 750);
    QCOMPARE(hi, 2250);
    // And that window has to contain the modem, which is the whole point.
    QVERIFY2(lo < 1000 && hi > 1937,
             "the digu_offset preset window does not contain the modem's "
             "1000–1937.5 Hz occupancy");
}

void RadeV2ModeIdentityTest::dstarStaysOutOfTheDigitalFamily() {
    // The classification is on the UNDERLYING mode, not on "is it a
    // digital-voice mode". D-STAR runs on DFM and belongs with the FM modes:
    // pulling it into the DIG family would disable its squelch, which for an
    // FM-family mode is a behaviour change to a shipped feature.
    QVERIFY(DigitalVoiceModeRegistry::modeForRadioMode(QStringLiteral("DSTR"))
                .has_value());
    QVERIFY2(!ModeFamily::isDigital(QStringLiteral("DSTR")),
             "D-STAR was pulled into the DIG family — it runs on DFM and its "
             "squelch is meaningful");
    QVERIFY(!ModeFamily::usesDiguOffsetPresets(QStringLiteral("DSTR")));
    QVERIFY(!ModeFamily::isUsbFamilyDigitalVoice(QStringLiteral("DSTR")));
    QVERIFY(ModeFamily::isUsbFamilyDigitalVoice(QStringLiteral("RAD2")));
}

void RadeV2ModeIdentityTest::rad2SurvivesTheHelperAvailabilityFilter() {
    // The trap that had nothing to do with RADE and would have removed it
    // anyway: filterUnavailableDigitalVoiceModes() used to strip EVERY
    // registered digital-voice mode on a build without the out-of-process
    // helper. That test was "is it digital voice", which was indistinguishable
    // from "does it need the helper" while D-STAR was the only entry.
    //
    // RAD2's codec is in-process. On a build without the helper it would have
    // vanished from every mode combo with no error — the operator simply could
    // not select it.
    const QStringList offered = filterUnavailableDigitalVoiceModes(
        {QStringLiteral("USB"), QStringLiteral("DIGU"),
         QStringLiteral("DSTR"), QStringLiteral("RAD2")});

    QVERIFY2(offered.contains(QStringLiteral("RAD2")),
             "RAD2 was filtered out as unavailable — it does not need the "
             "digital-voice helper, and on a build without one it would be "
             "silently unselectable");
    QVERIFY(offered.contains(QStringLiteral("USB")));
    QVERIFY(offered.contains(QStringLiteral("DIGU")));

    // D-STAR's own gating is unchanged, in both directions.
    QCOMPARE(offered.contains(QStringLiteral("DSTR")),
             AetherSDR::kLocalDigitalVoiceWaveformAvailable);
}

QTEST_APPLESS_MAIN(RadeV2ModeIdentityTest)
#include "rade_v2_mode_identity_test.moc"
