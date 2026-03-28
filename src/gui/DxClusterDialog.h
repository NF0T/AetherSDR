#pragma once

#include <QDialog>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QVector>
#include "core/DxClusterClient.h"

class QLineEdit;
class QSpinBox;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QTabWidget;
class QTableView;

namespace AetherSDR {

class DxClusterClient;
class RadioModel;

// ── Spot list table model ───────────────────────────────────────────────────

class SpotTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { ColTime, ColFreq, ColDxCall, ColComment, ColSpotter, ColBand, ColSource, ColCount };

    explicit SpotTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& = {}) const override { return m_spots.size(); }
    int columnCount(const QModelIndex& = {}) const override { return ColCount; }
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void addSpot(const DxSpot& spot);
    void addSpots(const QVector<DxSpot>& spots);
    void clear();
    void setMaxSpots(int max) { m_maxSpots = max; }
    double freqAtRow(int row) const;

private:
    static QString bandForFreq(double mhz);

    QVector<DxSpot> m_spots;
    int m_maxSpots{500};
};

// ── Band filter proxy ───────────────────────────────────────────────────────

class BandFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit BandFilterProxy(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}

    void setBandVisible(const QString& band, bool visible);
    bool isBandVisible(const QString& band) const { return !m_hiddenBands.contains(band); }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QSet<QString> m_hiddenBands;
};

// ── Dialog ──────────────────────────────────────────────────────────────────

class DxClusterDialog : public QDialog {
    Q_OBJECT

public:
    explicit DxClusterDialog(DxClusterClient* clusterClient, DxClusterClient* rbnClient,
                             RadioModel* radioModel, QWidget* parent = nullptr);

    void updateStatus();
    void setTotalSpots(int count);

signals:
    void connectRequested(const QString& host, quint16 port, const QString& callsign);
    void disconnectRequested();
    void rbnConnectRequested(const QString& host, quint16 port, const QString& callsign);
    void rbnDisconnectRequested();
    void tuneRequested(double freqMhz);
    void settingsChanged();
    void spotsClearedAll();

private:
    void buildClusterTab(QTabWidget* tabs);
    void buildRbnTab(QTabWidget* tabs);
    void buildSpotListTab(QTabWidget* tabs);
    void buildDisplayTab(QTabWidget* tabs);

    DxClusterClient* m_client;
    DxClusterClient* m_rbnClient;
    RadioModel*      m_radioModel;

    // Cluster tab
    QLineEdit*      m_hostEdit;
    QSpinBox*       m_portSpin;
    QLineEdit*      m_callEdit;
    QPushButton*    m_connectBtn;
    QPushButton*    m_autoConnectBtn;
    QLabel*         m_statusLabel;
    QPlainTextEdit* m_console;
    QLineEdit*      m_cmdEdit;
    QPushButton*    m_sendBtn;

    // RBN tab
    QLineEdit*      m_rbnHostEdit;
    QSpinBox*       m_rbnPortSpin;
    QLineEdit*      m_rbnCallEdit;
    QPushButton*    m_rbnConnectBtn;
    QPushButton*    m_rbnAutoConnectBtn;
    QLabel*         m_rbnStatusLabel;
    QPlainTextEdit* m_rbnConsole;
    QLineEdit*      m_rbnCmdEdit;
    QPushButton*    m_rbnSendBtn;

    // Spot batching (1/sec flush)
    QVector<DxSpot>        m_spotBatch;
    QTimer*                m_spotBatchTimer{nullptr};
    void flushSpotBatch();

    // Spot list tab
    SpotTableModel*        m_spotModel;
    QTableView*            m_spotTable;
    BandFilterProxy*       m_proxyModel;

    // Display tab
    QLabel*         m_totalSpotsLabel{nullptr};
};

} // namespace AetherSDR
