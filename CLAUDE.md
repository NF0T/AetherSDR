# AetherSDR — Project Context for Claude

## Project Goal

Replicate the **Windows-only FlexRadio SmartSDR client** (written in C#) as a
**Linux-native C++ application** using Qt6 and C++20. The aim is to mirror the
look, feel, and every function SmartSDR is capable of. The reference radio is a
**FLEX-8600 running firmware v1.4.0.0**.

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/AetherSDR
```

Dependencies (Arch): `qt6-base qt6-multimedia cmake ninja pkgconf`

Current version: **0.1.4** (set in both `CMakeLists.txt` and `README.md`).

---

## Architecture Overview

```
src/
├── main.cpp
├── core/
│   ├── RadioDiscovery      — UDP 4992 broadcast listener, emits radioDiscovered/Lost
│   ├── RadioConnection     — TCP 4992 command channel, V/H/R/S/M SmartSDR protocol
│   ├── CommandParser       — Stateless protocol line parser + command builder
│   ├── PanadapterStream    — VITA-49 UDP receiver: routes FFT, waterfall, audio by PCC
│   └── AudioEngine         — QAudioSink push-fed by PanadapterStream (RX); TX stub
├── models/
│   ├── RadioModel          — Central state: owns connection, slices, panadapter config
│   └── SliceModel          — Per-slice state (freq, mode, filter, DSP, RIT/XIT, etc.)
└── gui/
    ├── MainWindow          — Dark-themed QMainWindow, wires everything together
    ├── ConnectionPanel     — Radio list + connect/disconnect button
    ├── FrequencyDial       — Custom 9-digit MHz display with click/scroll/keyboard tuning
    ├── SpectrumWidget      — FFT spectrum + scrolling waterfall + frequency scale
    ├── AppletPanel         — Toggle-button column of applet panels (RX, TX stubs, etc.)
    └── RxApplet            — Full RX controls: antenna, filter, AGC, AF gain, pan, DSP, RIT/XIT
```

### Data Flow

```
UDP bcast (4992)  →  RadioDiscovery  →  ConnectionPanel (GUI)
TCP (4992)        →  RadioConnection →  RadioModel → SliceModel → GUI
UDP VITA-49 (4991)→  PanadapterStream
                       ├── PCC 0x8003 (FFT bins)      → SpectrumWidget.updateSpectrum()
                       ├── PCC 0x8004 (waterfall tiles)→ SpectrumWidget.updateWaterfallRow()
                       ├── PCC 0x03E3 (audio float32)  → AudioEngine.feedAudioData()
                       └── PCC 0x0123 (audio int16)    → AudioEngine.feedAudioData()
```

---

## SmartSDR Protocol (Firmware v1.4.0.0)

### Message Types

| Prefix | Dir | Meaning |
|--------|-----|---------|
| `V` | Radio→Client | Firmware version |
| `H` | Radio→Client | Hex client handle |
| `C` | Client→Radio | Command: `C<seq>\|<cmd>\n` |
| `R` | Radio→Client | Response: `R<seq>\|<hex_code>\|<body>` |
| `S` | Radio→Client | Status: `S<handle>\|<object> key=val ...` |
| `M` | Radio→Client | Informational message |

### Status Object Names

Object names are **multi-word**: `slice 0`, `display pan 0x40000000`,
`display waterfall 0x42000000`, `interlock band 9`. The parser finds the split
between object name and key=value pairs by locating the last space before the
first `=` sign.

### Connection Sequence

1. TCP connect → radio sends `V<version>` then `H<handle>`
2. Subscribe: `sub slice all`, `sub pan all`, `sub tx all`, `sub atu all`,
   `sub meter all`, `sub audio all`
3. `client gui` + `client program AetherSDR` + `client station AetherSDR`
4. Bind UDP socket, send `\x00` to radio:4992 (port registration)
5. `client udpport <port>` (returns error 0x50001000 on v1.4.0.0 — expected)
6. `slice list` → if empty, create default slice (14.225 MHz USB ANT1)
7. `stream create type=remote_audio_rx compression=none` → radio starts sending
   VITA-49 audio to our UDP port

### Firmware v1.4.0.0 Quirks

- `client set udpport` returns `0x50001000` — use the one-byte UDP packet method
- `display panafall create` returns `0x50000016` — use `panadapter create`
- Slice frequency is `RF_frequency` (not `freq`) in status messages
- All VITA-49 streams use `ExtDataWithStream` (type 3, top nibble `0x3`)
- Streams are discriminated by **PacketClassCode** (PCC), NOT by packet type

---

## VITA-49 Packet Format

### Header (28 bytes)

Words 0–6 of the VITA-49 header. Key field: **PCC** in lower 16 bits of word 3.

### Packet Class Codes

| PCC | Content | Payload Format |
|------|---------|---------------|
| `0x8003` | FFT panadapter bins | uint16 big-endian, linear map to dBm |
| `0x8004` | Waterfall tiles | 36-byte sub-header + uint16 bins |
| `0x03E3` | RX audio (uncompressed) | float32 stereo, big-endian |
| `0x0123` | DAX audio (reduced BW) | int16 mono, big-endian |
| `0x8002` | Meter data | (not yet decoded) |

### FFT Bin Conversion

```
dBm = min_dbm + (sample / 65535.0) × (max_dbm − min_dbm)
```

`min_dbm` / `max_dbm` come from `display pan` status messages (typically -135 / -40).

### FFT Frame Assembly

FFT data may span multiple VITA-49 packets. A 12-byte sub-header at offset 28
contains: `start_bin_index`, `num_bins`, `bin_size`, `total_bins_in_frame`,
`frame_index`. `PanadapterStream::FrameAssembler` stitches partial frames.

### Waterfall Tile Format

36-byte sub-header at offset 28:

| Offset | Type | Field |
|--------|------|-------|
| 0 | int64 | FrameLowFreq |
| 8 | int64 | BinBandwidth |
| 16 | uint32 | LineDurationMS |
| 20 | uint16 | Width |
| 22 | uint16 | Height |
| 24 | uint32 | Timecode |
| 28 | uint32 | AutoBlackLevel |
| 32 | uint16 | TotalBinsInFrame |
| 34 | uint16 | FirstBinIndex |

Payload: `Width × Height` uint16 bins (big-endian). Conversion:

```
intensity = static_cast<int16>(raw_uint16) / 128.0f
```

This yields an **arbitrary positive intensity scale** (NOT actual dBm).
Observed values: noise floor ~96–106, signal peaks ~110–115 on HF.
The waterfall colour range is calibrated to [104, 120] by default and is
**decoupled** from the FFT spectrum's dBm range.

### Audio Payload

- PCC 0x03E3: big-endian float32 stereo → byte-swap uint32, memcpy to float,
  scale to int16 for QAudioSink (24 kHz stereo)
- PCC 0x0123: big-endian int16 mono → byte-swap, duplicate to stereo

### Stream IDs (observed)

- `0x40000000` — panadapter FFT (same as pan object ID)
- `0x42000000` — waterfall tiles
- `0x04xxxxxx` — remote audio RX (dynamically assigned)
- `0x00000700` — meter data

---

## GUI Design

### Theme

Dark theme: background `#0f0f1a`, text `#c8d8e8`, accent `#00b4d8`, borders `#203040`.

### Layout

Three-pane horizontal splitter:
1. **ConnectionPanel** (left) — radio list, connect/disconnect
2. **Center** — SpectrumWidget (top: FFT 40%, bottom: waterfall 60%, frequency
   scale bar 20px), FrequencyDial below, mode selector, TX button, volume controls
3. **AppletPanel** (right, 260px fixed) — toggle-button column for RX/TX/etc. applets

### SpectrumWidget

- FFT spectrum: exponential smoothing (α=0.35), dB grid every 20 dB, freq grid ~50 kHz
- Waterfall: 7-stop colour gradient (black→dark blue→blue→cyan→green→yellow→red)
- Overlays: filter passband (semi-transparent), slice center line (orange + triangle)
- Mouse: click-to-tune (snapped to step size), scroll-wheel tunes by step size
- Native waterfall tiles (PCC 0x8004) suppress FFT-derived waterfall rows

### FrequencyDial

- 9 digits: `XXX.XXX.XXX` (MHz.kHz.Hz)
- Click top/bottom half of digit to tune up/down by that place value
- Scroll wheel over digit tunes that specific digit
- Scroll wheel elsewhere tunes by step size
- Double-click for direct text entry
- Range: 0.001–54.0 MHz

### RxApplet Controls

Header row → step stepper → filter presets → AGC mode+threshold →
AF gain + audio pan → squelch → NB/NR/ANF DSP toggles → RIT → XIT.

Step sizes: 10, 50, 100, 250, 500, 1000, 2500, 5000, 10000 Hz.

---

## Key Implementation Patterns

### GUI↔Radio Sync (No Feedback Loops)

- `SliceModel` setters emit `commandReady(cmd)` → `RadioModel` sends to radio
- Radio status pushes update `SliceModel` via `applyStatus(kvs)`
- `MainWindow` uses `m_updatingFromModel` guard to prevent echoing model updates
  back to the radio
- `RxApplet` uses `QSignalBlocker` extensively when updating UI from model state

### Auto-Reconnect

`RadioModel` has a 3-second `m_reconnectTimer` for unexpected disconnects.
Disabled by `m_intentionalDisconnect` flag on user-initiated disconnect.

### SmartConnect

If the radio already has slices (`slice list` returns IDs), `RadioModel` fetches
them with `slice get <id>` rather than creating new ones.

---

## Known Quirks / Lessons Learned

- `QMap<K,V>` needs `#include <QMap>` in headers — forward-declaration in
  `qcontainerfwd.h` leaves the field as incomplete type
- `static constexpr` class members are private by default; file-scope
  `constexpr` copies needed for free functions in the same .cpp
- Qt6: `Qt::AA_UseHighDpiPixmaps` removed — do not use
- Qt6: `QMenu::addAction(text, obj, slot, shortcut)` deprecated — use
  `addAction(text)` + `setShortcut()` + `connect()` separately
- Filter width presets are mode-aware: LSB/DIGL/CWL use negative low offset,
  CW centers 200 Hz above, others use 0 to high

---

## What's Implemented (v0.1.4)

- UDP radio discovery and TCP command/control
- SmartSDR V/H/R/S/M protocol parsing
- Panadapter VITA-49 FFT spectrum display with dBm calibration
- Native VITA-49 waterfall tiles (PCC 0x8004) with colour mapping
- Audio RX (float32 stereo + int16 mono) via VITA-49 → QAudioSink
- Volume / mute control with RMS level meter
- Full RX applet: antennas, filter presets, AGC, AF gain, pan, squelch,
  NB/NR/ANF, RIT/XIT, tuning step stepper, tune lock
- Frequency dial: click, scroll, keyboard, direct entry
- Spectrum: click-to-tune, scroll-to-tune, filter passband overlay
- AppletPanel: toggle-button column for independent applet visibility
- TX button (sends `xmit 1` / `xmit 0`)
- Persistent window geometry

## What's NOT Yet Implemented

- Slice filter passband shading on spectrum
- Multi-slice support (slice tabs or overlaid markers)
- Audio TX (microphone → radio, full VITA-49 framing)
- Band stacking / band map
- CW keyer / memories
- Equalizer (EQ applet)
- Full meter display (SWR, ALC, power, etc.)
- DAX / CAT interface
- Spot / DX cluster integration
- Memory channels
- Macro / voice keyer
- Network audio (Opus compression)
- TNF (tracking notch filter) management
- Antenna tuner (ATU) control
- Full TX applet (mic gain, compression, monitor, etc.)
