#pragma once

#ifdef HAVE_MQTT

#include <QObject>
#include <QMqttClient>
#include <QFile>
#include <QString>
#include <QTimer>
#include <atomic>
#include "DxClusterClient.h"  // for DxSpot

namespace AetherSDR {

// PSKReporter MQTT client — connects to mqtt.pskreporter.info and receives
// real-time reception reports as JSON. Emits spotReceived() for each report.
class PskReporterClient : public QObject {
    Q_OBJECT

public:
    explicit PskReporterClient(QObject* parent = nullptr);
    ~PskReporterClient() override;

    void connectToServer(const QString& callsign);
    void disconnect();
    bool isConnected() const { return m_connected; }

    QString logFilePath() const;

    // Filter: subscribe to specific band/mode (empty = all)
    void setFilter(const QString& band = {}, const QString& mode = {});

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& error);
    void spotReceived(const DxSpot& spot);
    void rawLineReceived(const QString& line);

private slots:
    void onConnected();
    void onDisconnected();
    void onMessageReceived(const QByteArray& message, const QMqttTopicName& topic);
    void onErrorChanged(QMqttClient::ClientError error);
    void onReconnectTimer();

private:
    void subscribe();
    bool parseSpotJson(const QByteArray& json, DxSpot& spot);

    QMqttClient* m_mqttClient;
    QTimer*      m_reconnectTimer;
    QFile        m_logFile;

    QString m_callsign;
    QString m_filterBand;
    QString m_filterMode;
    std::atomic<bool> m_connected{false};
    bool    m_intentionalDisconnect{false};
    int     m_reconnectAttempts{0};

    static constexpr int MaxReconnectDelayMs     = 60000;
    static constexpr int InitialReconnectDelayMs  = 5000;
};

} // namespace AetherSDR

#endif // HAVE_MQTT
