#include "RxApplet.h"
#include "models/SliceModel.h"

#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QMenu>
#include <QAction>

namespace AetherSDR {

// ─── Helpers ──────────────────────────────────────────────────────────────────

static QFrame* hLine()
{
    auto* f = new QFrame;
    f->setFrameShape(QFrame::HLine);
    f->setStyleSheet("QFrame { color: #1e2e3e; }");
    return f;
}

// Gradient title bar that heads each applet section (matches SmartSDR style).
static QWidget* appletTitleBar(const QString& text)
{
    auto* bar = new QWidget;
    bar->setFixedHeight(16);
    bar->setStyleSheet(
        "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "stop:0 #3a4a5a, stop:0.5 #2a3a4a, stop:1 #1a2a38); "
        "border-bottom: 1px solid #0a1a28; }");

    auto* lbl = new QLabel(text, bar);
    lbl->setStyleSheet("QLabel { background: transparent; color: #8aa8c0; "
                       "font-size: 10px; font-weight: bold; }");
    lbl->setGeometry(6, 1, 200, 14);
    return bar;
}

// Small checkable flat button used throughout the applet.
static QPushButton* mkToggle(const QString& text, QWidget* parent = nullptr)
{
    auto* b = new QPushButton(text, parent);
    b->setCheckable(true);
    b->setFlat(false);         // keep border so "checked" state is clearly visible
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setFixedHeight(22);
    b->setStyleSheet("font-size: 11px;");
    return b;
}

// Small non-checkable step button (< / >).
static QPushButton* mkStep(const QString& text, QWidget* parent = nullptr)
{
    auto* b = new QPushButton(text, parent);
    b->setFlat(false);
    b->setFixedSize(22, 22);
    b->setStyleSheet("font-size: 11px;");
    return b;
}

// Shared stylesheet: blue when checked (used for ANT, filter, AGC).
static const QString kBlueActive =
    "QPushButton:checked { background-color: #0070c0; color: #ffffff; "
    "border: 1px solid #0090e0; }";

// Green when checked (used for NB/NR/ANF and SQL).
static const QString kGreenActive =
    "QPushButton:checked { background-color: #006040; color: #00ff88; "
    "border: 1px solid #00a060; }";

// Amber when checked (used for RIT/XIT on/off).
static const QString kAmberActive =
    "QPushButton:checked { background-color: #604000; color: #ffb800; "
    "border: 1px solid #906000; }";

// ─── Construction ─────────────────────────────────────────────────────────────

RxApplet::RxApplet(QWidget* parent) : QWidget(parent)
{
    buildUI();
}

void RxApplet::buildUI()
{
    // Outer layout: title bar flush to edges, then padded content below.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(appletTitleBar("RX"));

    auto* inner = new QWidget;
    auto* root = new QVBoxLayout(inner);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(3);
    outer->addWidget(inner);

    // ── Header: slice badge | lock | RX ant | TX ant | filter width | QSK ──
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(3);

        // Slice letter badge (A/B/C/D)
        m_sliceBadge = new QLabel("A");
        m_sliceBadge->setFixedSize(22, 22);
        m_sliceBadge->setAlignment(Qt::AlignCenter);
        m_sliceBadge->setStyleSheet(
            "QLabel { background: #0070c0; color: #ffffff; "
            "border-radius: 3px; font-weight: bold; font-size: 11px; }");
        row->addWidget(m_sliceBadge);

        // Tune-lock toggle (🔓 unlocked / 🔒 locked)
        m_lockBtn = new QPushButton("\U0001F513");  // 🔓
        m_lockBtn->setCheckable(true);
        m_lockBtn->setFixedSize(22, 22);
        m_lockBtn->setFlat(true);
        m_lockBtn->setStyleSheet(
            "QPushButton { font-size: 13px; padding: 0; }"
            "QPushButton:checked { color: #4488ff; }");
        connect(m_lockBtn, &QPushButton::toggled, this, [this](bool locked) {
            m_lockBtn->setText(locked ? "\U0001F512" : "\U0001F513");
            if (m_slice) m_slice->setLocked(locked);
        });
        row->addWidget(m_lockBtn);

        // RX antenna dropdown (blue)
        m_rxAntBtn = new QPushButton("ANT1");
        m_rxAntBtn->setFixedHeight(22);
        m_rxAntBtn->setStyleSheet(
            "QPushButton { color: #4488ff; border: 1px solid #4488ff; "
            "border-radius: 3px; padding: 0 4px; font-size: 11px; }");
        connect(m_rxAntBtn, &QPushButton::clicked, this, [this] {
            QMenu menu(this);
            const QString cur = m_slice ? m_slice->rxAntenna() : "";
            for (const QString& ant : m_antList) {
                QAction* act = menu.addAction(ant);
                act->setCheckable(true);
                act->setChecked(ant == cur);
            }
            const QAction* sel = menu.exec(
                m_rxAntBtn->mapToGlobal(QPoint(0, m_rxAntBtn->height())));
            if (sel && m_slice)
                m_slice->setRxAntenna(sel->text());
        });
        row->addWidget(m_rxAntBtn, 1);

        // TX antenna dropdown (red)
        m_txAntBtn = new QPushButton("ANT1");
        m_txAntBtn->setFixedHeight(22);
        m_txAntBtn->setStyleSheet(
            "QPushButton { color: #ff4444; border: 1px solid #ff4444; "
            "border-radius: 3px; padding: 0 4px; font-size: 11px; }");
        connect(m_txAntBtn, &QPushButton::clicked, this, [this] {
            QMenu menu(this);
            const QString cur = m_slice ? m_slice->txAntenna() : "";
            for (const QString& ant : m_antList) {
                QAction* act = menu.addAction(ant);
                act->setCheckable(true);
                act->setChecked(ant == cur);
            }
            const QAction* sel = menu.exec(
                m_txAntBtn->mapToGlobal(QPoint(0, m_txAntBtn->height())));
            if (sel && m_slice)
                m_slice->setTxAntenna(sel->text());
        });
        row->addWidget(m_txAntBtn, 1);

        // Filter width label (e.g. "2.7K")
        m_filterWidthLbl = new QLabel("2.7K");
        m_filterWidthLbl->setFixedHeight(22);
        m_filterWidthLbl->setAlignment(Qt::AlignCenter);
        m_filterWidthLbl->setStyleSheet(
            "QLabel { color: #00c8ff; font-size: 11px; font-weight: bold; "
            "border: 1px solid #1e3e5e; border-radius: 3px; padding: 0 3px; }");
        row->addWidget(m_filterWidthLbl);

        // QSK toggle
        m_qskBtn = mkToggle("QSK");
        m_qskBtn->setFixedWidth(36);
        m_qskBtn->setStyleSheet(kAmberActive + " QPushButton { font-size: 11px; }");
        connect(m_qskBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_slice) m_slice->setQsk(on);
        });
        row->addWidget(m_qskBtn);

        root->addLayout(row);
    }

    root->addWidget(hLine());

    // ── Filter presets ────────────────────────────────────────────────────────
    {
        auto* lbl = new QLabel("Filter:");
        lbl->setStyleSheet("color: #708090; font-size: 11px;");
        root->addWidget(lbl);

        auto* grid = new QGridLayout;
        grid->setSpacing(3);
        const char* labels[] = {"1.8K","2.1K","2.4K","2.7K","3.3K","6.0K"};
        for (int i = 0; i < 6; ++i) {
            m_filterBtns[i] = mkToggle(labels[i]);
            m_filterBtns[i]->setStyleSheet(kBlueActive);
            const int w = FILTER_WIDTHS[i];
            connect(m_filterBtns[i], &QPushButton::clicked, this, [this, w](bool) {
                applyFilterPreset(w);
            });
            grid->addWidget(m_filterBtns[i], i / 3, i % 3);
        }
        root->addLayout(grid);
    }

    root->addWidget(hLine());

    // ── AGC mode ──────────────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(3);
        auto* lbl = new QLabel("AGC:");
        lbl->setFixedWidth(28);
        lbl->setStyleSheet("color: #708090; font-size: 11px;");
        row->addWidget(lbl);

        const char* labels[] = {"Off","Slw","Med","Fst"};
        for (int i = 0; i < 4; ++i) {
            m_agcBtns[i] = mkToggle(labels[i]);
            m_agcBtns[i]->setStyleSheet(kBlueActive);
            const QString mode = AGC_MODES[i];
            connect(m_agcBtns[i], &QPushButton::clicked, this, [this, mode](bool) {
                if (m_slice) m_slice->setAgcMode(mode);
            });
            row->addWidget(m_agcBtns[i]);
        }
        root->addLayout(row);
    }

    root->addWidget(hLine());

    // ── AF / RF gain ──────────────────────────────────────────────────────────
    auto mkGainRow = [&](const QString& labelText, QSlider*& slider, QLabel*& valLbl,
                         int initVal) {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        auto* lbl = new QLabel(labelText);
        lbl->setFixedWidth(18);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(lbl);

        slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setValue(initVal);
        row->addWidget(slider, 1);

        valLbl = new QLabel(QString::number(initVal));
        valLbl->setFixedWidth(24);
        valLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        valLbl->setStyleSheet("font-size: 11px;");
        row->addWidget(valLbl);
        root->addLayout(row);
    };

    mkGainRow("AF", m_afSlider, m_afLabel, 70);
    mkGainRow("RF", m_rfSlider, m_rfLabel, 0);

    connect(m_afSlider, &QSlider::valueChanged, this, [this](int v) {
        m_afLabel->setText(QString::number(v));
        emit afGainChanged(v);
    });
    connect(m_rfSlider, &QSlider::valueChanged, this, [this](int v) {
        m_rfLabel->setText(QString::number(v));
        if (m_slice) m_slice->setRfGain(static_cast<float>(v));
    });

    root->addWidget(hLine());

    // ── Squelch ───────────────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_sqlBtn = mkToggle("SQL");
        m_sqlBtn->setFixedWidth(36);
        m_sqlBtn->setStyleSheet(kGreenActive);
        row->addWidget(m_sqlBtn);

        m_sqlSlider = new QSlider(Qt::Horizontal);
        m_sqlSlider->setRange(0, 100);
        m_sqlSlider->setValue(20);
        row->addWidget(m_sqlSlider, 1);

        m_sqlLabel = new QLabel("20");
        m_sqlLabel->setFixedWidth(24);
        m_sqlLabel->setStyleSheet("font-size: 11px;");
        m_sqlLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(m_sqlLabel);

        root->addLayout(row);

        connect(m_sqlBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_slice) m_slice->setSquelch(on, m_sqlSlider->value());
        });
        connect(m_sqlSlider, &QSlider::valueChanged, this, [this](int v) {
            m_sqlLabel->setText(QString::number(v));
            if (m_slice && m_sqlBtn->isChecked())
                m_slice->setSquelch(true, v);
        });
    }

    root->addWidget(hLine());

    // ── DSP toggles ───────────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_nbBtn  = mkToggle("NB");
        m_nrBtn  = mkToggle("NR");
        m_anfBtn = mkToggle("ANF");

        for (auto* b : {m_nbBtn, m_nrBtn, m_anfBtn})
            b->setStyleSheet(kGreenActive);

        connect(m_nbBtn,  &QPushButton::toggled, this, [this](bool on) {
            if (m_slice) m_slice->setNb(on);
        });
        connect(m_nrBtn,  &QPushButton::toggled, this, [this](bool on) {
            if (m_slice) m_slice->setNr(on);
        });
        connect(m_anfBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_slice) m_slice->setAnf(on);
        });

        row->addWidget(m_nbBtn);
        row->addWidget(m_nrBtn);
        row->addWidget(m_anfBtn);
        root->addLayout(row);
    }

    root->addWidget(hLine());

    // ── RIT ───────────────────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_ritOnBtn = mkToggle("RIT");
        m_ritOnBtn->setFixedWidth(34);
        m_ritOnBtn->setStyleSheet(kAmberActive);
        row->addWidget(m_ritOnBtn);

        m_ritMinus = mkStep("<");
        row->addWidget(m_ritMinus);

        m_ritLabel = new QLabel("0 Hz");
        m_ritLabel->setAlignment(Qt::AlignCenter);
        m_ritLabel->setMinimumWidth(50);
        m_ritLabel->setStyleSheet(
            "QLabel { font-size: 11px; background: #0a0a18; border: 1px solid #1e2e3e; "
            "border-radius: 3px; padding: 1px 3px; }");
        row->addWidget(m_ritLabel, 1);

        m_ritPlus = mkStep(">");
        row->addWidget(m_ritPlus);

        root->addLayout(row);

        connect(m_ritOnBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_slice) m_slice->setRit(on, m_slice->ritFreq());
        });
        connect(m_ritMinus, &QPushButton::clicked, this, [this] {
            if (!m_slice) return;
            m_slice->setRit(m_ritOnBtn->isChecked(), m_slice->ritFreq() - RIT_STEP_HZ);
        });
        connect(m_ritPlus, &QPushButton::clicked, this, [this] {
            if (!m_slice) return;
            m_slice->setRit(m_ritOnBtn->isChecked(), m_slice->ritFreq() + RIT_STEP_HZ);
        });
    }

    // ── XIT ───────────────────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_xitOnBtn = mkToggle("XIT");
        m_xitOnBtn->setFixedWidth(34);
        m_xitOnBtn->setStyleSheet(kAmberActive);
        row->addWidget(m_xitOnBtn);

        m_xitMinus = mkStep("<");
        row->addWidget(m_xitMinus);

        m_xitLabel = new QLabel("0 Hz");
        m_xitLabel->setAlignment(Qt::AlignCenter);
        m_xitLabel->setMinimumWidth(50);
        m_xitLabel->setStyleSheet(
            "QLabel { font-size: 11px; background: #0a0a18; border: 1px solid #1e2e3e; "
            "border-radius: 3px; padding: 1px 3px; }");
        row->addWidget(m_xitLabel, 1);

        m_xitPlus = mkStep(">");
        row->addWidget(m_xitPlus);

        root->addLayout(row);

        connect(m_xitOnBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_slice) m_slice->setXit(on, m_slice->xitFreq());
        });
        connect(m_xitMinus, &QPushButton::clicked, this, [this] {
            if (!m_slice) return;
            m_slice->setXit(m_xitOnBtn->isChecked(), m_slice->xitFreq() - RIT_STEP_HZ);
        });
        connect(m_xitPlus, &QPushButton::clicked, this, [this] {
            if (!m_slice) return;
            m_slice->setXit(m_xitOnBtn->isChecked(), m_slice->xitFreq() + RIT_STEP_HZ);
        });
    }

    root->addStretch();
}

// ─── Slice wiring ─────────────────────────────────────────────────────────────

void RxApplet::setSlice(SliceModel* slice)
{
    if (m_slice) disconnectSlice(m_slice);
    m_slice = slice;
    if (m_slice) connectSlice(m_slice);
}

void RxApplet::setAntennaList(const QStringList& ants)
{
    if (ants.isEmpty()) return;
    m_antList = ants;
    // Update button labels if the current antenna is still valid
    if (m_slice) {
        m_rxAntBtn->setText(m_slice->rxAntenna());
        m_txAntBtn->setText(m_slice->txAntenna());
    }
}

void RxApplet::connectSlice(SliceModel* s)
{
    // ── Header ─────────────────────────────────────────────────────────────

    // Slice badge letter (0→A, 1→B, 2→C, 3→D)
    const QChar letter = QChar('A' + s->sliceId());
    m_sliceBadge->setText(QString(letter));

    // Lock
    {
        QSignalBlocker b(m_lockBtn);
        m_lockBtn->setChecked(s->isLocked());
        m_lockBtn->setText(s->isLocked() ? "\U0001F512" : "\U0001F513");
    }
    connect(s, &SliceModel::lockedChanged, this, [this](bool locked) {
        QSignalBlocker b(m_lockBtn);
        m_lockBtn->setChecked(locked);
        m_lockBtn->setText(locked ? "\U0001F512" : "\U0001F513");
    });

    // RX antenna
    m_rxAntBtn->setText(s->rxAntenna());
    connect(s, &SliceModel::rxAntennaChanged, this, [this](const QString& ant) {
        m_rxAntBtn->setText(ant);
    });

    // TX antenna
    m_txAntBtn->setText(s->txAntenna());
    connect(s, &SliceModel::txAntennaChanged, this, [this](const QString& ant) {
        m_txAntBtn->setText(ant);
    });

    // Filter width label
    m_filterWidthLbl->setText(formatFilterWidth(s->filterLow(), s->filterHigh()));

    // QSK
    {
        QSignalBlocker b(m_qskBtn);
        m_qskBtn->setChecked(s->qskOn());
    }
    connect(s, &SliceModel::qskChanged, this, [this](bool on) {
        QSignalBlocker b(m_qskBtn); m_qskBtn->setChecked(on);
    });

    // ── Filter ─────────────────────────────────────────────────────────────
    updateFilterButtons();
    connect(s, &SliceModel::filterChanged, this, [this](int lo, int hi) {
        updateFilterButtons();
        m_filterWidthLbl->setText(formatFilterWidth(lo, hi));
    });

    // AGC
    updateAgcButtons();
    connect(s, &SliceModel::agcModeChanged, this, [this](const QString&) {
        updateAgcButtons();
    });

    // RF gain
    {
        QSignalBlocker b(m_rfSlider);
        m_rfSlider->setValue(static_cast<int>(s->rfGain()));
        m_rfLabel->setText(QString::number(static_cast<int>(s->rfGain())));
    }

    // Squelch
    {
        QSignalBlocker b1(m_sqlBtn), b2(m_sqlSlider);
        m_sqlBtn->setChecked(s->squelchOn());
        m_sqlSlider->setValue(s->squelchLevel());
        m_sqlLabel->setText(QString::number(s->squelchLevel()));
    }
    connect(s, &SliceModel::squelchChanged, this, [this](bool on, int level) {
        QSignalBlocker b1(m_sqlBtn), b2(m_sqlSlider);
        m_sqlBtn->setChecked(on);
        m_sqlSlider->setValue(level);
        m_sqlLabel->setText(QString::number(level));
    });

    // DSP toggles
    {
        QSignalBlocker b1(m_nbBtn), b2(m_nrBtn), b3(m_anfBtn);
        m_nbBtn->setChecked(s->nbOn());
        m_nrBtn->setChecked(s->nrOn());
        m_anfBtn->setChecked(s->anfOn());
    }
    connect(s, &SliceModel::nbChanged,  this, [this](bool on) {
        QSignalBlocker b(m_nbBtn);  m_nbBtn->setChecked(on);
    });
    connect(s, &SliceModel::nrChanged,  this, [this](bool on) {
        QSignalBlocker b(m_nrBtn);  m_nrBtn->setChecked(on);
    });
    connect(s, &SliceModel::anfChanged, this, [this](bool on) {
        QSignalBlocker b(m_anfBtn); m_anfBtn->setChecked(on);
    });

    // RIT
    {
        QSignalBlocker b(m_ritOnBtn);
        m_ritOnBtn->setChecked(s->ritOn());
        m_ritLabel->setText(formatHz(s->ritFreq()));
    }
    connect(s, &SliceModel::ritChanged, this, [this](bool on, int hz) {
        QSignalBlocker b(m_ritOnBtn);
        m_ritOnBtn->setChecked(on);
        m_ritLabel->setText(formatHz(hz));
    });

    // XIT
    {
        QSignalBlocker b(m_xitOnBtn);
        m_xitOnBtn->setChecked(s->xitOn());
        m_xitLabel->setText(formatHz(s->xitFreq()));
    }
    connect(s, &SliceModel::xitChanged, this, [this](bool on, int hz) {
        QSignalBlocker b(m_xitOnBtn);
        m_xitOnBtn->setChecked(on);
        m_xitLabel->setText(formatHz(hz));
    });
}

void RxApplet::disconnectSlice(SliceModel* s)
{
    s->disconnect(this);
}

// ─── Private helpers ──────────────────────────────────────────────────────────

QString RxApplet::formatHz(int hz)
{
    return (hz >= 0 ? "+" : "") + QString::number(hz) + " Hz";
}

QString RxApplet::formatFilterWidth(int lo, int hi)
{
    const int w = hi - lo;
    if (w <= 0) return "?";
    if (w >= 1000) return QString::number(w / 1000.0, 'f', 1) + "K";
    return QString::number(w);
}

void RxApplet::applyFilterPreset(int widthHz)
{
    if (!m_slice) return;

    int lo, hi;
    const QString& mode = m_slice->mode();

    if (mode == "LSB" || mode == "DIGL" || mode == "CWL") {
        lo = -widthHz;
        hi = 0;
    } else if (mode == "CW") {
        // Centred 200 Hz above carrier (600 Hz sidetone pitch)
        lo = 200;
        hi = 200 + widthHz;
    } else {
        // USB, DIGU, AM, FM, DIG, etc.
        lo = 0;
        hi = widthHz;
    }

    m_slice->setFilterWidth(lo, hi);
}

void RxApplet::updateFilterButtons()
{
    const int width = m_slice ? (m_slice->filterHigh() - m_slice->filterLow()) : -1;
    for (int i = 0; i < 6; ++i) {
        QSignalBlocker sb(m_filterBtns[i]);
        m_filterBtns[i]->setChecked(width >= 0 &&
                                    std::abs(width - FILTER_WIDTHS[i]) <= 150);
    }
}

void RxApplet::updateAgcButtons()
{
    const QString cur = m_slice ? m_slice->agcMode() : "";
    for (int i = 0; i < 4; ++i) {
        QSignalBlocker sb(m_agcBtns[i]);
        m_agcBtns[i]->setChecked(cur == AGC_MODES[i]);
    }
}

} // namespace AetherSDR
