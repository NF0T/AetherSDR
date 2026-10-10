#pragma once

#include <QObject>
#include <QMutex>
#include <QWebSocket>
#include <QTimer>
#include <QFile>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QDateTime>
#include <QMetaType>
#include <atomic>
#include "DxClusterClient.h"  // for DxSpot

namespace AetherSDR {

// FreeDV Reporter spot client — connects to qso.freedv.org via WebSocket
// with manual Engine.IO v4 / Socket.IO v4 framing. Tracks station state
// (callsign, frequency, grid, mode) and emits spotReceived() for each
// rx_report event (station heard another station).
//
// View-only by default. Call enableReporting() when RADE is active to
// register as a full participant: transmits freq_change, tx_report, and
// periodic rx_report (SNR) events to the server.
class FreeDvClient : public QObject {
    Q_OBJECT

public:
    // Per-station state tracked by Socket.IO session ID.
    // Public so FreeDvReporterModel and the dialog can use it directly.
    struct StationInfo {
        QString  callsign;
        QString  gridSquare;
        double   freqMhz{0.0};
        QString  mode;
        bool     rxOnly{false};
        QString  version;
        QString  message;
        QString  status;           // "RX", "TX", or ""
        QDateTime lastTxTime;
        QString  rxCallsign;
        float    snr{-99.0f};
        QDateTime lastUpdate;
    };

    explicit FreeDvClient(QObject* parent = nullptr);
    ~FreeDvClient() override;

    void startConnection();
    void stopConnection();
    bool isConnected() const { return m_connected.load(); }
    // Atomic like m_connected: written on the client's worker thread, read
    // from the GUI thread (the reporter dialog gates its message field on
    // it — #4231).
    bool isReportingEnabled() const { return m_reportingEnabled.load(); }

    // The `mode` string this station publishes in tx_report / rx_report.
    //
    // FreeDV Reporter has no enumeration — it takes whatever string the client
    // sends and groups the map by it, so this has to match what the rest of the
    // ecosystem sends or our spots sort into a category of one. freedv-gui's V2
    // branch returns "RADEV2" (ms-rade-v2, freedv_interface.cpp), which is
    // implementation rather than published spec — their own protocol doc still
    // shows "RADEV1" in its examples.
    //
    // Defaults to RADEV1, which is what every current caller means. This exists
    // so the string stops being hardcoded in three places; it does NOT by itself
    // make V2 report (see the note on reportTxState()).
    void setReportingMode(const QString& mode);
    QString reportingMode() const;
    QString myGrid() const { return m_myGrid; }
    QHash<QString, StationInfo> stations() const { return m_stations; }

    QString logFilePath() const;

signals:
    void started();
    void stopped();
    void connectionError(const QString& error);
    void spotReceived(const DxSpot& spot);
    void rawLineReceived(const QString& line);
    void stationsCleared();
    void stationUpdated(const QString& sid, const AetherSDR::FreeDvClient::StationInfo& info);
    void stationRemoved(const QString& sid);
    // Full-participant reporting turned on/off. The reporter dialog uses this
    // to enable its message field, since updateMessage() only reaches the
    // server while reporting is active (#4231).
    void reportingStateChanged(bool enabled);

public slots:
    // Defer QWebSocket + timer construction to the worker thread (#1929) —
    // see DxClusterClient::initialize().
    void initialize();

    // Station reporting — call from RADE activate/deactivate paths.
    // All methods are safe to call from any thread via queued connection.
    void enableReporting(const QString& callsign, const QString& grid,
                         const QString& message, const QString& softwareVersion,
                         double freqMhz);
    void disableReporting();

    void reportFreqChange(double freqMhz);
    // NOTE: nothing wires RADE **V2** into this client yet, deliberately.
    // RFC §9.3 gates V2 reporting on three things. Two are now met: the
    // framing is published and adopted (third_party/radae_v2_text), and a
    // CRC-8-validated LDPC codeword is a far stronger decode-confidence
    // signal than rade_sync(), which §7.1b showed stays lit through a
    // sideband error. The third is not a technical gate: publishing V2 spots
    // puts them on a SHARED PUBLIC map while FreeDV's own status notice still
    // says on-air V2 use is not recommended outside their test campaign. That
    // is the operator's call, not a default. Until then V2 reports nothing
    // rather than reporting as V1.
    void reportTxState(bool transmitting);
    void updateMessage(const QString& message);

    // Feed live SNR from RADEEngine::snrChanged — throttled to 1 s by internal timer.
    void updateRxSnr(float snrDb);
    void updateRxSynced(bool synced);

    // Feed decoded far-end callsign from RADEEngine::eooCallsignReceived.
    // Included in the next rx_report sent to qso.freedv.org.
    void updateRxCallsign(const QString& callsign);

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessage(const QString& message);
    void onWsError(QAbstractSocket::SocketError err);
    void onReconnectTimer();
    void onSnrTimer();
    void onMessageUpdate(const QJsonObject& data);

private:
    void handleEngineIO(const QString& raw);
    void handleSocketIO(const QString& payload);
    void handleEvent(const QString& eventName, const QJsonObject& data);
    void onNewConnection(const QJsonObject& data);
    void onFreqChange(const QJsonObject& data);
    void onRxReport(const QJsonObject& data);
    void onTxReport(const QJsonObject& data);
    void onRemoveConnection(const QJsonObject& data);
    void onBulkUpdate(const QJsonArray& pairs);

    void sendEvent(const QString& name, const QJsonObject& data);
    void sendInitialFreqChange();

    QWebSocket*  m_ws{nullptr};
    QTimer*      m_pingTimer{nullptr};
    QTimer*      m_reconnectTimer{nullptr};
    QTimer*      m_snrTimer{nullptr};
    QFile        m_logFile;

    QHash<QString, StationInfo> m_stations;  // sid → station info

    // Station reporting state
    std::atomic<bool> m_reportingEnabled{false};
    // Guarded because reporting runs off a timer thread as well as the GUI.
    mutable QMutex m_reportingModeMutex;
    QString m_reportingMode{QStringLiteral("RADEV1")};
    bool    m_authNeedsRefresh{false};  // reconnect in progress for auth change
    QString m_myCallsign;
    QString m_myGrid;
    QString m_mySoftwareVersion;
    QString m_myMessage;
    double  m_myFreqMhz{0.0};
    float   m_lastSnr{-99.0f};
    bool    m_radeSynced{false};
    QString m_rxCallsign;

    std::atomic<bool> m_connected{false};
    bool    m_intentionalDisconnect{false};
    bool    m_inBulkUpdate{false};
    int     m_reconnectAttempts{0};
    int     m_pingIntervalMs{25000};

    static constexpr const char* WsUrl =
        "wss://qso.freedv.org/socket.io/?EIO=4&transport=websocket";
    static constexpr int MaxReconnectDelayMs     = 60000;
    static constexpr int InitialReconnectDelayMs  = 5000;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::FreeDvClient::StationInfo)
