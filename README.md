<p align="center">
  <img src="docs/logo-circle.png" alt="AetherSDR Logo" width="200">
</p>

# AetherSDR

**A Linux-native client for FlexRadio Systems transceivers**

[![CI](https://github.com/ten9876/AetherSDR/actions/workflows/ci.yml/badge.svg)](https://github.com/ten9876/AetherSDR/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Qt6](https://img.shields.io/badge/Qt-6-green.svg)](https://www.qt.io/)

AetherSDR brings FlexRadio operation to Linux without Wine or virtual machines. Built from the ground up with Qt6 and C++20, it speaks the SmartSDR protocol natively and aims to replicate the full SmartSDR experience.

**Current version: 0.2.8** | [Releases](https://github.com/ten9876/AetherSDR/releases) | [Discussions](https://github.com/ten9876/AetherSDR/discussions)

![AetherSDR Screenshot](docs/screenshot-v4.png)

---

## Supported Radios

Tested with the **FLEX-8600** running v4.1.5 software. Should work with other FlexRadio models that use the SmartSDR protocol (FLEX-6000 series, FLEX-8000 series).

---

## Features

### Panadapter & Waterfall
- Real-time FFT spectrum and scrolling waterfall display
- Draggable FFT/waterfall split, bandwidth zoom, pan
- Draggable filter passband edges
- dBm scale with drag-to-adjust
- Floating VFO widget with S-meter, frequency, and quick controls
- Band selector with ARRL band plan defaults
- Display sub-menu: AVG, FPS, FFT fill (opacity + color), weighted average
- Waterfall controls: gain, black level (+ auto), scroll rate
- Native VITA-49 waterfall tiles (PCC 0x8004) with full frame assembly
- FFT and waterfall rendering fully decoupled

### Receiver Controls
- Full RX controls: antenna, filter presets, AGC, AF gain, pan, squelch
- All DSP modes: NB, NR, ANF, NRL, NRS, RNN, NRF, ANFL, ANFT, APF
- DSP level sliders (0-100) for all supported features
- Mode-specific controls (CW pitch-centered filters, RTTY mark/shift, DIG offset, FM duplex)
- RIT / XIT with step buttons
- Per-mode filter presets and tuning step sizes

### Transmit Controls
- TX power and tune power sliders
- TX/mic profile management
- TUNE, MOX, ATU, and MEM buttons
- ATU status indicators and APD control
- P/CW applet: mic level/compression gauges, mic source, processor, monitor
- PHONE applet: VOX, AM carrier, TX filter
- 8-band graphic equalizer (RX and TX)

### Metering
- Analog S-Meter gauge with peak hold
- TX power, SWR, mic level, and compression gauges
- 4o3a Tuner Genius XL (TGXL) applet with relay bars and autotune

### FM Duplex
- CTCSS tone encode (41 standard tones)
- Repeater offset with simplex/up/down direction
- REV (reverse) toggle

### CAT Control & Integration
- Hamlib rigctld-compatible TCP server (port 4532) for WSJT-X, fldigi, N1MM
- Virtual serial port (PTY) at `/tmp/AetherSDR-CAT`
- Autostart options for rigctld and TTY
- Supports: get/set frequency, get/set mode, PTT, split, dump_state

### Radio Setup
- Full settings dialog (8 tabs): Radio, Network, GPS, TX, Phone/CW, RX, Filters, XVTR
- Per-band TX settings: RF power, tune power, PTT inhibit, interlock routing
- TX profile management
- XVTR transverter configuration
- Network diagnostics, memory channels, spot settings

### General
- Auto-discovery of radios on the network
- Auto-reconnect on connection loss
- Auto-connect to last used radio on launch
- Click-to-tune and scroll-wheel tuning on spectrum
- Multi-Flex support (independent operation alongside SmartSDR/Maestro)
- XML settings persistence (SSDR-compatible format)
- Persistent window layout and display preferences
- Desktop integration (`.desktop` file, icon, `cmake --install`)
- PC audio TX via DAX stream (mic TX also supported)

---

## Installation

### Dependencies

```bash
# Arch / CachyOS / Manjaro
sudo pacman -S qt6-base qt6-multimedia cmake ninja pkgconf

# Ubuntu 24.04+ / Debian
sudo apt install qt6-base-dev qt6-multimedia-dev cmake ninja-build pkg-config

# Fedora
sudo dnf install qt6-qtbase-devel qt6-qtmultimedia-devel cmake ninja-build

# macOS (Homebrew)
brew install qt@6 ninja portaudio pkgconf cmake
```

### Build & Run

```bash
git clone https://github.com/ten9876/AetherSDR.git
cd AetherSDR
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/AetherSDR
```

The application will automatically discover FlexRadio transceivers on your local network.

### Install (optional)

Install the binary, desktop entry, and icon system-wide:

```bash
sudo cmake --install build
```

This places `AetherSDR` in `/usr/local/bin`, the `.desktop` file in the app launcher, and the icon in the system icon theme. You can then launch from your application menu or by typing `AetherSDR` in a terminal.

---

## Roadmap

- [ ] DAX audio channels — PipeWire virtual devices for digital mode apps (FreeDV, WSJT-X, fldigi, JS8Call)
- [ ] Multi-slice support
- [ ] TNF (tracking notch filter) management
- [ ] Band stacking registers
- [ ] Spot / DX cluster integration
- [ ] CW keyer and memory support
- [ ] SmartLink remote operation
- [ ] Keyboard shortcuts and hotkeys

See the full [issue tracker](https://github.com/ten9876/AetherSDR/issues) for 45+ tracked features and enhancements.

---

## Contributing

PRs and bug reports welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

The codebase is modular — each subsystem (core protocol, models, GUI widgets) can be worked on independently. Check [Issues](https://github.com/ten9876/AetherSDR/issues) for current tasks.

---

## License

AetherSDR is free and open-source software licensed under the [GNU General Public License v3](LICENSE).

*AetherSDR is an independent project and is not affiliated with or endorsed by FlexRadio Systems.*
