#include "BandSettings.h"

#include <QSettings>
#include <cstring>

namespace AetherSDR {

BandSettings::BandSettings(QObject* parent)
    : QObject(parent)
{
}

QString BandSettings::bandForFrequency(double freqMhz)
{
    for (int i = 0; i < kBandCount; ++i) {
        if (freqMhz >= kBands[i].lowMhz && freqMhz <= kBands[i].highMhz)
            return QString::fromLatin1(kBands[i].name);
    }
    return QStringLiteral("GEN");
}

const BandDef& BandSettings::bandDef(const QString& name)
{
    if (name == "WWV") return kWwvBand;
    for (int i = 0; i < kBandCount; ++i) {
        if (name == QLatin1String(kBands[i].name))
            return kBands[i];
    }
    return kGenBand;
}

void BandSettings::saveBandState(const QString& bandName, const BandSnapshot& snap)
{
    m_bandStates[bandName] = snap;
}

BandSnapshot BandSettings::loadBandState(const QString& bandName) const
{
    if (m_bandStates.contains(bandName))
        return m_bandStates[bandName];

    // Return defaults from band definition
    const auto& def = bandDef(bandName);
    BandSnapshot snap;
    snap.frequencyMhz    = def.defaultFreqMhz;
    snap.mode            = QString::fromLatin1(def.defaultMode);
    snap.panCenterMhz    = def.defaultFreqMhz;
    snap.panBandwidthMhz = 0.200;
    snap.minDbm          = -130.0f;
    snap.maxDbm          = -40.0f;
    snap.spectrumFrac    = 0.40f;
    return snap;
}

bool BandSettings::hasSavedState(const QString& bandName) const
{
    return m_bandStates.contains(bandName);
}

void BandSettings::saveToFile() const
{
    QSettings s;
    s.beginGroup("bands");
    s.setValue("currentBand", m_currentBand);

    for (auto it = m_bandStates.constBegin(); it != m_bandStates.constEnd(); ++it) {
        s.beginGroup(it.key());
        const auto& snap = it.value();
        s.setValue("frequency",    snap.frequencyMhz);
        s.setValue("mode",         snap.mode);
        s.setValue("rxAntenna",    snap.rxAntenna);
        s.setValue("filterLow",    snap.filterLow);
        s.setValue("filterHigh",   snap.filterHigh);
        s.setValue("agcMode",      snap.agcMode);
        s.setValue("agcThreshold", snap.agcThreshold);
        s.setValue("rfGain",       snap.rfGain);
        s.setValue("wnbOn",        snap.wnbOn);
        s.setValue("wnbLevel",     snap.wnbLevel);
        s.setValue("panCenter",    snap.panCenterMhz);
        s.setValue("panBandwidth", snap.panBandwidthMhz);
        s.setValue("minDbm",       static_cast<double>(snap.minDbm));
        s.setValue("maxDbm",       static_cast<double>(snap.maxDbm));
        s.setValue("spectrumFrac", static_cast<double>(snap.spectrumFrac));
        s.endGroup();
    }

    s.endGroup();
}

void BandSettings::loadFromFile()
{
    QSettings s;
    s.beginGroup("bands");
    m_currentBand = s.value("currentBand", "20m").toString();

    for (const QString& bandName : s.childGroups()) {
        s.beginGroup(bandName);
        BandSnapshot snap;
        snap.frequencyMhz    = s.value("frequency",    0.0).toDouble();
        snap.mode            = s.value("mode",          "").toString();
        snap.rxAntenna       = s.value("rxAntenna",     "").toString();
        snap.filterLow       = s.value("filterLow",     0).toInt();
        snap.filterHigh      = s.value("filterHigh",    0).toInt();
        snap.agcMode         = s.value("agcMode",       "").toString();
        snap.agcThreshold    = s.value("agcThreshold",  0).toInt();
        snap.rfGain          = s.value("rfGain",        0).toInt();
        snap.wnbOn           = s.value("wnbOn",         false).toBool();
        snap.wnbLevel        = s.value("wnbLevel",      50).toInt();
        snap.panCenterMhz    = s.value("panCenter",     0.0).toDouble();
        snap.panBandwidthMhz = s.value("panBandwidth",  0.200).toDouble();
        snap.minDbm          = s.value("minDbm",        -130.0).toFloat();
        snap.maxDbm          = s.value("maxDbm",        -40.0).toFloat();
        snap.spectrumFrac    = s.value("spectrumFrac",  0.40).toFloat();
        s.endGroup();

        if (snap.isValid())
            m_bandStates[bandName] = snap;
    }

    s.endGroup();
}

} // namespace AetherSDR
