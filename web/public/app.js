// Zero EQ web simulator — main thread.
//
// Two instances of the same wasm module run: one inside the AudioWorklet (the audio
// path) and one here (curve mathematics only — the same EQBand::design code draws
// the response curve, so the picture can't drift from the sound). All parameter
// changes go to both.

import createZeroEQModule from './zeroeq.js';

const $ = (id) => document.getElementById(id);

// --- main-thread wasm instance (curve maths + parameter metadata) ---
const M = await createZeroEQModule();
const C = {
  init: M.cwrap('zeq_init', null, ['number']),
  setParam: M.cwrap('zeq_set_param', null, ['string', 'number']),
  numParams: M.cwrap('zeq_num_params', 'number', []),
  paramId: M.cwrap('zeq_param_id', 'string', ['number']),
  pDefault: M.cwrap('zeq_param_default', 'number', ['string']),
  pMin: M.cwrap('zeq_param_min', 'number', ['string']),
  pMax: M.cwrap('zeq_param_max', 'number', ['string']),
  pSkew: M.cwrap('zeq_param_skew', 'number', ['string']),
  compositeDb: M.cwrap('zeq_composite_magnitude_db', 'number', ['number']),
  bandDb: M.cwrap('zeq_band_magnitude_db', 'number', ['number', 'number', 'number']),
};
C.init(48000); // curve instance; the audio instance re-inits at the real device rate

// --- parameter state, harvested from the plugin's own layout ---
const params = new Map();
const meta = new Map();
for (let i = 0, n = C.numParams(); i < n; i++) {
  const id = C.paramId(i);
  meta.set(id, { def: C.pDefault(id), min: C.pMin(id), max: C.pMax(id), skew: C.pSkew(id) });
  params.set(id, C.pDefault(id));
}

let workletPort = null;
let pendingToWorklet = [];
let curveDirty = true;

function setParam(id, value) {
  params.set(id, value);
  C.setParam(id, value);
  pendingToWorklet.push([id, value]);
  curveDirty = true;
  scheduleFlush();
}
function flushParams() {
  if (workletPort && pendingToWorklet.length) {
    workletPort.postMessage({ type: 'params', entries: pendingToWorklet });
    pendingToWorklet = [];
  }
}
// Flush on a microtask, NOT from the render loop: requestAnimationFrame is
// throttled to zero in hidden/background tabs, and audio must keep following
// parameter changes even when the page isn't being painted.
let flushScheduled = false;
function scheduleFlush() {
  if (flushScheduled) return;
  flushScheduled = true;
  queueMicrotask(() => { flushScheduled = false; flushParams(); });
}
const P = (band, name) => `band${band}_${name}`;
const getP = (id) => params.get(id);

// --- JUCE NormalisableRange skew mapping (value <-> normalised 0..1) ---
function fromNorm(id, p) {
  const m = meta.get(id);
  const prop = m.skew === 1 ? p : Math.pow(p, 1 / m.skew);
  return m.min + (m.max - m.min) * prop;
}
function toNorm(id, v) {
  const m = meta.get(id);
  const prop = (v - m.min) / (m.max - m.min);
  return m.skew === 1 ? prop : Math.pow(Math.max(prop, 0), m.skew);
}

// --- formatting ---
const fmtHz = (v) => v >= 1000 ? `${(v / 1000).toFixed(v < 10000 ? 2 : 1)} kHz` : `${v.toFixed(0)} Hz`;
const fmtDb = (v) => `${v >= 0 ? '+' : ''}${v.toFixed(1)} dB`;
const fmtMs = (v) => `${v.toFixed(v < 10 ? 1 : 0)} ms`;
const fmtRatio = (v) => `${v.toFixed(1)}:1`;
const fmtQ = (v) => v.toFixed(2);

const typeHasGain = (t) => t === 0 || t === 1 || t === 2 || t === 7;

// ===========================================================================
// Band selection + panel binding
// ===========================================================================
let selectedBand = 3;
const BAND_COLORS = ['#e06666', '#e8a23c', '#e8d44b', '#7fc95f', '#4fc9b0', '#59a7ff', '#9b7fe8', '#e070c0'];

// A slider bound to a (possibly skewed) parameter of the currently-selected scope.
function bindSlider(inputId, valId, paramIdFor, fmt) {
  const input = $(inputId), val = $(valId);
  input.addEventListener('input', () => {
    const id = paramIdFor();
    const v = fromNorm(id, parseFloat(input.value));
    setParam(id, v);
    val.textContent = fmt(v);
  });
  return () => { // refresh from state
    const id = paramIdFor();
    input.value = toNorm(id, getP(id));
    val.textContent = fmt(getP(id));
  };
}
function bindSelect(selId, paramIdFor) {
  const sel = $(selId);
  sel.addEventListener('change', () => { setParam(paramIdFor(), sel.selectedIndex); refreshBandPanel(); });
  return () => { sel.selectedIndex = Math.round(getP(paramIdFor())); };
}
function bindCheck(checkId, paramIdFor, after) {
  const box = $(checkId);
  box.addEventListener('change', () => { setParam(paramIdFor(), box.checked ? 1 : 0); if (after) after(); });
  return () => { box.checked = getP(paramIdFor()) > 0.5; };
}

const refreshers = [
  bindSelect('bType', () => P(selectedBand, 'type')),
  bindSelect('bCharacter', () => P(selectedBand, 'character')),
  bindSelect('bSlope', () => P(selectedBand, 'slope')),
  bindCheck('bActive', () => P(selectedBand, 'active'), () => renderChips()),
  bindCheck('bSolo', () => P(selectedBand, 'solo'), () => renderChips()),
  bindSlider('bFreq', 'bFreqVal', () => P(selectedBand, 'freq'), fmtHz),
  bindSlider('bGain', 'bGainVal', () => P(selectedBand, 'gain'), fmtDb),
  bindSlider('bQ', 'bQVal', () => P(selectedBand, 'q'), fmtQ),
  bindSlider('bBlend', 'bBlendVal', () => P(selectedBand, 'harmonic_blend'), (v) => `${Math.round(v * 100)}% odd`),
  bindCheck('dActive', () => P(selectedBand, 'dyn_active'), () => refreshBandPanel()),
  bindSelect('dDirection', () => P(selectedBand, 'dyn_direction')),
  bindCheck('dSidechain', () => P(selectedBand, 'dyn_sidechain')),
  bindSlider('dThreshold', 'dThresholdVal', () => P(selectedBand, 'dyn_threshold'), fmtDb),
  bindSlider('dRatio', 'dRatioVal', () => P(selectedBand, 'dyn_ratio'), fmtRatio),
  bindSlider('dAttack', 'dAttackVal', () => P(selectedBand, 'dyn_attack'), fmtMs),
  bindSlider('dRelease', 'dReleaseVal', () => P(selectedBand, 'dyn_release'), fmtMs),
  bindSlider('dRange', 'dRangeVal', () => P(selectedBand, 'dyn_range'), fmtDb),
  // compressor + IO (fixed param ids)
  bindCheck('cActive', () => 'comp_active'),
  bindSelect('cDetector', () => 'comp_detector'),
  bindCheck('cAutoMakeup', () => 'comp_auto_makeup'),
  bindSlider('cThreshold', 'cThresholdVal', () => 'comp_threshold', fmtDb),
  bindSlider('cRatio', 'cRatioVal', () => 'comp_ratio', fmtRatio),
  bindSlider('cAttack', 'cAttackVal', () => 'comp_attack', fmtMs),
  bindSlider('cRelease', 'cReleaseVal', () => 'comp_release', fmtMs),
  bindSlider('cKnee', 'cKneeVal', () => 'comp_knee', fmtDb),
  bindSlider('cMakeup', 'cMakeupVal', () => 'comp_makeup', fmtDb),
  bindCheck('eqActive', () => 'eq_active'),
  bindSlider('inGain', 'inGainVal', () => 'input_gain', fmtDb),
  bindSlider('outGain', 'outGainVal', () => 'output_gain', fmtDb),
];

function refreshBandPanel() {
  for (const r of refreshers) r();
  const type = Math.round(getP(P(selectedBand, 'type')));
  const character = Math.round(getP(P(selectedBand, 'character')));
  const hasGain = typeHasGain(type);
  $('bGain').disabled = !hasGain;
  $('bSlope').disabled = !(type === 3 || type === 4);
  $('bBlend').disabled = !(hasGain && character === 2);
  $('dynGrid').classList.toggle('disabled-block', !hasGain);
  renderChips();
}

function renderChips() {
  const wrap = $('bandChips');
  wrap.innerHTML = '';
  for (let b = 0; b < 8; b++) {
    const chip = document.createElement('span');
    chip.className = 'chip' + (b === selectedBand ? ' sel' : '') + (getP(P(b, 'active')) > 0.5 ? '' : ' off');
    const dot = document.createElement('span');
    dot.className = 'dot';
    dot.style.background = BAND_COLORS[b];
    chip.appendChild(dot);
    chip.appendChild(document.createTextNode(`${b + 1}`));
    chip.addEventListener('click', () => { selectedBand = b; refreshBandPanel(); });
    chip.addEventListener('dblclick', () => {
      setParam(P(b, 'active'), getP(P(b, 'active')) > 0.5 ? 0 : 1);
      refreshBandPanel();
    });
    wrap.appendChild(chip);
  }
}

// ===========================================================================
// Curve display + editor
// ===========================================================================
const canvas = $('curve');
const ctx2d = canvas.getContext('2d');
const FMIN = 20, FMAX = 20000, DBMAX = 30;
let cw = 0, ch = 0, dpr = 1;
let liveDeltas = new Array(8).fill(0);
let analyserPre = null, analyserPost = null;
let specBinsPre = null, specBinsPost = null;

function resizeCanvas() {
  dpr = window.devicePixelRatio || 1;
  cw = canvas.clientWidth;
  ch = canvas.clientHeight;
  canvas.width = Math.round(cw * dpr);
  canvas.height = Math.round(ch * dpr);
  curveDirty = true;
}
window.addEventListener('resize', resizeCanvas);

const xOfF = (f) => cw * Math.log(f / FMIN) / Math.log(FMAX / FMIN);
const fOfX = (x) => FMIN * Math.pow(FMAX / FMIN, x / cw);
const yOfDb = (db) => ch / 2 - (db / DBMAX) * (ch / 2 - 14);
const dbOfY = (y) => (ch / 2 - y) * DBMAX / (ch / 2 - 14);

const N_POINTS = 220;
const freqGrid = new Float64Array(N_POINTS);
for (let i = 0; i < N_POINTS; i++) freqGrid[i] = FMIN * Math.pow(FMAX / FMIN, i / (N_POINTS - 1));

function anyDyn() {
  for (let b = 0; b < 8; b++)
    if (getP(P(b, 'dyn_active')) > 0.5 && getP(P(b, 'active')) > 0.5 && typeHasGain(Math.round(getP(P(b, 'type')))))
      return true;
  return false;
}
const dynEligible = (b) =>
  getP(P(b, 'dyn_active')) > 0.5 && getP(P(b, 'active')) > 0.5 && typeHasGain(Math.round(getP(P(b, 'type'))));

function drawCurve() {
  ctx2d.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx2d.clearRect(0, 0, cw, ch);

  // grid
  ctx2d.strokeStyle = '#20242b';
  ctx2d.fillStyle = '#5a606b';
  ctx2d.font = '10px sans-serif';
  ctx2d.lineWidth = 1;
  for (const f of [30, 50, 100, 200, 300, 500, 1000, 2000, 3000, 5000, 10000]) {
    const x = xOfF(f);
    ctx2d.beginPath(); ctx2d.moveTo(x, 0); ctx2d.lineTo(x, ch); ctx2d.stroke();
    ctx2d.fillText(f >= 1000 ? `${f / 1000}k` : `${f}`, x + 3, ch - 4);
  }
  for (let db = -24; db <= 24; db += 6) {
    const y = yOfDb(db);
    ctx2d.strokeStyle = db === 0 ? '#2e343d' : '#20242b';
    ctx2d.beginPath(); ctx2d.moveTo(0, y); ctx2d.lineTo(cw, y); ctx2d.stroke();
    if (db !== 0) ctx2d.fillText(`${db > 0 ? '+' : ''}${db}`, 4, y - 2);
  }

  // spectrum (pre dim fill, post line), -100..0 dBFS over full height
  if (analyserPre) {
    analyserPre.getByteFrequencyData(specBinsPre);
    analyserPost.getByteFrequencyData(specBinsPost);
    const sr = audioCtx.sampleRate, nBins = specBinsPre.length;
    const binF = (i) => (i * sr) / (2 * nBins);
    const drawSpec = (bins, asFill, style) => {
      ctx2d.beginPath();
      let started = false;
      for (let i = 1; i < nBins; i++) {
        const f = binF(i);
        if (f < FMIN || f > FMAX) continue;
        const y = ch * (1 - bins[i] / 255);
        const x = xOfF(f);
        if (!started) { ctx2d.moveTo(x, asFill ? ch : y); started = true; }
        ctx2d.lineTo(x, y);
      }
      if (asFill) {
        ctx2d.lineTo(cw, ch);
        ctx2d.closePath();
        ctx2d.fillStyle = style;
        ctx2d.fill();
      } else {
        ctx2d.strokeStyle = style;
        ctx2d.lineWidth = 1;
        ctx2d.stroke();
      }
    };
    drawSpec(specBinsPre, true, 'rgba(74,90,106,0.28)');
    drawSpec(specBinsPost, false, 'rgba(127,183,217,0.8)');
  }

  // dynamic range envelopes (shaded reach of each dynamic band)
  for (let b = 0; b < 8; b++) {
    if (!dynEligible(b)) continue;
    const g = getP(P(b, 'gain'));
    const range = getP(P(b, 'dyn_range'));
    const limitGain = Math.round(getP(P(b, 'dyn_direction'))) === 0 ? g - range : g + range;
    ctx2d.beginPath();
    for (let i = 0; i < N_POINTS; i++) {
      const y = yOfDb(C.compositeDb(freqGrid[i]));
      i === 0 ? ctx2d.moveTo(xOfF(freqGrid[i]), y) : ctx2d.lineTo(xOfF(freqGrid[i]), y);
    }
    for (let i = N_POINTS - 1; i >= 0; i--) {
      const f = freqGrid[i];
      const env = C.compositeDb(f) + (C.bandDb(b, f, limitGain) - C.bandDb(b, f, g));
      ctx2d.lineTo(xOfF(f), yOfDb(env));
    }
    ctx2d.closePath();
    ctx2d.fillStyle = BAND_COLORS[b] + '22';
    ctx2d.fill();
  }

  // static composite curve
  ctx2d.beginPath();
  for (let i = 0; i < N_POINTS; i++) {
    const y = yOfDb(C.compositeDb(freqGrid[i]));
    i === 0 ? ctx2d.moveTo(xOfF(freqGrid[i]), y) : ctx2d.lineTo(xOfF(freqGrid[i]), y);
  }
  ctx2d.strokeStyle = '#e8b84b';
  ctx2d.lineWidth = 2;
  ctx2d.stroke();

  // live curve (static + current dynamic deltas)
  if (anyDyn() && liveDeltas.some((d) => Math.abs(d) > 0.1)) {
    ctx2d.beginPath();
    for (let i = 0; i < N_POINTS; i++) {
      const f = freqGrid[i];
      let db = C.compositeDb(f);
      for (let b = 0; b < 8; b++) {
        if (!dynEligible(b) || Math.abs(liveDeltas[b]) < 0.05) continue;
        const g = getP(P(b, 'gain'));
        db += C.bandDb(b, f, g + liveDeltas[b]) - C.bandDb(b, f, g);
      }
      const y = yOfDb(db);
      i === 0 ? ctx2d.moveTo(xOfF(f), y) : ctx2d.lineTo(xOfF(f), y);
    }
    ctx2d.strokeStyle = 'rgba(255,255,255,0.85)';
    ctx2d.lineWidth = 1.2;
    ctx2d.stroke();
  }

  // band nodes
  for (let b = 0; b < 8; b++) {
    const type = Math.round(getP(P(b, 'type')));
    const f = getP(P(b, 'freq'));
    const g = typeHasGain(type) ? getP(P(b, 'gain')) : 0;
    const x = xOfF(f), y = yOfDb(g);
    const active = getP(P(b, 'active')) > 0.5;
    ctx2d.globalAlpha = active ? 1 : 0.35;
    if (dynEligible(b) && Math.abs(liveDeltas[b]) > 0.3) {
      ctx2d.beginPath();
      ctx2d.arc(x, y, 11, 0, Math.PI * 2);
      ctx2d.strokeStyle = '#ff9d3c';
      ctx2d.lineWidth = 2;
      ctx2d.stroke();
    }
    ctx2d.beginPath();
    ctx2d.arc(x, y, 7, 0, Math.PI * 2);
    ctx2d.fillStyle = BAND_COLORS[b];
    ctx2d.fill();
    if (b === selectedBand) {
      ctx2d.beginPath();
      ctx2d.arc(x, y, 9.5, 0, Math.PI * 2);
      ctx2d.strokeStyle = '#fff';
      ctx2d.lineWidth = 1.5;
      ctx2d.stroke();
    }
    ctx2d.fillStyle = '#101216';
    ctx2d.font = 'bold 9px sans-serif';
    ctx2d.textAlign = 'center';
    ctx2d.fillText(`${b + 1}`, x, y + 3);
    ctx2d.textAlign = 'left';
    ctx2d.globalAlpha = 1;
  }
}

// --- interactions ---
let dragBand = -1;
function hitTest(mx, my) {
  for (let b = 7; b >= 0; b--) {
    const type = Math.round(getP(P(b, 'type')));
    const g = typeHasGain(type) ? getP(P(b, 'gain')) : 0;
    const dx = mx - xOfF(getP(P(b, 'freq'))), dy = my - yOfDb(g);
    if (dx * dx + dy * dy < 14 * 14) return b;
  }
  return -1;
}
canvas.addEventListener('pointerdown', (e) => {
  const r = canvas.getBoundingClientRect();
  const b = hitTest(e.clientX - r.left, e.clientY - r.top);
  if (b >= 0) {
    dragBand = b;
    selectedBand = b;
    refreshBandPanel();
    canvas.setPointerCapture(e.pointerId);
  }
});
canvas.addEventListener('pointermove', (e) => {
  if (dragBand < 0) return;
  const r = canvas.getBoundingClientRect();
  const f = Math.min(FMAX, Math.max(FMIN, fOfX(e.clientX - r.left)));
  setParam(P(dragBand, 'freq'), f);
  if (typeHasGain(Math.round(getP(P(dragBand, 'type'))))) {
    const g = Math.min(24, Math.max(-24, dbOfY(e.clientY - r.top)));
    setParam(P(dragBand, 'gain'), g);
  }
  refreshBandPanel();
});
canvas.addEventListener('pointerup', () => { dragBand = -1; });
canvas.addEventListener('wheel', (e) => {
  e.preventDefault();
  const r = canvas.getBoundingClientRect();
  const over = hitTest(e.clientX - r.left, e.clientY - r.top);
  const b = over >= 0 ? over : selectedBand;
  const q = Math.min(18, Math.max(0.1, getP(P(b, 'q')) * Math.exp(-e.deltaY * 0.002)));
  setParam(P(b, 'q'), q);
  refreshBandPanel();
}, { passive: false });
canvas.addEventListener('dblclick', (e) => {
  const r = canvas.getBoundingClientRect();
  const b = hitTest(e.clientX - r.left, e.clientY - r.top);
  if (b >= 0) {
    setParam(P(b, 'active'), getP(P(b, 'active')) > 0.5 ? 0 : 1);
    refreshBandPanel();
  }
});

// ===========================================================================
// Audio graph + sources
// ===========================================================================
let audioCtx = null, workletNode = null, srcGain = null, scGain = null;
let activeSources = [];
let playing = false;
let uploadedBuffer = null;
const bufferCache = new Map();

function makeNoiseBuffer(kind, seconds = 4) {
  const len = Math.round(seconds * audioCtx.sampleRate);
  const buf = audioCtx.createBuffer(2, len, audioCtx.sampleRate);
  for (let chn = 0; chn < 2; chn++) {
    const d = buf.getChannelData(chn);
    let b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (let i = 0; i < len; i++) {
      const white = Math.random() * 2 - 1;
      if (kind === 'white') {
        d[i] = white * 0.5;
      } else {
        // Paul Kellet's economy pink noise filter
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        d[i] = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362) * 0.11;
        b6 = white * 0.115926;
      }
    }
  }
  return buf;
}

async function makeBandPinkBuffer() {
  const pink = await getBuffer('pink');
  const off = new OfflineAudioContext(2, pink.length, audioCtx.sampleRate);
  const src = off.createBufferSource();
  src.buffer = pink;
  // 24 dB/oct Butterworth pair each side: 200 Hz HP, 5 kHz LP
  let nodeChain = src;
  for (const [type, f, q] of [['highpass', 200, 0.541], ['highpass', 200, 1.307],
                               ['lowpass', 5000, 0.541], ['lowpass', 5000, 1.307]]) {
    const bq = off.createBiquadFilter();
    bq.type = type; bq.frequency.value = f; bq.Q.value = q;
    nodeChain.connect(bq);
    nodeChain = bq;
  }
  const g = off.createGain();
  g.gain.value = 1.6; // make up band-limiting energy loss
  nodeChain.connect(g).connect(off.destination);
  src.start();
  return off.startRendering();
}

function makePulseBuffer(kind) {
  const seconds = 1.6, on = 0.2, period = 0.4;
  const len = Math.round(seconds * audioCtx.sampleRate);
  const buf = audioCtx.createBuffer(2, len, audioCtx.sampleRate);
  const sr = audioCtx.sampleRate;
  for (let chn = 0; chn < 2; chn++) {
    const d = buf.getChannelData(chn);
    for (let i = 0; i < len; i++) {
      const t = i / sr;
      const tin = t % period;
      if (tin > on) { d[i] = 0; continue; }
      const env = Math.min(1, tin / 0.005, (on - tin) / 0.005);
      d[i] = 0.5 * env * (kind === 'tonepulse' ? Math.sin(2 * Math.PI * 1000 * t) : Math.random() * 2 - 1);
    }
  }
  return buf;
}

// Synthesized 4-bar demo groove at 110 BPM (all in-browser, nothing licensed).
async function makeGrooveBuffer() {
  const sr = audioCtx.sampleRate;
  const bpm = 110, beat = 60 / bpm, bars = 4, dur = bars * 4 * beat;
  const off = new OfflineAudioContext(2, Math.round(dur * sr), sr);
  const master = off.createGain();
  master.gain.value = 0.55;
  const comp = off.createDynamicsCompressor();
  comp.threshold.value = -14; comp.ratio.value = 3; comp.attack.value = 0.005; comp.release.value = 0.12;
  master.connect(comp).connect(off.destination);

  const noiseBuf = off.createBuffer(1, Math.round(sr * 0.5), sr);
  const nd = noiseBuf.getChannelData(0);
  for (let i = 0; i < nd.length; i++) nd[i] = Math.random() * 2 - 1;

  const env = (g, t, a, peak, d) => {
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(peak, t + a);
    g.gain.exponentialRampToValueAtTime(0.001, t + a + d);
  };
  const kick = (t) => {
    const o = off.createOscillator(), g = off.createGain();
    o.frequency.setValueAtTime(130, t);
    o.frequency.exponentialRampToValueAtTime(44, t + 0.11);
    env(g, t, 0.002, 0.9, 0.26);
    o.connect(g).connect(master);
    o.start(t); o.stop(t + 0.3);
  };
  const snare = (t) => {
    const s = off.createBufferSource(); s.buffer = noiseBuf;
    const bp = off.createBiquadFilter(); bp.type = 'bandpass'; bp.frequency.value = 1900; bp.Q.value = 0.6;
    const g = off.createGain();
    env(g, t, 0.001, 0.5, 0.16);
    s.connect(bp).connect(g).connect(master);
    s.start(t); s.stop(t + 0.2);
    const o = off.createOscillator(); o.frequency.value = 190;
    const g2 = off.createGain();
    env(g2, t, 0.001, 0.35, 0.09);
    o.connect(g2).connect(master);
    o.start(t); o.stop(t + 0.12);
  };
  const hat = (t, open) => {
    const s = off.createBufferSource(); s.buffer = noiseBuf;
    const hp = off.createBiquadFilter(); hp.type = 'highpass'; hp.frequency.value = 7200;
    const g = off.createGain();
    env(g, t, 0.001, open ? 0.16 : 0.13, open ? 0.25 : 0.035);
    s.connect(hp).connect(g).connect(master);
    s.start(t); s.stop(t + (open ? 0.3 : 0.06));
  };
  const bassNote = (t, freq, len) => {
    const o = off.createOscillator(); o.type = 'triangle'; o.frequency.value = freq;
    const lp = off.createBiquadFilter(); lp.type = 'lowpass'; lp.frequency.value = 900; lp.Q.value = 0.9;
    const g = off.createGain();
    env(g, t, 0.004, 0.5, len);
    o.connect(lp).connect(g).connect(master);
    o.start(t); o.stop(t + len + 0.05);
  };
  const chord = (t, freqs, len) => {
    for (const f of freqs) {
      for (const det of [-5, 5]) {
        const o = off.createOscillator(); o.type = 'sawtooth';
        o.frequency.value = f; o.detune.value = det;
        const lp = off.createBiquadFilter(); lp.type = 'lowpass'; lp.frequency.value = 1400;
        const g = off.createGain();
        g.gain.setValueAtTime(0, t);
        g.gain.linearRampToValueAtTime(0.045, t + 0.08);
        g.gain.setValueAtTime(0.045, t + len - 0.25);
        g.gain.linearRampToValueAtTime(0, t + len);
        o.connect(lp).connect(g).connect(master);
        o.start(t); o.stop(t + len);
      }
    }
  };

  const A1 = 55, C2 = 65.41, D2 = 73.42, E2 = 82.41, G2 = 98, F1 = 43.65;
  const bassPatterns = [
    [A1, 0, A1, A1, 0, C2, 0, E2],
    [A1, 0, A1, A1, 0, G2, 0, D2],
    [F1, 0, F1, F1, 0, C2, 0, A1],
    [F1, 0, F1, F1, 0, E2, 0, G2],
  ];
  const chords = [
    [220, 261.63, 329.63],        // Am
    [220, 261.63, 329.63],
    [174.61, 220, 261.63, 349.23], // F
    [174.61, 220, 261.63, 329.63],
  ];
  for (let bar = 0; bar < bars; bar++) {
    const t0 = bar * 4 * beat;
    chord(t0, chords[bar], 4 * beat);
    for (let b = 0; b < 4; b++) kick(t0 + b * beat);
    snare(t0 + 1 * beat); snare(t0 + 3 * beat);
    for (let e = 0; e < 8; e++) hat(t0 + e * beat / 2, bar === bars - 1 && e === 7);
    bassPatterns[bar].forEach((f, e) => { if (f) bassNote(t0 + e * beat / 2, f, beat * 0.45); });
  }
  return off.startRendering();
}

async function getBuffer(kind) {
  if (kind === 'file') return uploadedBuffer;
  if (bufferCache.has(kind)) return bufferCache.get(kind);
  let buf;
  if (kind === 'white' || kind === 'pink') buf = makeNoiseBuffer(kind);
  else if (kind === 'bandpink') buf = await makeBandPinkBuffer();
  else if (kind === 'groove') buf = await makeGrooveBuffer();
  else if (kind === 'tonepulse' || kind === 'noisepulse') buf = makePulseBuffer(kind);
  bufferCache.set(kind, buf);
  return buf;
}

async function ensureAudio() {
  if (audioCtx) return;
  audioCtx = new AudioContext();
  await audioCtx.audioWorklet.addModule('worklet.js');
  workletNode = new AudioWorkletNode(audioCtx, 'zero-eq', {
    numberOfInputs: 2,
    numberOfOutputs: 1,
    outputChannelCount: [2],
  });
  workletNode.port.onmessage = (e) => {
    const msg = e.data;
    if (msg.type === 'ready') {
      // push the full current state so the audio instance matches the UI
      workletPort = workletNode.port;
      const all = [];
      for (const [id, v] of params) all.push([id, v]);
      workletPort.postMessage({ type: 'params', entries: all });
      workletPort.postMessage({ type: 'safetyClip', on: $('safetyClip').checked });
    } else if (msg.type === 'status') {
      onStatus(msg);
    }
  };
  srcGain = audioCtx.createGain();
  scGain = audioCtx.createGain();
  srcGain.connect(workletNode, 0, 0);
  scGain.connect(workletNode, 0, 1);
  analyserPre = audioCtx.createAnalyser();
  analyserPost = audioCtx.createAnalyser();
  analyserPre.fftSize = 4096; analyserPost.fftSize = 4096;
  analyserPre.smoothingTimeConstant = 0.82; analyserPost.smoothingTimeConstant = 0.82;
  specBinsPre = new Uint8Array(analyserPre.frequencyBinCount);
  specBinsPost = new Uint8Array(analyserPost.frequencyBinCount);
  srcGain.connect(analyserPre);
  workletNode.connect(analyserPost);
  workletNode.connect(audioCtx.destination);
  setLevel();
}

function setLevel() {
  if (srcGain) srcGain.gain.value = Math.pow(10, parseFloat($('srcLevel').value) / 20);
}

function stopSources() {
  for (const s of activeSources) { try { s.stop(); } catch { /* already stopped */ } }
  activeSources = [];
}

async function startSources() {
  const kind = $('sourceSel').value;
  stopSources();
  if (kind === 'sine') {
    const o = audioCtx.createOscillator();
    o.frequency.value = sineFreqValue();
    o.connect(srcGain);
    o.start();
    activeSources.push(o);
  } else {
    const buf = await getBuffer(kind);
    if (!buf) { alert('Choose an audio file first.'); return false; }
    const s = audioCtx.createBufferSource();
    s.buffer = buf; s.loop = true;
    s.connect(srcGain);
    s.start();
    activeSources.push(s);
  }
  const scKind = $('scSel').value;
  if (scKind !== 'none') {
    const s = audioCtx.createBufferSource();
    s.buffer = await getBuffer(scKind);
    s.loop = true;
    s.connect(scGain);
    s.start();
    activeSources.push(s);
  }
  return true;
}

$('playBtn').addEventListener('click', async () => {
  if (!playing) {
    await ensureAudio();
    await audioCtx.resume();
    if (!(await startSources())) return;
    playing = true;
    $('playBtn').textContent = '■ Stop';
    $('playBtn').classList.add('playing');
  } else {
    stopSources();
    if (workletPort) workletPort.postMessage({ type: 'reset' });
    playing = false;
    $('playBtn').textContent = '▶ Play';
    $('playBtn').classList.remove('playing');
  }
});

$('sourceSel').addEventListener('change', async () => {
  $('sineFreqRow').style.display = $('sourceSel').value === 'sine' ? '' : 'none';
  if ($('sourceSel').value === 'file' && !uploadedBuffer) $('fileInput').click();
  if (playing) await startSources();
});
$('scSel').addEventListener('change', async () => { if (playing) await startSources(); });
$('fileInput').addEventListener('change', async () => {
  const f = $('fileInput').files[0];
  if (!f) return;
  await ensureAudio();
  uploadedBuffer = await audioCtx.decodeAudioData(await f.arrayBuffer());
  $('sourceSel').value = 'file';
  if (playing) await startSources();
});
$('sourceSel').addEventListener('dblclick', () => { if ($('sourceSel').value === 'file') $('fileInput').click(); });

const sineFreqValue = () => 20 * Math.pow(1000, parseFloat($('sineFreq').value));
$('sineFreq').addEventListener('input', () => {
  $('sineFreqVal').textContent = fmtHz(sineFreqValue());
  for (const s of activeSources) if (s.frequency) s.frequency.value = sineFreqValue();
});
$('sineFreqVal').textContent = fmtHz(sineFreqValue());
$('srcLevel').addEventListener('input', () => {
  $('srcLevelVal').textContent = `${parseFloat($('srcLevel').value).toFixed(1)} dB`;
  setLevel();
});
$('safetyClip').addEventListener('change', () => {
  if (workletPort) workletPort.postMessage({ type: 'safetyClip', on: $('safetyClip').checked });
});

// ===========================================================================
// Meters + live status
// ===========================================================================
function drawMeter(canvasEl, vuDb, peakDb) {
  const c = canvasEl.getContext('2d');
  const w = canvasEl.width, h = canvasEl.height;
  c.clearRect(0, 0, w, h);
  const yOf = (db) => h * (1 - Math.min(1, Math.max(0, (db + 60) / 60)));
  const vuY = yOf(vuDb);
  const grad = c.createLinearGradient(0, h, 0, 0);
  grad.addColorStop(0, '#3f9d55');
  grad.addColorStop(0.8, '#c9c93f');
  grad.addColorStop(1, '#d95f3f');
  c.fillStyle = grad;
  c.fillRect(4, vuY, w - 8, h - vuY);
  const pkY = yOf(peakDb);
  c.fillStyle = '#e8e8e8';
  c.fillRect(2, pkY - 1, w - 4, 2);
}

function onStatus(s) {
  liveDeltas = s.deltas;
  drawMeter($('inMeter'), s.inVu, s.inPeak);
  drawMeter($('outMeter'), s.outVu, s.outPeak);
  $('inRead').textContent = s.inPeak <= -99 ? '−∞' : `${s.inPeak.toFixed(1)}`;
  $('outRead').textContent = s.outPeak <= -99 ? '−∞' : `${s.outPeak.toFixed(1)}`;
  $('inClipLamp').classList.toggle('on', !!s.inClip);
  $('outClipLamp').classList.toggle('on', !!s.outClip);
  $('grRead').textContent = getP('comp_active') > 0.5 ? `GR ${s.compGr.toFixed(1)} dB` : '';
  const d = s.deltas[selectedBand];
  $('dynLive').textContent = dynEligible(selectedBand) ? `Δ ${d >= 0 ? '+' : ''}${d.toFixed(1)} dB` : '';
}

// ===========================================================================
// Render loop
// ===========================================================================
function frame() {
  flushParams(); // belt-and-braces for anything queued before the worklet was ready
  // spectrum + live curve animate while playing; otherwise redraw only when dirty
  if (playing || curveDirty) {
    drawCurve();
    curveDirty = false;
  }
  requestAnimationFrame(frame);
}

resizeCanvas();
refreshBandPanel();
frame();
