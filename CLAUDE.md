# zero-eq (ZeroEQ)

Zero-latency EQ + dynamic-EQ (+ sidechain) + band-isolated harmonic generation + compressor + presets + metering plugin. VST3/AU, JUCE/C++, CMake. Tagged v0.2.0–v0.7.0; all roadmap items shipped (no backlog).

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Built plugins land under `build/` as VST3/AU bundles.

## Notes
- JUCE plugin — DSP is real-time/allocation-sensitive; keep the audio thread lock-free and allocation-free.
- "Zero-latency" is a product guarantee: don't introduce lookahead/latency without surfacing it.
- Public repo. Ships user-facing AI disclaimer. Multi-platform release CI; cross-compile macOS x86_64 on macos-14 (never macos-13). "Commit" = commit **and** push.

## Verifying DSP changes
Build a throwaway console harness that pushes real audio through the actual shipped
`ZeroEQAudioProcessor`, check it numerically, then delete it before committing. Never
leave test scaffolding in `CMakeLists.txt`. `pluginval` and GUI inspection have missed
every real bug this project has had.

**The stimulus must be able to distinguish right from wrong.** v0.3.0 shipped a
broadband saturator as a "harmonic EQ" for four releases because it was verified with a
*single tone at the band's own frequency* — an input for which correct and incorrect
behaviour are byte-identical. For anything frequency-selective, the test signal needs
content both inside and outside the region under test.

## Diagnostics

`Source/Diag/` gives a rotating log and an in-memory ring. **`installCrashHandler` is
`false` and must stay that way**: this plugin runs inside a DAW, and a process-wide signal
handler here would intercept faults that are not ours and interfere with the host's own
handling. Log through the `CP_LOG_*` macros, never `DBG` or `std::cout`.
