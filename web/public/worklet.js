// AudioWorklet host for the Zero EQ wasm core. Input 0 = main stereo, input 1 =
// optional sidechain. All DSP happens inside the wasm module (the plugin's own
// Source/DSP code); this file only shuttles samples and messages.

import createZeroEQModule from './zeroeq.js';

const BLOCK = 128;
const STATUS_INTERVAL_BLOCKS = 6; // ~16 ms at 48 kHz

class ZeroEQProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.safetyClip = true;
    this.pendingParams = [];

    createZeroEQModule().then((M) => {
      this.M = M;
      this.C = {
        init: M.cwrap('zeq_init', null, ['number']),
        reset: M.cwrap('zeq_reset', null, []),
        mainL: M.cwrap('zeq_main_l', 'number', []),
        mainR: M.cwrap('zeq_main_r', 'number', []),
        scL: M.cwrap('zeq_sc_l', 'number', []),
        scR: M.cwrap('zeq_sc_r', 'number', []),
        setParam: M.cwrap('zeq_set_param', null, ['string', 'number']),
        process: M.cwrap('zeq_process', null, ['number', 'number']),
        dynDelta: M.cwrap('zeq_band_dyn_delta', 'number', ['number']),
        compGr: M.cwrap('zeq_comp_gr', 'number', []),
        inPeak: M.cwrap('zeq_in_peak', 'number', []),
        inTp: M.cwrap('zeq_in_tp', 'number', []),
        inVu: M.cwrap('zeq_in_vu', 'number', []),
        inClip: M.cwrap('zeq_in_clip', 'number', []),
        outPeak: M.cwrap('zeq_out_peak', 'number', []),
        outTp: M.cwrap('zeq_out_tp', 'number', []),
        outVu: M.cwrap('zeq_out_vu', 'number', []),
        outClip: M.cwrap('zeq_out_clip', 'number', []),
      };
      this.C.init(sampleRate);
      this.pMainL = this.C.mainL() >> 2;
      this.pMainR = this.C.mainR() >> 2;
      this.pScL = this.C.scL() >> 2;
      this.pScR = this.C.scR() >> 2;
      for (const [id, value] of this.pendingParams) this.C.setParam(id, value);
      this.pendingParams = [];
      this.ready = true;
      this.blockCount = 0;
      this.port.postMessage({ type: 'ready' });
    });

    this.port.onmessage = (e) => {
      const msg = e.data;
      if (msg.type === 'params') {
        if (this.ready) {
          for (const [id, value] of msg.entries) this.C.setParam(id, value);
        } else {
          this.pendingParams.push(...msg.entries);
        }
      } else if (msg.type === 'reset') {
        if (this.ready) this.C.reset();
      } else if (msg.type === 'safetyClip') {
        this.safetyClip = !!msg.on;
      }
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!this.ready || out.length === 0) return true;

    const main = inputs[0];
    const sc = inputs[1];
    const n = out[0].length; // render quantum (128)
    const H = this.M.HEAPF32;

    const mainL = main.length > 0 ? main[0] : null;
    const mainR = main.length > 1 ? main[1] : mainL;
    const scConnected = sc && sc.length > 0 && sc[0] && sc[0].some((v) => v !== 0);
    const scL = scConnected ? sc[0] : null;
    const scR = scConnected ? (sc.length > 1 ? sc[1] : sc[0]) : null;

    for (let i = 0; i < n; i++) {
      H[this.pMainL + i] = mainL ? mainL[i] : 0;
      H[this.pMainR + i] = mainR ? mainR[i] : 0;
      H[this.pScL + i] = scL ? scL[i] : 0;
      H[this.pScR + i] = scR ? scR[i] : 0;
    }

    this.C.process(n, scConnected ? 1 : 0);

    // Safety clip (web-only, defaults on): hard ceiling at -0.1 dBFS so extreme
    // settings can't send full-scale-plus into someone's headphones. The plugin
    // itself has no such stage - the UI labels it clearly and it can be disabled.
    const ceil = 0.988553;
    const oL = out[0];
    const oR = out.length > 1 ? out[1] : null;
    if (this.safetyClip) {
      for (let i = 0; i < n; i++) {
        oL[i] = Math.max(-ceil, Math.min(ceil, H[this.pMainL + i]));
        if (oR) oR[i] = Math.max(-ceil, Math.min(ceil, H[this.pMainR + i]));
      }
    } else {
      for (let i = 0; i < n; i++) {
        oL[i] = H[this.pMainL + i];
        if (oR) oR[i] = H[this.pMainR + i];
      }
    }

    if (++this.blockCount >= STATUS_INTERVAL_BLOCKS) {
      this.blockCount = 0;
      const deltas = new Array(8);
      for (let b = 0; b < 8; b++) deltas[b] = this.C.dynDelta(b);
      this.port.postMessage({
        type: 'status',
        inPeak: this.C.inPeak(), inTp: this.C.inTp(), inVu: this.C.inVu(), inClip: this.C.inClip(),
        outPeak: this.C.outPeak(), outTp: this.C.outTp(), outVu: this.C.outVu(), outClip: this.C.outClip(),
        compGr: this.C.compGr(),
        deltas,
      });
    }

    return true;
  }
}

registerProcessor('zero-eq', ZeroEQProcessor);
