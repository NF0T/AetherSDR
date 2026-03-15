#include "SpectrumWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QSettings>
#include <cmath>
#include <cstring>

namespace AetherSDR {

SpectrumWidget::SpectrumWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);

    // Restore saved FFT/waterfall split ratio
    QSettings settings;
    m_spectrumFrac = std::clamp(
        settings.value("spectrum/splitRatio", 0.40).toFloat(), 0.10f, 0.90f);
}

void SpectrumWidget::setFrequencyRange(double centerMhz, double bandwidthMhz)
{
    m_centerMhz    = centerMhz;
    m_bandwidthMhz = bandwidthMhz;
    update();
}

void SpectrumWidget::setDbmRange(float minDbm, float maxDbm)
{
    m_refLevel     = maxDbm;
    m_dynamicRange = maxDbm - minDbm;
    update();
}

void SpectrumWidget::setSliceFrequency(double freqMhz)
{
    m_sliceFreqMhz = freqMhz;
    update();
}

void SpectrumWidget::setSliceFilter(int lowHz, int highHz)
{
    m_filterLowHz  = lowHz;
    m_filterHighHz = highHz;
    update();
}

void SpectrumWidget::updateSpectrum(const QVector<float>& binsDbm)
{
    if (m_smoothed.size() != binsDbm.size())
        m_smoothed = binsDbm;
    else {
        for (int i = 0; i < binsDbm.size(); ++i)
            m_smoothed[i] = SMOOTH_ALPHA * binsDbm[i] + (1.0f - SMOOTH_ALPHA) * m_smoothed[i];
    }
    m_bins = binsDbm;

    // Use FFT data for waterfall — always matches the panadapter frequency range.
    if (!m_waterfall.isNull())
        pushWaterfallRow(binsDbm, m_waterfall.width());

    update();
}

void SpectrumWidget::updateWaterfallRow(const QVector<float>& binsDbm,
                                        double /*lowFreqMhz*/, double /*highFreqMhz*/)
{
    // Native waterfall tiles from the radio have unreliable frequency metadata
    // (tile range often doesn't match the panadapter display range).
    // Use FFT-derived waterfall rows instead for correct frequency alignment.
    // We still mark native tiles as received to avoid double-drawing, but
    // let the FFT path handle waterfall rendering.
    Q_UNUSED(binsDbm);
    m_hasNativeWaterfall = false;  // let FFT drive the waterfall
}

// ─── Layout helpers ────────────────────────────────────────────────────────────

int SpectrumWidget::mhzToX(double mhz) const
{
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    return static_cast<int>((mhz - startMhz) / m_bandwidthMhz * width());
}

// ─── Mouse ────────────────────────────────────────────────────────────────────

// Snap a frequency (MHz) to the nearest multiple of m_stepHz.
static double snapToStep(double mhz, int stepHz)
{
    if (stepHz <= 0) return mhz;
    const double stepMhz = stepHz / 1e6;
    return std::round(mhz / stepMhz) * stepMhz;
}

void SpectrumWidget::mousePressEvent(QMouseEvent* ev)
{
    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int y = static_cast<int>(ev->position().y());

    // Click on the divider bar → start split drag
    if (y >= specH && y < specH + DIVIDER_H) {
        m_draggingDivider = true;
        setCursor(Qt::SplitVCursor);
        ev->accept();
        return;
    }

    // Click on the freq scale bar → start bandwidth drag
    const int scaleY = specH + DIVIDER_H;
    if (y >= scaleY && y < scaleY + FREQ_SCALE_H) {
        m_draggingBandwidth = true;
        m_bwDragStartX = static_cast<int>(ev->position().x());
        m_bwDragStartBw = m_bandwidthMhz;
        setCursor(Qt::SizeHorCursor);
        ev->accept();
        return;
    }

    // Click in waterfall area → start pan drag (tune on double-click only)
    const int wfY = scaleY + FREQ_SCALE_H;
    if (y >= wfY) {
        m_draggingPan = true;
        m_panDragStartX = static_cast<int>(ev->position().x());
        m_panDragStartCenter = m_centerMhz;
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }

    // Click in FFT area → tune immediately
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double rawMhz = startMhz + (ev->position().x() / width()) * m_bandwidthMhz;
    emit frequencyClicked(snapToStep(rawMhz, m_stepHz));
}

void SpectrumWidget::mouseMoveEvent(QMouseEvent* ev)
{
    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int y = static_cast<int>(ev->position().y());

    if (m_draggingDivider) {
        // Clamp the divider position: 10%–90% of content area
        float frac = static_cast<float>(y) / contentH;
        m_spectrumFrac = std::clamp(frac, 0.10f, 0.90f);
        // Rebuild waterfall image for new size
        const int wfHeight = static_cast<int>(contentH * (1.0f - m_spectrumFrac));
        if (wfHeight > 0 && width() > 0) {
            QImage newWf(width(), wfHeight, QImage::Format_RGB32);
            newWf.fill(Qt::black);
            if (!m_waterfall.isNull())
                newWf = m_waterfall.scaled(width(), wfHeight, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            m_waterfall = std::move(newWf);
        }
        update();
        ev->accept();
        return;
    }

    if (m_draggingBandwidth) {
        const int dx = static_cast<int>(ev->position().x()) - m_bwDragStartX;
        // Drag right = zoom in (narrower BW), drag left = zoom out (wider BW)
        // 4x multiplier: dragging 1/4 of widget width doubles/halves bandwidth
        const double scale = std::pow(2.0, static_cast<double>(-dx) / (width() / 4.0));
        const double newBw = std::clamp(m_bwDragStartBw * scale, 0.004, 14.0);
        // Update local display immediately for responsive feel
        m_bandwidthMhz = newBw;
        update();
        emit bandwidthChangeRequested(newBw);
        ev->accept();
        return;
    }

    if (m_draggingPan) {
        const int dx = static_cast<int>(ev->position().x()) - m_panDragStartX;
        // Dragging right moves the view right → center shifts left
        const double deltaMhz = -(static_cast<double>(dx) / width()) * m_bandwidthMhz;
        const double newCenter = m_panDragStartCenter + deltaMhz;
        m_centerMhz = newCenter;
        update();
        emit centerChangeRequested(newCenter);
        ev->accept();
        return;
    }

    // Update cursor based on hover position
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int wfY = specH + DIVIDER_H + FREQ_SCALE_H;
    if (y >= specH && y < specH + DIVIDER_H)
        setCursor(Qt::SplitVCursor);
    else if (y >= specH + DIVIDER_H && y < wfY)
        setCursor(Qt::SizeHorCursor);
    else if (y >= wfY)
        setCursor(Qt::CrossCursor);
    else
        setCursor(Qt::CrossCursor);
}

void SpectrumWidget::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_draggingDivider) {
        m_draggingDivider = false;
        setCursor(Qt::CrossCursor);
        QSettings settings;
        settings.setValue("spectrum/splitRatio", static_cast<double>(m_spectrumFrac));
        ev->accept();
        return;
    }
    if (m_draggingBandwidth) {
        m_draggingBandwidth = false;
        setCursor(Qt::CrossCursor);
        ev->accept();
        return;
    }
    if (m_draggingPan) {
        m_draggingPan = false;
        setCursor(Qt::CrossCursor);
        ev->accept();
    }
}

void SpectrumWidget::mouseDoubleClickEvent(QMouseEvent* ev)
{
    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int wfY = specH + DIVIDER_H + FREQ_SCALE_H;
    const int y = static_cast<int>(ev->position().y());

    // Double-click in waterfall → tune to clicked frequency
    if (y >= wfY) {
        const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
        const double rawMhz = startMhz + (ev->position().x() / width()) * m_bandwidthMhz;
        emit frequencyClicked(snapToStep(rawMhz, m_stepHz));
        ev->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(ev);
}

void SpectrumWidget::wheelEvent(QWheelEvent* ev)
{
    // Skip scroll on the divider + freq scale bar.
    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH2 = height() - chromeH;
    const int specH2 = static_cast<int>(contentH2 * m_spectrumFrac);
    const int chromeTop = specH2;
    const int chromeBot = specH2 + chromeH;
    if (ev->position().y() >= chromeTop && ev->position().y() < chromeBot) {
        ev->ignore();
        return;
    }
    const int ticks = ev->angleDelta().y() / 120;   // +1 per notch up, -1 per notch down
    if (ticks == 0) { ev->ignore(); return; }

    const double newMhz = snapToStep(m_sliceFreqMhz + ticks * m_stepHz / 1e6, m_stepHz);
    emit frequencyClicked(newMhz);
    ev->accept();
}

// ─── Resize ───────────────────────────────────────────────────────────────────

void SpectrumWidget::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);

    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int wfHeight = static_cast<int>(contentH * (1.0f - m_spectrumFrac));
    if (wfHeight > 0 && width() > 0) {
        QImage newWf(width(), wfHeight, QImage::Format_RGB32);
        newWf.fill(Qt::black);
        if (!m_waterfall.isNull())
            newWf = m_waterfall.scaled(width(), wfHeight, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        m_waterfall = newWf;
    }
}

// ─── Colour map ───────────────────────────────────────────────────────────────

QRgb SpectrumWidget::dbmToRgb(float dbm) const
{
    // Normalise into [0, 1] over the waterfall display range.
    const float t = qBound(0.0f, (dbm - m_wfMinDbm) / (m_wfMaxDbm - m_wfMinDbm), 1.0f);

    // Multi-stop gradient: black → blue → cyan → green → yellow → red
    struct Stop { float pos; int r, g, b; };
    static constexpr Stop stops[] = {
        {0.00f,   0,   0,   0},   // black  (noise floor)
        {0.15f,   0,   0, 128},   // dark blue
        {0.30f,   0,  64, 255},   // blue
        {0.45f,   0, 200, 255},   // cyan
        {0.60f,   0, 220,   0},   // green
        {0.80f, 255, 255,   0},   // yellow
        {1.00f, 255,   0,   0},   // red     (strong signal)
    };
    static constexpr int N = sizeof(stops) / sizeof(stops[0]);

    // Find the two stops bracketing t and interpolate.
    int i = 0;
    while (i < N - 2 && stops[i + 1].pos < t) ++i;
    const float seg = (t - stops[i].pos) / (stops[i + 1].pos - stops[i].pos);
    const int r = static_cast<int>(stops[i].r + seg * (stops[i + 1].r - stops[i].r));
    const int g = static_cast<int>(stops[i].g + seg * (stops[i + 1].g - stops[i].g));
    const int b = static_cast<int>(stops[i].b + seg * (stops[i + 1].b - stops[i].b));
    return qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
}

// ─── Waterfall update ─────────────────────────────────────────────────────────

void SpectrumWidget::pushWaterfallRow(const QVector<float>& bins, int destWidth,
                                      double tileLowMhz, double tileHighMhz)
{
    if (m_waterfall.isNull() || destWidth <= 0) return;

    const int h = m_waterfall.height();
    if (h <= 1) return;

    uchar* bits = m_waterfall.bits();
    const qsizetype bpl = m_waterfall.bytesPerLine();
    std::memmove(bits + bpl, bits, static_cast<size_t>(bpl) * (h - 1));

    auto* row = reinterpret_cast<QRgb*>(bits);

    Q_UNUSED(tileLowMhz);
    Q_UNUSED(tileHighMhz);

    for (int x = 0; x < destWidth; ++x) {
        const int binIdx = x * bins.size() / destWidth;
        const float dbm = (binIdx >= 0 && binIdx < bins.size()) ? bins[binIdx] : m_wfMinDbm;
        row[x] = dbmToRgb(dbm);
    }
}

// ─── Paint ────────────────────────────────────────────────────────────────────

void SpectrumWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH    = static_cast<int>(contentH * m_spectrumFrac);
    const int wfH      = contentH - specH;

    const int divY     = specH;
    const int scaleY   = specH + DIVIDER_H;
    const int wfY      = scaleY + FREQ_SCALE_H;

    const QRect specRect (0, 0,       width(), specH);
    const QRect divRect  (0, divY,    width(), DIVIDER_H);
    const QRect scaleRect(0, scaleY,  width(), FREQ_SCALE_H);
    const QRect wfRect   (0, wfY,     width(), wfH);

    p.fillRect(specRect, QColor(0x0a, 0x0a, 0x14));

    drawGrid(p, specRect);
    drawSpectrum(p, specRect);

    // Draggable divider bar
    p.fillRect(divRect, QColor(0x18, 0x28, 0x38));
    p.setPen(QColor(m_draggingDivider ? 0x00b4d8 : 0x304050));
    p.drawLine(divRect.left(), divRect.center().y(), divRect.right(), divRect.center().y());

    drawFreqScale(p, scaleRect);
    drawWaterfall(p, wfRect);
    drawSliceOverlay(p, specRect, wfRect);
}

// ─── Grid ─────────────────────────────────────────────────────────────────────

void SpectrumWidget::drawGrid(QPainter& p, const QRect& r)
{
    const int w = r.width();
    const int h = r.height();

    // Horizontal dB lines every 20 dB
    const int steps = static_cast<int>(m_dynamicRange / 20.0f);
    for (int i = 0; i <= steps; ++i) {
        const int y = r.top() + static_cast<int>(h * i / static_cast<float>(steps));
        p.setPen(QPen(QColor(0x20, 0x30, 0x40), 1, Qt::DotLine));
        p.drawLine(0, y, w, y);

        const float dbm = m_refLevel - (m_dynamicRange * i / steps);
        p.setPen(QColor(0x40, 0x60, 0x70));
        p.drawText(2, y + 12, QString("%1").arg(static_cast<int>(dbm)));
    }

    // Vertical frequency grid lines — adaptive step matching the scale bar
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double endMhz   = m_centerMhz + m_bandwidthMhz / 2.0;
    const double rawStep  = m_bandwidthMhz / 5.0;
    const double gridMag  = std::pow(10.0, std::floor(std::log10(rawStep)));
    const double gridNorm = rawStep / gridMag;
    double gridStep;
    if      (gridNorm >= 5.0) gridStep = 5.0 * gridMag;
    else if (gridNorm >= 2.0) gridStep = 2.0 * gridMag;
    else                      gridStep = 1.0 * gridMag;
    const double firstLine = std::ceil(startMhz / gridStep) * gridStep;

    p.setPen(QPen(QColor(0x20, 0x30, 0x40), 1, Qt::DotLine));
    for (double f = firstLine; f <= endMhz; f += gridStep)
        p.drawLine(mhzToX(f), r.top(), mhzToX(f), r.bottom());
}

// ─── Spectrum line ────────────────────────────────────────────────────────────

void SpectrumWidget::drawSpectrum(QPainter& p, const QRect& r)
{
    if (m_smoothed.isEmpty()) {
        p.setPen(QColor(0x00, 0x60, 0x80));
        p.drawText(r, Qt::AlignCenter, "No panadapter data — waiting for radio stream");
        return;
    }

    const int w = r.width();
    const int h = r.height();
    const int n = m_smoothed.size();

    // Build the spectrum line path (data points only)
    QPainterPath linePath;
    bool first = true;

    for (int i = 0; i < n; ++i) {
        const float dbm  = m_smoothed[i];
        const float norm = qBound(0.0f, (m_refLevel - dbm) / m_dynamicRange, 1.0f);
        const int   x    = r.left() + static_cast<int>(static_cast<float>(i) / n * w);
        const int   y    = r.top()  + qMin(static_cast<int>(norm * h), h - 1);

        if (first) { linePath.moveTo(x, y); first = false; }
        else        linePath.lineTo(x, y);
    }

    // Closed fill path (line + bottom edges) for gradient fill only
    QPainterPath fillPath(linePath);
    fillPath.lineTo(r.right(), r.bottom());
    fillPath.lineTo(r.left(),  r.bottom());
    fillPath.closeSubpath();

    QLinearGradient grad(0, r.top(), 0, r.bottom());
    grad.setColorAt(0.0, QColor(0x00, 0xe5, 0xff, 200));
    grad.setColorAt(1.0, QColor(0x00, 0x40, 0x60,  60));

    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillPath(fillPath, grad);
    // Stroke only the spectrum line, not the fill closure
    p.setPen(QPen(QColor(0x00, 0xe5, 0xff), 1.5));
    p.drawPath(linePath);
    p.setRenderHint(QPainter::Antialiasing, false);
}

// ─── Waterfall ────────────────────────────────────────────────────────────────

void SpectrumWidget::drawWaterfall(QPainter& p, const QRect& r)
{
    if (m_waterfall.isNull()) {
        p.fillRect(r, Qt::black);
        return;
    }
    p.drawImage(r, m_waterfall);
}

// ─── Slice overlay (filter passband + center line) ────────────────────────────

void SpectrumWidget::drawSliceOverlay(QPainter& p, const QRect& specRect, const QRect& wfRect)
{
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double endMhz   = m_centerMhz + m_bandwidthMhz / 2.0;
    if (m_sliceFreqMhz < startMhz || m_sliceFreqMhz > endMhz) return;

    const double filterLowMhz  = m_sliceFreqMhz + m_filterLowHz  / 1.0e6;
    const double filterHighMhz = m_sliceFreqMhz + m_filterHighHz / 1.0e6;

    const int sliceX   = mhzToX(m_sliceFreqMhz);
    const int filterX1 = mhzToX(filterLowMhz);
    const int filterX2 = mhzToX(filterHighMhz);
    const int filterW  = filterX2 - filterX1;

    // ── Filter passband shading ──────────────────────────────────────────────

    // Spectrum: slightly brighter fill so the passband stands out over the FFT
    p.fillRect(QRect(filterX1, specRect.top(), filterW, specRect.height()),
               QColor(0x00, 0x80, 0xff, 35));

    // Waterfall: subtle overlay so colour map is still readable
    p.fillRect(QRect(filterX1, wfRect.top(), filterW, wfRect.height()),
               QColor(0x00, 0x60, 0xe0, 25));

    // Filter edge lines (spectrum only — would clutter the waterfall)
    p.setPen(QPen(QColor(0x00, 0x90, 0xff, 130), 1));
    p.drawLine(filterX1, specRect.top(), filterX1, specRect.bottom());
    p.drawLine(filterX2, specRect.top(), filterX2, specRect.bottom());

    // ── Slice center line ────────────────────────────────────────────────────

    p.setPen(QPen(QColor(0xff, 0xa0, 0x00, 220), 1.5));
    p.drawLine(sliceX, specRect.top(), sliceX, wfRect.bottom());

    // Triangle marker at top of spectrum
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xff, 0xa0, 0x00));
    QPolygon tri;
    tri << QPoint(sliceX - 6, specRect.top())
        << QPoint(sliceX + 6, specRect.top())
        << QPoint(sliceX, specRect.top() + 10);
    p.drawPolygon(tri);
}

// ─── Frequency scale bar ──────────────────────────────────────────────────────

void SpectrumWidget::drawFreqScale(QPainter& p, const QRect& r)
{
    p.fillRect(r, QColor(0x06, 0x06, 0x10));

    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double endMhz   = m_centerMhz + m_bandwidthMhz / 2.0;

    // Pick a step using 1-2-5 sequence to give ~5-10 labels at any zoom level.
    double rawStep = m_bandwidthMhz / 5.0;
    const double mag = std::pow(10.0, std::floor(std::log10(rawStep)));
    const double norm = rawStep / mag;
    double stepMhz;
    if      (norm >= 5.0) stepMhz = 5.0 * mag;
    else if (norm >= 2.0) stepMhz = 2.0 * mag;
    else                  stepMhz = 1.0 * mag;

    const double firstLine = std::ceil(startMhz / stepMhz) * stepMhz;

    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    const QFontMetrics fm(f);

    // Decimal places: enough to distinguish labels at this step size
    int decimals;
    if      (stepMhz < 0.0001) decimals = 6;
    else if (stepMhz < 0.001)  decimals = 5;
    else if (stepMhz < 0.01)   decimals = 4;
    else if (stepMhz < 1.0)    decimals = 3;
    else                        decimals = 2;

    for (double freq = firstLine; freq <= endMhz; freq += stepMhz) {
        const int x = mhzToX(freq);

        // Tick mark
        p.setPen(QColor(0x40, 0x60, 0x80));
        p.drawLine(x, r.top(), x, r.top() + 4);

        const QString label = QString::number(freq, 'f', decimals);
        const int tw = fm.horizontalAdvance(label);
        const int lx = qBound(0, x - tw / 2, width() - tw);

        p.setPen(QColor(0x70, 0x90, 0xb0));
        p.drawText(lx, r.bottom() - 2, label);
    }
}

} // namespace AetherSDR
