#include "PipeWireAudioBridge.h"

#include <QTimer>
#include <QProcess>
#include <QDebug>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace AetherSDR {

PipeWireAudioBridge::PipeWireAudioBridge(QObject* parent)
    : QObject(parent)
{}

PipeWireAudioBridge::~PipeWireAudioBridge()
{
    close();
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

// Unload any stale aethersdr pipe modules from a previous crashed session
static void cleanupStaleModules()
{
    QProcess proc;
    proc.start("pactl", {"list", "modules", "short"});
    if (!proc.waitForFinished(3000)) return;

    for (const auto& line : proc.readAllStandardOutput().split('\n')) {
        if (line.contains("aethersdr-")) {
            auto parts = line.split('\t');
            if (parts.size() >= 1) {
                QProcess::execute("pactl", {"unload-module", parts[0].trimmed()});
            }
        }
    }
}

bool PipeWireAudioBridge::open()
{
    if (m_open) return true;

    cleanupStaleModules();

    // Create 4 RX pipe sources (radio → apps)
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        if (!loadPipeSource(i)) {
            qWarning() << "PipeWireAudioBridge: failed to create RX pipe" << (i + 1);
            close();
            return false;
        }
    }

    // Create TX pipe sink (apps → radio)
    if (!loadPipeSink()) {
        qWarning() << "PipeWireAudioBridge: failed to create TX pipe";
        close();
        return false;
    }

    // Poll TX pipe for incoming audio from apps
    m_txReadTimer = new QTimer(this);
    m_txReadTimer->setInterval(10);
    connect(m_txReadTimer, &QTimer::timeout, this, &PipeWireAudioBridge::readTxPipe);
    m_txReadTimer->start();

    m_open = true;
    qInfo() << "PipeWireAudioBridge: opened — 4 RX sources + 1 TX sink";
    return true;
}

void PipeWireAudioBridge::close()
{
    if (m_txReadTimer) {
        m_txReadTimer->stop();
        delete m_txReadTimer;
        m_txReadTimer = nullptr;
    }

    // Close pipe file descriptors
    for (auto& rx : m_rx) {
        if (rx.fd >= 0) { ::close(rx.fd); rx.fd = -1; }
    }
    if (m_tx.fd >= 0) { ::close(m_tx.fd); m_tx.fd = -1; }

    unloadModules();

    // Remove pipe files
    for (auto& rx : m_rx) {
        if (!rx.pipePath.isEmpty()) {
            ::unlink(rx.pipePath.toUtf8().constData());
            rx.pipePath.clear();
        }
    }
    if (!m_tx.pipePath.isEmpty()) {
        ::unlink(m_tx.pipePath.toUtf8().constData());
        m_tx.pipePath.clear();
    }

    m_open = false;
    qInfo() << "PipeWireAudioBridge: closed";
}

// ── Module loading via pactl ─────────────────────────────────────────────────

static uint32_t runPactl(const QStringList& args)
{
    QProcess proc;
    proc.start("pactl", args);
    if (!proc.waitForFinished(5000)) {
        qWarning() << "PipeWireAudioBridge: pactl timed out:" << args;
        return 0;
    }
    if (proc.exitCode() != 0) {
        qWarning() << "PipeWireAudioBridge: pactl failed:" << proc.readAllStandardError().trimmed();
        return 0;
    }
    // pactl load-module returns the module index
    bool ok = false;
    uint32_t idx = proc.readAllStandardOutput().trimmed().toUInt(&ok);
    return ok ? idx : 0;
}

bool PipeWireAudioBridge::loadPipeSource(int index)
{
    auto pipePath = QStringLiteral("/tmp/aethersdr-dax-%1.pipe").arg(index + 1);
    auto sourceName = QStringLiteral("aethersdr-dax-%1").arg(index + 1);
    auto sourceDesc = QStringLiteral("AetherSDR DAX %1").arg(index + 1);

    // Create the named pipe (FIFO)
    ::unlink(pipePath.toUtf8().constData());
    if (::mkfifo(pipePath.toUtf8().constData(), 0666) != 0) {
        qWarning() << "PipeWireAudioBridge: mkfifo failed:" << strerror(errno);
        return false;
    }

    // Load PulseAudio pipe-source module
    uint32_t modIdx = runPactl({
        "load-module", "module-pipe-source",
        QStringLiteral("file=%1").arg(pipePath),
        QStringLiteral("source_name=%1").arg(sourceName),
        QStringLiteral("source_properties=device.description=\"%1\"").arg(sourceDesc),
        QStringLiteral("format=s16le"),
        QStringLiteral("rate=%1").arg(SAMPLE_RATE),
        QStringLiteral("channels=%1").arg(STEREO),
    });

    if (modIdx == 0) {
        ::unlink(pipePath.toUtf8().constData());
        return false;
    }

    // Open the pipe for writing (non-blocking to avoid hanging if no reader)
    int fd = ::open(pipePath.toUtf8().constData(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        qWarning() << "PipeWireAudioBridge: open pipe failed:" << strerror(errno);
        runPactl({"unload-module", QString::number(modIdx)});
        ::unlink(pipePath.toUtf8().constData());
        return false;
    }

    m_rx[index].fd = fd;
    m_rx[index].moduleIndex = modIdx;
    m_rx[index].pipePath = pipePath;

    qDebug() << "PipeWireAudioBridge: RX" << (index + 1) << "pipe source loaded, module" << modIdx;
    return true;
}

bool PipeWireAudioBridge::loadPipeSink()
{
    auto pipePath = QStringLiteral("/tmp/aethersdr-tx.pipe");
    auto sinkName = QStringLiteral("aethersdr-tx");
    auto sinkDesc = QStringLiteral("AetherSDR TX");

    ::unlink(pipePath.toUtf8().constData());
    if (::mkfifo(pipePath.toUtf8().constData(), 0666) != 0) {
        qWarning() << "PipeWireAudioBridge: mkfifo failed:" << strerror(errno);
        return false;
    }

    uint32_t modIdx = runPactl({
        "load-module", "module-pipe-sink",
        QStringLiteral("file=%1").arg(pipePath),
        QStringLiteral("sink_name=%1").arg(sinkName),
        QStringLiteral("sink_properties=device.description=\"%1\"").arg(sinkDesc),
        QStringLiteral("format=s16le"),
        QStringLiteral("rate=%1").arg(SAMPLE_RATE),
        QStringLiteral("channels=%1").arg(STEREO),
    });

    if (modIdx == 0) {
        ::unlink(pipePath.toUtf8().constData());
        return false;
    }

    // Open the pipe for reading (non-blocking)
    int fd = ::open(pipePath.toUtf8().constData(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        qWarning() << "PipeWireAudioBridge: open TX pipe failed:" << strerror(errno);
        runPactl({"unload-module", QString::number(modIdx)});
        ::unlink(pipePath.toUtf8().constData());
        return false;
    }

    m_tx.fd = fd;
    m_tx.moduleIndex = modIdx;
    m_tx.pipePath = pipePath;

    qDebug() << "PipeWireAudioBridge: TX pipe sink loaded, module" << modIdx;
    return true;
}

void PipeWireAudioBridge::unloadModules()
{
    for (auto& rx : m_rx) {
        if (rx.moduleIndex > 0) {
            runPactl({"unload-module", QString::number(rx.moduleIndex)});
            rx.moduleIndex = 0;
        }
    }
    if (m_tx.moduleIndex > 0) {
        runPactl({"unload-module", QString::number(m_tx.moduleIndex)});
        m_tx.moduleIndex = 0;
    }
}

// ── Audio I/O ────────────────────────────────────────────────────────────────

void PipeWireAudioBridge::setGain(float g)
{
    m_gain = std::clamp(g, 0.0f, 1.0f);
    for (int i = 0; i < NUM_CHANNELS; ++i)
        m_channelGain[i] = m_gain;
}

void PipeWireAudioBridge::setChannelGain(int channel, float g)
{
    if (channel >= 1 && channel <= NUM_CHANNELS)
        m_channelGain[channel - 1] = std::clamp(g, 0.0f, 1.0f);
}

void PipeWireAudioBridge::setTxGain(float g)
{
    m_txGain = std::clamp(g, 0.0f, 1.0f);
}

void PipeWireAudioBridge::feedDaxAudio(int channel, const QByteArray& pcm)
{
    if (!m_open) return;
    if (channel < 1 || channel > NUM_CHANNELS) return;
    if (m_transmitting) return;

    auto& rx = m_rx[channel - 1];
    if (rx.fd < 0) return;

    const auto* src = reinterpret_cast<const int16_t*>(pcm.constData());
    int nSamples = pcm.size() / sizeof(int16_t);
    float chGain = m_channelGain[channel - 1];

    if (chGain == 1.0f) {
        ::write(rx.fd, pcm.constData(), pcm.size());
    } else {
        QByteArray scaled(nSamples * sizeof(int16_t), Qt::Uninitialized);
        auto* dst = reinterpret_cast<int16_t*>(scaled.data());
        for (int i = 0; i < nSamples; ++i)
            dst[i] = static_cast<int16_t>(src[i] * chGain);
        ::write(rx.fd, scaled.constData(), scaled.size());
    }

    // Calculate RMS for meter display (every ~100ms = ~10 calls at 24kHz)
    static int meterCount[NUM_CHANNELS]{};
    if (++meterCount[channel - 1] % 10 == 0) {
        float sum = 0;
        for (int i = 0; i < nSamples; i += 2)  // left channel only
            sum += (src[i] / 32768.0f) * (src[i] / 32768.0f);
        float rms = std::sqrt(sum / std::max(1, nSamples / 2));
        emit daxRxLevel(channel, rms);
    }
}

void PipeWireAudioBridge::setTransmitting(bool tx)
{
    m_transmitting = tx;
}

void PipeWireAudioBridge::readTxPipe()
{
    if (m_tx.fd < 0) return;

    // Read available data from the TX pipe (int16 stereo from apps)
    // Convert to float32 stereo for AudioEngine::feedDaxTxAudio
    char buf[4096];
    ssize_t n = ::read(m_tx.fd, buf, sizeof(buf));
    if (n <= 0) return;

    // Convert int16 → float32 with TX gain
    int nSamples = n / sizeof(int16_t);
    const auto* src = reinterpret_cast<const int16_t*>(buf);
    QByteArray out(nSamples * sizeof(float), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(out.data());
    for (int i = 0; i < nSamples; ++i)
        dst[i] = (src[i] / 32768.0f) * m_txGain;

    emit txAudioReady(out);

    // TX level meter (every ~100ms)
    static int txMeterCount = 0;
    if (++txMeterCount % 10 == 0) {
        float sum = 0;
        for (int i = 0; i < nSamples; i += 2)
            sum += dst[i] * dst[i];
        emit daxTxLevel(std::sqrt(sum / std::max(1, nSamples / 2)));
    }
}

} // namespace AetherSDR
