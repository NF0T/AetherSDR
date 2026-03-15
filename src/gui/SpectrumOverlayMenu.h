#pragma once

#include <QWidget>
#include <QVector>
#include <QStringList>

class QPushButton;
class QComboBox;
class QSlider;
class QLabel;

namespace AetherSDR {

class SliceModel;

// Floating overlay menu anchored to the top-left of the SpectrumWidget.
// Open by default; collapses to a single arrow button when closed.
// Buttons are placeholders — signals emitted for parent to wire.
class SpectrumOverlayMenu : public QWidget {
    Q_OBJECT

public:
    explicit SpectrumOverlayMenu(QWidget* parent = nullptr);

    // Set the antenna list (from RadioModel::antListChanged).
    void setAntennaList(const QStringList& ants);

    // Connect/disconnect the ANT panel to a slice model.
    void setSlice(SliceModel* slice);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

signals:
    void addRxClicked();
    void addTnfClicked();
    void displayClicked();
    void daxClicked();
    // Emitted when user selects a band from the sub-panel.
    void bandSelected(const QString& bandName, double freqMhz, const QString& mode);
    // Emitted when WNB toggle changes.
    void wnbToggled(bool on);
    // Emitted when WNB level slider changes (0–100).
    void wnbLevelChanged(int level);
    // Emitted when RF gain slider changes (panadapter-level).
    void rfGainChanged(int gain);

private:
    void toggle();
    void updateLayout();
    void toggleBandPanel();
    void buildBandPanel();
    void toggleAntPanel();
    void buildAntPanel();
    void toggleDspPanel();
    void buildDspPanel();
    void syncDspPanel();
    void hideAllSubPanels();
    void syncAntPanel();

    QPushButton* m_toggleBtn{nullptr};
    QVector<QPushButton*> m_menuBtns;
    bool m_expanded{true};

    // Band sub-panel (shown to the right of the menu)
    QWidget* m_bandPanel{nullptr};
    bool m_bandPanelVisible{false};

    // ANT sub-panel
    QWidget*     m_antPanel{nullptr};
    bool         m_antPanelVisible{false};
    QComboBox*   m_rxAntCmb{nullptr};
    QSlider*     m_rfGainSlider{nullptr};
    QLabel*      m_rfGainLabel{nullptr};
    QPushButton* m_wnbBtn{nullptr};
    QSlider*     m_wnbSlider{nullptr};
    QLabel*      m_wnbLabel{nullptr};

    // DSP sub-panel
    QWidget* m_dspPanel{nullptr};
    bool     m_dspPanelVisible{false};
    struct DspRow {
        QPushButton* btn{nullptr};
        QSlider*     slider{nullptr};
        QLabel*      valueLbl{nullptr};
    };
    QVector<DspRow> m_dspRows;

    QStringList  m_antList;
    SliceModel*  m_slice{nullptr};
    bool         m_updatingFromModel{false};
};

} // namespace AetherSDR
