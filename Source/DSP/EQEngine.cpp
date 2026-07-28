#include "EQEngine.h"

namespace ZeroEQ
{

// Gets every band and dynamic detector ready for playback at the host's sample rate.
//
// The 0.02 figure gives each of frequency, gain and Q a 20-millisecond glide. Jumping a
// filter straight to a new setting produces an audible click, so when a knob is turned the
// value slides to its destination over that period instead. firstBlock is raised here so
// the very first block after starting snaps directly to the current knob positions rather
// than gliding up from silence.
void EQEngine::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    for (auto& band : bands)
        band.prepare(spec);
    for (auto& d : dynamicDetectors)
        d.prepare(spec);

    for (auto& s : smoothed)
    {
        s.freq.reset(spec.sampleRate, 0.02);
        s.gain.reset(spec.sampleRate, 0.02);
        s.q.reset(spec.sampleRate, 0.02);
    }

    firstBlock = true;
}

// Clears every filter's memory of the audio that came before. Called when playback stops
// or the transport jumps, so old audio can't bleed into the new position as a click.
void EQEngine::reset()
{
    for (auto& band : bands)
        band.reset();
    for (auto& d : dynamicDetectors)
        d.reset();
    firstBlock = true;
}

// The main entry point: reads the current knob positions and applies the whole EQ to a
// block of audio in place.
//
// For each band in turn it collects that band's settings, lets any dynamic behaviour
// adjust the gain based on what the audio is doing, then runs the audio through the band.
// Bands are applied one after another, so their effects stack.
//
// Solo is handled across all bands first, because soloing one band has to mute the others,
// which can't be decided while looking at a single band in isolation.
//
// sidechainBuffer is the optional external input: a band set to sidechain reacts to that
// signal instead of the audio passing through, which is how ducking one instrument against
// another is done. It may legitimately be empty when the host has nothing connected.
void EQEngine::updateAndProcess(juce::AudioBuffer<float>& buffer, juce::AudioProcessorValueTreeState& apvts,
                                 const juce::AudioBuffer<float>& sidechainBuffer)
{
    const bool sidechainConnected = sidechainBuffer.getNumChannels() > 0 && sidechainBuffer.getNumSamples() > 0;

    bool anySolo = false;
    for (int i = 0; i < numBands; ++i)
        if (apvts.getRawParameterValue(ParamIDs::bandSolo(i))->load() > 0.5f)
            anySolo = true;

    const int numSamples = buffer.getNumSamples();

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    for (int i = 0; i < numBands; ++i)
    {
        const auto type      = (FilterType) (int) apvts.getRawParameterValue(ParamIDs::bandType(i))->load();
        const float freq     = apvts.getRawParameterValue(ParamIDs::bandFreq(i))->load();
        const float gain     = apvts.getRawParameterValue(ParamIDs::bandGain(i))->load();
        const float q        = apvts.getRawParameterValue(ParamIDs::bandQ(i))->load();
        const auto character = (FilterCharacter) (int) apvts.getRawParameterValue(ParamIDs::bandCharacter(i))->load();
        const auto slope     = (FilterSlope) (int) apvts.getRawParameterValue(ParamIDs::bandSlope(i))->load();
        const bool active    = apvts.getRawParameterValue(ParamIDs::bandActive(i))->load() > 0.5f;
        const bool solo      = apvts.getRawParameterValue(ParamIDs::bandSolo(i))->load() > 0.5f;

        const bool dynActive        = apvts.getRawParameterValue(ParamIDs::bandDynActive(i))->load() > 0.5f;
        const auto dynDirection     = (DynamicDirection) (int) apvts.getRawParameterValue(ParamIDs::bandDynDirection(i))->load();
        const float dynThresholdDb  = apvts.getRawParameterValue(ParamIDs::bandDynThreshold(i))->load();
        const float dynRatio        = apvts.getRawParameterValue(ParamIDs::bandDynRatio(i))->load();
        const float dynAttackMs     = apvts.getRawParameterValue(ParamIDs::bandDynAttack(i))->load();
        const float dynReleaseMs    = apvts.getRawParameterValue(ParamIDs::bandDynRelease(i))->load();
        const float dynRangeDb      = apvts.getRawParameterValue(ParamIDs::bandDynRange(i))->load();
        const bool dynSidechain     = apvts.getRawParameterValue(ParamIDs::bandDynSidechain(i))->load() > 0.5f;
        const float harmonicBlend   = apvts.getRawParameterValue(ParamIDs::bandHarmonicBlend(i))->load();

        auto& s = smoothed[(size_t) i];
        if (firstBlock)
        {
            s.freq.setCurrentAndTargetValue(freq);
            s.gain.setCurrentAndTargetValue(gain);
            s.q.setCurrentAndTargetValue(q);
        }
        else
        {
            s.freq.setTargetValue(freq);
            s.gain.setTargetValue(gain);
            s.q.setTargetValue(q);
        }

        if (numSamples > 1)
        {
            s.freq.skip(numSamples - 1);
            s.gain.skip(numSamples - 1);
            s.q.skip(numSamples - 1);
        }
        const float curFreq = s.freq.getNextValue();
        const float curGain = s.gain.getNextValue();
        const float curQ    = s.q.getNextValue();

        bands[(size_t) i].isActive = active && (! anySolo || solo);

        float totalGain = curGain;
        const bool dynEligible = dynActive && bands[(size_t) i].isActive && filterTypeHasGain(type);
        if (dynEligible)
        {
            DynamicEQDetector::Params dp;
            dp.direction   = dynDirection;
            dp.thresholdDb = dynThresholdDb;
            dp.ratio       = dynRatio;
            dp.attackMs    = dynAttackMs;
            dp.releaseMs   = dynReleaseMs;
            dp.rangeDb     = dynRangeDb;

            // Detection normally reads the buffer as it currently stands (i.e. after
            // every earlier band in the series chain has already processed it, but
            // before this band does) - no lookahead, so this adds zero latency. If
            // this band asks for external sidechain detection and the host has
            // actually connected the sidechain bus, detect against that signal
            // instead (still no lookahead - same block, just a different source).
            const auto& detectionSource = (dynSidechain && sidechainConnected) ? sidechainBuffer : buffer;
            const float delta = dynamicDetectors[(size_t) i].process(detectionSource, type, curFreq, curQ, dp);
            totalGain += delta;
        }
        else
        {
            dynamicDetectors[(size_t) i].reset();
        }

        bands[(size_t) i].update(type, curFreq, totalGain, curQ, character, slope, harmonicBlend);
        bands[(size_t) i].process(context);
    }

    firstBlock = false;
}

EQEngine::BandSnapshot EQEngine::readSnapshot(juce::AudioProcessorValueTreeState& apvts, int i)
{
    BandSnapshot s;
    s.type      = (FilterType) (int) apvts.getRawParameterValue(ParamIDs::bandType(i))->load();
    s.freqHz    = apvts.getRawParameterValue(ParamIDs::bandFreq(i))->load();
    s.gainDb    = apvts.getRawParameterValue(ParamIDs::bandGain(i))->load();
    s.q         = apvts.getRawParameterValue(ParamIDs::bandQ(i))->load();
    s.character = (FilterCharacter) (int) apvts.getRawParameterValue(ParamIDs::bandCharacter(i))->load();
    s.slope     = (FilterSlope) (int) apvts.getRawParameterValue(ParamIDs::bandSlope(i))->load();
    s.active    = apvts.getRawParameterValue(ParamIDs::bandActive(i))->load() > 0.5f;
    s.solo      = apvts.getRawParameterValue(ParamIDs::bandSolo(i))->load() > 0.5f;

    s.dynActive     = apvts.getRawParameterValue(ParamIDs::bandDynActive(i))->load() > 0.5f;
    s.dynDirection  = (DynamicDirection) (int) apvts.getRawParameterValue(ParamIDs::bandDynDirection(i))->load();
    s.dynThresholdDb = apvts.getRawParameterValue(ParamIDs::bandDynThreshold(i))->load();
    s.dynRatio      = apvts.getRawParameterValue(ParamIDs::bandDynRatio(i))->load();
    s.dynAttackMs   = apvts.getRawParameterValue(ParamIDs::bandDynAttack(i))->load();
    s.dynReleaseMs  = apvts.getRawParameterValue(ParamIDs::bandDynRelease(i))->load();
    s.dynRangeDb    = apvts.getRawParameterValue(ParamIDs::bandDynRange(i))->load();
    s.dynSidechain  = apvts.getRawParameterValue(ParamIDs::bandDynSidechain(i))->load() > 0.5f;
    s.harmonicBlend = apvts.getRawParameterValue(ParamIDs::bandHarmonicBlend(i))->load();
    return s;
}

std::array<EQEngine::BandSnapshot, numBands> EQEngine::readAllSnapshots(juce::AudioProcessorValueTreeState& apvts)
{
    std::array<BandSnapshot, numBands> result;
    for (int i = 0; i < numBands; ++i)
        result[(size_t) i] = readSnapshot(apvts, i);
    return result;
}

float EQEngine::getCompositeMagnitude(const std::array<BandSnapshot, numBands>& snapshots, double frequencyHz, double sr)
{
    bool anySolo = false;
    for (auto& b : snapshots)
        if (b.solo)
            anySolo = true;

    double magnitude = 1.0;
    for (auto& b : snapshots)
    {
        if (! b.active || (anySolo && ! b.solo))
            continue;

        magnitude *= EQBand::computeMagnitudeForFrequency(b.type, b.freqHz, b.gainDb, b.q, b.character, b.slope,
                                                            frequencyHz, sr);
    }

    return (float) magnitude;
}

float EQEngine::getBandDynamicGainDeltaDb(int bandIndex) const
{
    if (bandIndex < 0 || bandIndex >= numBands)
        return 0.0f;
    return dynamicDetectors[(size_t) bandIndex].getCurrentGainDeltaDb();
}

} // namespace ZeroEQ
