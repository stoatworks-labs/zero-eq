#!/bin/bash
# Builds the Zero EQ DSP core (the plugin's own Source/DSP files, unmodified) to
# WebAssembly via the shim JuceHeader.h. Output is a single self-contained ES module
# (wasm embedded, synchronous compile) so the same file loads in an AudioWorklet, on
# the main thread, and in Node for the verification harness.
set -euo pipefail
cd "$(dirname "$0")"

SRC=../../Source

emcc -O3 -std=c++17 \
  -I shim -I "$SRC" \
  zeroeq_web.cpp \
  "$SRC/DSP/EQBand.cpp" \
  "$SRC/DSP/EQEngine.cpp" \
  "$SRC/DSP/DynamicEQDetector.cpp" \
  "$SRC/DSP/HarmonicShaper.cpp" \
  "$SRC/DSP/Compressor.cpp" \
  "$SRC/DSP/LevelMeter.cpp" \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createZeroEQModule \
  -sSINGLE_FILE=1 \
  -sWASM_ASYNC_COMPILATION=0 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sENVIRONMENT=web,worker,node \
  -sEXPORTED_RUNTIME_METHODS=cwrap,ccall,HEAPF32,UTF8ToString \
  -sINCOMING_MODULE_JS_API=instantiateWasm,locateFile \
  -o ../public/zeroeq.js

echo "Built ../public/zeroeq.js ($(du -h ../public/zeroeq.js | cut -f1))"
