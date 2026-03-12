# AetherSDR Linux Native Client

A Linux-native SmartSDR-compatible client for FlexRadio Systems transceivers,
built with **Qt6** and **C++20**.

---

## Features (v0.1 prototype)

| Feature | Status |
|---|---|
| UDP discovery (port 4992) | ✅ |
| TCP command/control connection | ✅ |
| SmartSDR protocol parser (V/H/R/S/M) | ✅ |
| Slice model (frequency, mode, filter) | ✅ |
| Frequency dial widget (scroll/click) | ✅ |
| Mode selector (USB/LSB/CW/AM/FM/DIG…) | ✅ |
| Audio RX via VITA-49 UDP + Qt Multimedia | ✅ |
| Audio TX (microphone → radio) | ✅ |
| Volume / mute control | ✅ |
| Panadapter spectrum widget (stub/placeholder) | ✅ |
| TX button | ✅ |
| Persistent window geometry | ✅ |

---

## Architecture

```
src/
├── main.cpp
├── core/
│   ├── RadioDiscovery.h/.cpp    # UDP broadcast listener (port 4992)
│   ├── RadioConnection.h/.cpp   # TCP command channel + heartbeat
│   ├── CommandParser.h/.cpp     # Stateless line parser/builder
│   └── AudioEngine.h/.cpp       # VITA-49 UDP → Qt Multimedia audio
├── models/
│   ├── RadioModel.h/.cpp        # Central radio state, owns connection
│   └── SliceModel.h/.cpp        # Per-slice receiver state
└── gui/
    ├── MainWindow.h/.cpp        # Main application window
    ├── FrequencyDial.h/.cpp     # Custom 9-digit frequency widget
    ├── ConnectionPanel.h/.cpp   # Radio list + connect/disconnect
    └── SpectrumWidget.h/.cpp    # Panadapter display (FFT bins)
```

### Data flow

```
                   ┌─────────────────────┐
 UDP bcast (4992)  │   RadioDiscovery    │
 ──────────────────▶   (QUdpSocket)      │──▶ ConnectionPanel (GUI)
                   └─────────────────────┘

 TCP (4992)        ┌─────────────────────┐
 ──────────────────▶   RadioConnection   │──▶ RadioModel ──▶ SliceModel ──▶ GUI
 Commands ◀────────   (QTcpSocket)       │
                   └─────────────────────┘

 UDP audio         ┌─────────────────────┐
 ──────────────────▶   AudioEngine       │──▶ QAudioSink (speakers)
 Mic audio ◀───────   (VITA-49 framing)  │◀── QAudioSource (microphone)
                   └─────────────────────┘
```

---

## Building

### Dependencies

```bash
# Arch / CachyOS
sudo pacman -S qt6-base qt6-multimedia cmake ninja pkgconf

# Ubuntu 24.04+
sudo apt install qt6-base-dev qt6-multimedia-dev cmake ninja-build pkg-config
```

### Configure & build

```bash
git clone https://github.com/ten9876/AetherSDR.git
cd AetherSDR
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/AetherSDR
```

### Debug build (with AddressSanitizer)

```bash
cmake -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dbg -j$(nproc)
./build-dbg/AetherSDR
```

---

## SmartSDR Protocol Notes

The protocol is ASCII-based over TCP port 4992.

| Prefix | Direction | Example |
|--------|-----------|---------|
| `V`    | Radio→Client | `V3.3.28.0` — firmware version |
| `H`    | Radio→Client | `H0A1B2C3D` — assigned client handle |
| `C`    | Client→Radio | `C1\|sub slice all` — command |
| `R`    | Radio→Client | `R1\|0\|` — response (0 = OK) |
| `S`    | Radio→Client | `S0A1B\|slice 0 freq=14.225000 mode=USB` |
| `M`    | Radio→Client | `M0A1B\|<encoded message>` |

---

## Next Steps — Panadapter & Spectrum Visualization

The `SpectrumWidget` is ready to receive data. To complete the panadapter:

### 1. Subscribe & negotiate the panadapter stream

After connecting, send:
```
C5|panadapter create
```
The radio responds with a stream ID and the UDP port it will use.
Then send audio stream creation for each panadapter:
```
C6|stream create type=panadapter pan=<pan_id>
```

### 2. Parse VITA-49 panadapter datagrams

Each UDP packet from the panadapter stream has a 28-byte VITA-49 header
followed by **16-bit signed integer FFT bins** (little-endian), each
representing `(value / 128.0) - 128.0` dBm.

Add to `AudioEngine` or a new `PanadapterStream` class:

```cpp
// In a new PanadapterStream class:
void PanadapterStream::processDatagram(const QByteArray& data)
{
    // Skip 28-byte VITA-49 header
    const int16_t* bins = reinterpret_cast<const int16_t*>(data.constData() + 28);
    const int numBins   = (data.size() - 28) / 2;

    QVector<float> dbm(numBins);
    for (int i = 0; i < numBins; ++i)
        dbm[i] = bins[i] / 128.0f - 128.0f;

    emit spectrumReady(dbm);
}
```

Connect `spectrumReady` → `SpectrumWidget::updateSpectrum`.

### 3. Add waterfall history

Extend `SpectrumWidget` with a scrolling `QImage` (waterfall):

```cpp
// In SpectrumWidget:
QImage m_waterfall;   // width = numBins, height = history lines

void SpectrumWidget::appendWaterfallLine(const QVector<float>& dbm)
{
    // Scroll up by 1 row
    m_waterfall.scroll(0, 1, m_waterfall.rect());
    // Map dBm → color for top row
    for (int x = 0; x < dbm.size(); ++x) {
        const float norm = (dbm[x] - minDbm) / (maxDbm - minDbm);
        m_waterfall.setPixelColor(x, 0, dbmToColor(norm));
    }
    update();
}
```

### 4. Add OpenGL rendering (optional, for high FFT bin counts)

Replace `QPainter` with a `QOpenGLWidget` and upload the FFT data as a 1D
texture rendered via a simple fragment shader. This allows 4096+ bins at 30 fps
without CPU-side compositing overhead.

```glsl
// fragment shader sketch
uniform sampler1D u_spectrum;
uniform float u_refLevel;
uniform float u_dynamicRange;

void main() {
    float dbm  = texture(u_spectrum, texCoord.x).r;
    float norm = clamp((u_refLevel - dbm) / u_dynamicRange, 0.0, 1.0);
    fragColor  = mix(vec4(0,0,0.5,1), vec4(0,1,1,1), norm);
}
```

### 5. Add slice filter shading

Draw a translucent rectangle between `filterLow` and `filterHigh` offsets from
the slice frequency to visually show the passband on the panadapter.

---

## Contributing

PRs welcome. See the modular architecture above — each subsystem is independent
and can be developed/tested in isolation.
