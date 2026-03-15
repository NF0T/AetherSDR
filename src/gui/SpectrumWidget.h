#pragma once

#include <QWidget>
#include <QVector>
#include <QImage>

namespace AetherSDR {

class SpectrumOverlayMenu;

// Panadapter / spectrum display widget.
//
// Layout (top to bottom):
//   ~40% — spectrum line plot (current FFT frame, smoothed)
//   ~60% — waterfall (scrolling heat-map history)
//   20px — absolute frequency scale bar
//
// Overlays (drawn on top of spectrum + waterfall):
//   - Filter passband: semi-transparent band from filterLow to filterHigh Hz
//   - VFO marker: vertical orange line at the tuned VFO frequency
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

    // Set the VFO frequency (draws the orange VFO marker).
    void setVfoFrequency(double freqMhz);

    // Set the filter edges (Hz offsets from VFO frequency).
    void setVfoFilter(int lowHz, int highHz);

    // Getters for band settings capture.
    float spectrumFrac()  const { return m_spectrumFrac; }
    float refLevel()      const { return m_refLevel; }
    float dynamicRange()  const { return m_dynamicRange; }

    // Set the FFT/waterfall split ratio programmatically.
    void setSpectrumFrac(float f);

    // Set the click/scroll tuning step size in Hz (default 100).
    void setStepSize(int hz) { m_stepHz = hz; }

    // Set the per-mode filter limits (Hz). Called when mode changes.
    void setFilterLimits(int minHz, int maxHz) { m_filterMinHz = minHz; m_filterMaxHz = maxHz; }

    // Set the current demod mode (for zoom centering behavior).
    void setMode(const QString& mode) { m_mode = mode; }

    // Access the floating overlay menu (for wiring signals).
    SpectrumOverlayMenu* overlayMenu() const { return m_overlayMenu; }

    // WNB and RF gain state for on-screen indicators.
    bool wnbActive()   const { return m_wnbActive; }
    int  rfGainValue() const { return m_rfGainValue; }
    void setWnbActive(bool on) { m_wnbActive = on; update(); }
    void setRfGain(int gain) { m_rfGainValue = gain; update(); }

    // Set slice info for the off-screen VFO indicator.
    void setSliceInfo(int sliceId, bool isTxSlice) {
        m_sliceId = sliceId; m_isTxSlice = isTxSlice;
    }

signals:
    // Emitted when the user clicks or scrolls in the panadapter area.
    void frequencyClicked(double mhz);
    // Emitted when the user drags the frequency scale bar to change bandwidth.
    void bandwidthChangeRequested(double newBandwidthMhz);
    // Emitted when the user drags the waterfall to pan the center frequency.
    void centerChangeRequested(double newCenterMhz);
    // Emitted when the user drags a filter edge to resize the passband.
    void filterChangeRequested(int lowHz, int highHz);
    // Emitted when the user adjusts the dBm scale (drag or arrows).
    void dbmRangeChangeRequested(float minDbm, float maxDbm);

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
    void drawVfoMarker(QPainter& p, const QRect& specRect, const QRect& wfRect);
    void drawWaterfall(QPainter& p, const QRect& r);
    void drawFreqScale(QPainter& p, const QRect& r);
    void drawDbmScale(QPainter& p, const QRect& specRect);
    void drawOffScreenVfo(QPainter& p, const QRect& specRect);

    void pushWaterfallRow(const QVector<float>& bins, int destWidth,
                          double tileLowMhz = -1, double tileHighMhz = -1);
    QRgb dbmToRgb(float dbm) const;

    // Pixel x coordinate for a given frequency in MHz (0 = left edge).
    int mhzToX(double mhz) const;
    // Convert pixel x back to MHz.
    double xToMhz(int x) const;

    QVector<float> m_bins;       // raw FFT frame (dBm)
    QVector<float> m_smoothed;   // exponential-smoothed for visual stability

    double m_centerMhz{14.225};
    double m_bandwidthMhz{0.200};
    double m_vfoFreqMhz{14.225};
    int    m_filterLowHz{-1500};   // Hz offset from VFO
    int    m_filterHighHz{1500};   // Hz offset from VFO
    int    m_filterMinHz{-12000};  // per-mode lower bound
    int    m_filterMaxHz{12000};   // per-mode upper bound
    QString m_mode{"USB"};         // current demod mode
    int     m_sliceId{0};          // slice index (0=A, 1=B, etc.)
    bool    m_isTxSlice{false};    // whether this is the TX slice

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
    // Filter edge drag state
    enum class FilterEdge { None, Low, High };
    FilterEdge m_draggingFilter{FilterEdge::None};
    // dBm scale strip drag state
    static constexpr int DBM_STRIP_W = 36;  // width of the dBm scale strip
    static constexpr int DBM_ARROW_H = 14;  // height of each arrow button
    bool  m_draggingDbm{false};
    int   m_dbmDragStartY{0};
    float m_dbmDragStartRef{0.0f};
    // Off-screen VFO indicator hit rect (for hover/double-click)
    QRect m_offScreenVfoRect;
    bool  m_hoveringOffScreenVfo{false};

    // On-screen indicators (WNB, RF Gain)
    bool m_wnbActive{false};
    int  m_rfGainValue{0};

    // Floating overlay menu (child widget, anchored top-left)
    SpectrumOverlayMenu* m_overlayMenu{nullptr};
};

} // namespace AetherSDR
