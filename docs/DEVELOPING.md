# Developing Zero EQ

Build, verification and extension guide. For the architecture and the rules behind it, read
[`AGENTS.md`](../AGENTS.md); for the parameter surface, [`API.md`](API.md).

---

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Requires CMake and a C++ toolchain; JUCE is pulled in by the build.

> **Building has a side effect on your machine.** The install step copies the built VST3 to
> `~/Library/Audio/Plug-Ins/VST3/Zero EQ.vst3`, replacing whatever is installed. If you have
> a working copy you care about, back it up before building a branch.

### macOS universal builds — the silent failure

**`CMAKE_OSX_ARCHITECTURES` must be set BEFORE `project()`.** Set it after and CMake ignores
it: you get an arm64-only binary, and the build log reports success.

**Verify the artefact, never the log:**
```bash
lipo -archs "build/.../Zero EQ.vst3/Contents/MacOS/Zero EQ"
```

CI cross-compiles macOS x86_64 on **`macos-14`**. Never `macos-13` — those Intel runners are
retired and the job just fails.

---

## The constraint that shapes every decision

**Zero added latency is a product guarantee.** It rules out, permanently:

- lookahead of any kind
- linear-phase / FFT-convolution EQ
- oversampling (hence ADAA in the harmonic stage)
- internal buffering beyond the host's block

If a problem seems to need one of those, solve it another way or escalate. **Never silently
report latency to the host** — that breaks the one promise the product makes.

---

## Layout

```
Source/
  PluginProcessor.{h,cpp}   Host entry point; owns the engine
  PluginEditor.{h,cpp}      Top-level GUI
  PluginParameters.h        Every ID, range and default. numBands = 8.
  PresetManager.{h,cpp}     Factory + user presets
  DSP/                      Audio processing - real-time constrained
    EQEngine       Orchestrates 8 bands; main per-block entry point
    EQBand         One band = cascaded minimum-phase biquads
    HarmonicShaper Per-channel harmonic saturation (ADAA)
    DynamicEQDetector  Per-band detection driving dynamic gain
    Compressor     Feed-forward, soft knee
    LevelMeter     Peak / true-peak / VU
    SpectrumAnalyzer
  GUI/                      Components only - no DSP decisions here
```

---

## Real-time rules

- **No allocation, no locks, no reference counting on the audio thread.**
- **The GUI must never touch live filter state.** That's why `EQBand` exposes *static,
  stateless* `design()` and `computeMagnitudeForFrequency()` for curve drawing, separate
  from the instance methods the audio thread uses.
- Meter values cross threads via **atomics**.
- The cached `lastIsolation*` fields exist to avoid redesigning coefficients every block —
  don't remove them for tidiness.

---

## How to verify DSP changes — the important part

**Build a throwaway console harness that pushes real audio through the actual shipped
`ZeroEQAudioProcessor`, check the output numerically, then delete it before committing.**
Never leave test scaffolding in `CMakeLists.txt`.

**`pluginval` and GUI inspection have missed every real bug this project has had.** They are
necessary, not sufficient.

### The stimulus must be able to distinguish right from wrong

This is the lesson that cost four releases.

**v0.3.0 shipped a broadband saturator as a "harmonic EQ" for four releases** because it was
verified with a *single tone at the band's own frequency* — an input for which correct
(band-isolated) and incorrect (broadband) behaviour produce **byte-identical output**. The
test could not have failed.

For anything frequency-selective, **the test signal needs content both inside and outside the
region under test**. The current two-tone FFT check measures **91 dB of selectivity**, with
the out-of-band fundamental passing within **0.01 dB**. That's the bar.

Other checks worth preserving as patterns:
- pure-odd blend produces odd harmonics **and no even ones**
- a Harmonic band at 0 dB gain is **fully transparent**
- a full factory-preset sweep: every preset applies, produces finite (non-NaN) audio, and
  reports **zero added latency**
- metering: peak attack/release timing, VU ballistics, true-peak inter-sample detection and
  clip-hold duration, each against a known signal
- sidechain: a dynamic band tracks the **external** signal when enabled, the internal one
  when not, and falls back cleanly when no sidechain audio is connected

---

## Adding a parameter

1. Add the ID to `ParamIDs` and the parameter to the layout in `PluginParameters.h`.
2. Use a **versioned** `juce::ParameterID(id, 1)` so existing automation survives.
3. Read it in `EQEngine::updateAndProcess` (or the compressor path).
4. Add smoothing if it's continuous and audible — 20 ms is the house value.
5. Add the control in `GUI/`.
6. **Document it in [`API.md`](API.md)**, including any interaction (e.g. "ignored when X is
   on").

`getAllParameterIDs()` should stay complete — things iterate it.

---

## Known-good oddities — don't "fix" these

- **Two AU `pluginval` warnings are expected**: *"Disabling non-main buses failed"* (JUCE/AUv2
  quirk with a sidechain bus) and *"Current program is -1"*. `auval` exits 0.
- **The clip threshold is 0.988553 (-0.1 dBFS), not 1.0.** A nominally unity-gain stage isn't
  bit-exact 1.0 after a normalised-parameter round trip (~6e-8 off observed), so `>= 1.0f`
  misses genuinely full-scale signals.
- **Clip hold counts samples, not blocks**, so it's a real ~1.5 s regardless of host block
  size.
- **Auto-makeup is deliberately halved** — a "gentle" heuristic, not loudness matching.
- **Scene colours / gamma:** n/a here, but note the sibling openstage repo has a similar
  don't-touch item; check `AGENTS.md` before assuming a constant is wrong.

---

## Conventions

- **No non-ASCII characters in string literals** — JUCE's `String(const char*)` asserts and
  mangles the text. Em dashes are fine in comments, never in literals.
- Comments are pitched at a reader who is **not** a DSP programmer; match that register.
- Public repo, ships a user-facing AI-assisted disclaimer.
- "Commit" means commit **and** push.
