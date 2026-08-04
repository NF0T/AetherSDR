#pragma once

#if defined(HAVE_RADE) || defined(HAVE_RADE_V2)

#include <QWidget>

class QLabel;

namespace AetherSDR {

// Mirrors VfoWidget RADE status in the sidebar. Signal-driven from RADEEngine — no separate timer.
class RadeApplet : public QWidget {
    Q_OBJECT
public:
    explicit RadeApplet(QWidget* parent = nullptr);

public slots:
    void setRadeActive(bool on, const QString& label = {});

    void setRadeSynced(bool synced);
    void setRadeSnr(float snrDb);
    void setRadeFreqOffset(float hz);
    void setRadeCallsign(const QString& callsign);

    // Caption rendered before the received-text field.
    //
    // V1 leaves this empty and shows the value bare, which is right: its EOO
    // payload IS a callsign and reads as one. V2's inline channel carries
    // arbitrary text (§9.2 — a 4-bit length field, up to 15 characters), so
    // showing it bare would present whatever arrived as if it were a station
    // identifier. Set once at activation, not per decode.
    void setRadeTextCaption(const QString& caption);

private:
    QLabel*  m_statusLabel{nullptr};
    QLabel*  m_snrLabel{nullptr};
    QLabel*  m_callsignLabel{nullptr};
    QString  m_textCaption;            // empty = show the value bare (V1)
    QLabel*  m_offsetLabel{nullptr};
    QWidget* m_dataRows{nullptr};
    QLabel*  m_inactiveLabel{nullptr};

    QString  m_modeLabel;
    bool     m_active{false};
};

} // namespace AetherSDR

#endif // HAVE_RADE
