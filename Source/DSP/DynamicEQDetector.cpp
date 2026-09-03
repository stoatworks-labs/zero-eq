#include "DynamicEQDetector.h"
#include "EQBand.h"

namespace ZeroEQ
{

// Called before playback starts, and again if the sample rate changes.
//
// The analysis filter is deliberately set up as mono: the channels are summed before
// measurement so both get an identical gain change. Measuring per channel would let a loud
// sound on one side move only that side and smear the stereo image.
void DynamicEQDetector::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    auto monoSpec = spec;
    monoSpec.numChannels = 1;
    analysisFilter.prepare(monoSpec);
    haveAnalysisDesign = false;   // redesign on the next block, not from the cache
    reset();
}

// Forgets the measured level and the current gain movement, so playback restarts from
// rest rather than resuming part-way through a reaction.
void DynamicEQDetector::reset()
{
    analysisFilter.reset();
    rmsStateSquared = 0.0f;
    smoothedGainDeltaDb = 0.0f;
    currentGainDeltaDb.store(0.0f);
}

DynamicEQDetector::Coeffs::Ptr DynamicEQDetector::designAnalysisFilter(FilterType bandType, float freqHz, float q, double sr)
{
    // "The region this band affects" is one concept, shared with the Harmonic
    // character's saturation stage - keep it defined in exactly one place.
    return EQBand::designRegionIsolationFilter(bandType, freqHz, q, sr);
}

// Works out how much this band's gain should move right now, in decibels, and returns it.
//
// This is what makes a band "dynamic": instead of a fixed boost or cut, the band reacts to
// how much energy the audio currently has *in that band's own frequency region*. A
// de-esser is the everyday example - the band only dips when there is actually sibilance
// present, and leaves the rest of the vocal alone.
//
// It listens rather than alters: the audio is filtered down to the band's region purely to
// measure it, and this function returns a number for the caller to apply. The buffer it is
// given is read-only and never modified.
//
// Downward means "pull back when the region gets loud", Upward means "lift when the region
// gets quiet", and rangeDb caps how far either can go.
float DynamicEQDetector::process(const juce::AudioBuffer<float>& buffer, FilterType bandType,
                                  float freqHz, float q, const Params& params)
{
    // Only when the region being listened to has actually moved. See the note on
    // the cache fields: this design does not depend on the detector's own output,
    // so a band sitting at a fixed frequency and Q designs once and never again.
    if (! haveAnalysisDesign
        || bandType != lastAnalysisType
        || ! juce::approximatelyEqual(freqHz, lastAnalysisFreq)
        || ! juce::approximatelyEqual(q, lastAnalysisQ)
        || ! juce::approximatelyEqual(sampleRate, lastAnalysisSampleRate)
        || analysisFilter.coefficients == nullptr)
    {
        auto designed = designAnalysisFilter(bandType, freqHz, q, sampleRate);
        if (analysisFilter.coefficients == nullptr)
            analysisFilter.coefficients = designed;
        else
            *analysisFilter.coefficients = *designed;

        haveAnalysisDesign     = true;
        lastAnalysisType       = bandType;
        lastAnalysisFreq       = freqHz;
        lastAnalysisQ          = q;
        lastAnalysisSampleRate = sampleRate;
    }

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numSamples == 0 || numChannels == 0)
        return smoothedGainDeltaDb;

    const float attackCoeff  = std::exp(-1.0f / (float) (sampleRate * (params.attackMs * 0.001f)));
    const float releaseCoeff = std::exp(-1.0f / (float) (sampleRate * (params.releaseMs * 0.001f)));
    const float rmsCoeff     = std::exp(-1.0f / (float) (sampleRate * 0.005));

    auto* const* channelData = buffer.getArrayOfReadPointers();

    for (int n = 0; n < numSamples; ++n)
    {
        float monoSample = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            monoSample += channelData[ch][n];
        monoSample /= (float) numChannels;

        const float filtered = analysisFilter.processSample(monoSample);

        rmsStateSquared = rmsCoeff * rmsStateSquared + (1.0f - rmsCoeff) * (filtered * filtered);
        const float envelope = std::sqrt(juce::jmax(0.0f, rmsStateSquared));
        const float levelDb = juce::Decibels::gainToDecibels(envelope, -100.0f);

        float targetDeltaDb = 0.0f;
        if (params.direction == DynamicDirection::Downward)
        {
            const float over = levelDb - params.thresholdDb;
            if (over > 0.0f)
                targetDeltaDb = -juce::jmin(params.rangeDb, over * (1.0f - 1.0f / params.ratio));
        }
        else
        {
            const float under = params.thresholdDb - levelDb;
            if (under > 0.0f)
                targetDeltaDb = juce::jmin(params.rangeDb, under * (1.0f - 1.0f / params.ratio));
        }

        // Branched attack/release: attack while the magnitude of the delta is growing,
        // release while it's shrinking back toward zero. Same technique as Compressor.
        if (std::abs(targetDeltaDb) > std::abs(smoothedGainDeltaDb))
            smoothedGainDeltaDb = attackCoeff * smoothedGainDeltaDb + (1.0f - attackCoeff) * targetDeltaDb;
        else
            smoothedGainDeltaDb = releaseCoeff * smoothedGainDeltaDb + (1.0f - releaseCoeff) * targetDeltaDb;
    }

    currentGainDeltaDb.store(smoothedGainDeltaDb);
    return smoothedGainDeltaDb;
}

} // namespace ZeroEQ
