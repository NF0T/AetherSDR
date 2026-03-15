#include "MainWindow.h"
#include "ConnectionPanel.h"
#include "PanadapterApplet.h"
#include "SpectrumWidget.h"
#include "AppletPanel.h"
#include "RxApplet.h"
#include "SMeterWidget.h"
#include "TunerApplet.h"
#include "TxApplet.h"
#include "PhoneCwApplet.h"
#include "PhoneApplet.h"
#include "EqApplet.h"
#include "models/SliceModel.h"
#include "models/MeterModel.h"
#include "models/TunerModel.h"
#include "models/TransmitModel.h"
#include "models/EqualizerModel.h"

#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QLabel>
#include <QCloseEvent>
#include <QMessageBox>
#include <QSettings>
#include <QDebug>

namespace AetherSDR {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("AetherSDR");
    setWindowIcon(QIcon(":/icon.png"));
    setMinimumSize(1024, 600);
    resize(1400, 800);

    applyDarkTheme();
    buildMenuBar();
    buildUI();

    // ── Wire up discovery ──────────────────────────────────────────────────
    connect(&m_discovery, &RadioDiscovery::radioDiscovered,
            m_connPanel, &ConnectionPanel::onRadioDiscovered);
    connect(&m_discovery, &RadioDiscovery::radioUpdated,
            m_connPanel, &ConnectionPanel::onRadioUpdated);
    connect(&m_discovery, &RadioDiscovery::radioLost,
            m_connPanel, &ConnectionPanel::onRadioLost);

    connect(m_connPanel, &ConnectionPanel::connectRequested,
            this, [this](const RadioInfo& info){
        m_connPanel->setStatusText("Connecting…");
        m_radioModel.connectToRadio(info);
        // Remember this radio for auto-connect on next launch
        QSettings s("AetherSDR", "AetherSDR");
        s.setValue("lastRadioSerial", info.serial);
    });

    // Auto-connect: when a radio is discovered, check if it matches the last one
    connect(&m_discovery, &RadioDiscovery::radioDiscovered,
            this, [this](const RadioInfo& info) {
        QSettings s("AetherSDR", "AetherSDR");
        const QString lastSerial = s.value("lastRadioSerial").toString();
        if (!lastSerial.isEmpty() && info.serial == lastSerial
            && !m_radioModel.isConnected()) {
            qDebug() << "Auto-connecting to" << info.displayName();
            m_connPanel->setStatusText("Auto-connecting…");
            m_radioModel.connectToRadio(info);
        }
    });
    connect(m_connPanel, &ConnectionPanel::disconnectRequested,
            this, [this]{ m_radioModel.disconnectFromRadio(); });

    // ── Wire up radio model ────────────────────────────────────────────────
    connect(&m_radioModel, &RadioModel::connectionStateChanged,
            this, &MainWindow::onConnectionStateChanged);
    connect(&m_radioModel, &RadioModel::connectionError,
            this, &MainWindow::onConnectionError);
    connect(&m_radioModel, &RadioModel::sliceAdded,
            this, &MainWindow::onSliceAdded);
    connect(&m_radioModel, &RadioModel::sliceRemoved,
            this, &MainWindow::onSliceRemoved);

    // ── Panadapter stream → spectrum widget ───────────────────────────────
    connect(m_radioModel.panStream(), &PanadapterStream::spectrumReady,
            spectrum(), &SpectrumWidget::updateSpectrum);
    connect(m_radioModel.panStream(), &PanadapterStream::waterfallRowReady,
            spectrum(), &SpectrumWidget::updateWaterfallRow);
    connect(&m_radioModel, &RadioModel::panadapterInfoChanged,
            spectrum(), &SpectrumWidget::setFrequencyRange);
    connect(&m_radioModel, &RadioModel::panadapterLevelChanged,
            spectrum(), &SpectrumWidget::setDbmRange);

    // ── Click-to-tune on the spectrum ─────────────────────────────────────
    connect(spectrum(), &SpectrumWidget::frequencyClicked,
            this, &MainWindow::onFrequencyChanged);

    // ── Panadapter stream → audio engine ──────────────────────────────────
    // All VITA-49 traffic arrives on the single client udpport socket owned
    // by PanadapterStream. It strips the header from IF-Data packets and emits
    // audioDataReady(); we feed that directly to the QAudioSink.
    connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            &m_audio, &AudioEngine::feedAudioData);

    // ── AF gain from applet panel → audio engine ──────────────────────────
    connect(m_appletPanel->rxApplet(), &RxApplet::afGainChanged, this, [this](int v) {
        m_audio.setRxVolume(v / 100.0f);
    });

    // ── Tuning step size → spectrum widget ─────────────────────────────────
    connect(m_appletPanel->rxApplet(), &RxApplet::stepSizeChanged,
            spectrum(), &SpectrumWidget::setStepSize);
    spectrum()->setStepSize(100);

    // ── Antenna list from radio → applet panel ─────────────────────────────
    connect(&m_radioModel, &RadioModel::antListChanged,
            m_appletPanel, &AppletPanel::setAntennaList);

    // ── S-Meter: MeterModel → SMeterWidget ────────────────────────────────
    connect(m_radioModel.meterModel(), &MeterModel::sLevelChanged,
            m_appletPanel->sMeterWidget(), &SMeterWidget::setLevel);
    connect(m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            m_appletPanel->sMeterWidget(), &SMeterWidget::setTxMeters);
    connect(m_radioModel.meterModel(), &MeterModel::micMetersChanged,
            m_appletPanel->sMeterWidget(), &SMeterWidget::setMicMeters);
    connect(m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_appletPanel->sMeterWidget(), &SMeterWidget::setTransmitting);

    // ── Tuner: MeterModel TX meters → TunerApplet gauges ────────────────
    connect(m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            m_appletPanel->tunerApplet(), &TunerApplet::updateMeters);
    m_appletPanel->tunerApplet()->setTunerModel(m_radioModel.tunerModel());
    m_appletPanel->tunerApplet()->setMeterModel(m_radioModel.meterModel());

    // Show/hide TUNE button + applet based on TGXL presence
    connect(m_radioModel.tunerModel(), &TunerModel::presenceChanged,
            m_appletPanel, &AppletPanel::setTunerVisible);

    // Switch Fwd Power gauge scale when a power amplifier (PGXL) is detected
    connect(&m_radioModel, &RadioModel::amplifierChanged,
            m_appletPanel->tunerApplet(), &TunerApplet::setAmplifierMode);

    // ── TX applet: meters + model ───────────────────────────────────────────
    connect(m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            m_appletPanel->txApplet(), &TxApplet::updateMeters);
    m_appletPanel->txApplet()->setTransmitModel(m_radioModel.transmitModel());

    // ── P/CW applet: mic meters + ALC meter + model ────────────────────────
    connect(m_radioModel.meterModel(), &MeterModel::micMetersChanged,
            m_appletPanel->phoneCwApplet(), &PhoneCwApplet::updateMeters);
    connect(m_radioModel.meterModel(), &MeterModel::alcChanged,
            m_appletPanel->phoneCwApplet(), &PhoneCwApplet::updateAlc);
    m_appletPanel->phoneCwApplet()->setTransmitModel(m_radioModel.transmitModel());

    // ── PHNE applet: VOX + CW controls ──────────────────────────────────────
    m_appletPanel->phoneApplet()->setTransmitModel(m_radioModel.transmitModel());

    // ── EQ applet: graphic equalizer ─────────────────────────────────────────
    m_appletPanel->eqApplet()->setEqualizerModel(m_radioModel.equalizerModel());

    // ── Status bar telemetry ──────────────────────────────────────────────────
    connect(&m_radioModel, &RadioModel::networkQualityChanged,
            this, [this](const QString& quality, int pingMs) {
        // Color code: Excellent/VeryGood=green, Good=cyan, Fair=amber, Poor=red
        QString color = "#00cc66";
        if (quality == "Fair") color = "#cc9900";
        else if (quality == "Poor") color = "#cc3333";
        else if (quality == "Good") color = "#00b4d8";
        Q_UNUSED(pingMs);
        m_networkLabel->setText(QString("Network: [<span style='color:%1'>%2</span>]")
            .arg(color, quality));
        m_networkLabel->setTextFormat(Qt::RichText);
    });

    connect(m_radioModel.meterModel(), &MeterModel::hwTelemetryChanged,
            this, [this](float paTemp, float supplyVolts) {
        m_paTempLabel->setText(QString("PA %1\u00B0C  |  %2 V")
            .arg(paTemp, 0, 'f', 0)
            .arg(supplyVolts, 0, 'f', 1));
    });

    connect(&m_radioModel, &RadioModel::gpsStatusChanged,
            this, [this](const QString& status, int tracked, int visible,
                         const QString& grid, const QString& /*alt*/,
                         const QString& /*lat*/, const QString& /*lon*/,
                         const QString& utcTime) {
        m_gpsLabel->setText(QString("GPS %1/%2 [%3]")
            .arg(tracked).arg(visible).arg(status));

        // Grid square
        if (!grid.isEmpty())
            m_gridLabel->setText(grid);

        // GPS UTC time (center of status bar)
        if (!utcTime.isEmpty())
            m_gpsTimeLabel->setText(utcTime);
    });

    // Start discovery
    m_discovery.startListening();

    // Restore saved geometry
    QSettings settings("AetherSDR", "AetherSDR");
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
    if (settings.contains("splitterState"))
        m_splitter->restoreState(settings.value("splitterState").toByteArray());
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event)
{
    QSettings settings("AetherSDR", "AetherSDR");
    settings.setValue("geometry",      saveGeometry());
    settings.setValue("windowState",   saveState());
    settings.setValue("splitterState", m_splitter->saveState());
    m_discovery.stopListening();
    m_radioModel.disconnectFromRadio();
    m_audio.stopRxStream();
    QMainWindow::closeEvent(event);
}

// ─── UI Construction ──────────────────────────────────────────────────────────

void MainWindow::buildMenuBar()
{
    auto* fileMenu = menuBar()->addMenu("&File");
    auto* quitAct = fileMenu->addAction("&Quit");
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);

    auto* viewMenu = menuBar()->addMenu("&View");
    auto* themeAct = viewMenu->addAction("Toggle Dark/Light Theme");
    connect(themeAct, &QAction::triggered, this, [this]{
        // Placeholder — full theme switching left as an exercise
        applyDarkTheme();
    });

    auto* helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("About AetherSDR", this, [this]{
        const QString text = QString(
            "<b>AetherSDR</b> v%1<br>"
            "Linux-native SmartSDR-compatible client.<br>"
            "Built with Qt6 and C++20."
            ).arg(QCoreApplication::applicationVersion());
        QMessageBox about(this);
        about.setWindowTitle("About AetherSDR");
        about.setIconPixmap(QPixmap(":/icon.png").scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        about.setText(text);
        about.exec();
    });
}

void MainWindow::buildUI()
{
    // ── Central splitter: [sidebar | spectrum | applets] ──────────────────
    m_splitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(m_splitter);
    auto* splitter = m_splitter;

    // Left sidebar — connection panel
    m_connPanel = new ConnectionPanel(splitter);
    m_connPanel->setFixedWidth(260);
    splitter->addWidget(m_connPanel);

    // Centre — panadapter applet (title bar + FFT spectrum + waterfall)
    m_panApplet = new PanadapterApplet(splitter);
    splitter->addWidget(m_panApplet);
    splitter->setStretchFactor(1, 1);

    // Right — applet panel (includes S-Meter)
    m_appletPanel = new AppletPanel(splitter);
    splitter->addWidget(m_appletPanel);
    splitter->setStretchFactor(2, 0);
    splitter->setCollapsible(2, false);

    // Set initial splitter sizes: left=260, center=stretch, right=260
    // The center pane gets whatever is left after the fixed-width sidebars.
    const int centerWidth = qMax(400, width() - 260 - 260);
    splitter->setSizes({260, centerWidth, 260});

    // ── Status bar ─────────────────────────────────────────────────────────
    const QString statusStyle = "QLabel { color: #8aa8c0; font-size: 11px; background: transparent; }";

    m_connStatusLabel = new QLabel("Disconnected", this);
    m_connStatusLabel->setStyleSheet(statusStyle);
    statusBar()->addWidget(m_connStatusLabel);

    m_networkLabel = new QLabel("", this);
    m_networkLabel->setStyleSheet(statusStyle);
    statusBar()->addWidget(m_networkLabel);

    m_radioInfoLabel = new QLabel("", this);
    m_radioInfoLabel->setStyleSheet(statusStyle);
    statusBar()->addWidget(m_radioInfoLabel);

    m_gpsTimeLabel = new QLabel("", this);
    m_gpsTimeLabel->setStyleSheet(statusStyle);
    m_gpsTimeLabel->setAlignment(Qt::AlignCenter);
    statusBar()->addWidget(m_gpsTimeLabel, 1);

    m_paTempLabel = new QLabel("", this);
    m_paTempLabel->setStyleSheet(statusStyle);
    statusBar()->addPermanentWidget(m_paTempLabel);

    m_gpsLabel = new QLabel("", this);
    m_gpsLabel->setStyleSheet(statusStyle);
    statusBar()->addPermanentWidget(m_gpsLabel);

    m_gridLabel = new QLabel("", this);
    m_gridLabel->setStyleSheet(statusStyle);
    statusBar()->addPermanentWidget(m_gridLabel);
}

// ─── Theme ────────────────────────────────────────────────────────────────────

void MainWindow::applyDarkTheme()
{
    setStyleSheet(R"(
        QWidget {
            background-color: #0f0f1a;
            color: #c8d8e8;
            font-family: "Inter", "Segoe UI", sans-serif;
            font-size: 13px;
        }
        QGroupBox {
            border: 1px solid #203040;
            border-radius: 4px;
            margin-top: 8px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 8px;
            color: #00b4d8;
        }
        QPushButton {
            background-color: #1a2a3a;
            border: 1px solid #203040;
            border-radius: 4px;
            padding: 4px 10px;
            color: #c8d8e8;
        }
        QPushButton:hover  { background-color: #203040; }
        QPushButton:pressed { background-color: #00b4d8; color: #000; }
        QComboBox {
            background-color: #1a2a3a;
            border: 1px solid #203040;
            border-radius: 4px;
            padding: 3px 6px;
        }
        QComboBox::drop-down { border: none; }
        QListWidget {
            background-color: #111120;
            border: 1px solid #203040;
            alternate-background-color: #161626;
        }
        QListWidget::item:selected { background-color: #00b4d8; color: #000; }
        QSlider::groove:horizontal {
            height: 4px;
            background: #203040;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 14px; height: 14px;
            margin: -5px 0;
            background: #00b4d8;
            border-radius: 7px;
        }
        QMenuBar { background-color: #0a0a14; }
        QMenuBar::item:selected { background-color: #1a2a3a; }
        QMenu { background-color: #111120; border: 1px solid #203040; }
        QMenu::item:selected { background-color: #00b4d8; color: #000; }
        QStatusBar { background-color: #0a0a14; border-top: 1px solid #203040; }
        QProgressBar {
            background-color: #111120;
            border: 1px solid #203040;
            border-radius: 3px;
        }
        QSplitter::handle { background-color: #203040; width: 2px; }
    )");
}

// ─── Radio/model event handlers ───────────────────────────────────────────────

void MainWindow::onConnectionStateChanged(bool connected)
{
    m_connPanel->setConnected(connected);
    if (connected) {
        const QString info = QString("%1  %2")
            .arg(m_radioModel.model(), m_radioModel.version());
        m_connStatusLabel->setText("Connected");
        m_radioInfoLabel->setText(info);
        m_connPanel->setStatusText("Connected");
        m_audio.startRxStream();
    } else {
        m_connStatusLabel->setText("Disconnected");
        m_radioInfoLabel->setText("");
        m_connPanel->setStatusText("Not connected");
        m_audio.stopRxStream();
    }
}

void MainWindow::onConnectionError(const QString& msg)
{
    m_connPanel->setStatusText("Error: " + msg);
    m_connStatusLabel->setText("Error");
    statusBar()->showMessage("Connection error: " + msg, 5000);
}

void MainWindow::onSliceAdded(SliceModel* s)
{
    qDebug() << "MainWindow: slice added" << s->sliceId();
    // Update controls to reflect the first (active) slice
    if (m_radioModel.slices().size() == 1) {
        spectrum()->setSliceFrequency(s->frequency());
        spectrum()->setSliceFilter(s->filterLow(), s->filterHigh());
        m_panApplet->setSliceId(s->sliceId());
        m_appletPanel->setSlice(s);
    }

    // Forward slice frequency/mode changes → spectrum
    connect(s, &SliceModel::frequencyChanged, this, [this](double mhz){
        m_updatingFromModel = true;
        spectrum()->setSliceFrequency(mhz);
        m_updatingFromModel = false;
    });
    connect(s, &SliceModel::filterChanged, spectrum(), &SpectrumWidget::setSliceFilter);
}

void MainWindow::onSliceRemoved(int /*id*/) {}

SliceModel* MainWindow::activeSlice() const
{
    const auto& slices = m_radioModel.slices();
    return slices.isEmpty() ? nullptr : slices.first();
}

SpectrumWidget* MainWindow::spectrum() const
{
    return m_panApplet->spectrumWidget();
}

// ─── GUI control handlers ─────────────────────────────────────────────────────

void MainWindow::onFrequencyChanged(double mhz)
{
    // If the slice is locked, snap spectrum back to the current freq.
    if (auto* s = activeSlice(); s && s->isLocked()) {
        m_updatingFromModel = true;
        spectrum()->setSliceFrequency(s->frequency());
        m_updatingFromModel = false;
        return;
    }

    spectrum()->setSliceFrequency(mhz);
    if (!m_updatingFromModel) {
        if (auto* s = activeSlice())
            s->setFrequency(mhz);
    }
}

} // namespace AetherSDR
