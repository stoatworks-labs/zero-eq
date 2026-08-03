// Verification harness for the Zero EQ wasm core. Node-only; not shipped to the site.
//
// Same discipline as the plugin's throwaway harnesses (see CLAUDE.md): real audio
// through the actual compiled processor, checked numerically, with stimuli chosen so
// correct and incorrect behaviour produce DIFFERENT outputs. In particular the
// harmonic test is two-tone (content inside AND outside the band), because the
// v0.3.0 single-tone test was structurally unable to catch the broadband-saturator
// bug. Processing runs in 128-sample chunks, matching the AudioWorklet render quantum.
//
// Run: node web/test/harness.mjs

import createZeroEQModule from '../public/zeroeq.js';

const SR = 48000;
const BLOCK = 128;

const M = await createZeroEQModule();
const C = {
  init: M.cwrap('zeq_init', null, ['number']),
  reset: M.cwrap('zeq_reset', null, []),
  mainL: M.cwrap('zeq_main_l', 'number', []),
  mainR: M.cwrap('zeq_main_r', 'number', []),
  scL: M.cwrap('zeq_sc_l', 'number', []),
  scR: M.cwrap('zeq_sc_r', 'number', []),
  setParam: M.cwrap('zeq_set_param', null, ['string', 'number']),
  getParam: M.cwrap('zeq_get_param', 'number', ['string']),
  process: M.cwrap('zeq_process', null, ['number', 'number']),
  dynDelta: M.cwrap('zeq_band_dyn_delta', 'number', ['number']),
  compGr: M.cwrap('zeq_comp_gr', 'number', []),
  inPeak: M.cwrap('zeq_in_peak', 'number', []),
  inVu: M.cwrap('zeq_in_vu', 'number', []),
  inClip: M.cwrap('zeq_in_clip', 'number', []),
  compositeDb: M.cwrap('zeq_composite_magnitude_db', 'number', ['number']),
  numParams: M.cwrap('zeq_num_params', 'number', []),
  paramId: M.cwrap('zeq_param_id', 'string', ['number']),
  paramDefault: M.cwrap('zeq_param_default', 'number', ['string']),
};

let failures = 0;
function check(name, cond, detail) {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${name}${detail ? `  (${detail})` : ''}`);
  if (!cond) failures++;
}

// Pushes stereo (identical L/R) audio through the core in BLOCK chunks.
// Returns { out: Float32Array, deltas: per-block dyn delta for `trackBand` }.
function processAudio(input, { sidechain = null, trackBand = -1 } = {}) {
  const out = new Float32Array(input.length);
  const deltas = [];
  const pL = C.mainL() >> 2, pR = C.mainR() >> 2;
  const sL = C.scL() >> 2, sR = C.scR() >> 2;

  for (let start = 0; start < input.length; start += BLOCK) {
    const n = Math.min(BLOCK, input.length - start);
    for (let i = 0; i < n; i++) {
      M.HEAPF32[pL + i] = input[start + i];
      M.HEAPF32[pR + i] = input[start + i];
      if (sidechain) {
        M.HEAPF32[sL + i] = sidechain[start + i];
        M.HEAPF32[sR + i] = sidechain[start + i];
      }
    }
    C.process(n, sidechain ? 1 : 0);
    for (let i = 0; i < n; i++) out[start + i] = M.HEAPF32[pL + i];
    if (trackBand >= 0) deltas.push(C.dynDelta(trackBand));
  }
  return { out, deltas };
}

function sine(freq, seconds, amp = 0.5) {
  const buf = new Float32Array(Math.round(seconds * SR));
  for (let i = 0; i < buf.length; i++) buf[i] = amp * Math.sin(2 * Math.PI * freq * i / SR);
  return buf;
}

function addInPlace(a, b) { for (let i = 0; i < a.length; i++) a[i] += b[i]; return a; }

function rmsDb(buf, from = 0, to = buf.length) {
  let s = 0;
  for (let i = from; i < to; i++) s += buf[i] * buf[i];
  return 10 * Math.log10(s / (to - from) + 1e-30);
}

// Goertzel magnitude (dBFS of the sine component) over [from, to).
function toneDb(buf, freq, from, to) {
  const n = to - from;
  const w = 2 * Math.PI * freq / SR;
  const coeff = 2 * Math.cos(w);
  let s0 = 0, s1 = 0, s2 = 0;
  for (let i = from; i < to; i++) {
    s0 = buf[i] + coeff * s1 - s2;
    s2 = s1; s1 = s0;
  }
  const power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  const amp = 2 * Math.sqrt(Math.max(power, 0)) / n;
  return 20 * Math.log10(amp + 1e-15);
}

function setDefaults() {
  C.init(SR);
}

const P = (band, name) => `band${band}_${name}`;

// ---------------------------------------------------------------------------
console.log(`\n=== Zero EQ wasm core verification (SR=${SR}, block=${BLOCK}) ===\n`);

// --- 0. Parameter harvest sanity ---
setDefaults();
check('parameter layout harvested', C.numParams() === 8 * 17 + 12, `${C.numParams()} params`);
check('band3 default freq is 1 kHz', Math.abs(C.paramDefault('band3_freq') - 1000) < 1e-3);
check('band0 default type is HighPass', C.paramDefault('band0_type') === 3);

// --- 1. Defaults pass-through + not-silence (ProcessorDuplicator regression) ---
{
  setDefaults();
  const input = sine(1000, 1.0, 0.25);
  const { out } = processAudio(input);
  const inDb = rmsDb(input, SR / 2), outDb = rmsDb(out, SR / 2);
  check('defaults: 1 kHz passes at unity', Math.abs(outDb - inDb) < 0.2,
        `in ${inDb.toFixed(2)} dB, out ${outDb.toFixed(2)} dB`);
  check('defaults: output is not silence', outDb > -60);

  // Zero added latency: output best-aligned with input at lag 0.
  const corr = (lag) => {
    let s = 0;
    for (let i = SR / 2; i < SR / 2 + 4800; i++) s += input[i] * out[i + lag];
    return s;
  };
  check('zero added latency (lag-0 correlation dominates)',
        corr(0) > corr(1) && corr(0) > corr(-1) && corr(0) > 0);
}

// --- 2. Static bell: processed audio matches the design maths ---
{
  setDefaults();
  C.setParam(P(3, 'gain'), 12.0);
  const input = addInPlace(sine(1000, 1.0, 0.06), sine(8000, 1.0, 0.06));
  const { out } = processAudio(input);
  const settle = SR / 2, end = SR;
  const g1k = toneDb(out, 1000, settle, end) - toneDb(input, 1000, settle, end);
  const g8k = toneDb(out, 8000, settle, end) - toneDb(input, 8000, settle, end);
  const q1k = C.compositeDb(1000), q8k = C.compositeDb(8000);
  check('bell +12 dB: 1 kHz measured ≈ designed', Math.abs(g1k - q1k) < 0.3,
        `measured ${g1k.toFixed(2)}, designed ${q1k.toFixed(2)}`);
  check('bell +12 dB: 1 kHz gains ≈ +12 dB', Math.abs(g1k - 12) < 0.5, `${g1k.toFixed(2)} dB`);
  check('bell +12 dB: 8 kHz measured ≈ designed', Math.abs(g8k - q8k) < 0.3,
        `measured ${g8k.toFixed(2)}, designed ${q8k.toFixed(2)}`);
}

// --- 3. HP slopes: Butterworth cascade steepness (design query + measured audio) ---
{
  const slopes = [12, 24, 36, 48];
  for (let s = 0; s < 4; s++) {
    setDefaults();
    C.setParam(P(0, 'slope'), s);
    C.setParam(P(0, 'freq'), 1000);
    // Deep in the stopband the rolloff approaches the asymptotic slope.
    const measuredSlope = C.compositeDb(100) - C.compositeDb(50);
    check(`HP slope ${slopes[s]} dB/oct: designed rolloff`, Math.abs(measuredSlope - slopes[s]) < 1.5,
          `${measuredSlope.toFixed(1)} dB/oct at 50→100 Hz`);
  }
  // Measured audio for the default 24 dB/oct case: tone deep in the stopband.
  setDefaults();
  C.setParam(P(0, 'slope'), 1);
  C.setParam(P(0, 'freq'), 1000);
  const input = sine(125, 1.0, 0.25);
  const { out } = processAudio(input);
  const atten = toneDb(out, 125, SR / 2, SR) - toneDb(input, 125, SR / 2, SR);
  const designed = C.compositeDb(125);
  check('HP 24 dB/oct: measured attenuation ≈ designed', Math.abs(atten - designed) < 0.5,
        `measured ${atten.toFixed(1)}, designed ${designed.toFixed(1)}`);
}

// --- 4. Harmonic character: two-tone band-isolation selectivity ---
// One tone inside the band (200 Hz), one far outside (6 kHz). A correct harmonic EQ
// generates harmonics of the in-band tone only; a broadband saturator (the v0.3.0
// bug) would distort both. Single-tone input cannot tell these apart - two-tone can.
{
  setDefaults();
  C.setParam(P(3, 'freq'), 200);
  C.setParam(P(3, 'gain'), 12.0);
  C.setParam(P(3, 'character'), 2); // Harmonic
  C.setParam(P(3, 'harmonic_blend'), 0.0); // pure even -> 2nd harmonic
  const input = addInPlace(sine(200, 1.5, 0.12), sine(6000, 1.5, 0.12));
  const { out } = processAudio(input);
  const settle = SR / 2, end = Math.floor(1.5 * SR);
  const h2in = toneDb(out, 400, settle, end);    // 2nd harmonic of in-band tone
  const h2out = toneDb(out, 12000, settle, end); // 2nd harmonic of out-of-band tone
  const f6k = toneDb(out, 6000, settle, end) - toneDb(input, 6000, settle, end);
  const q6k = C.compositeDb(6000);
  check('harmonic: in-band 2nd harmonic generated', h2in > -60, `400 Hz at ${h2in.toFixed(1)} dB`);
  check('harmonic: out-of-band tone NOT saturated', h2in - h2out > 40,
        `selectivity ${(h2in - h2out).toFixed(1)} dB`);
  check('harmonic: out-of-band fundamental passes per design', Math.abs(f6k - q6k) < 0.3,
        `measured ${f6k.toFixed(2)}, designed ${q6k.toFixed(2)}`);

  // Blend=1 (tanh, odd function): odd harmonics only - 3rd present, 2nd at the floor.
  C.setParam(P(3, 'harmonic_blend'), 1.0);
  C.reset();
  const { out: outOdd } = processAudio(addInPlace(sine(200, 1.5, 0.12), sine(6000, 1.5, 0.12)));
  const h3odd = toneDb(outOdd, 600, settle, end);
  const h2odd = toneDb(outOdd, 400, settle, end);
  check('harmonic odd: 3rd harmonic dominates 2nd', h3odd - h2odd > 30,
        `3rd ${h3odd.toFixed(1)} dB, 2nd ${h2odd.toFixed(1)} dB`);
}

// --- 5. Dynamic EQ: duck tracks loud/quiet, matches analytic depth ---
{
  setDefaults();
  C.setParam(P(3, 'dyn_active'), 1);
  C.setParam(P(3, 'dyn_threshold'), -20);
  C.setParam(P(3, 'dyn_ratio'), 4);
  C.setParam(P(3, 'dyn_attack'), 5);
  C.setParam(P(3, 'dyn_release'), 50);
  C.setParam(P(3, 'dyn_range'), 24);
  // 0.2 s segments: loud (amp 0.5 -> RMS -9 dB) / quiet (amp 0.02 -> RMS -37 dB)
  const seg = Math.round(0.2 * SR);
  const input = new Float32Array(seg * 6);
  for (let i = 0; i < input.length; i++) {
    const loud = Math.floor(i / seg) % 2 === 0;
    input[i] = (loud ? 0.5 : 0.02) * Math.sin(2 * Math.PI * 1000 * i / SR);
  }
  const { deltas } = processAudio(input, { trackBand: 3 });
  const blocksPerSeg = seg / BLOCK;
  const endOfSeg = (k) => deltas[Math.floor((k + 1) * blocksPerSeg) - 2];
  // Expected duck: over = -9 - (-20) = 11 dB, delta = -11 * (1 - 1/4) = -8.25 dB
  check('dynamic EQ: ducks loud segment ≈ analytic -8.25 dB', Math.abs(endOfSeg(2) + 8.25) < 1.0,
        `${endOfSeg(2).toFixed(2)} dB`);
  check('dynamic EQ: releases in quiet segment', endOfSeg(3) > -2.0, `${endOfSeg(3).toFixed(2)} dB`);
}

// --- 6. Sidechain routing: detection source proves out ---
{
  const seg = Math.round(0.2 * SR);
  const constantMain = sine(1000, 1.2, 0.25);
  const pulsedSc = new Float32Array(seg * 6);
  for (let i = 0; i < pulsedSc.length; i++) {
    const loud = Math.floor(i / seg) % 2 === 0;
    pulsedSc[i] = (loud ? 0.5 : 0.005) * Math.sin(2 * Math.PI * 1000 * i / SR);
  }
  const spread = (deltas) => {
    const settled = deltas.slice(Math.floor(deltas.length / 3));
    return Math.max(...settled) - Math.min(...settled);
  };
  const dynSetup = () => {
    setDefaults();
    C.setParam(P(3, 'dyn_active'), 1);
    C.setParam(P(3, 'dyn_threshold'), -20);
    C.setParam(P(3, 'dyn_ratio'), 4);
    C.setParam(P(3, 'dyn_attack'), 5);
    C.setParam(P(3, 'dyn_release'), 50);
  };

  dynSetup();
  C.setParam(P(3, 'dyn_sidechain'), 1);
  const withSc = processAudio(constantMain, { sidechain: pulsedSc, trackBand: 3 });
  check('sidechain on+connected: delta tracks the PULSED sidechain', spread(withSc.deltas) > 4,
        `spread ${spread(withSc.deltas).toFixed(2)} dB over constant main`);

  dynSetup();
  C.setParam(P(3, 'dyn_sidechain'), 0);
  const noSc = processAudio(constantMain, { sidechain: pulsedSc, trackBand: 3 });
  check('sidechain off: delta steady from the CONSTANT main', spread(noSc.deltas) < 0.5,
        `spread ${spread(noSc.deltas).toFixed(2)} dB`);

  dynSetup();
  C.setParam(P(3, 'dyn_sidechain'), 1);
  const scUnconnected = processAudio(constantMain, { trackBand: 3 });
  check('sidechain on but unconnected: falls back to internal detection',
        spread(scUnconnected.deltas) < 0.5, `spread ${spread(scUnconnected.deltas).toFixed(2)} dB`);
}

// --- 7. Compressor: gain reduction matches the static curve ---
{
  setDefaults();
  C.setParam('comp_active', 1);
  C.setParam('comp_threshold', -18);
  C.setParam('comp_ratio', 4);
  C.setParam('comp_knee', 0);
  C.setParam('comp_auto_makeup', 0);
  C.setParam('comp_makeup', 0);
  C.setParam('comp_detector', 1); // RMS
  const input = sine(1000, 1.0, 1.0); // 0 dBFS peak -> RMS -3 dB
  const { out } = processAudio(input);
  // over = -3 - (-18) = 15 dB; GR = -15 * (1 - 1/4) = -11.25 dB
  const gr = C.compGr();
  check('compressor: GR ≈ analytic -11.25 dB', Math.abs(gr + 11.25) < 1.0, `${gr.toFixed(2)} dB`);
  const drop = rmsDb(out, SR / 2) - rmsDb(input, SR / 2);
  check('compressor: output level drops by ≈ GR', Math.abs(drop - gr) < 1.0,
        `drop ${drop.toFixed(2)} dB`);
}

// --- 8. Meters ---
{
  setDefaults();
  const { } = processAudio(sine(1000, 0.5, 1.0));
  check('meter: input peak ≈ 0 dBFS', Math.abs(C.inPeak()) < 0.2, `${C.inPeak().toFixed(2)} dB`);
  check('meter: input VU ≈ -3 dB (sine RMS)', Math.abs(C.inVu() + 3) < 0.7, `${C.inVu().toFixed(2)} dB`);
  check('meter: clip indicator lit at full scale', C.inClip() === 1);
}

// --- 9. Coefficient-update regression sweep: every type/character stays audible ---
// Guards the ProcessorDuplicator class of bug: parameter changes must actually reach
// the running filters, for every filter type, with audio flowing throughout.
{
  setDefaults();
  let allFinite = true, anySilentTail = false;
  const noise = new Float32Array(SR);
  let seed = 1;
  for (let i = 0; i < noise.length; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    noise[i] = (seed / 0x3fffffff - 1) * 0.25;
  }
  for (let type = 0; type < 8; type++) {
    for (let character = 0; character < 3; character++) {
      C.setParam(P(3, 'type'), type);
      C.setParam(P(3, 'character'), character);
      C.setParam(P(3, 'gain'), type % 2 === 0 ? 12 : -12);
      const { out } = processAudio(noise);
      for (let i = 0; i < out.length; i++)
        if (!Number.isFinite(out[i])) { allFinite = false; break; }
      if (rmsDb(out, Math.floor(SR * 0.75)) < -60) anySilentTail = true;
    }
  }
  check('sweep: all 8 types × 3 characters stay finite', allFinite);
  check('sweep: no type/character combination goes silent', !anySilentTail);
}

console.log(`\n${failures === 0 ? 'ALL CHECKS PASSED' : `${failures} CHECK(S) FAILED`}\n`);
process.exit(failures === 0 ? 0 : 1);
