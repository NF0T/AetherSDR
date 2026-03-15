#include "SpectrumOverlayMenu.h"
#include "models/SliceModel.h"
#include "models/BandDefs.h"

#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QLabel>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QEvent>

namespace AetherSDR {

static constexpr int BTN_W = 60;
static constexpr int BTN_H = 22;

// Band button size (slightly smaller for the grid)
static constexpr int BAND_BTN_W = 48;
static constexpr int BAND_BTN_H = 26;

static const QString kPanelStyle =
    "QWidget { background: rgba(15, 15, 26, 220); "
    "border: 1px solid #304050; border-radius: 3px; }";

static const QString kLabelStyle =
    "QLabel { background: transparent; border: none; "
    "color: #8aa8c0; font-size: 10px; font-weight: bold; }";

static const QString kComboStyle =
    "QComboBox { background: #1a2a3a; border: 1px solid #304050; "
    "border-radius: 2px; color: #c8d8e8; font-size: 11px; "
    "font-weight: bold; padding: 1px 4px; }"
    "QComboBox::drop-down { border: none; }"
    "QComboBox QAbstractItemView { background: #1a2a3a; "
    "border: 1px solid #304050; color: #c8d8e8; "
    "selection-background-color: #0070c0; }";

static const QString kSliderStyle =
    "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; "
    "border-radius: 2px; }"
    "QSlider::handle:horizontal { background: #c8d8e8; width: 10px; "
    "margin: -4px 0; border-radius: 5px; }";

static const QString kMenuBtnNormal =
    "QPushButton { background: rgba(15, 15, 26, 200); "
    "border: 1px solid #304050; border-radius: 2px; "
    "color: #c8d8e8; font-size: 11px; font-weight: bold; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); "
    "border: 1px solid #0090e0; }";

static const QString kMenuBtnActive =
    "QPushButton { background: rgba(0, 112, 192, 180); "
    "border: 1px solid #0090e0; border-radius: 2px; "
    "color: #ffffff; font-size: 11px; font-weight: bold; }";

static QPushButton* makeMenuBtn(const QString& text, QWidget* parent)
{
    auto* btn = new QPushButton(text, parent);
    btn->setFixedSize(BTN_W, BTN_H);
    btn->setStyleSheet(kMenuBtnNormal);
    return btn;
}

// Band button grid uses shared BandDefs + special entries.
// Index into this array for the grid layout below.
struct BandGridEntry {
    const char* label;
    const char* bandName;  // key for BandSettings (e.g. "20m")
    double freqMhz;
    const char* mode;
};

static constexpr BandGridEntry BAND_GRID[] = {
    {"160",  "160m",  1.900,  "LSB"},   // 0
    {"80",   "80m",   3.800,  "LSB"},   // 1
    {"60",   "60m",   5.357,  "USB"},   // 2
    {"40",   "40m",   7.200,  "LSB"},   // 3
    {"30",   "30m",  10.125,  "DIGU"},  // 4
    {"20",   "20m",  14.225,  "USB"},   // 5
    {"17",   "17m",  18.130,  "USB"},   // 6
    {"15",   "15m",  21.300,  "USB"},   // 7
    {"12",   "12m",  24.950,  "USB"},   // 8
    {"10",   "10m",  28.400,  "USB"},   // 9
    {"6",    "6m",   50.150,  "USB"},   // 10
    {"WWV",  "WWV",  10.000,  "AM"},    // 11
    {"GEN",  "GEN",   0.500,  "AM"},    // 12
    {"2200", "2200m", 0.1375, "CW"},    // 13
    {"630",  "630m",  0.475,  "CW"},    // 14
    {"XVTR", "",      0.0,    ""},      // 15
};

SpectrumOverlayMenu::SpectrumOverlayMenu(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);

    // Toggle button (arrow)
    m_toggleBtn = new QPushButton(this);
    m_toggleBtn->setFixedSize(BTN_W, BTN_H);
    m_toggleBtn->setStyleSheet(
        "QPushButton { background: rgba(15, 15, 26, 200); "
        "border: 1px solid #304050; border-radius: 2px; "
        "color: #c8d8e8; font-size: 13px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(0, 112, 192, 180); "
        "border: 1px solid #0090e0; }");
    connect(m_toggleBtn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggle);

    // Menu buttons — Band, ANT, DSP handled specially (sub-panels)
    struct BtnDef { QString text; int specialIdx; void (SpectrumOverlayMenu::*sig)(); };
    const BtnDef defs[] = {
        {"+RX",      -1, &SpectrumOverlayMenu::addRxClicked},   // 0
        {"+TNF",     -1, &SpectrumOverlayMenu::addTnfClicked},  // 1
        {"Band",      0, nullptr},   // 2 — toggleBandPanel
        {"ANT",       1, nullptr},   // 3 — toggleAntPanel
        {"DSP",       2, nullptr},   // 4 — toggleDspPanel
        {"Display",  -1, &SpectrumOverlayMenu::displayClicked}, // 5
        {"DAX",      -1, &SpectrumOverlayMenu::daxClicked},     // 6
    };

    for (const auto& def : defs) {
        auto* btn = makeMenuBtn(def.text, this);
        if (def.specialIdx == 0)
            connect(btn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggleBandPanel);
        else if (def.specialIdx == 1)
            connect(btn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggleAntPanel);
        else if (def.specialIdx == 2)
            connect(btn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggleDspPanel);
        else
            connect(btn, &QPushButton::clicked, this, def.sig);
        m_menuBtns.append(btn);
    }

    buildBandPanel();
    buildAntPanel();
    buildDspPanel();
    updateLayout();
}

// ── Band sub-panel ────────────────────────────────────────────────────────────

void SpectrumOverlayMenu::buildBandPanel()
{
    m_bandPanel = new QWidget(parentWidget());
    m_bandPanel->setAttribute(Qt::WA_NoSystemBackground, true);
    m_bandPanel->setAutoFillBackground(false);
    m_bandPanel->hide();

    auto* grid = new QGridLayout(m_bandPanel);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(2);

    const QString bandBtnStyle =
        "QPushButton { background: rgba(30, 40, 55, 220); "
        "border: 1px solid #304050; border-radius: 3px; "
        "color: #c8d8e8; font-size: 11px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(0, 112, 192, 180); "
        "border: 1px solid #0090e0; }";

    constexpr int layout[][3] = {
        {0, 1, 2},      // 160, 80, 60
        {3, 4, 5},      // 40, 30, 20
        {6, 7, 8},      // 17, 15, 12
        {9, 10, -1},    // 10, 6
        {11, 12, -1},   // WWV, GEN
        {13, 14, 15},   // 2200, 630, XVTR
    };

    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 3; ++col) {
            int idx = layout[row][col];
            if (idx < 0) continue;

            auto* btn = new QPushButton(BAND_GRID[idx].label, m_bandPanel);
            btn->setFixedSize(BAND_BTN_W, BAND_BTN_H);
            btn->setStyleSheet(bandBtnStyle);

            QString bandName = QString::fromLatin1(BAND_GRID[idx].bandName);
            double freq = BAND_GRID[idx].freqMhz;
            QString mode = QString::fromLatin1(BAND_GRID[idx].mode);
            if (bandName.isEmpty()) {
                btn->setEnabled(false);
            } else {
                connect(btn, &QPushButton::clicked, this, [this, bandName, freq, mode]() {
                    emit bandSelected(bandName, freq, mode);
                });
            }

            grid->addWidget(btn, row, col);
        }
    }

    m_bandPanel->adjustSize();
}

// ── ANT sub-panel ─────────────────────────────────────────────────────────────

void SpectrumOverlayMenu::buildAntPanel()
{
    m_antPanel = new QWidget(parentWidget());
    m_antPanel->setStyleSheet(kPanelStyle);
    m_antPanel->hide();

    auto* vbox = new QVBoxLayout(m_antPanel);
    vbox->setContentsMargins(6, 6, 6, 6);
    vbox->setSpacing(4);

    constexpr int kLabelW = 48;
    constexpr int kValueW = 20;

    // RX ANT row
    auto* antRow = new QHBoxLayout;
    antRow->setSpacing(4);
    auto* antLabel = new QLabel("RX ANT:");
    antLabel->setStyleSheet(kLabelStyle);
    antLabel->setFixedWidth(kLabelW);
    antRow->addWidget(antLabel);
    m_rxAntCmb = new QComboBox;
    m_rxAntCmb->setStyleSheet(kComboStyle);
    antRow->addWidget(m_rxAntCmb, 1);
    vbox->addLayout(antRow);

    connect(m_rxAntCmb, &QComboBox::currentTextChanged,
            this, [this](const QString& ant) {
        if (!m_updatingFromModel && m_slice && !ant.isEmpty())
            m_slice->setRxAntenna(ant);
    });

    // RF Gain row
    auto* gainRow = new QHBoxLayout;
    gainRow->setSpacing(4);
    auto* gainLabel = new QLabel("RF Gain:");
    gainLabel->setStyleSheet(kLabelStyle);
    gainLabel->setFixedWidth(kLabelW);
    gainRow->addWidget(gainLabel);
    m_rfGainSlider = new QSlider(Qt::Horizontal);
    m_rfGainSlider->setRange(-8, 32);
    m_rfGainSlider->setSingleStep(8);
    m_rfGainSlider->setPageStep(8);
    m_rfGainSlider->setTickInterval(8);
    m_rfGainSlider->setTickPosition(QSlider::TicksBelow);
    m_rfGainSlider->setStyleSheet(kSliderStyle);
    gainRow->addWidget(m_rfGainSlider, 1);
    m_rfGainLabel = new QLabel("0");
    m_rfGainLabel->setStyleSheet(kLabelStyle);
    m_rfGainLabel->setFixedWidth(kValueW);
    m_rfGainLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    gainRow->addWidget(m_rfGainLabel);
    vbox->addLayout(gainRow);

    connect(m_rfGainSlider, &QSlider::valueChanged, this, [this](int v) {
        // Snap to nearest multiple of 8
        int snapped = qRound(v / 8.0) * 8;
        if (snapped != v) {
            QSignalBlocker sb(m_rfGainSlider);
            m_rfGainSlider->setValue(snapped);
        }
        m_rfGainLabel->setText(QString::number(snapped));
        if (!m_updatingFromModel)
            emit rfGainChanged(snapped);
    });

    // WNB row: toggle button + level slider
    auto* wnbRow = new QHBoxLayout;
    wnbRow->setSpacing(4);
    m_wnbBtn = new QPushButton("WNB");
    m_wnbBtn->setCheckable(true);
    m_wnbBtn->setFixedSize(48, 22);
    m_wnbBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
        "border-radius: 2px; color: #c8d8e8; font-size: 11px; font-weight: bold; }"
        "QPushButton:checked { background: #0070c0; color: #ffffff; "
        "border: 1px solid #0090e0; }"
        "QPushButton:hover { border: 1px solid #0090e0; }");
    wnbRow->addWidget(m_wnbBtn);
    m_wnbSlider = new QSlider(Qt::Horizontal);
    m_wnbSlider->setRange(0, 100);
    m_wnbSlider->setValue(50);
    m_wnbSlider->setStyleSheet(kSliderStyle);
    wnbRow->addWidget(m_wnbSlider, 1);
    m_wnbLabel = new QLabel("50");
    m_wnbLabel->setStyleSheet(kLabelStyle);
    m_wnbLabel->setFixedWidth(kValueW);
    m_wnbLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    wnbRow->addWidget(m_wnbLabel);
    vbox->addLayout(wnbRow);

    connect(m_wnbBtn, &QPushButton::toggled, this, &SpectrumOverlayMenu::wnbToggled);
    connect(m_wnbSlider, &QSlider::valueChanged, this, [this](int v) {
        m_wnbLabel->setText(QString::number(v));
        emit wnbLevelChanged(v);
    });

    m_antPanel->setFixedWidth(180);
    m_antPanel->adjustSize();
}

void SpectrumOverlayMenu::setAntennaList(const QStringList& ants)
{
    m_antList = ants;
    QSignalBlocker sb(m_rxAntCmb);
    QString cur = m_rxAntCmb->currentText();
    m_rxAntCmb->clear();
    m_rxAntCmb->addItems(ants);
    if (ants.contains(cur))
        m_rxAntCmb->setCurrentText(cur);
}

void SpectrumOverlayMenu::setSlice(SliceModel* slice)
{
    if (m_slice)
        m_slice->disconnect(this);
    m_slice = slice;
    if (!m_slice) return;

    connect(m_slice, &SliceModel::rxAntennaChanged, this, [this](const QString& ant) {
        m_updatingFromModel = true;
        m_rxAntCmb->setCurrentText(ant);
        m_updatingFromModel = false;
    });

    // DSP toggle connections
    using S = SliceModel;
    struct DspToggleDef {
        void (S::*setter)(bool);
        void (S::*signal)(bool);
    };
    const DspToggleDef toggleDefs[] = {
        {&S::setNb,   &S::nbChanged},    // 0
        {&S::setNr,   &S::nrChanged},    // 1
        {&S::setAnf,  &S::anfChanged},   // 2
        {&S::setNrl,  &S::nrlChanged},   // 3
        {&S::setNrs,  &S::nrsChanged},   // 4
        {&S::setRnn,  &S::rnnChanged},   // 5
        {&S::setNrf,  &S::nrfChanged},   // 6
        {&S::setAnfl, &S::anflChanged},  // 7
        {&S::setAnft, &S::anftChanged},  // 8
    };

    for (int i = 0; i < 9; ++i) {
        auto* btn = m_dspRows[i].btn;
        auto setter = toggleDefs[i].setter;
        auto signal = toggleDefs[i].signal;

        connect(btn, &QPushButton::toggled, this, [this, setter](bool on) {
            if (!m_updatingFromModel && m_slice)
                (m_slice->*setter)(on);
        });
        connect(m_slice, signal, this, [this, i](bool on) {
            m_updatingFromModel = true;
            QSignalBlocker sb(m_dspRows[i].btn);
            m_dspRows[i].btn->setChecked(on);
            m_updatingFromModel = false;
        });
    }

    // DSP level connections (only for features with sliders)
    struct DspLevelDef {
        int row;
        void (S::*setter)(int);
        void (S::*signal)(int);
    };
    const DspLevelDef levelDefs[] = {
        {0, &S::setNbLevel,   &S::nbLevelChanged},
        {1, &S::setNrLevel,   &S::nrLevelChanged},
        {2, &S::setAnfLevel,  &S::anfLevelChanged},
        {3, &S::setNrlLevel,  &S::nrlLevelChanged},
        {4, &S::setNrsLevel,  &S::nrsLevelChanged},
        {6, &S::setNrfLevel,  &S::nrfLevelChanged},
        {7, &S::setAnflLevel, &S::anflLevelChanged},
    };

    for (const auto& ld : levelDefs) {
        auto* slider = m_dspRows[ld.row].slider;
        auto* lbl = m_dspRows[ld.row].valueLbl;
        auto setter = ld.setter;
        int row = ld.row;

        connect(slider, &QSlider::valueChanged, this, [this, setter, lbl](int v) {
            lbl->setText(QString::number(v));
            if (!m_updatingFromModel && m_slice)
                (m_slice->*setter)(v);
        });
        connect(m_slice, ld.signal, this, [this, row](int v) {
            m_updatingFromModel = true;
            QSignalBlocker sb(m_dspRows[row].slider);
            m_dspRows[row].slider->setValue(v);
            m_dspRows[row].valueLbl->setText(QString::number(v));
            m_updatingFromModel = false;
        });
    }

    syncAntPanel();
    syncDspPanel();
}

void SpectrumOverlayMenu::syncAntPanel()
{
    if (!m_slice) return;
    m_updatingFromModel = true;
    m_rxAntCmb->setCurrentText(m_slice->rxAntenna());
    m_updatingFromModel = false;
}

// ── DSP sub-panel ─────────────────────────────────────────────────────────────

void SpectrumOverlayMenu::buildDspPanel()
{
    m_dspPanel = new QWidget(parentWidget());
    m_dspPanel->setStyleSheet(kPanelStyle);
    m_dspPanel->hide();

    auto* vbox = new QVBoxLayout(m_dspPanel);
    vbox->setContentsMargins(6, 6, 6, 6);
    vbox->setSpacing(3);

    const QString dspBtnStyle =
        "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
        "border-radius: 2px; color: #c8d8e8; font-size: 10px; font-weight: bold; "
        "padding: 1px 2px; }"
        "QPushButton:checked { background: #1a6030; color: #ffffff; "
        "border: 1px solid #20a040; }"
        "QPushButton:hover { border: 1px solid #0090e0; }";

    // DSP feature definitions
    struct DspDef {
        const char* label;
        bool hasLevel;
    };
    const DspDef defs[] = {
        {"NB",   true},   // 0
        {"NR",   true},   // 1
        {"ANF",  true},   // 2
        {"NRL",  true},   // 3
        {"NRS",  true},   // 4
        {"RNN",  false},  // 5
        {"NRF",  true},   // 6
        {"ANFL", true},   // 7
        {"ANFT", false},  // 8
    };

    for (const auto& def : defs) {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        auto* btn = new QPushButton(def.label);
        btn->setCheckable(true);
        btn->setFixedSize(40, 20);
        btn->setStyleSheet(dspBtnStyle);
        row->addWidget(btn);

        DspRow dspRow;
        dspRow.btn = btn;

        if (def.hasLevel) {
            auto* slider = new QSlider(Qt::Horizontal);
            slider->setRange(0, 100);
            slider->setValue(50);
            slider->setStyleSheet(kSliderStyle);
            row->addWidget(slider, 1);

            auto* lbl = new QLabel("50");
            lbl->setStyleSheet(kLabelStyle);
            lbl->setFixedWidth(22);
            lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(lbl);

            slider->installEventFilter(this);
            dspRow.slider = slider;
            dspRow.valueLbl = lbl;
        } else {
            row->addStretch(1);
        }

        vbox->addLayout(row);
        m_dspRows.append(dspRow);
    }

    m_dspPanel->setFixedWidth(200);
    m_dspPanel->adjustSize();

    // Wiring is done in setSlice() since we need the slice model
}

void SpectrumOverlayMenu::syncDspPanel()
{
    if (!m_slice) return;
    m_updatingFromModel = true;

    // Toggle states
    m_dspRows[0].btn->setChecked(m_slice->nbOn());
    m_dspRows[1].btn->setChecked(m_slice->nrOn());
    m_dspRows[2].btn->setChecked(m_slice->anfOn());
    m_dspRows[3].btn->setChecked(m_slice->nrlOn());
    m_dspRows[4].btn->setChecked(m_slice->nrsOn());
    m_dspRows[5].btn->setChecked(m_slice->rnnOn());
    m_dspRows[6].btn->setChecked(m_slice->nrfOn());
    m_dspRows[7].btn->setChecked(m_slice->anflOn());
    m_dspRows[8].btn->setChecked(m_slice->anftOn());

    // Level values (only for features that have sliders)
    auto syncSlider = [](DspRow& r, int v) {
        if (r.slider) { r.slider->setValue(v); r.valueLbl->setText(QString::number(v)); }
    };
    syncSlider(m_dspRows[0], m_slice->nbLevel());
    syncSlider(m_dspRows[1], m_slice->nrLevel());
    syncSlider(m_dspRows[2], m_slice->anfLevel());
    syncSlider(m_dspRows[3], m_slice->nrlLevel());
    syncSlider(m_dspRows[4], m_slice->nrsLevel());
    syncSlider(m_dspRows[6], m_slice->nrfLevel());
    syncSlider(m_dspRows[7], m_slice->anflLevel());

    m_updatingFromModel = false;
}

void SpectrumOverlayMenu::toggleDspPanel()
{
    bool wasVisible = m_dspPanelVisible;
    hideAllSubPanels();
    if (!wasVisible) {
        syncDspPanel();
        m_dspPanelVisible = true;
        // Center vertically on the DSP button (index 4)
        int btnCenterY = m_menuBtns[4]->y() + m_menuBtns[4]->height() / 2;
        int panelY = y() + btnCenterY - m_dspPanel->sizeHint().height() / 2;
        m_dspPanel->move(x() + width(), std::max(0, panelY));
        m_dspPanel->raise();
        m_dspPanel->show();
        m_menuBtns[4]->setStyleSheet(kMenuBtnActive);
    }
}

// ── Sub-panel toggle helpers ──────────────────────────────────────────────────

void SpectrumOverlayMenu::hideAllSubPanels()
{
    if (m_bandPanelVisible) { m_bandPanelVisible = false; m_bandPanel->hide(); }
    if (m_antPanelVisible)  { m_antPanelVisible = false;  m_antPanel->hide(); }
    if (m_dspPanelVisible)  { m_dspPanelVisible = false;  m_dspPanel->hide(); }
    m_menuBtns[2]->setStyleSheet(kMenuBtnNormal);  // Band
    m_menuBtns[3]->setStyleSheet(kMenuBtnNormal);  // ANT
    m_menuBtns[4]->setStyleSheet(kMenuBtnNormal);  // DSP
}

void SpectrumOverlayMenu::toggleBandPanel()
{
    bool wasVisible = m_bandPanelVisible;
    hideAllSubPanels();
    if (!wasVisible) {
        m_bandPanelVisible = true;
        m_bandPanel->move(x() + width(), y());
        m_bandPanel->raise();
        m_bandPanel->show();
        m_menuBtns[2]->setStyleSheet(kMenuBtnActive);
    }
}

void SpectrumOverlayMenu::toggleAntPanel()
{
    bool wasVisible = m_antPanelVisible;
    hideAllSubPanels();
    if (!wasVisible) {
        syncAntPanel();
        m_antPanelVisible = true;
        // Center vertically on the ANT button (index 3)
        int antBtnCenterY = m_menuBtns[3]->y() + m_menuBtns[3]->height() / 2;
        int panelY = y() + antBtnCenterY - m_antPanel->sizeHint().height() / 2;
        m_antPanel->move(x() + width(), std::max(0, panelY));
        m_antPanel->raise();
        m_antPanel->show();
        m_menuBtns[3]->setStyleSheet(kMenuBtnActive);
    }
}

// ── Main menu toggle and layout ───────────────────────────────────────────────

void SpectrumOverlayMenu::toggle()
{
    m_expanded = !m_expanded;
    if (!m_expanded)
        hideAllSubPanels();
    updateLayout();
}

void SpectrumOverlayMenu::updateLayout()
{
    constexpr int pad = 2;
    constexpr int gap = 2;

    m_toggleBtn->setText(m_expanded ? QStringLiteral("\u2190") : QStringLiteral("\u2192"));
    m_toggleBtn->move(pad, pad);

    int y = pad + BTN_H + gap;
    for (auto* btn : m_menuBtns) {
        btn->setVisible(m_expanded);
        if (m_expanded) {
            btn->move(pad, y);
            y += BTN_H + gap;
        }
    }

    int totalH = m_expanded ? (pad + BTN_H + gap + m_menuBtns.size() * (BTN_H + gap))
                            : (pad + BTN_H + pad);
    setFixedSize(pad + BTN_W + pad, totalH);
}

bool SpectrumOverlayMenu::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick) {
        if (auto* slider = qobject_cast<QSlider*>(obj)) {
            slider->setValue(50);
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace AetherSDR
