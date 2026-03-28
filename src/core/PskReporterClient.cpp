#ifdef HAVE_MQTT

#include "PskReporterClient.h"
#include "LogManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMqttTopicFilter>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace AetherSDR {

PskReporterClient::PskReporterClient(QObject* parent)
    : QObject(parent)
    , m_mqttClient(new QMqttClient(this))
    , m_reconnectTimer(new QTimer(this))
{
    m_mqttClient->setHostname("mqtt.pskreporter.info");
    m_mqttClient->setPort(1883);

    connect(m_mqttClient, &QMqttClient::connected, this, &PskReporterClient::onConnected);
    connect(m_mqttClient, &QMqttClient::disconnected, this, &PskReporterClient::onDisconnected);
    connect(m_mqttClient, &QMqttClient::errorChanged, this, &PskReporterClient::onErrorChanged);
    connect(m_mqttClient, &QMqttClient::messageReceived,
            this, &PskReporterClient::onMessageReceived);

    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &PskReporterClient::onReconnectTimer);
}

PskReporterClient::~PskReporterClient()
{
    m_intentionalDisconnect = true;
    m_reconnectTimer->stop();
    m_logFile.close();
    if (m_mqttClient->state() != QMqttClient::Disconnected)
        m_mqttClient->disconnectFromHost();
}

QString PskReporterClient::logFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/AetherSDR/pskreporter.log";
}

void PskReporterClient::connectToServer(const QString& callsign)
{
    if (m_connected) return;

    m_callsign = callsign;
    m_intentionalDisconnect = false;

    qCDebug(lcDxCluster) << "PskReporterClient: connecting to mqtt.pskreporter.info";

    // Open log file (truncate on new connection)
    m_logFile.close();
    m_logFile.setFileName(logFilePath());
    QDir().mkpath(QFileInfo(m_logFile).absolutePath());
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_logFile.write(QString("--- Connected to PSKReporter MQTT at %1 ---\n")
            .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss UTC"))
            .toUtf8());
        m_logFile.flush();
    }

    m_mqttClient->setClientId(QString("AetherSDR-%1-%2")
        .arg(callsign).arg(QDateTime::currentMSecsSinceEpoch() % 100000));
    m_mqttClient->connectToHost();
}

void PskReporterClient::disconnect()
{
    m_intentionalDisconnect = true;
    m_reconnectTimer->stop();
    if (m_mqttClient->state() != QMqttClient::Disconnected)
        m_mqttClient->disconnectFromHost();
}

void PskReporterClient::setFilter(const QString& band, const QString& mode)
{
    m_filterBand = band;
    m_filterMode = mode;
    if (m_connected)
        subscribe();
}

// ── MQTT callbacks ──────────────────────────────────────────────────────────

void PskReporterClient::onConnected()
{
    qCDebug(lcDxCluster) << "PskReporterClient: MQTT connected";
    m_connected = true;
    m_reconnectAttempts = 0;
    subscribe();
    emit connected();
}

void PskReporterClient::onDisconnected()
{
    qCDebug(lcDxCluster) << "PskReporterClient: MQTT disconnected";
    bool wasConnected = m_connected;
    m_connected = false;

    if (wasConnected)
        emit disconnected();

    if (!m_intentionalDisconnect) {
        int delay = std::min(InitialReconnectDelayMs * (1 << m_reconnectAttempts),
                             MaxReconnectDelayMs);
        qCDebug(lcDxCluster) << "PskReporterClient: reconnecting in" << delay << "ms";
        m_reconnectTimer->start(delay);
        m_reconnectAttempts++;
    }
}

void PskReporterClient::onErrorChanged(QMqttClient::ClientError error)
{
    if (error == QMqttClient::NoError) return;
    QString msg;
    switch (error) {
    case QMqttClient::InvalidProtocolVersion: msg = "Invalid protocol version"; break;
    case QMqttClient::IdRejected:             msg = "Client ID rejected"; break;
    case QMqttClient::ServerUnavailable:      msg = "Server unavailable"; break;
    case QMqttClient::BadUsernameOrPassword:  msg = "Bad credentials"; break;
    case QMqttClient::NotAuthorized:          msg = "Not authorized"; break;
    case QMqttClient::TransportInvalid:       msg = "Transport invalid"; break;
    case QMqttClient::ProtocolViolation:      msg = "Protocol violation"; break;
    default:                                  msg = QString("Error %1").arg(static_cast<int>(error)); break;
    }
    qCWarning(lcDxCluster) << "PskReporterClient: MQTT error:" << msg;
    emit connectionError(msg);
}

void PskReporterClient::onReconnectTimer()
{
    if (m_intentionalDisconnect) return;
    qCDebug(lcDxCluster) << "PskReporterClient: attempting reconnect";
    m_mqttClient->connectToHost();
}

void PskReporterClient::subscribe()
{
    // Topic: pskr/filter/v2/{band}/{mode}/{sendercall}/{receivercall}/
    //        {senderlocator}/{receiverlocator}/{sendercountry}/{receivercountry}
    // Subscribe with sendercall=our callsign — shows stations that heard us
    QString band = m_filterBand.isEmpty() ? "+" : m_filterBand;
    QString mode = m_filterMode.isEmpty() ? "+" : m_filterMode;
    QString topic = QString("pskr/filter/v2/%1/%2/%3/+/+/+/+/+")
        .arg(band, mode, m_callsign);

    qCDebug(lcDxCluster) << "PskReporterClient: subscribing to" << topic;
    m_mqttClient->subscribe(QMqttTopicFilter(topic), 0);
}

void PskReporterClient::onMessageReceived(const QByteArray& message, const QMqttTopicName& /*topic*/)
{
    DxSpot spot;
    if (parseSpotJson(message, spot)) {
        // Log the spot
        QString logLine = QString("DX de %1: %2 %3 %4 %5Z")
            .arg(spot.spotterCall,
                 QString::number(spot.freqMhz * 1000.0, 'f', 1),
                 spot.dxCall,
                 spot.comment,
                 spot.utcTime.toString("HHmm"));
        if (m_logFile.isOpen()) {
            m_logFile.write((logLine + "\n").toUtf8());
            m_logFile.flush();
        }
        emit rawLineReceived(logLine);
        emit spotReceived(spot);
    }
}

bool PskReporterClient::parseSpotJson(const QByteArray& json, DxSpot& spot)
{
    QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return false;

    QJsonObject obj = doc.object();

    // We subscribe with sendercall=our callsign, so:
    //   sc = us (sender), rc = station that heard us (receiver)
    // Display the receiver as the "DX call" — they're the ones hearing us
    spot.dxCall      = obj.value("rc").toString();    // who heard us
    spot.spotterCall = obj.value("sc").toString();    // us (sender)
    spot.comment     = obj.value("md").toString();    // mode

    double freqHz = obj.value("f").toDouble();
    spot.freqMhz = freqHz / 1.0e6;

    // SNR if available
    int snr = obj.value("rp").toInt(0);
    if (snr != 0)
        spot.comment += QString(" %1 dB").arg(snr);

    // Timestamp
    qint64 ts = obj.value("t").toInteger();
    if (ts > 0)
        spot.utcTime = QDateTime::fromSecsSinceEpoch(ts, Qt::UTC).time();
    else
        spot.utcTime = QDateTime::currentDateTimeUtc().time();

    return spot.freqMhz > 0.0 && !spot.dxCall.isEmpty();
}

} // namespace AetherSDR

#endif // HAVE_MQTT
