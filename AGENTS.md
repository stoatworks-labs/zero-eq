# AGENTS.md — bringing an LLM up to speed on Zero EQ

Orientation for an AI assistant (or a new human) picking this project up cold. Read this
before proposing changes. `CLAUDE.md` holds the short command reference; this file explains
the *why*, and the traps.

---

## 1. What this is

A **zero-added-latency parametric EQ + compressor** audio plugin, built with JUCE in C++,
shipping as VST3 and AU (plus a Standalone build). Eight EQ bands, each of which can also
act as a dynamic EQ or generate harmonics, plus a feed-forward compressor, presets and
ballistics-accurate metering.

Public repo, MIT-ish posture, ships a user-facing AI-assisted disclaimer in the README.
Tagged v0.2.0 through v0.7.0; all roadmap items are shipped and there is no backlog.

## 2. The one rule that matters most

**Zero added latency is a product guarantee, not an implementation detail.**

It is why the design rules out several textbook solutions:

- No lookahead, anywhere.
- No linear-phase / FFT-convolution EQ mode (deliberately out of scope, not an oversight).
- No oversampling — this is why the harmonic stage uses ADAA instead (see §4).
- No internal buffering beyond the host's own block size.

If you find yourself reaching for any of those to solve a problem, stop. Either solve it
another way, or surface the latency explicitly and get a human decision first. Silently
reporting latency to the host would break the promise that this is safe to track through
live.

## 3. Layout

```
Source/
  PluginProcessor.{h,cpp}    Host-facing entry point; owns the engine and buffers
  PluginEditor.{h,cpp}       Top-level GUI window
  PluginParameters.h         Every parameter ID, range and default. numBands = 8.
  PresetManager.{h,cpp}      Factory + user presets

  DSP/                       All audio processing. Real-time constrained.
    EQEngine.{h,cpp}           Orchestrates the 8 bands; the main per-block entry point
    EQBand.{h,cpp}             One band = cascaded minimum-phase biquads
    HarmonicShaper.{h,cpp}     Per-channel harmonic saturation (ADAA)
    DynamicEQDetector.{h,cpp}  Per-band level detection driving dynamic gain
    Compressor.{h,cpp}         Feed-forward compressor with soft knee
    LevelMeter.{h,cpp}         Peak / true-peak / VU metering
    SpectrumAnalyzer.{h,cpp}   FFT display feed

  GUI/                       Components only; no DSP decisions live here
```

`.github/workflows/release.yml` builds all platforms.

## 4. How it actually works

**Per block**, `EQEngine::updateAndProcess` reads current parameter values, resolves solo
across all bands *first* (soloing one band mutes others, which can't be decided per-band),
then runs each band in turn over the buffer. Bands are applied in series, so their effects
stack.

**Parameter smoothing.** Frequency, gain and Q each glide over 20 ms. Jumping a filter
straight to a new coefficient set clicks audibly. `firstBlock` exists so the first block
after starting snaps to current knob positions instead of gliding up from zero.

**Band characters** are three distinct behaviours, not cosmetic presets:
- *Modern* — textbook independent-Q parametric.
- *Vintage* — proportional Q; bandwidth widens as gain increases, imitating passive/console
  EQ interaction.
- *Harmonic* — Modern's linear response **plus** harmonics generated from only that band's
  spectral region and summed back in parallel.

**The Harmonic character is the subtle part.** It is *not* a saturator in series. The audio
is filtered down to the band's own region, that isolated region is driven into a
nonlinearity, and the result is summed back into the dry signal. That is what makes it a
harmonic *EQ* rather than a broadband distortion. `EQBand::designRegionIsolationFilter` is
the single definition of "the region this band affects", deliberately shared with
`DynamicEQDetector` so the two can't drift apart.

**ADAA instead of oversampling.** Distortion invents frequencies above Nyquist that fold
back as aliasing. The normal fix is oversampling, which costs latency this plugin cannot
spend. Instead `HarmonicShaper::adaaStep` averages the shaping curve across each sample
step using hand-derived antiderivatives, needing only the previous sample. Reference:
Parker/Zavalishin/Bilbao, *Antiderivative Antialiasing for Memoryless Nonlinearities*.

**Threading.** DSP state is audio-thread-only. The GUI must never read live filter state —
that's why `EQBand` exposes *static, stateless* `design` / `computeMagnitudeForFrequency`
for curve drawing, alongside the instance method for audio. Meter values cross threads via
atomics. Keep the audio thread lock-free and allocation-free; note the cached
`lastIsolation*` fields exist purely to avoid reallocating coefficients per block.

## 5. Building and verifying

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Be aware:** the install step copies the built VST3 into
`~/Library/Audio/Plug-Ins/VST3/Zero EQ.vst3`, replacing the installed plugin. Building has a
side effect on the user's system.

macOS universal builds: `CMAKE_OSX_ARCHITECTURES` must be set **before** `project()`, or you
silently ship arm64-only. Verify the artefact with `lipo`/`file`, never the build log. CI
cross-compiles macOS x86_64 on `macos-14` — `macos-13` Intel runners are retired.

### How to verify DSP changes — read this carefully

Build a **throwaway console harness** that pushes real audio through the actual shipped
`ZeroEQAudioProcessor`, check the output numerically, then delete it before committing.
Never leave test scaffolding in `CMakeLists.txt`.

`pluginval` and GUI inspection have missed **every** real bug this project has had.

**The stimulus must be able to distinguish right from wrong.** v0.3.0 shipped a broadband
saturator as a "harmonic EQ" for four releases, because it was verified with a *single tone
at the band's own frequency* — an input for which correct and incorrect behaviour are
byte-identical. For anything frequency-selective, the test signal needs content both inside
and outside the region under test. The current two-tone FFT check measures 91 dB of
selectivity with the out-of-band fundamental passing within 0.01 dB; that is the standard to
maintain.

## 6. Known-good oddities — don't "fix" these

- **Two AU `pluginval` warnings are expected and benign**: *"Disabling non-main buses
  failed"* (a documented JUCE/AUv2 quirk for plugins with a sidechain bus) and *"Current
  program is -1"* (the AU wrapper reports no preset selected until a host picks one).
  Apple's own `auval` passes with exit code 0.
- **The clip threshold is -0.1 dBFS (0.988553), not 1.0.** A nominally unity-gain stage
  isn't always bit-exact 1.0 after a normalised-parameter round-trip (~6e-8 off in
  practice), so a strict `>= 1.0f` check misses genuinely full-scale signals.
- **Clip hold is counted in samples, not blocks**, so the hold duration is a real ~1.5 s
  regardless of host block size.
- **Auto-makeup is deliberately halved** — it reads as "gentle" rather than fully restoring
  peak level. It is a heuristic, not a loudness-matching guarantee.

## 7. Conventions

- JUCE string literals must stay ASCII — non-ASCII literals assert and mangle text.
- Comments are written for a reader who is *not* a DSP programmer; match that register.
- Public repo: no client names, no unreleased hardware details.
- "Commit" means commit **and** push.

## 8. State of play

Everything on the roadmap is shipped. The single outstanding item is real-world:
**it has never been run on real audio hardware in a live signal chain.** Analytical checks,
numeric harnesses, `pluginval`, `auval` and hosting in REAPER all pass — but that is not the
same as a live gig. Treat any "it works" claim about live use as unverified.

## Diagnostics

`Source/Diag/` gives a rotating log and an in-memory ring. **`installCrashHandler` is
`false` and must stay that way**: this plugin runs inside a DAW, and a process-wide signal
handler here would intercept faults that are not ours and interfere with the host's own
handling. Log through the `CP_LOG_*` macros, never `DBG` or `std::cout`.
