#include "DxClusterDialog.h"
#include "core/DxClusterClient.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QCheckBox>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTabWidget>
#include <QTableView>
#include <QHeaderView>
#include <QSortFilterProxyModel>
#include <QSlider>
#include <QColorDialog>
#include <QFile>
#include <QRegularExpression>

namespace AetherSDR {

// ── SpotTableModel ──────────────────────────────────────────────────────────

QVariant SpotTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_spots.size())
        return {};

    const auto& spot = m_spots[index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColTime:    return spot.utcTime.toString("HH:mm");
        case ColFreq:    return QString::number(spot.freqMhz * 1000.0, 'f', 1);
        case ColDxCall:  return spot.dxCall;
        case ColComment: return spot.comment;
        case ColSpotter: return spot.spotterCall;
        case ColBand:    return bandForFreq(spot.freqMhz);
        case ColSource:  return spot.source;
        }
    }
    if (role == Qt::TextAlignmentRole) {
        if (index.column() == ColFreq)
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        if (index.column() == ColTime)
            return QVariant(Qt::AlignCenter);
    }
    if (role == Qt::ForegroundRole) {
        if (index.column() == ColDxCall)
            return QColor(0x00, 0xb4, 0xd8);  // accent
        if (index.column() == ColFreq)
            return QColor(0xe0, 0xd0, 0x60);  // yellow-ish
    }
    // Store freq in UserRole for sorting
    if (role == Qt::UserRole && index.column() == ColFreq)
        return spot.freqMhz;

    return {};
}

QVariant SpotTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColTime:    return "Time";
    case ColFreq:    return "Freq (kHz)";
    case ColDxCall:  return "DX Call";
    case ColComment: return "Comment";
    case ColSpotter: return "Spotter";
    case ColBand:    return "Band";
    case ColSource:  return "Source";
    }
    return {};
}

void SpotTableModel::addSpot(const DxSpot& spot)
{
    beginInsertRows({}, 0, 0);
    m_spots.prepend(spot);
    endInsertRows();

    if (m_spots.size() > m_maxSpots) {
        beginRemoveRows({}, m_maxSpots, m_spots.size() - 1);
        m_spots.resize(m_maxSpots);
        endRemoveRows();
    }
}

void SpotTableModel::addSpots(const QVector<DxSpot>& spots)
{
    if (spots.isEmpty()) return;
    int count = spots.size();
    beginInsertRows({}, 0, count - 1);
    // Prepend in reverse so newest is at index 0
    for (int i = count - 1; i >= 0; --i)
        m_spots.prepend(spots[i]);
    endInsertRows();

    if (m_spots.size() > m_maxSpots) {
        beginRemoveRows({}, m_maxSpots, m_spots.size() - 1);
        m_spots.resize(m_maxSpots);
        endRemoveRows();
    }
}

double SpotTableModel::freqAtRow(int row) const
{
    if (row >= 0 && row < m_spots.size())
        return m_spots[row].freqMhz;
    return 0.0;
}

void SpotTableModel::clear()
{
    beginResetModel();
    m_spots.clear();
    endResetModel();
}

QString SpotTableModel::bandForFreq(double mhz)
{
    if (mhz >= 1.8   && mhz <= 2.0)    return "160m";
    if (mhz >= 3.5   && mhz <= 4.0)    return "80m";
    if (mhz >= 5.0   && mhz <= 5.5)    return "60m";
    if (mhz >= 7.0   && mhz <= 7.3)    return "40m";
    if (mhz >= 10.1  && mhz <= 10.15)  return "30m";
    if (mhz >= 14.0  && mhz <= 14.35)  return "20m";
    if (mhz >= 18.068 && mhz <= 18.168) return "17m";
    if (mhz >= 21.0  && mhz <= 21.45)  return "15m";
    if (mhz >= 24.89 && mhz <= 24.99)  return "12m";
    if (mhz >= 28.0  && mhz <= 29.7)   return "10m";
    if (mhz >= 50.0  && mhz <= 54.0)   return "6m";
    if (mhz >= 144.0 && mhz <= 148.0)  return "2m";
    return "";
}

// ── BandFilterProxy ─────────────────────────────────────────────────────────

void BandFilterProxy::setBandVisible(const QString& band, bool visible)
{
    if (visible)
        m_hiddenBands.remove(band);
    else
        m_hiddenBands.insert(band);
    invalidateFilter();
}

bool BandFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (m_hiddenBands.isEmpty())
        return true;
    auto idx = sourceModel()->index(sourceRow, SpotTableModel::ColBand, sourceParent);
    QString band = sourceModel()->data(idx, Qt::DisplayRole).toString();
    if (band.isEmpty())
        return true;  // unknown band — always show
    return !m_hiddenBands.contains(band);
}

// ── DxClusterDialog ─────────────────────────────────────────────────────────

DxClusterDialog::DxClusterDialog(DxClusterClient* clusterClient, DxClusterClient* rbnClient,
                                   RadioModel* radioModel, QWidget* parent)
    : QDialog(parent), m_client(clusterClient), m_rbnClient(rbnClient),
      m_radioModel(radioModel)
{
    setWindowTitle("SpotHub");
    setMinimumSize(620, 500);
    resize(700, 580);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(0);
    root->setContentsMargins(4, 4, 4, 4);

    auto* tabs = new QTabWidget;
    tabs->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #203040; }"
        "QTabBar::tab { background: #1a1a2e; color: #808890; border: 1px solid #203040; "
        "  padding: 6px 16px; margin-right: 2px; }"
        "QTabBar::tab:selected { background: #0f0f1a; color: #00b4d8; border-bottom: none; }");

    buildClusterTab(tabs);
    buildRbnTab(tabs);
    buildSpotListTab(tabs);
    buildDisplayTab(tabs);

    root->addWidget(tabs);

    // ── Spot batch timer (1/sec flush) ──────────────────────────────────
    m_spotBatchTimer = new QTimer(this);
    m_spotBatchTimer->start(1000);
    connect(m_spotBatchTimer, &QTimer::timeout, this, &DxClusterDialog::flushSpotBatch);

    // Auto-scroll helper: only scroll if user is already at the bottom
    auto isAtBottom = [](QAbstractScrollArea* w) {
        auto* sb = w->verticalScrollBar();
        return sb->value() >= sb->maximum() - 2;
    };

    // ── Live updates from client ────────────────────────────────────────
    connect(clusterClient, &DxClusterClient::rawLineReceived, this, [this, isAtBottom](const QString& line) {
        bool follow = isAtBottom(m_console);
        m_console->appendPlainText(line);
        if (follow) {
            auto* sb = m_console->verticalScrollBar();
            sb->setValue(sb->maximum());
        }
    });

    connect(clusterClient, &DxClusterClient::spotReceived, this, [this](DxSpot spot) {
        spot.source = "Cluster";
        m_spotBatch.append(spot);
    });

    connect(clusterClient, &DxClusterClient::connected, this, [this] {
        m_statusLabel->setText(QString("Connected to %1:%2").arg(m_client->host()).arg(m_client->port()));
        m_statusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_connectBtn->setText("Disconnect");
        m_cmdEdit->setEnabled(true);
        m_sendBtn->setEnabled(true);
        m_console->appendPlainText("--- Connected ---");
    });
    connect(clusterClient, &DxClusterClient::disconnected, this, [this] {
        m_statusLabel->setText("Disconnected");
        m_statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_connectBtn->setText("Connect");
        m_cmdEdit->setEnabled(false);
        m_sendBtn->setEnabled(false);
        m_console->appendPlainText("--- Disconnected ---");
    });
    connect(clusterClient, &DxClusterClient::connectionError, this, [this](const QString& err) {
        m_statusLabel->setText("Error: " + err);
        m_statusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 11px; }");
        m_console->appendPlainText("--- Error: " + err + " ---");
    });

    // Load existing log file into console and replay spots into table
    QFile logFile(clusterClient->logFilePath());
    if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Static regex — same as DxClusterClient::parseDxSpotLine
        static const QRegularExpression rx(
            R"(^DX\s+de\s+(\S+?):\s+(\d+\.?\d*)\s+(\S+)\s+(.*?)\s+(\d{4})Z)",
            QRegularExpression::CaseInsensitiveOption);

        while (!logFile.atEnd()) {
            QString line = QString::fromUtf8(logFile.readLine()).trimmed();
            if (line.isEmpty()) continue;
            m_console->appendPlainText(line);

            // Try to parse as spot for the table
            auto match = rx.match(line);
            if (match.hasMatch()) {
                DxSpot spot;
                spot.spotterCall = match.captured(1);
                spot.freqMhz = match.captured(2).toDouble() / 1000.0;
                spot.dxCall = match.captured(3);
                spot.comment = match.captured(4).trimmed();
                QString timeStr = match.captured(5);
                spot.utcTime = QTime(timeStr.left(2).toInt(), timeStr.mid(2, 2).toInt());
                if (spot.freqMhz > 0.0 && !spot.dxCall.isEmpty()) {
                    spot.source = "Cluster";
                    m_spotModel->addSpot(spot);
                }
            }
        }
        // Scroll console to bottom
        auto* sb = m_console->verticalScrollBar();
        sb->setValue(sb->maximum());
    }

    // ── Live updates from RBN client ──────────────────────────────────
    connect(rbnClient, &DxClusterClient::rawLineReceived, this, [this, isAtBottom](const QString& line) {
        bool follow = isAtBottom(m_rbnConsole);
        m_rbnConsole->appendPlainText(line);
        if (follow) {
            auto* sb = m_rbnConsole->verticalScrollBar();
            sb->setValue(sb->maximum());
        }
    });

    connect(rbnClient, &DxClusterClient::spotReceived, this, [this](DxSpot spot) {
        spot.source = "RBN";
        m_spotBatch.append(spot);
    });

    connect(rbnClient, &DxClusterClient::connected, this, [this] {
        m_rbnStatusLabel->setText(QString("Connected to %1:%2").arg(m_rbnClient->host()).arg(m_rbnClient->port()));
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_rbnConnectBtn->setText("Disconnect");
        m_rbnCmdEdit->setEnabled(true);
        m_rbnSendBtn->setEnabled(true);
        m_rbnConsole->appendPlainText("--- Connected ---");
    });
    connect(rbnClient, &DxClusterClient::disconnected, this, [this] {
        m_rbnStatusLabel->setText("Disconnected");
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_rbnConnectBtn->setText("Connect");
        m_rbnCmdEdit->setEnabled(false);
        m_rbnSendBtn->setEnabled(false);
        m_rbnConsole->appendPlainText("--- Disconnected ---");
    });
    connect(rbnClient, &DxClusterClient::connectionError, this, [this](const QString& err) {
        m_rbnStatusLabel->setText("Error: " + err);
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 11px; }");
        m_rbnConsole->appendPlainText("--- Error: " + err + " ---");
    });

    // Load RBN log file into console and replay spots
    QFile rbnLogFile(rbnClient->logFilePath());
    if (rbnLogFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        static const QRegularExpression rx2(
            R"(^DX\s+de\s+(\S+?):\s+(\d+\.?\d*)\s+(\S+)\s+(.*?)\s+(\d{4})Z)",
            QRegularExpression::CaseInsensitiveOption);

        while (!rbnLogFile.atEnd()) {
            QString line = QString::fromUtf8(rbnLogFile.readLine()).trimmed();
            if (line.isEmpty()) continue;
            m_rbnConsole->appendPlainText(line);

            auto match = rx2.match(line);
            if (match.hasMatch()) {
                DxSpot spot;
                spot.spotterCall = match.captured(1);
                spot.freqMhz = match.captured(2).toDouble() / 1000.0;
                spot.dxCall = match.captured(3);
                spot.comment = match.captured(4).trimmed();
                QString timeStr = match.captured(5);
                spot.utcTime = QTime(timeStr.left(2).toInt(), timeStr.mid(2, 2).toInt());
                if (spot.freqMhz > 0.0 && !spot.dxCall.isEmpty()) {
                    spot.source = "RBN";
                    m_spotModel->addSpot(spot);
                }
            }
        }
        auto* sb = m_rbnConsole->verticalScrollBar();
        sb->setValue(sb->maximum());
    }

    // Scroll spot table to show newest entries
    m_spotTable->scrollToBottom();

    updateStatus();
}

void DxClusterDialog::buildClusterTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();

    // ── Connection settings ─────────────────────────────────────────────
    auto* connGroup = new QGroupBox("Connection");
    auto* connLayout = new QVBoxLayout(connGroup);
    connLayout->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    grid->addWidget(new QLabel("Server:"), row, 0);
    m_hostEdit = new QLineEdit(s.value("DxClusterHost", "dxc.nc7j.com").toString());
    m_hostEdit->setPlaceholderText("dxc.nc7j.com");
    m_hostEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_hostEdit, row, 1);
    row++;

    grid->addWidget(new QLabel("Port:"), row, 0);
    m_portSpin = new QSpinBox;
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(s.value("DxClusterPort", 7300).toInt());
    m_portSpin->setStyleSheet("QSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_portSpin, row, 1);
    row++;

    grid->addWidget(new QLabel("Callsign:"), row, 0);
    m_callEdit = new QLineEdit(s.value("DxClusterCallsign").toString());
    m_callEdit->setPlaceholderText("your callsign");
    m_callEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_callEdit, row, 1);
    row++;

    connLayout->addLayout(grid);

    // Button row
    auto* btnRow = new QHBoxLayout;
    m_autoConnectBtn = new QPushButton(
        s.value("DxClusterAutoConnect", "False").toString() == "True" ? "Auto-Connect: ON" : "Auto-Connect: OFF");
    m_autoConnectBtn->setCheckable(true);
    m_autoConnectBtn->setChecked(s.value("DxClusterAutoConnect", "False").toString() == "True");
    m_autoConnectBtn->setStyleSheet(
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 4px 10px; }"
        "QPushButton:!checked { background: #603020; }");
    connect(m_autoConnectBtn, &QPushButton::toggled, this, [this](bool on) {
        m_autoConnectBtn->setText(on ? "Auto-Connect: ON" : "Auto-Connect: OFF");
        auto& s = AppSettings::instance();
        s.setValue("DxClusterAutoConnect", on ? "True" : "False");
        s.save();
    });
    btnRow->addWidget(m_autoConnectBtn);
    btnRow->addStretch();

    m_statusLabel = new QLabel("Disconnected");
    m_statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    btnRow->addWidget(m_statusLabel);
    btnRow->addStretch();

    m_connectBtn = new QPushButton(m_client->isConnected() ? "Disconnect" : "Connect");
    m_connectBtn->setFixedWidth(100);
    m_connectBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "border: 1px solid #008ba8; padding: 4px; border-radius: 3px; }"
        "QPushButton:hover { background: #00c8f0; }"
        "QPushButton:disabled { background: #404060; color: #808080; }");
    connect(m_connectBtn, &QPushButton::clicked, this, [this] {
        if (m_client->isConnected()) {
            emit disconnectRequested();
            return;
        }
        QString host = m_hostEdit->text().trimmed();
        QString call = m_callEdit->text().trimmed().toUpper();
        quint16 port = static_cast<quint16>(m_portSpin->value());
        if (host.isEmpty() || call.isEmpty()) {
            m_statusLabel->setText("Server and callsign are required");
            m_statusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 11px; }");
            return;
        }
        auto& s = AppSettings::instance();
        s.setValue("DxClusterHost", host);
        s.setValue("DxClusterPort", port);
        s.setValue("DxClusterCallsign", call);
        s.save();
        emit connectRequested(host, port, call);
    });
    btnRow->addWidget(m_connectBtn);
    connLayout->addLayout(btnRow);

    layout->addWidget(connGroup);

    // ── Console output ──────────────────────────────────────────────────
    auto* consoleLabel = new QLabel("Cluster Console");
    consoleLabel->setStyleSheet("QLabel { color: #00b4d8; font-weight: bold; }");
    layout->addWidget(consoleLabel);

    m_console = new QPlainTextEdit;
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(2000);
    m_console->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #a0b0c0;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "  padding: 4px;"
        "}");
    layout->addWidget(m_console, 1);

    // Command input row
    auto* cmdRow = new QHBoxLayout;
    m_cmdEdit = new QLineEdit;
    m_cmdEdit->setPlaceholderText("Type a cluster command (e.g. sh/dx 20, set/filter, bye)");
    m_cmdEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; font-family: monospace; }");
    m_cmdEdit->setEnabled(m_client->isConnected());
    connect(m_cmdEdit, &QLineEdit::returnPressed, this, [this] {
        QString cmd = m_cmdEdit->text().trimmed();
        if (cmd.isEmpty() || !m_client->isConnected()) return;
        QMetaObject::invokeMethod(m_client, [client=m_client, cmd] { client->sendCommand(cmd); });
        m_console->appendPlainText("> " + cmd);
        m_cmdEdit->clear();
    });
    m_sendBtn = new QPushButton("Send");
    m_sendBtn->setFixedWidth(60);
    m_sendBtn->setEnabled(m_client->isConnected());
    connect(m_sendBtn, &QPushButton::clicked, this, [this] {
        m_cmdEdit->returnPressed();
    });
    cmdRow->addWidget(m_cmdEdit, 1);
    cmdRow->addWidget(m_sendBtn);
    layout->addLayout(cmdRow);

    tabs->addTab(page, "Cluster");
}

void DxClusterDialog::buildRbnTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();
    QString defaultCall = s.value("RbnCallsign").toString();
    if (defaultCall.isEmpty())
        defaultCall = s.value("DxClusterCallsign").toString();

    // ── Connection settings ─────────────────────────────────────────────
    auto* connGroup = new QGroupBox("RBN Connection");
    auto* connLayout = new QVBoxLayout(connGroup);
    connLayout->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    grid->addWidget(new QLabel("Server:"), row, 0);
    m_rbnHostEdit = new QLineEdit(s.value("RbnHost", "telnet.reversebeacon.net").toString());
    m_rbnHostEdit->setPlaceholderText("telnet.reversebeacon.net");
    m_rbnHostEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_rbnHostEdit, row, 1);
    row++;

    grid->addWidget(new QLabel("Port:"), row, 0);
    m_rbnPortSpin = new QSpinBox;
    m_rbnPortSpin->setRange(1, 65535);
    m_rbnPortSpin->setValue(s.value("RbnPort", 7000).toInt());
    m_rbnPortSpin->setStyleSheet("QSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_rbnPortSpin, row, 1);
    row++;

    grid->addWidget(new QLabel("Callsign:"), row, 0);
    m_rbnCallEdit = new QLineEdit(defaultCall);
    m_rbnCallEdit->setPlaceholderText("your callsign");
    m_rbnCallEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_rbnCallEdit, row, 1);
    row++;

    // Rate limit
    grid->addWidget(new QLabel("Rate Limit:"), row, 0);
    auto* rateRow = new QHBoxLayout;
    auto* rateSpin = new QSpinBox;
    rateSpin->setRange(1, 100);
    rateSpin->setValue(s.value("RbnRateLimit", 10).toInt());
    rateSpin->setSuffix(" spots/sec");
    rateSpin->setStyleSheet("QSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    connect(rateSpin, &QSpinBox::valueChanged, this, [](int v) {
        auto& s = AppSettings::instance();
        s.setValue("RbnRateLimit", v);
        s.save();
    });
    rateRow->addWidget(rateSpin);
    rateRow->addStretch();
    grid->addLayout(rateRow, row, 1);
    row++;

    connLayout->addLayout(grid);

    // Button row
    auto* btnRow = new QHBoxLayout;
    m_rbnAutoConnectBtn = new QPushButton(
        s.value("RbnAutoConnect", "False").toString() == "True" ? "Auto-Connect: ON" : "Auto-Connect: OFF");
    m_rbnAutoConnectBtn->setCheckable(true);
    m_rbnAutoConnectBtn->setChecked(s.value("RbnAutoConnect", "False").toString() == "True");
    m_rbnAutoConnectBtn->setStyleSheet(
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 4px 10px; }"
        "QPushButton:!checked { background: #603020; }");
    connect(m_rbnAutoConnectBtn, &QPushButton::toggled, this, [this](bool on) {
        m_rbnAutoConnectBtn->setText(on ? "Auto-Connect: ON" : "Auto-Connect: OFF");
        auto& s = AppSettings::instance();
        s.setValue("RbnAutoConnect", on ? "True" : "False");
        s.save();
    });
    btnRow->addWidget(m_rbnAutoConnectBtn);
    btnRow->addStretch();

    m_rbnStatusLabel = new QLabel("Disconnected");
    m_rbnStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    btnRow->addWidget(m_rbnStatusLabel);
    btnRow->addStretch();

    m_rbnConnectBtn = new QPushButton(m_rbnClient->isConnected() ? "Disconnect" : "Connect");
    m_rbnConnectBtn->setFixedWidth(100);
    m_rbnConnectBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "border: 1px solid #008ba8; padding: 4px; border-radius: 3px; }"
        "QPushButton:hover { background: #00c8f0; }"
        "QPushButton:disabled { background: #404060; color: #808080; }");
    connect(m_rbnConnectBtn, &QPushButton::clicked, this, [this] {
        if (m_rbnClient->isConnected()) {
            emit rbnDisconnectRequested();
            return;
        }
        QString host = m_rbnHostEdit->text().trimmed();
        QString call = m_rbnCallEdit->text().trimmed().toUpper();
        quint16 port = static_cast<quint16>(m_rbnPortSpin->value());
        if (host.isEmpty() || call.isEmpty()) {
            m_rbnStatusLabel->setText("Server and callsign are required");
            m_rbnStatusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 11px; }");
            return;
        }
        auto& s = AppSettings::instance();
        s.setValue("RbnHost", host);
        s.setValue("RbnPort", port);
        s.setValue("RbnCallsign", call);
        s.save();
        emit rbnConnectRequested(host, port, call);
    });
    btnRow->addWidget(m_rbnConnectBtn);
    connLayout->addLayout(btnRow);

    layout->addWidget(connGroup);

    // ── Console output ──────────────────────────────────────────────────
    auto* consoleLabel = new QLabel("RBN Console");
    consoleLabel->setStyleSheet("QLabel { color: #00b4d8; font-weight: bold; }");
    layout->addWidget(consoleLabel);

    m_rbnConsole = new QPlainTextEdit;
    m_rbnConsole->setReadOnly(true);
    m_rbnConsole->setMaximumBlockCount(2000);
    m_rbnConsole->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #a0b0c0;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "  padding: 4px;"
        "}");
    layout->addWidget(m_rbnConsole, 1);

    // Command input row
    auto* cmdRow = new QHBoxLayout;
    m_rbnCmdEdit = new QLineEdit;
    m_rbnCmdEdit->setPlaceholderText("Type an RBN command (e.g. set/skimmer, set/ft8, bye)");
    m_rbnCmdEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; font-family: monospace; }");
    m_rbnCmdEdit->setEnabled(m_rbnClient->isConnected());
    connect(m_rbnCmdEdit, &QLineEdit::returnPressed, this, [this] {
        QString cmd = m_rbnCmdEdit->text().trimmed();
        if (cmd.isEmpty() || !m_rbnClient->isConnected()) return;
        QMetaObject::invokeMethod(m_rbnClient, [client=m_rbnClient, cmd] { client->sendCommand(cmd); });
        m_rbnConsole->appendPlainText("> " + cmd);
        m_rbnCmdEdit->clear();
    });
    m_rbnSendBtn = new QPushButton("Send");
    m_rbnSendBtn->setFixedWidth(60);
    m_rbnSendBtn->setEnabled(m_rbnClient->isConnected());
    connect(m_rbnSendBtn, &QPushButton::clicked, this, [this] {
        m_rbnCmdEdit->returnPressed();
    });
    cmdRow->addWidget(m_rbnCmdEdit, 1);
    cmdRow->addWidget(m_rbnSendBtn);
    layout->addLayout(cmdRow);

    tabs->addTab(page, "RBN");
}

void DxClusterDialog::buildSpotListTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(4);

    // Band filter checkboxes
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(2);
    auto* filterLabel = new QLabel("Bands:");
    filterLabel->setStyleSheet("QLabel { color: #808080; font-size: 13px; }");
    filterRow->addWidget(filterLabel);

    // Table model + band filter proxy
    m_spotModel = new SpotTableModel(this);
    m_proxyModel = new BandFilterProxy(this);
    m_proxyModel->setSourceModel(m_spotModel);
    m_proxyModel->setSortRole(Qt::UserRole);

    static constexpr const char* bands[] = {
        "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"
    };
    QString cbStyle =
        "QCheckBox { color: #a0b0c0; font-size: 12px; spacing: 3px; }"
        "QCheckBox::indicator { width: 13px; height: 13px; }";
    auto& sf = AppSettings::instance();
    for (const char* band : bands) {
        auto* cb = new QCheckBox(band);
        QString key = QString("SpotBandFilter_%1").arg(band);
        bool on = sf.value(key, "True").toString() == "True";
        cb->setChecked(on);
        if (!on)
            m_proxyModel->setBandVisible(QString(band), false);
        cb->setStyleSheet(cbStyle);
        connect(cb, &QCheckBox::toggled, this, [this, b = QString(band), key](bool on) {
            m_proxyModel->setBandVisible(b, on);
            auto& s = AppSettings::instance();
            s.setValue(key, on ? "True" : "False");
            s.save();
        });
        filterRow->addWidget(cb, 1);  // equal stretch across row
    }
    layout->addLayout(filterRow);

    m_spotTable = new QTableView;
    m_spotTable->setModel(m_proxyModel);
    m_spotTable->setSortingEnabled(true);
    m_spotTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_spotTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_spotTable->setAlternatingRowColors(true);
    m_spotTable->verticalHeader()->setVisible(false);
    m_spotTable->verticalHeader()->setDefaultSectionSize(20);
    m_spotTable->horizontalHeader()->setStretchLastSection(true);
    m_spotTable->setStyleSheet(
        "QTableView {"
        "  background: #0a0a14;"
        "  alternate-background-color: #0f0f1e;"
        "  color: #c8d8e8;"
        "  gridline-color: #1a2a3a;"
        "  border: 1px solid #203040;"
        "  font-size: 11px;"
        "}"
        "QTableView::item:selected {"
        "  background: #1a3a5a;"
        "  color: #e0f0ff;"
        "}"
        "QHeaderView::section {"
        "  background: #1a1a2e;"
        "  color: #00b4d8;"
        "  border: 1px solid #203040;"
        "  padding: 3px 6px;"
        "  font-weight: bold;"
        "  font-size: 11px;"
        "}");

    // Column widths
    m_spotTable->setColumnWidth(SpotTableModel::ColTime, 50);
    m_spotTable->setColumnWidth(SpotTableModel::ColFreq, 80);
    m_spotTable->setColumnWidth(SpotTableModel::ColDxCall, 90);
    m_spotTable->setColumnWidth(SpotTableModel::ColComment, 200);
    m_spotTable->setColumnWidth(SpotTableModel::ColSpotter, 80);
    m_spotTable->setColumnWidth(SpotTableModel::ColBand, 45);
    m_spotTable->setColumnWidth(SpotTableModel::ColSource, 55);

    // No default sort — insertion order is newest-first
    m_spotTable->horizontalHeader()->setSortIndicatorShown(false);

    // Double-click to tune
    connect(m_spotTable, &QTableView::doubleClicked, this, [this](const QModelIndex& idx) {
        auto srcIdx = m_proxyModel->mapToSource(idx);
        double freq = m_spotModel->freqAtRow(srcIdx.row());
        if (freq > 0.0)
            emit tuneRequested(freq);
    });

    layout->addWidget(m_spotTable, 1);

    // Bottom bar: spot count + clear
    auto* bottomRow = new QHBoxLayout;
    auto* countLabel = new QLabel("0 spots");
    countLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    connect(m_spotModel, &QAbstractTableModel::rowsInserted, this, [this, countLabel] {
        countLabel->setText(QString("%1 spots").arg(m_spotModel->rowCount()));
    });
    bottomRow->addWidget(countLabel);
    bottomRow->addStretch();

    auto* clearBtn = new QPushButton("Clear");
    clearBtn->setFixedWidth(60);
    connect(clearBtn, &QPushButton::clicked, this, [this, countLabel] {
        m_spotModel->clear();
        countLabel->setText("0 spots");
    });
    bottomRow->addWidget(clearBtn);
    layout->addLayout(bottomRow);

    tabs->addTab(page, "Spot List");
}

void DxClusterDialog::buildDisplayTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();
    bool spotsEnabled     = s.value("IsSpotsEnabled", "True").toString() == "True";
    bool overrideColors   = s.value("IsSpotsOverrideColorsEnabled", "False").toString() == "True";
    bool overrideBg       = s.value("IsSpotsOverrideBackgroundColorsEnabled", "True").toString() == "True";
    bool overrideBgAuto   = s.value("IsSpotsOverrideToAutoBackgroundColorEnabled", "True").toString() == "True";
    int levels            = s.value("SpotsMaxLevel", 3).toInt();
    int position          = s.value("SpotsStartingHeightPercentage", 50).toInt();
    int fontSize          = s.value("SpotFontSize", 16).toInt();
    int lifetimeMin       = s.value("DxClusterSpotLifetime", 30).toInt();
    QColor spotColor(s.value("SpotsOverrideColor", "#FFFF00").toString());
    QColor bgColor(s.value("SpotsOverrideBgColor", "#000000").toString());
    int bgOpacity         = s.value("SpotsBackgroundOpacity", 48).toInt();

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    auto save = [this](const QString& key, const QVariant& val) {
        auto& s = AppSettings::instance();
        s.setValue(key, val);
        s.save();
        emit settingsChanged();
    };

    auto updateSwatch = [](QPushButton* btn, const QColor& color) {
        btn->setStyleSheet(QString(
            "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
            "QPushButton:hover { border-color: #c8d8e8; }").arg(color.name()));
    };

    // ── Spots: Enabled/Disabled ─────────────────────────────────────────
    grid->addWidget(new QLabel("Spots:"), row, 0);
    auto* spotsToggle = new QPushButton(spotsEnabled ? "Enabled" : "Disabled");
    spotsToggle->setCheckable(true);
    spotsToggle->setChecked(spotsEnabled);
    spotsToggle->setFixedWidth(80);
    spotsToggle->setStyleSheet(
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 3px; }"
        "QPushButton:!checked { background: #603020; }");
    connect(spotsToggle, &QPushButton::toggled, this, [spotsToggle, save](bool on) {
        spotsToggle->setText(on ? "Enabled" : "Disabled");
        save("IsSpotsEnabled", on ? "True" : "False");
    });
    grid->addWidget(spotsToggle, row++, 1, Qt::AlignLeft);

    // ── Levels slider ───────────────────────────────────────────────────
    grid->addWidget(new QLabel("Levels:"), row, 0);
    auto* levelsRow = new QHBoxLayout;
    auto* levelsSlider = new QSlider(Qt::Horizontal);
    levelsSlider->setRange(1, 10);
    levelsSlider->setValue(levels);
    auto* levelsValue = new QLabel(QString::number(levels));
    levelsValue->setFixedWidth(24);
    levelsValue->setAlignment(Qt::AlignRight);
    levelsRow->addWidget(levelsSlider);
    levelsRow->addWidget(levelsValue);
    connect(levelsSlider, &QSlider::valueChanged, this, [levelsValue, save](int v) {
        levelsValue->setText(QString::number(v));
        save("SpotsMaxLevel", QString::number(v));
    });
    grid->addLayout(levelsRow, row++, 1);

    // ── Position slider ─────────────────────────────────────────────────
    grid->addWidget(new QLabel("Position:"), row, 0);
    auto* posRow = new QHBoxLayout;
    auto* posSlider = new QSlider(Qt::Horizontal);
    posSlider->setRange(0, 100);
    posSlider->setValue(position);
    auto* posValue = new QLabel(QString::number(position));
    posValue->setFixedWidth(24);
    posValue->setAlignment(Qt::AlignRight);
    posRow->addWidget(posSlider);
    posRow->addWidget(posValue);
    connect(posSlider, &QSlider::valueChanged, this, [posValue, save](int v) {
        posValue->setText(QString::number(v));
        save("SpotsStartingHeightPercentage", QString::number(v));
    });
    grid->addLayout(posRow, row++, 1);

    // ── Font Size slider ────────────────────────────────────────────────
    grid->addWidget(new QLabel("Font Size:"), row, 0);
    auto* fontRow = new QHBoxLayout;
    auto* fontSlider = new QSlider(Qt::Horizontal);
    fontSlider->setRange(8, 32);
    fontSlider->setValue(fontSize);
    auto* fontValue = new QLabel(QString::number(fontSize));
    fontValue->setFixedWidth(24);
    fontValue->setAlignment(Qt::AlignRight);
    fontRow->addWidget(fontSlider);
    fontRow->addWidget(fontValue);
    connect(fontSlider, &QSlider::valueChanged, this, [fontValue, save](int v) {
        fontValue->setText(QString::number(v));
        save("SpotFontSize", QString::number(v));
    });
    grid->addLayout(fontRow, row++, 1);

    // ── Spot Lifetime slider ────────────────────────────────────────────
    grid->addWidget(new QLabel("Spot Lifetime:"), row, 0);
    auto* lifeRow = new QHBoxLayout;
    auto* lifeSlider = new QSlider(Qt::Horizontal);
    lifeSlider->setRange(1, 1440);
    lifeSlider->setValue(lifetimeMin);
    auto formatLifetime = [](int mins) -> QString {
        if (mins < 60)
            return QString("%1 min%2").arg(mins).arg(mins == 1 ? "" : "s");
        int hrs = mins / 60;
        int rem = mins % 60;
        if (rem == 0)
            return QString("%1 hr%2").arg(hrs).arg(hrs == 1 ? "" : "s");
        return QString("%1 hr%2 %3 min%4")
            .arg(hrs).arg(hrs == 1 ? "" : "s")
            .arg(rem).arg(rem == 1 ? "" : "s");
    };
    auto* lifeValue = new QLabel(formatLifetime(lifetimeMin));
    lifeValue->setFixedWidth(90);
    lifeValue->setAlignment(Qt::AlignRight);
    lifeRow->addWidget(lifeSlider);
    lifeRow->addWidget(lifeValue);
    connect(lifeSlider, &QSlider::valueChanged, this, [lifeValue, formatLifetime, save](int v) {
        lifeValue->setText(formatLifetime(v));
        save("DxClusterSpotLifetime", QString::number(v));
    });
    grid->addLayout(lifeRow, row++, 1);

    // ── Override Colors + color picker ──────────────────────────────────
    grid->addWidget(new QLabel("Override Colors:"), row, 0);
    auto* colorRow = new QHBoxLayout;
    auto* overrideToggle = new QPushButton(overrideColors ? "Enabled" : "Disabled");
    overrideToggle->setCheckable(true);
    overrideToggle->setChecked(overrideColors);
    overrideToggle->setFixedWidth(80);
    overrideToggle->setStyleSheet(
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 3px; }"
        "QPushButton:!checked { background: #603020; }");
    connect(overrideToggle, &QPushButton::toggled, this, [overrideToggle, save](bool on) {
        overrideToggle->setText(on ? "Enabled" : "Disabled");
        save("IsSpotsOverrideColorsEnabled", on ? "True" : "False");
    });
    colorRow->addWidget(overrideToggle);

    auto* colorBtn = new QPushButton;
    colorBtn->setFixedSize(24, 24);
    updateSwatch(colorBtn, spotColor);
    connect(colorBtn, &QPushButton::clicked, this, [this, colorBtn, updateSwatch, save, spotColor]() mutable {
        QColor c = QColorDialog::getColor(spotColor, this, "Spot Text Color");
        if (c.isValid()) {
            spotColor = c;
            updateSwatch(colorBtn, c);
            save("SpotsOverrideColor", c.name());
        }
    });
    colorRow->addWidget(colorBtn);
    colorRow->addStretch();
    grid->addLayout(colorRow, row++, 1);

    // ── Override Background + Auto + color picker ───────────────────────
    grid->addWidget(new QLabel("Override Background:"), row, 0);
    auto* bgRow = new QHBoxLayout;
    QString bgStyle =
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 3px; }"
        "QPushButton:!checked { background: #603020; }";
    auto* bgEnabledBtn = new QPushButton("Enabled");
    bgEnabledBtn->setCheckable(true);
    bgEnabledBtn->setChecked(overrideBg);
    bgEnabledBtn->setFixedWidth(70);
    bgEnabledBtn->setStyleSheet(bgStyle);
    auto* bgAutoBtn = new QPushButton("Auto");
    bgAutoBtn->setCheckable(true);
    bgAutoBtn->setChecked(overrideBgAuto);
    bgAutoBtn->setFixedWidth(50);
    bgAutoBtn->setStyleSheet(bgStyle);
    connect(bgEnabledBtn, &QPushButton::toggled, this, [save](bool on) {
        save("IsSpotsOverrideBackgroundColorsEnabled", on ? "True" : "False");
    });
    connect(bgAutoBtn, &QPushButton::toggled, this, [save](bool on) {
        save("IsSpotsOverrideToAutoBackgroundColorEnabled", on ? "True" : "False");
    });
    bgRow->addWidget(bgEnabledBtn);
    bgRow->addWidget(bgAutoBtn);

    auto* bgColorBtn = new QPushButton;
    bgColorBtn->setFixedSize(24, 24);
    updateSwatch(bgColorBtn, bgColor);
    connect(bgColorBtn, &QPushButton::clicked, this, [this, bgColorBtn, updateSwatch, save, bgColor]() mutable {
        QColor c = QColorDialog::getColor(bgColor, this, "Spot Background Color");
        if (c.isValid()) {
            bgColor = c;
            updateSwatch(bgColorBtn, c);
            save("SpotsOverrideBgColor", c.name());
        }
    });
    bgRow->addWidget(bgColorBtn);
    bgRow->addStretch();
    grid->addLayout(bgRow, row++, 1);

    // ── Background Opacity slider ───────────────────────────────────────
    grid->addWidget(new QLabel("Background Opacity:"), row, 0);
    auto* opacRow = new QHBoxLayout;
    auto* opacSlider = new QSlider(Qt::Horizontal);
    opacSlider->setRange(0, 100);
    opacSlider->setValue(bgOpacity);
    auto* opacValue = new QLabel(QString::number(bgOpacity));
    opacValue->setFixedWidth(24);
    opacValue->setAlignment(Qt::AlignRight);
    opacRow->addWidget(opacSlider);
    opacRow->addWidget(opacValue);
    connect(opacSlider, &QSlider::valueChanged, this, [opacValue, save](int v) {
        opacValue->setText(QString::number(v));
        save("SpotsBackgroundOpacity", QString::number(v));
    });
    grid->addLayout(opacRow, row++, 1);

    // ── Total Spots ─────────────────────────────────────────────────────
    grid->addWidget(new QLabel("Total Spots:"), row, 0);
    m_totalSpotsLabel = new QLabel("0");
    m_totalSpotsLabel->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; }");
    grid->addWidget(m_totalSpotsLabel, row++, 1);

    layout->addLayout(grid);
    layout->addStretch();

    // ── Clear All Spots button ──────────────────────────────────────────
    auto* btnRow2 = new QHBoxLayout;
    auto* clearAllBtn = new QPushButton("Clear All Spots");
    clearAllBtn->setFixedWidth(120);
    connect(clearAllBtn, &QPushButton::clicked, this, [this] {
        m_radioModel->sendCommand("spot clear");
        m_spotModel->clear();
        if (m_totalSpotsLabel)
            m_totalSpotsLabel->setText("0");
        emit spotsClearedAll();
        emit settingsChanged();
    });
    btnRow2->addWidget(clearAllBtn);
    btnRow2->addStretch();
    layout->addLayout(btnRow2);

    tabs->addTab(page, "Display");
}

void DxClusterDialog::setTotalSpots(int count)
{
    if (m_totalSpotsLabel)
        m_totalSpotsLabel->setText(QString::number(count));
}

void DxClusterDialog::flushSpotBatch()
{
    if (m_spotBatch.isEmpty()) return;

    auto isAtBottom = [](QAbstractScrollArea* w) {
        auto* sb = w->verticalScrollBar();
        return sb->value() >= sb->maximum() - 2;
    };
    bool follow = isAtBottom(m_spotTable);

    m_spotModel->addSpots(m_spotBatch);
    m_spotBatch.clear();

    if (follow)
        m_spotTable->scrollToBottom();
}

void DxClusterDialog::updateStatus()
{
    // Cluster status
    if (m_client->isConnected()) {
        m_statusLabel->setText(QString("Connected to %1:%2").arg(m_client->host()).arg(m_client->port()));
        m_statusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_connectBtn->setText("Disconnect");
        m_cmdEdit->setEnabled(true);
        m_sendBtn->setEnabled(true);
    } else {
        m_statusLabel->setText("Disconnected");
        m_statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_connectBtn->setText("Connect");
        m_cmdEdit->setEnabled(false);
        m_sendBtn->setEnabled(false);
    }
    // RBN status
    if (m_rbnClient->isConnected()) {
        m_rbnStatusLabel->setText(QString("Connected to %1:%2").arg(m_rbnClient->host()).arg(m_rbnClient->port()));
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_rbnConnectBtn->setText("Disconnect");
        m_rbnCmdEdit->setEnabled(true);
        m_rbnSendBtn->setEnabled(true);
    } else {
        m_rbnStatusLabel->setText("Disconnected");
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_rbnConnectBtn->setText("Connect");
        m_rbnCmdEdit->setEnabled(false);
        m_rbnSendBtn->setEnabled(false);
    }
}

} // namespace AetherSDR
