#include "MqttApplet.h"
#include "core/AppSettings.h"
#include "core/MqttClient.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QTextBlock>

namespace AetherSDR {

static const QString kLabelStyle =
    "QLabel { color: #8090a0; font-size: 10px; background: transparent; }";
static const QString kEditStyle =
    "QLineEdit { background: #0a0a14; color: #c8d8e8; border: 1px solid #203040; "
    "padding: 2px 4px; font-size: 10px; }";
static const QString kBtnOff =
    "QPushButton { background: #1a2a3a; color: #8090a0; "
    "border: 1px solid #205070; padding: 2px 8px; border-radius: 3px; font-size: 10px; }";
static const QString kBtnOn =
    "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
    "border: 1px solid #008ba8; padding: 2px 8px; border-radius: 3px; font-size: 10px; }";

MqttApplet::MqttApplet(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
}

void MqttApplet::buildUI()
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(3);

    // Header
    auto* header = new QLabel("MQTT");
    header->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; font-weight: bold; }");
    vbox->addWidget(header);

    // Broker settings grid
    auto* grid = new QGridLayout;
    grid->setSpacing(2);
    grid->setContentsMargins(0, 0, 0, 0);

    auto& s = AppSettings::instance();

    auto addRow = [&](int row, const QString& label, QLineEdit*& edit,
                      const QString& key, const QString& def, bool password = false) {
        auto* lbl = new QLabel(label);
        lbl->setStyleSheet(kLabelStyle);
        grid->addWidget(lbl, row, 0);
        edit = new QLineEdit(s.value(key, def).toString());
        edit->setStyleSheet(kEditStyle);
        if (password) { edit->setEchoMode(QLineEdit::Password); }
        grid->addWidget(edit, row, 1);
    };

    addRow(0, "Host:", m_hostEdit, "MqttHost", "localhost");
    addRow(1, "Port:", m_portEdit, "MqttPort", "1883");
    addRow(2, "User:", m_userEdit, "MqttUser", "");
    addRow(3, "Pass:", m_passEdit, "MqttPass", "", true);

    auto* topicLbl = new QLabel("Topics:");
    topicLbl->setStyleSheet(kLabelStyle);
    grid->addWidget(topicLbl, 4, 0, Qt::AlignTop);
    m_topicsEdit = new QLineEdit(s.value("MqttTopics", "").toString());
    m_topicsEdit->setStyleSheet(kEditStyle);
    m_topicsEdit->setPlaceholderText("topic1, topic2, ...");
    m_topicsEdit->setToolTip("Comma-separated MQTT topics to subscribe to");
    grid->addWidget(m_topicsEdit, 4, 1);

    vbox->addLayout(grid);

    // Enable button + status
    auto* ctrlRow = new QHBoxLayout;
    ctrlRow->setSpacing(4);
    m_enableBtn = new QPushButton("Off");
    m_enableBtn->setFixedWidth(36);
    m_enableBtn->setStyleSheet(kBtnOff);
    ctrlRow->addWidget(m_enableBtn);

    m_statusLabel = new QLabel("Disconnected");
    m_statusLabel->setStyleSheet("QLabel { color: #506070; font-size: 10px; }");
    ctrlRow->addWidget(m_statusLabel, 1);
    vbox->addLayout(ctrlRow);

    // Message log
    m_messageLog = new QTextEdit;
    m_messageLog->setReadOnly(true);
    m_messageLog->setMaximumHeight(120);
    m_messageLog->setStyleSheet(
        "QTextEdit { background: #0a0a14; color: #c8d8e8; border: 1px solid #203040; "
        "font-size: 10px; font-family: monospace; }");
    vbox->addWidget(m_messageLog);

    // Enable toggle
    connect(m_enableBtn, &QPushButton::clicked, this, [this] {
        bool wasOn = m_enableBtn->text() == "On";
        if (wasOn) {
            emit disconnectRequested();
            m_enableBtn->setText("Off");
            m_enableBtn->setStyleSheet(kBtnOff);
        } else {
            // Save settings
            auto& ss = AppSettings::instance();
            ss.setValue("MqttHost", m_hostEdit->text().trimmed());
            ss.setValue("MqttPort", m_portEdit->text().trimmed());
            ss.setValue("MqttUser", m_userEdit->text().trimmed());
            ss.setValue("MqttPass", m_passEdit->text().trimmed());
            ss.setValue("MqttTopics", m_topicsEdit->text().trimmed());
            ss.save();

            QStringList topics;
            for (const QString& t : m_topicsEdit->text().split(',', Qt::SkipEmptyParts)) {
                topics.append(t.trimmed());
            }

            emit connectRequested(
                m_hostEdit->text().trimmed(),
                m_portEdit->text().trimmed().toUShort(),
                m_userEdit->text().trimmed(),
                m_passEdit->text().trimmed(),
                topics);

            m_enableBtn->setText("On");
            m_enableBtn->setStyleSheet(kBtnOn);
        }
    });
}

void MqttApplet::setMqttClient(MqttClient* client)
{
    m_client = client;
    if (!client) return;

    connect(client, &MqttClient::connected, this, [this] {
        updateStatus("Connected", true);
    });
    connect(client, &MqttClient::disconnected, this, [this] {
        updateStatus("Disconnected", false);
        m_enableBtn->setText("Off");
        m_enableBtn->setStyleSheet(kBtnOff);
    });
    connect(client, &MqttClient::connectionError, this, [this](const QString& err) {
        updateStatus(err, false);
    });
    connect(client, &MqttClient::messageReceived, this, &MqttApplet::onMessageReceived);
}

void MqttApplet::updateStatus(const QString& text, bool ok)
{
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(
        ok ? "QLabel { color: #00c040; font-size: 10px; }"
           : "QLabel { color: #506070; font-size: 10px; }");
}

void MqttApplet::onMessageReceived(const QString& topic, const QByteArray& payload)
{
    // Extract short topic name (last segment after /)
    QString shortTopic = topic.section('/', -1);
    if (shortTopic.isEmpty()) { shortTopic = topic; }

    QString line = QString("%1: %2").arg(shortTopic, QString::fromUtf8(payload).left(80));
    m_messageLog->append(line);

    // Keep log trimmed to last 50 lines
    QTextDocument* doc = m_messageLog->document();
    while (doc->blockCount() > 50) {
        QTextCursor cursor(doc->begin());
        cursor.select(QTextCursor::BlockUnderCursor);
        cursor.removeSelectedText();
        cursor.deleteChar();  // remove newline
    }
}

} // namespace AetherSDR
