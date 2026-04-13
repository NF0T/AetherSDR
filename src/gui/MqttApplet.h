#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;

namespace AetherSDR {

class MqttClient;

// Applet for MQTT station device integration (#699).
// Shows broker connection controls and received messages.
class MqttApplet : public QWidget {
    Q_OBJECT

public:
    explicit MqttApplet(QWidget* parent = nullptr);

    void setMqttClient(MqttClient* client);

signals:
    void connectRequested(const QString& host, quint16 port,
                          const QString& user, const QString& pass,
                          const QStringList& topics);
    void disconnectRequested();

private:
    void buildUI();
    void updateStatus(const QString& text, bool ok);
    void onMessageReceived(const QString& topic, const QByteArray& payload);

    MqttClient* m_client{nullptr};
    QLineEdit*   m_hostEdit{nullptr};
    QLineEdit*   m_portEdit{nullptr};
    QLineEdit*   m_userEdit{nullptr};
    QLineEdit*   m_passEdit{nullptr};
    QLineEdit*   m_topicsEdit{nullptr};
    QPushButton* m_enableBtn{nullptr};
    QLabel*      m_statusLabel{nullptr};
    QTextEdit*   m_messageLog{nullptr};
};

} // namespace AetherSDR
