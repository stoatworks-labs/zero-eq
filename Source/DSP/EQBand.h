#pragma once

#include <JuceHeader.h>
#include "../PluginParameters.h"
#include "HarmonicShaper.h"

namespace ZeroEQ
{

// A single EQ band: one or more cascaded minimum-phase biquads (zero added latency).
// Handles the "Vintage" proportional-Q character (bandwidth widens with applied gain,
// approximating the interactive behaviour of passive/console-style musical EQs such as
// Cranborne Audio's Harmonic EQ), the "Harmonic" character (Modern-style linear
// response plus gain-driven even/odd harmonics generated from ONLY this band's
// spectral region and summed back in parallel, via HarmonicShaper), and a "Modern"
// independent-Q mode matching textbook zero-latency parametric EQ behaviour.
class EQBand
{
public:
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    EQBand() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    void update(FilterType type, float freqHz, float gainDb, float q,
                FilterCharacter character, FilterSlope slope, float harmonicBlend);

    void process(const juce::dsp::ProcessContextReplacing<float>& context);

    // Composite magnitude (linear gain) of THIS LIVE band at a given frequency.
    // Audio-thread state only; do not call from the GUI thread.
    float getMagnitudeForFrequency(double frequencyHz) const;

    bool isActive = true;

    // Pure, stateless coefficient design shared by the audio-thread updater and the
    // GUI curve renderer. Safe to call from any thread since it touches no shared state.
    static std::vector<Coeffs::Ptr> design(FilterType type, float freqHz, float gainDb, float q,
                                            FilterCharacter character, FilterSlope slope, double sampleRate);

    // Stateless magnitude query for GUI curve drawing - builds temporary coefficients,
    // does not touch the live filter state, so it's safe to call from the message thread
    // while the audio thread is concurrently processing.
    static float computeMagnitudeForFrequency(FilterType type, float freqHz, float gainDb, float q,
                                               FilterCharacter character, FilterSlope slope,
                                               double frequencyHz, double sampleRate);

    static float proportionalQ(float baseQ, float gainDb, FilterCharacter character);

    // Designs a filter isolating "the frequency region this band affects": a band-pass
    // at (freq, Q) for Bell/Tilt, a high-pass at the corner for High Shelf (the region
    // above it), a low-pass at the corner for Low Shelf (the region below it). Shared
    // by the Harmonic character's saturation stage and DynamicEQDetector, which both
    // need exactly this notion - a practical approximation of a band's spectral region,
    // not a claim of matching any specific commercial product's exact analysis shape.
    static Coeffs::Ptr designRegionIsolationFilter(FilterType type, float freqHz, float q, double sampleRate);

private:
    static constexpr int maxStages = 4;

    using Filter = juce::dsp::IIR::Filter<float>;
    using Duplicator = juce::dsp::ProcessorDuplicator<Filter, Coeffs>;

    std::array<Duplicator, maxStages> stages;
    int activeStageCount = 1;
    double currentSampleRate = 44100.0;

    std::vector<HarmonicShaper> harmonicShapers;
    // Per-channel copy of the region-isolation filter: the Harmonic character generates
    // harmonics from ONLY this band's spectral region, then sums them back into the dry
    // signal, so a Harmonic band colours the frequencies it targets instead of
    // saturating the whole broadband signal passing through it.
    std::vector<Filter> harmonicBandFilters;
    bool harmonicEnabled = false;
    float harmonicDriveAmount = 0.0f;
    float harmonicBlendAmount = 0.5f;

    // Cached so the isolation coefficients are only redesigned when they actually
    // change, rather than reallocating them on the audio thread every block.
    FilterType lastIsolationType = FilterType::Bell;
    float lastIsolationFreq = 0.0f;
    float lastIsolationQ = 0.0f;
};

} // namespace ZeroEQ
