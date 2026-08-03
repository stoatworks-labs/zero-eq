# Zero EQ — Web Simulator

Try Zero EQ in the browser: **https://zero-eq-sim.allan-sargeant.workers.dev**

This is the plugin's own DSP — the unmodified sources in [`../Source/DSP`](../Source/DSP)
— compiled to WebAssembly and run inside an AudioWorklet, wrapped in a small web app
with signal sources (synthesized demo groove, sine, white/pink/band-limited pink noise,
file upload) and the plugin's curve/analyzer/meter displays. Uploaded audio never
leaves the browser. A web-only "safety clip" stage (hard ceiling at −0.1 dBFS, on by
default, can be disabled) protects ears and speakers at extreme settings; the plugin
itself has no such stage.

## How it works

- [`wasm/shim/JuceHeader.h`](wasm/shim/JuceHeader.h) — a minimal JUCE-compatible shim
  that stands in for `<JuceHeader.h>` on the include path. The filter mathematics
  (coefficient formulas, DF2T state updates, Butterworth cascade Qs, SmoothedValue
  ramp semantics, ProcessorDuplicator's shared-coefficients aliasing) is transcribed
  from the exact JUCE sources the plugin builds against, so the web build and the
  shipped plugin produce the same output. The plugin's DSP sources compile against it
  **completely unmodified**.
- The shim's parameter classes *record* what `createParameterLayout()` registers, so
  every parameter id, default, range and skew in the web app is harvested from the
  plugin's own source — there is no hand-maintained copy to drift.
- [`wasm/zeroeq_web.cpp`](wasm/zeroeq_web.cpp) — plays PluginProcessor's role: owns the
  parameter store and runs the identical chain (input gain → input meter → EQ →
  compressor → output gain → output meter), exposed as a C API.
- Two instances of the same module run in the page: one in the AudioWorklet (audio),
  one on the main thread (response-curve maths), so the picture cannot drift from the
  sound.

## Build

Requires Emscripten (`brew install emscripten`) and Node.

```bash
./wasm/build.sh        # → public/zeroeq.js (single self-contained ES module)
node test/harness.mjs  # verification — must pass before deploying
```

`test/harness.mjs` follows the repo's DSP-verification discipline (see the top-level
CLAUDE.md): real audio through the actual compiled processor in worklet-sized blocks,
including the **two-tone** harmonic band-isolation test (stimuli must be able to
distinguish correct from incorrect behaviour), analytic dynamic-EQ/compressor depth
checks, sidechain routing proof, and a coefficient-update regression sweep across all
band types and characters.

## Deploy

Static-assets-only Cloudflare Worker (`wrangler.jsonc`), deployed manually:

```bash
cf-run npx wrangler deploy
```
