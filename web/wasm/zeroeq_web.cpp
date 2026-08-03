// WebAssembly wrapper around Zero EQ's unmodified DSP core.
//
// This file plays the role PluginProcessor.cpp plays in the plugin: it owns the
// parameter store, the EQEngine/Compressor/LevelMeter instances, and runs the exact
// same processing chain in the same order (input gain -> input meter -> EQ ->
// compressor -> output gain -> output meter). The DSP classes themselves are the
// plugin's own Source/DSP files, compiled verbatim against the shim JuceHeader.h.
//
// Parameter ids, defaults, ranges and skews are harvested from the plugin's real
// createParameterLayout() (running against the shim's recording parameter classes),
// so nothing here needs keeping in sync by hand when the plugin's parameters change.

#include <JuceHeader.h>
#include "PluginParameters.h"
#include "DSP/EQEngine.h"
#include "DSP/Compressor.h"
#include "DSP/LevelMeter.h"

#include <emscripten/emscripten.h>

using namespace ZeroEQ;

namespace
{

constexpr int kMaxBlock = 2048;

struct WebZeroEQ
{
    juce::AudioProcessorValueTreeState apvts;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    EQEngine engine;
    Compressor compressor;
    LevelMeter inputMeter, outputMeter;

    double sampleRate = 48000.0;

    float mainL[kMaxBlock] = {}, mainR[kMaxBlock] = {};
    float scL[kMaxBlock] = {}, scR[kMaxBlock] = {};
};

WebZeroEQ* Z = nullptr;

juce::RangedAudioParameter* findParam (const char* id)
{
    for (auto& p : Z->layout.params)
        if (p->paramID.toStdString() == id)
            return p.get();
    return nullptr;
}

} // namespace

extern "C"
{

EMSCRIPTEN_KEEPALIVE
void zeq_init (double sampleRate)
{
    delete Z;
    Z = new WebZeroEQ();
    Z->sampleRate = sampleRate;
    Z->layout = createParameterLayout();

    for (auto& p : Z->layout.params)
        Z->apvts.addParameter (p->paramID.toStdString(), p->defaultValue);

    const juce::dsp::ProcessSpec spec { sampleRate, (uint32_t) kMaxBlock, 2 };
    Z->engine.prepare (spec);
    Z->compressor.prepare (spec);
    Z->inputMeter.prepare (sampleRate);
    Z->outputMeter.prepare (sampleRate);
}

EMSCRIPTEN_KEEPALIVE
void zeq_reset()
{
    if (Z == nullptr)
        return;
    Z->engine.reset();
    Z->compressor.reset();
    Z->inputMeter.reset();
    Z->outputMeter.reset();
}

EMSCRIPTEN_KEEPALIVE float* zeq_main_l() { return Z->mainL; }
EMSCRIPTEN_KEEPALIVE float* zeq_main_r() { return Z->mainR; }
EMSCRIPTEN_KEEPALIVE float* zeq_sc_l()   { return Z->scL; }
EMSCRIPTEN_KEEPALIVE float* zeq_sc_r()   { return Z->scR; }
EMSCRIPTEN_KEEPALIVE int zeq_max_block() { return kMaxBlock; }

EMSCRIPTEN_KEEPALIVE
void zeq_set_param (const char* id, float value)
{
    if (Z != nullptr)
        Z->apvts.addParameter (id, value);
}

EMSCRIPTEN_KEEPALIVE
float zeq_get_param (const char* id)
{
    return Z != nullptr ? Z->apvts.getRawParameterValue (juce::String (id))->load() : 0.0f;
}

// --- parameter metadata (harvested from the plugin's own createParameterLayout) ---
EMSCRIPTEN_KEEPALIVE int zeq_num_params() { return Z != nullptr ? (int) Z->layout.params.size() : 0; }

EMSCRIPTEN_KEEPALIVE
const char* zeq_param_id (int index)
{
    return Z->layout.params[(size_t) index]->paramID.toRawUTF8();
}

EMSCRIPTEN_KEEPALIVE float zeq_param_default (const char* id) { auto* p = findParam (id); return p != nullptr ? p->defaultValue : 0.0f; }
EMSCRIPTEN_KEEPALIVE float zeq_param_min (const char* id)     { auto* p = findParam (id); return p != nullptr ? p->rangeStart : 0.0f; }
EMSCRIPTEN_KEEPALIVE float zeq_param_max (const char* id)     { auto* p = findParam (id); return p != nullptr ? p->rangeEnd : 1.0f; }
EMSCRIPTEN_KEEPALIVE float zeq_param_skew (const char* id)    { auto* p = findParam (id); return p != nullptr ? p->skew : 1.0f; }

// --- processing: mirrors ZeroEQAudioProcessor::processBlock stage for stage ---
EMSCRIPTEN_KEEPALIVE
void zeq_process (int numSamples, int sidechainConnected)
{
    if (Z == nullptr || numSamples <= 0 || numSamples > kMaxBlock)
        return;

    float* mainChans[2] = { Z->mainL, Z->mainR };
    juce::AudioBuffer<float> mainBuffer (mainChans, 2, numSamples);

    // --- Input gain + metering ---
    const float inputGainDb = Z->apvts.getRawParameterValue (ParamIDs::inputGain)->load();
    mainBuffer.applyGain (juce::Decibels::decibelsToGain (inputGainDb));
    Z->inputMeter.process (mainBuffer);

    // Sidechain view: 0 channels when unconnected, so EQEngine's per-band fallback
    // to internal detection engages exactly as it does in the plugin.
    float* scChans[2] = { Z->scL, Z->scR };
    juce::AudioBuffer<float> sidechainBuffer (scChans, sidechainConnected != 0 ? 2 : 0, numSamples);

    // --- EQ ---
    const bool eqActive = Z->apvts.getRawParameterValue (ParamIDs::eqActive)->load() > 0.5f;
    if (eqActive)
        Z->engine.updateAndProcess (mainBuffer, Z->apvts, sidechainBuffer);

    // --- Compressor ---
    const bool compActive = Z->apvts.getRawParameterValue (ParamIDs::compActive)->load() > 0.5f;
    if (compActive)
    {
        Compressor::Params p;
        p.thresholdDb = Z->apvts.getRawParameterValue (ParamIDs::compThreshold)->load();
        p.ratio       = Z->apvts.getRawParameterValue (ParamIDs::compRatio)->load();
        p.attackMs    = Z->apvts.getRawParameterValue (ParamIDs::compAttack)->load();
        p.releaseMs   = Z->apvts.getRawParameterValue (ParamIDs::compRelease)->load();
        p.kneeDb      = Z->apvts.getRawParameterValue (ParamIDs::compKnee)->load();
        p.makeupDb    = Z->apvts.getRawParameterValue (ParamIDs::compMakeup)->load();
        p.autoMakeup  = Z->apvts.getRawParameterValue (ParamIDs::compAutoMakeup)->load() > 0.5f;
        p.detector    = (DetectorType) (int) Z->apvts.getRawParameterValue (ParamIDs::compDetector)->load();
        Z->compressor.process (mainBuffer, p);
    }
    else
    {
        Z->compressor.reset();
    }

    // --- Output gain + metering ---
    const float outputGainDb = Z->apvts.getRawParameterValue (ParamIDs::outputGain)->load();
    mainBuffer.applyGain (juce::Decibels::decibelsToGain (outputGainDb));
    Z->outputMeter.process (mainBuffer);
}

// --- live readouts for the GUI ---
EMSCRIPTEN_KEEPALIVE float zeq_band_dyn_delta (int band) { return Z != nullptr ? Z->engine.getBandDynamicGainDeltaDb (band) : 0.0f; }
EMSCRIPTEN_KEEPALIVE float zeq_comp_gr()                 { return Z != nullptr ? Z->compressor.getCurrentGainReductionDb() : 0.0f; }

EMSCRIPTEN_KEEPALIVE float zeq_in_peak()   { return Z->inputMeter.getPeakDb(); }
EMSCRIPTEN_KEEPALIVE float zeq_in_tp()     { return Z->inputMeter.getTruePeakDb(); }
EMSCRIPTEN_KEEPALIVE float zeq_in_vu()     { return Z->inputMeter.getVuDb(); }
EMSCRIPTEN_KEEPALIVE int   zeq_in_clip()   { return Z->inputMeter.isClipping() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE float zeq_out_peak()  { return Z->outputMeter.getPeakDb(); }
EMSCRIPTEN_KEEPALIVE float zeq_out_tp()    { return Z->outputMeter.getTruePeakDb(); }
EMSCRIPTEN_KEEPALIVE float zeq_out_vu()    { return Z->outputMeter.getVuDb(); }
EMSCRIPTEN_KEEPALIVE int   zeq_out_clip()  { return Z->outputMeter.isClipping() ? 1 : 0; }

// --- curve math for the GUI, using the same static design code as the plugin ---

// Composite magnitude (dB) across all bands at `frequency`, honouring active/solo,
// exactly as EQEngine::getCompositeMagnitude does for the plugin's curve display.
EMSCRIPTEN_KEEPALIVE
float zeq_composite_magnitude_db (double frequency)
{
    if (Z == nullptr)
        return 0.0f;
    const auto snapshots = EQEngine::readAllSnapshots (Z->apvts);
    const float mag = EQEngine::getCompositeMagnitude (snapshots, frequency, Z->sampleRate);
    // Floor well below -100: a 48 dB/oct filter legitimately reaches -200 dB and the
    // curve display needs the true shape, not a clamp artefact.
    return juce::Decibels::gainToDecibels (mag, -300.0f);
}

// Single band's magnitude (dB) at `frequency` with an overridden gain - lets the GUI
// draw the live dynamic curve (static gain + live delta) and the dynamic range
// envelope (gain +/- range) from the same maths the audio path uses.
EMSCRIPTEN_KEEPALIVE
float zeq_band_magnitude_db (int band, double frequency, float gainDbOverride)
{
    if (Z == nullptr || band < 0 || band >= numBands)
        return 0.0f;
    const auto s = EQEngine::readSnapshot (Z->apvts, band);
    const float mag = EQBand::computeMagnitudeForFrequency (s.type, s.freqHz, gainDbOverride, s.q,
                                                            s.character, s.slope, frequency, Z->sampleRate);
    return juce::Decibels::gainToDecibels (mag, -300.0f);
}

} // extern "C"
