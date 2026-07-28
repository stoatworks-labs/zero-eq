#include "LevelMeter.h"

namespace ZeroEQ
{

namespace
{
    // Estimates the signal's value *between* two samples, using the two neighbours on
    // either side to follow the waveform's curve rather than cutting the corner with a
    // straight line.
    //
    // This is what "true peak" metering needs. Digital audio only stores the level at
    // fixed instants, but the real waveform reconstructed on playback keeps moving between
    // them, and its actual crest can sit higher than any stored sample. A meter that only
    // reported stored samples would read safe while the signal was in fact clipping.
    //
    // t slides from 0 at y1 to 1 at y2.
    float catmullRom(float y0, float y1, float y2, float y3, float t)
    {
        const float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float a2 = -0.5f * y0 + 0.5f * y2;
        const float a3 = y1;
        return ((a0 * t + a1) * t + a2) * t + a3;
    }
}

void LevelMeter::prepare(double sr)
{
    sampleRate = sr;
    reset();
}

void LevelMeter::reset()
{
    peakEnvelopeLinear = 0.0f;
    vuEnvelopeLinear = 0.0f;
    peakDb.store(-100.0f);
    truePeakDb.store(-100.0f);
    vuDb.store(-100.0f);
    clipHoldSamplesRemaining.store(0);
}

// Measures a block of audio and updates the meter readings. Read-only - it never alters
// the audio.
//
// Three readings are produced at once because they answer different questions:
//   - peak      the highest stored sample; catches brief transients
//   - true peak the highest point of the reconstructed waveform, including between
//               samples, which is what actually matters for clipping
//   - VU        a slow, averaged reading that tracks perceived loudness
//
// Peak rises instantly but falls back gradually, so a momentary spike stays visible long
// enough to read instead of vanishing before the display refreshes.
//
// The results are published through atomics because the GUI reads them on a different
// thread while audio keeps processing.
void LevelMeter::process(const juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples == 0 || numChannels == 0)
        return;

    // --- sample peak: instant attack, ~20dB/s release ---
    const float instantPeak = buffer.getMagnitude(0, numSamples);
    if (instantPeak > peakEnvelopeLinear)
        peakEnvelopeLinear = instantPeak;
    else
    {
        const float releaseDb = 20.0f * (float) numSamples / (float) sampleRate;
        peakEnvelopeLinear *= juce::Decibels::decibelsToGain(-releaseDb);
    }
    peakDb.store(juce::Decibels::gainToDecibels(peakEnvelopeLinear, -100.0f));

    // --- VU-style integrating meter (RMS-based, ~300ms to 99% of a step) ---
    double sumSq = 0.0;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = buffer.getReadPointer(ch);
        for (int n = 0; n < numSamples; ++n)
            sumSq += (double) data[n] * (double) data[n];
    }
    const float instantRms = (float) std::sqrt(sumSq / (double) (numSamples * numChannels));

    const float vuTau = 0.3f;
    const float vuCoeffPerSample = std::pow(0.01f, 1.0f / (float) (sampleRate * vuTau));
    const float vuCoeffPerBlock = std::pow(vuCoeffPerSample, (float) numSamples);
    vuEnvelopeLinear = vuCoeffPerBlock * vuEnvelopeLinear + (1.0f - vuCoeffPerBlock) * instantRms;
    vuDb.store(juce::Decibels::gainToDecibels(vuEnvelopeLinear, -100.0f));

    // --- true-peak estimate: 4x oversample via Catmull-Rom, catch inter-sample peaks ---
    float truePeakLinear = instantPeak;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = buffer.getReadPointer(ch);
        for (int n = 0; n < numSamples - 1; ++n)
        {
            const float y0 = data[juce::jmax(0, n - 1)];
            const float y1 = data[n];
            const float y2 = data[n + 1];
            const float y3 = data[juce::jmin(numSamples - 1, n + 2)];

            for (float t : { 0.25f, 0.5f, 0.75f })
            {
                const float interp = std::abs(catmullRom(y0, y1, y2, y3, t));
                if (interp > truePeakLinear)
                    truePeakLinear = interp;
            }
        }
    }
    truePeakDb.store(juce::Decibels::gainToDecibels(truePeakLinear, -100.0f));

    // --- clip indicator, held for ~1.5s so a brief clip is still visible ---
    // Threshold is a hair under exact 0dBFS (-0.1dB, ~0.9886 linear) rather than
    // exactly 1.0f: a nominally "unity gain" stage isn't always bit-exact 1.0 after a
    // normalized-parameter round-trip (observed ~6e-8 off in practice), so a strict
    // >= 1.0f check can miss a genuinely full-scale signal. A small headroom margin is
    // also just how real clip indicators normally work.
    constexpr float clipThresholdLinear = 0.988553f; // -0.1 dBFS
    if (instantPeak >= clipThresholdLinear || truePeakLinear >= clipThresholdLinear)
    {
        // Tracked in samples remaining (not "blocks remaining"), so the hold time is a
        // real ~1.5s of audio regardless of how the host chunks its process() calls -
        // a call counter would make the hold duration depend on whatever block size
        // happened to trigger the clip versus whatever sizes follow it.
        clipHoldSamplesRemaining.store((int) (sampleRate * 1.5));
    }
    else if (clipHoldSamplesRemaining.load() > 0)
    {
        clipHoldSamplesRemaining.store(juce::jmax(0, clipHoldSamplesRemaining.load() - numSamples));
    }
}

} // namespace ZeroEQ
