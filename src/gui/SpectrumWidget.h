#pragma once

#include <QWidget>
#include <QVector>
#include <QImage>

namespace AetherSDR {

// Panadapter / spectrum display widget.
//
// Layout (top to bottom):
//   ~40% — spectrum line plot (current FFT frame, smoothed)
//   ~60% — waterfall (scrolling heat-map history)
//   20px — absolute frequency scale bar
//
// Overlays (drawn on top of spectrum + waterfall):
//   - Filter passband: semi-transparent band from filterLow to filterHigh Hz
//   - Slice marker: vertical orange line at slice center frequency
//
// Click anywhere in the spectrum/waterfall area to emit frequencyClicked().
class SpectrumWidget : public QWidget {
    Q_OBJECT

public:
    explicit SpectrumWidget(QWidget* parent = nullptr);

    QSize sizeHint() const override { return {800, 300}; }

    // Set the frequency range covered by this panadapter.
    void setFrequencyRange(double centerMhz, double bandwidthMhz);

    // Feed a new FFT frame. bins are scaled dBm values.
    void updateSpectrum(const QVector<float>& binsDbm);

    // Feed a single waterfall row from a VITA-49 waterfall tile.
    // lowFreqMhz/highFreqMhz describe the tile's frequency span.
    // When waterfall tile data is available, this is used instead of
    // the FFT-derived waterfall rows from updateSpectrum().
    void updateWaterfallRow(const QVector<float>& binsDbm,
                            double lowFreqMhz, double highFreqMhz);

    // Update the dBm range used for the waterfall colour map and spectrum Y axis.
    void setDbmRange(float minDbm, float maxDbm);

    // Set the active slice center frequency (draws the orange marker).
    void setSliceFrequency(double freqMhz);

    // Set the active slice filter edges (Hz offsets from slice center).
    void setSliceFilter(int lowHz, int highHz);

    // Set the click/scroll tuning step size in Hz (default 100).
    void setStepSize(int hz) { m_stepHz = hz; }

signals:
    // Emitted when the user clicks or scrolls in the panadapter area.
    void frequencyClicked(double mhz);
    // Emitted when the user drags the frequency scale bar to change bandwidth.
    void bandwidthChangeRequested(double newBandwidthMhz);
    // Emitted when the user drags the waterfall to pan the center frequency.
    void centerChangeRequested(double newCenterMhz);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void drawGrid(QPainter& p, const QRect& r);
    void drawSpectrum(QPainter& p, const QRect& r);
    void drawSliceOverlay(QPainter& p, const QRect& specRect, const QRect& wfRect);
    void drawWaterfall(QPainter& p, const QRect& r);
    void drawFreqScale(QPainter& p, const QRect& r);

    void pushWaterfallRow(const QVector<float>& bins, int destWidth,
                          double tileLowMhz = -1, double tileHighMhz = -1);
    QRgb dbmToRgb(float dbm) const;

    // Pixel x coordinate for a given frequency in MHz (0 = left edge).
    int mhzToX(double mhz) const;

    QVector<float> m_bins;       // raw FFT frame (dBm)
    QVector<float> m_smoothed;   // exponential-smoothed for visual stability

    double m_centerMhz{14.225};
    double m_bandwidthMhz{0.200};
    double m_sliceFreqMhz{14.225};
    int    m_filterLowHz{-1500};   // Hz below slice center
    int    m_filterHighHz{1500};   // Hz above slice center

    float m_refLevel{-50.0f};       // top of display (dBm)
    float m_dynamicRange{100.0f};   // dB range shown in spectrum (-50 to -150)

    // Tuning step size for click-snap and wheel scroll (Hz)
    int m_stepHz{100};

    // Waterfall colour range (dBm). Using FFT-derived data, so these are
    // real dBm values. Noise floor ~-130 dBm, strong signals ~-50 dBm.
    float m_wfMinDbm{-130.0f};
    float m_wfMaxDbm{-50.0f};

    // Scrolling waterfall image (Format_RGB32)
    QImage m_waterfall;

    // True once we receive native waterfall tile data (PCC 0x8004).
    // When set, updateSpectrum() skips pushing FFT rows to the waterfall
    // because the radio provides dedicated waterfall tiles.
    bool m_hasNativeWaterfall{false};

    static constexpr float SMOOTH_ALPHA    = 0.35f;
    // Fraction of the panadapter area (above freq scale) used for spectrum
    float m_spectrumFrac{0.40f};
    // Height of the frequency scale bar
    static constexpr int   FREQ_SCALE_H    = 20;
    // Height of the draggable divider between FFT and freq scale
    static constexpr int   DIVIDER_H       = 4;
    // Divider drag state
    bool m_draggingDivider{false};
    // Bandwidth drag state (freq scale bar)
    bool m_draggingBandwidth{false};
    int  m_bwDragStartX{0};
    double m_bwDragStartBw{0.0};
    // Waterfall pan drag state
    bool m_draggingPan{false};
    int  m_panDragStartX{0};
    double m_panDragStartCenter{0.0};
};

} // namespace AetherSDR
