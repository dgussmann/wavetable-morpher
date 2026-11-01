# Wavetable Morpher

A VST3 / AU instrument (plus standalone app) for Apple Silicon Macs that builds morphing wavetables:

1. **Pick two oscillators**: built-in shapes, an imported WAV/AIFF/FLAC (single cycle or a whole wavetable, with a frame picker), or draw your own with the mouse.
2. **Morph** A into B across 8–256 frames: *Crossfade* (blend samples) or *Spectral* (blend each harmonic, smoother), with Linear / Ease / Exponential / Logarithmic curves.
3. **Preview** on the built-in 8-voice wavetable synth: play the on-screen keyboard or MIDI, sweep the Position knob, or turn up Scan Depth to hear the whole table move. Drag across the 3D view to scan.
4. **Name and save**: every save goes to `~/Music/Wavetable Morpher`, and optionally straight into **Serum 2**, **Vital** and **Massive XL** wavetable folders (auto-detected; change them under *Folders*).

Exported files are 32-bit float mono WAVs, 2048 samples per frame, with the `clm` frame-size marker Serum and Vital read. Massive XL slices any audio into 2048-sample frames, so the same file works there.
(Original Massive and Massive X can't load custom wavetables.)

## Getting the plugin

Every push to `main` is built by GitHub Actions on an Apple Silicon Mac. Open the latest run under **Actions → Build macOS**, download **Wavetable-Morpher-macOS-arm64**, unzip, and follow `HOW-TO-INSTALL.txt`.

## Where your synths see the tables

| Synth | Folder used | After saving |
|---|---|---|
| Serum 2 | `…/Serum 2 Presets/Tables/Wavetable Morpher` | Reopen the wavetable menu (or Serum) |
| Vital | `<Vital data folder>/User/Wavetables` | Reopen the wavetable browser |
| Massive XL | Its User Content `Wavetables` folder (you pick it once) | Settings → Rescan Content |

## Building it yourself (optional)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```
JUCE is downloaded automatically. Outputs land in `build/WavetableMorpher_artefacts/Release/`.

## Code map

- `Source/WaveSources.*`: built-in shapes, audio import
- `Source/WavetableEngine.*`: morphing (crossfade / spectral), band-limited playback tables
- `Source/WavetableExporter.*`: WAV writer and synth folder detection
- `Source/PreviewSynth.*`: the preview voices
- `Source/PluginProcessor.*`, `Source/PluginEditor.*`, `Source/Views.*`: plugin and UI
- `Tests/SelfTest.cpp`: engine and file-format checks (run in CI)
