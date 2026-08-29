#include "LpMeterConnection.h"
#include "LogManager.h"

#include <QDateTime>

#include <cmath>

namespace AetherSDR {

namespace {
// Link is up but the meter has gone quiet. Generous relative to the 100 ms
// poll interval so a brief hiccup, or the handover as a foreign poller stops
// and we take over, never trips it.
constexpr int kDataTimeoutMs = 2000;
// Reconnect cadence, matching AcomConnection/SpeConnection: retry forever
// until the meter returns or the operator disconnects.
constexpr int kReconnectMs = 5000;
}  // namespace

qint64 LpMeterConnection::nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

LpMeterConnection::LpMeterConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &LpMeterConnection::onTransportUp);
    connect(&m_socket, &QTcpSocket::disconnected, this, &LpMeterConnection::onTransportDown);
    connect(&m_socket, &QTcpSocket::readyRead, this, &LpMeterConnection::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        onTransportError(m_socket.errorString());
    });

    m_parser.setReadingCallback([this](const LpMeter::Reading& r) { onReading(r); });

    m_range.setCeilings(m_ceilings);
    m_range.reset();

    // The poll tick runs at the solo rate; PollGate decides per tick whether
    // a poll is actually sent. See its header for why gating on FOREIGN
    // records rather than on all records is what keeps the solo rate and the
    // suppression threshold independent.
    m_pollTimer.setInterval(static_cast<int>(LpMeter::PollGate::kSoloPollIntervalMs));
    connect(&m_pollTimer, &QTimer::timeout, this, &LpMeterConnection::pollTick);

    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(kReconnectMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_connected) { return; }
        if (m_mode == Mode::Network && !m_lastHost.isEmpty()) {
            connectNetwork(m_lastHost, m_lastPort);
#ifdef HAVE_SERIALPORT
        } else if (m_mode == Mode::Serial && !m_lastSerialPort.isEmpty()) {
            connectSerial(m_lastSerialPort);
#endif
        }
    });

    m_dataWatchdog.setSingleShot(true);
    m_dataWatchdog.setInterval(kDataTimeoutMs);
    connect(&m_dataWatchdog, &QTimer::timeout, this, [this]() {
        if (!m_connected) { return; }
        // Deliberately does NOT drop the link. The meter wedging with the
        // transport healthy is a real observed state, and tearing down the
        // socket would take the applet tile away at the exact moment the
        // operator needs to be told the meter stopped answering.
        qCWarning(lcTuner) << "LpMeterConnection: link up but no valid record in"
                           << kDataTimeoutMs / 1000.0
                           << "s — meter may be wedged (it can stop answering with"
                              " the serial link perfectly healthy) or the records"
                              " may be unparseable. Power-cycling the meter clears"
                              " the former.";
        setDataFlowing(false);
    });
}

QString LpMeterConnection::description() const
{
    if (m_mode == Mode::Network) {
        return QStringLiteral("%1:%2").arg(m_lastHost).arg(m_lastPort);
    }
#ifdef HAVE_SERIALPORT
    if (m_mode == Mode::Serial) {
        return m_lastSerialPort;
    }
#endif
    return QString();
}

QString LpMeterConnection::sourceLabel() const
{
    switch (m_mode) {
        case Mode::Network: return QStringLiteral("NETWORK");
        case Mode::Serial:  return QStringLiteral("SERIAL");
        default:            return QStringLiteral("—");
    }
}

void LpMeterConnection::setRangeCeilings(const LpMeter::RangeCeilings& ceilings)
{
    m_ceilings = ceilings;
    const double before = m_range.ceilingW();
    // RangeTracker applies its own up-only guard here, so a re-read or an
    // edited ceiling cannot shrink one that observed power already expanded
    // within this session.
    m_range.setCeilings(ceilings);
    if (std::abs(before - m_range.ceilingW()) > 1e-6) {
        emit gaugeCeilingChanged(m_range.ceilingW(), m_range.ceilingAutoExpanded());
    }
}

#ifdef HAVE_SERIALPORT
void LpMeterConnection::connectSerial(const QString& portName)
{
    m_mode = Mode::Serial;
    m_lastSerialPort = portName;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    teardownDevice();
    m_parser.reset();

    if (!m_serialPort) {
        m_serialPort = new QSerialPort(this);
        connect(m_serialPort, &QSerialPort::readyRead, this, &LpMeterConnection::onReadyRead);
        connect(m_serialPort, &QSerialPort::errorOccurred, this,
                [this](QSerialPort::SerialPortError err) {
            if (err == QSerialPort::NoError) { return; }
            const QString msg = m_serialPort->errorString();
            qCWarning(lcTuner) << "LpMeterConnection: serial error" << err << msg;
            onTransportError(msg);
            if (m_connected) { onTransportDown(); }
        });
    }

    m_serialPort->setPortName(portName);
    // Fixed by the meter's own protocol spec — not user-configurable.
    m_serialPort->setBaudRate(QSerialPort::Baud115200);
    m_serialPort->setDataBits(QSerialPort::Data8);
    m_serialPort->setParity(QSerialPort::NoParity);
    m_serialPort->setStopBits(QSerialPort::OneStop);
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serialPort->open(QIODevice::ReadWrite)) {
        const QString err = m_serialPort->errorString();
        qCWarning(lcTuner) << "LpMeterConnection: failed to open" << portName << err;
        emit connectionFailed(err);
        if (m_autoReconnect) { armReconnect(); }
        return;
    }
    // DELIBERATELY does not touch DTR/RTS — unlike AcomConnection, which
    // forces both low to stay clear of its amplifier's power-button logic.
    // The reference station's working path runs through ser2net with
    // `local -rtscts` and CLOCAL, i.e. modem control lines ignored entirely,
    // so the proven configuration is to leave them alone.

    m_device = m_serialPort;
    onTransportUp();
}
#endif

void LpMeterConnection::connectNetwork(const QString& host, quint16 port)
{
    m_mode = Mode::Network;
    m_lastHost = host;
    m_lastPort = port;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    teardownDevice();
    m_parser.reset();

    m_device = &m_socket;
    qCDebug(lcTuner) << "LpMeterConnection: connecting to" << host << ":" << port;
    m_socket.connectToHost(host, port);
    // onTransportUp() fires from the connected() signal (async).
}

void LpMeterConnection::disconnect()
{
    // Self-contained: don't depend on teardownDevice() indirectly triggering
    // onTransportDown(). It sometimes does (QTcpSocket::abort() synchronously
    // emits disconnected()) and sometimes doesn't (QSerialPort::close() emits
    // nothing at all) — relying on that is what left AcomConnection's applet
    // stuck showing "Connected" after a user-initiated disconnect.
    const bool wasConnected = m_connected;
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_pollTimer.stop();
    m_dataWatchdog.stop();
    m_connected = false;
    teardownDevice();
    m_parser.reset();
    m_gate.reset();
    if (wasConnected) {
        qCDebug(lcTuner) << "LpMeterConnection: disconnected";
        emit disconnected();
    }
    m_deliberateDisconnect = false;
}

void LpMeterConnection::teardownDevice()
{
    if (m_device == &m_socket && m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
#ifdef HAVE_SERIALPORT
    if (m_serialPort && m_device == m_serialPort && m_serialPort->isOpen()) {
        m_serialPort->close();
    }
#endif
    m_device = nullptr;
}

void LpMeterConnection::onTransportUp()
{
    m_connected = true;
    m_bytesWithoutRecords = false;
    m_ridingAlongLogged = false;
    m_lastRecordMs = -1;
    m_firstBytesMs = -1;
    m_dataFlowing = false;
    m_gate.reset();
    m_range.setCeilings(m_ceilings);
    m_range.reset();

    qCInfo(lcTuner) << "LpMeterConnection: connected via" << description();
    m_pollTimer.start();
    m_dataWatchdog.start();

    emit connected();
    emit gaugeCeilingChanged(m_range.ceilingW(), m_range.ceilingAutoExpanded());
}

void LpMeterConnection::onTransportDown()
{
    const bool wasConnected = m_connected;
    m_connected = false;
    m_pollTimer.stop();
    m_dataWatchdog.stop();
    m_parser.reset();
    m_gate.reset();
    setDataFlowing(false);
    if (wasConnected) {
        qCDebug(lcTuner) << "LpMeterConnection: disconnected";
        emit disconnected();
    }
    if (!m_deliberateDisconnect && m_autoReconnect) {
        armReconnect();
    }
    m_deliberateDisconnect = false;
}

void LpMeterConnection::onTransportError(const QString& errorString)
{
    qCWarning(lcTuner) << "LpMeterConnection: transport error" << errorString;
    emit connectionFailed(errorString);
    if (!m_deliberateDisconnect && m_autoReconnect && !m_connected) {
        armReconnect();
    }
}

void LpMeterConnection::armReconnect()
{
    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void LpMeterConnection::onReadyRead()
{
    if (!m_device) { return; }
    const QByteArray chunk = m_device->readAll();
    if (chunk.isEmpty()) { return; }
    m_parser.feed(chunk);

    // Bytes arriving but nothing decoding. With no checksum on this protocol
    // there is no other signal that the link is alive but the content is
    // wrong (a firmware predating 1.2.0.0, a telnet-mode proxy mangling the
    // stream, or a genuinely corrupt link). Latched so it logs on the
    // transition, not per chunk.
    if (m_firstBytesMs < 0) { m_firstBytesMs = nowMs(); }
    if (m_lastRecordMs < 0 && !m_bytesWithoutRecords
        && nowMs() - m_firstBytesMs > kDataTimeoutMs) {
        m_bytesWithoutRecords = true;
        qCWarning(lcTuner) << "LpMeterConnection: receiving bytes but decoding no"
                              " records. Check the proxy is in RAW mode (not telnet)"
                              " and that the meter's firmware is 1.2.0.0 or later"
                              " (earlier versions use a different baud rate and"
                              " omit dBm/SWR).";
    }
}

void LpMeterConnection::pollTick()
{
    if (!m_connected || !m_device) { return; }

    const qint64 now = nowMs();
    if (!m_gate.shouldPoll(now)) {
        if (!m_ridingAlongLogged) {
            m_ridingAlongLogged = true;
            qCInfo(lcTuner) << "LpMeterConnection: another client is polling this"
                               " meter (~" << m_gate.foreignIntervalMs()
                            << "ms cadence) — riding along on its replies and"
                               " sending no polls of our own.";
        }
        return;
    }
    if (m_ridingAlongLogged) {
        m_ridingAlongLogged = false;
        qCInfo(lcTuner) << "LpMeterConnection: no other client polling — taking over"
                           " at" << LpMeter::PollGate::kSoloPollIntervalMs << "ms.";
    }
    m_device->write(QByteArray(1, LpMeter::kPollCommand));
}

void LpMeterConnection::onReading(const LpMeter::Reading& reading)
{
    const qint64 now = nowMs();
    m_gate.onRecord(now);
    m_lastRecordMs = now;
    m_bytesWithoutRecords = false;

    const double before = m_range.ceilingW();
    m_range.onReading(reading.powerRange, reading.powerW, now);
    if (std::abs(before - m_range.ceilingW()) > 1e-6) {
        emit gaugeCeilingChanged(m_range.ceilingW(), m_range.ceilingAutoExpanded());
    }

    m_lastReading = reading;
    setDataFlowing(true);
    m_dataWatchdog.start();  // restart: a valid record is the liveness proof
    emit readingUpdated(reading);
}

void LpMeterConnection::setDataFlowing(bool flowing)
{
    if (m_dataFlowing == flowing) { return; }
    m_dataFlowing = flowing;
    if (flowing) {
        qCInfo(lcTuner) << "LpMeterConnection: records flowing again.";
    }
    emit dataFlowingChanged(flowing);
}

}  // namespace AetherSDR
