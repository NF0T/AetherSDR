# AetherSDR Audio Pipeline Test Plan

**Version:** v0.8.15.1
**Last updated:** 2026-04-15
**Purpose:** Comprehensive validation of all audio paths — RX, TX, TCI, DAX, DSP, and recovery mechanisms.

---

## Prerequisites

- FLEX radio connected, at least one slice active on a band with signals
- Headphones or speakers with stereo capability (for pan testing)
- WSJT-X or JTDX installed (for TCI testing)
- Protocol logging enabled (Support & Diagnostics → Commands/Status)
- A second slice available (for multi-slice tests)

---

## A. Basic RX Audio (Radio → Speaker)

### A1. Audio plays on connect
1. Launch AetherSDR, connect to radio
2. Tune to an active frequency (e.g., WWV 10 MHz, or a busy band)
3. Audio should play immediately — no silence, no stuttering

### A2. Volume slider full range
1. Drag master volume slider from 0 to 100
2. Volume should scale smoothly from silence to full
3. No clicks, pops, or distortion at any position

### A3. Mute/unmute
1. Click speaker icon to mute
2. Audio stops immediately
3. Click again to unmute
4. Audio resumes immediately — no restart delay

### A4. AF gain per-slice
1. Adjust the AF gain slider in the applet panel
2. Volume should change for the active slice only
3. Smooth transition, no clicks

### A5. Audio survives idle (15+ minutes)
1. Leave AetherSDR connected and idle for 15+ minutes
2. Return — audio should still be playing
3. Check log for "no audio data received" warnings — should NOT appear during normal operation
4. If liveness watchdog fires, note the circumstances

### A6. Audio survives screensaver/sleep wake
1. Let the system go to screensaver or sleep
2. Wake the system
3. Audio should resume within a few seconds
4. Check log for restart messages — one restart is acceptable, repeated restarts are a bug

---

## B. Audio Format Integrity

### B1. No buffer cap drops under normal load
1. Connect, listen to audio for 2 minutes
2. Open Support & Diagnostics → Network tab
3. Check RX buffer stats — underrun count should be near zero
4. Peak buffer size should be well under the 200ms cap

### B2. 48kHz output device
1. If your system uses a 48kHz output device (most do)
2. Audio should play without pitch shift or speed change
3. Verify in log: "RX stream started at 48000 Hz" or "24000 Hz"

### B3. Bandwidth-reduced audio (PCC 0x0123)
1. Narrow the slice bandwidth significantly (e.g., CW narrow filter)
2. Audio should still play — no silence from reduced-bandwidth packets
3. The int16 mono → float32 stereo conversion should be transparent

---

## C. DSP Paths

### C1. No DSP baseline
1. Disable all noise reduction (NR2, NR4, BNR, DFNR all off)
2. Audio should play cleanly — this is the reference

### C2. RN2 (RNNoise)
1. Enable RN2
2. Audio should play with noise reduction applied
3. **Pan test**: set pan control fully left, then fully right
4. Audio should pan correctly (RN2 is true stereo)

### C3. NR2 (Spectral)
1. Enable NR2
2. Audio should play with noise reduction applied
3. **Pan test**: set pan fully left, then fully right
4. **KNOWN BUG**: pan will be centered regardless of setting (#1460)
5. Note: this is expected until #1461 is merged

### C4. NR4 (Specbleach)
1. Enable NR4
2. Audio should play with noise reduction applied
3. **Pan test**: same as C3 — **KNOWN BUG**: pan lost (#1460)

### C5. BNR (NVIDIA)
1. Enable BNR (requires NVIDIA BNR service running)
2. Audio should play after ~50ms priming delay
3. **Pan test**: same as C3 — **KNOWN BUG**: pan lost (#1460)
4. Note the ~50ms onset delay — this is expected

### C6. DFNR (DeepFilter)
1. Enable DFNR
2. Audio should play with noise reduction applied
3. **Pan test**: set pan fully left, then fully right
4. Pan should work correctly (DFNR is true stereo)

### C7. DSP toggle rapid switching
1. Rapidly toggle NR2 on/off 5 times
2. Audio should not cut out, stutter, or require restart
3. Repeat with NR4, BNR, DFNR

### C8. DSP during TX
1. Enable NR2 (or any DSP)
2. Key TX (PTT or MOX)
3. DSP should bypass during TX (no processing of silence/sidetone)
4. Unkey — DSP should resume immediately

---

## D. TCI Audio (WSJT-X / JTDX)

### D1. TCI audio starts on audio_start
1. Launch WSJT-X configured for TCI (localhost:50001)
2. WSJT-X should receive audio and show waterfall activity
3. Check log for "TCI: creating DAX RX stream" and "TCI: registered DAX RX stream"

### D2. TCI audio format negotiation
1. WSJT-X typically requests int16 48kHz
2. Audio should decode properly — FT8/FT4 decodes appearing
3. No silence, no garbled decodes

### D3. TCI survives PC audio mute
1. While WSJT-X is decoding via TCI
2. Mute PC audio (speaker icon)
3. WSJT-X should continue decoding — TCI audio is independent of PC speaker

### D4. TCI client disconnect/reconnect
1. Close WSJT-X
2. Check log for "TCI: removed DAX RX stream" and "TCI: releasing DAX channel"
3. Reopen WSJT-X — TCI should reconnect and audio should flow again

### D5. TCI with DAX already running
1. Enable DAX in the DIGI applet (Autostart DAX)
2. Then connect WSJT-X via TCI
3. Both should work simultaneously
4. Disconnect WSJT-X — user's DAX should remain active

### D6. TCI control-only (no audio)
1. Connect a TCI client that only sends control commands (e.g., StreamDeck)
2. No `audio_start` sent
3. Check log — no DAX RX streams should be created
4. No impact on PC audio

---

## E. Multi-Slice Audio

### E1. Switch active slice
1. Create two slices on different bands
2. Switch between them by clicking slice tabs
3. Audio should switch to the new active slice immediately

### E2. TX Follows Active / Active Follows TX
1. Enable "Active Slice Follows TX" in Radio Setup → TX
2. Have WSJT-X move TX to slice B
3. Applet panel should switch to slice B
4. Disable the toggle — behavior should stop

### E3. Two slices simultaneous audio
1. Create two slices
2. Both should contribute audio to the speaker (mixed)
3. Mute one slice — only the other should play

---

## F. TX Audio (Mic → Radio)

### F1. Voice TX
1. Select a voice mode (SSB)
2. Key PTT, speak into mic
3. Monitor on the radio or a second receiver — voice should be clean
4. No distortion, no clipping, correct sideband

### F2. TX audio level
1. Adjust mic gain in Radio Setup → Audio
2. ALC meter should respond proportionally
3. No hard clipping at high gain

### F3. DAX TX (digital modes)
1. Configure WSJT-X for TCI TX
2. Initiate a TX cycle (FT8 call)
3. TX audio should flow from WSJT-X → TCI → radio
4. Monitor — should hear clean FT8 tones, correct timing

---

## G. Recovery Mechanisms

### G1. USB audio device disconnect
1. If using USB audio, unplug the device during playback
2. AetherSDR should detect the change and attempt restart
3. Plug device back in — audio should resume
4. Check log for "audio output device list changed, restarting RX"

### G2. No false restarts during normal operation
1. Listen to audio for 10 minutes with no interaction
2. Check log — should see zero "restarting RX" messages
3. Any restart during normal operation is a bug

### G3. Zombie watchdog does not trigger falsely
1. Listen to audio for 5 minutes
2. Check log for "sink appears zombie" — should not appear
3. If it does, note the audio device and OS

### G4. Liveness watchdog does not trigger during TX
1. Key TX for 20+ seconds (CW or voice)
2. Unkey
3. Audio should resume immediately
4. Check log — liveness watchdog should NOT fire (TX suppresses audio feed but watchdog should not restart during this gap)

---

## H. QSO Recorder

### H1. Record and playback
1. Enable QSO recorder
2. Listen to a signal for 10 seconds
3. Stop recording, play back
4. Playback should sound identical to live audio — no pitch shift, no noise

### H2. Recording format
1. Find the recorded WAV file
2. Verify: 24kHz, stereo, 16-bit int (WAV header)
3. Play in an external player — should sound correct

---

## I. CW Decoder

### I1. CW decode from audio tap
1. Tune to a CW signal, switch to CW mode
2. CW decode panel should appear and show decoded text
3. Decoded text should match the signal

### I2. CW decoder does not affect speaker audio
1. While CW decoder is active
2. Speaker audio should be unaffected — no volume change, no artifacts

---

## Pass/Fail Criteria

| Area | Pass | Known Fail (Expected) |
|------|------|-----------------------|
| Basic RX (A1-A6) | Clean audio, no silence, no stuttering | — |
| Buffer integrity (B1-B3) | No drops under normal load | — |
| RN2 pan (C2) | Pan works | — |
| NR2/NR4/BNR pan (C3-C5) | — | Pan lost (#1460, fix pending) |
| DFNR pan (C6) | Pan works | — |
| DSP switching (C7) | No audio dropout | — |
| TCI int16 (D2) | WSJT-X decodes | — |
| TCI + mute (D3) | TCI survives PC mute | — |
| TCI lifecycle (D4) | Clean connect/disconnect | — |
| Multi-slice (E1-E3) | Audio switches with slice | — |
| Voice TX (F1-F2) | Clean TX audio | — |
| DAX TX (F3) | Clean FT8 tones | — |
| Recovery (G1-G4) | Zero false restarts in 10 min | Possible G4 if TX > 15s |
| QSO recorder (H1-H2) | Clean playback | — |
| CW decoder (I1-I2) | Decodes without artifacts | — |

---

## Audio Pipeline Reference

```
FlexRadio (VITA-49 UDP)
  │
  ├─ PCC 0x03E3: float32 stereo BE → byte-swap → float32 stereo 24kHz
  ├─ PCC 0x0123: int16 mono BE → float32 stereo 24kHz (mono duplicated)
  │
  ▼
PanadapterStream
  │
  ├──▶ audioDataReady ──▶ AudioEngine::feedAudioData()
  │                         ├─ DSP branch (NR2/NR4/BNR/DFNR/none)
  │                         ├─ Optional resample 24k→48k
  │                         ├─ m_rxBuffer (200ms cap)
  │                         └─ QAudioSink → Speaker
  │
  ├──▶ audioDataReady ──▶ CwDecoder::feedAudio() (pre-DSP tap)
  ├──▶ audioDataReady ──▶ QsoRecorder::feedRxAudio() (pre-DSP tap)
  │
  └──▶ daxAudioReady ──▶ TciServer::onDaxAudioReady()
                           ├─ Per-client resample
                           ├─ Format conversion (float32/int16 × stereo/mono)
                           └─ WebSocket → TCI clients
```

### Active Recovery Mechanisms (v0.8.15.1)

| Mechanism | Trigger | Action |
|-----------|---------|--------|
| StoppedState handler (#1303) | QAudioSink stops unexpectedly | Restart RX |
| Device change monitor (#1361) | USB device list changes (Windows only) | Restart RX |
| Zombie watchdog (#1361) | bytesFree stuck at 0 for 2s | Restart RX |
| Liveness watchdog (#1411) | No feedAudioData() calls for 15s | Restart RX |
