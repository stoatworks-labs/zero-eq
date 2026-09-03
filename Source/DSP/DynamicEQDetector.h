#pragma once

#include <JuceHeader.h>
#include "../PluginParameters.h"

namespace ZeroEQ
{

// Per-band dynamic EQ detector: analyzes the signal arriving at this band's position
// in the chain through a type-appropriate analysis filter, envelope-follows it, and
// turns that into a gain-delta (dB) added on top of the band's static gain - turning
// a static EQ band into a frequency-selective compressor/expander. No lookahead: the
// detector only ever sees audio already in the current block, so this adds zero
// latency, same as every other stage in this plugin.
//
// The analysis filter shape is chosen per band type to approximate "the region this
// band affects": a band-pass at (freq, Q) for Bell/Tilt bands, a high-pass at freq for
// High Shelf (isolates the region above the corner it boosts/cuts), and a low-pass at
// freq for Low Shelf (isolates the region below the corner). This is a practical
// approximation for a first implementation, not a claim of matching any specific
// commercial dynamic EQ's exact detection algorithm.
class DynamicEQDetector
{
public:
    struct Params
    {
        DynamicDirection direction = DynamicDirection::Downward;
        float thresholdDb = -24.0f;
        float ratio = 2.0f;
        float attackMs = 10.0f;
        float releaseMs = 100.0f;
        float rangeDb = 12.0f;
    };

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    // Analyzes `buffer` (read-only; the signal arriving at this band before it
    // processes) and returns this block's dynamic gain delta in dB (0 = no effect,
    // negative = ducking, positive = boosting). `buffer` is NOT modified.
    float process(const juce::AudioBuffer<float>& buffer, FilterType bandType,
                  float freqHz, float q, const Params& params);

    float getCurrentGainDeltaDb() const { return currentGainDeltaDb.load(); }

private:
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    juce::dsp::IIR::Filter<float> analysisFilter;
    double sampleRate = 44100.0;

    // The analysis filter was redesigned on every block, on the audio thread, and
    // Coeffs::make* allocates. It only depends on the band's type, frequency and Q
    // — NOT on the gain the detector is computing — so for a band whose knobs are
    // still it never needs recomputing at all, however hard the detector is working.
    bool haveAnalysisDesign = false;
    FilterType lastAnalysisType {};
    float lastAnalysisFreq = 0.0f;
    float lastAnalysisQ = 0.0f;
    double lastAnalysisSampleRate = 0.0;

    float rmsStateSquared = 0.0f;
    float smoothedGainDeltaDb = 0.0f;
    std::atomic<float> currentGainDeltaDb { 0.0f };

    static Coeffs::Ptr designAnalysisFilter(FilterType bandType, float freqHz, float q, double sampleRate);
};

} // namespace ZeroEQ
