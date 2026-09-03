#include "EQBand.h"

namespace ZeroEQ
{

void EQBand::prepare(const juce::dsp::ProcessSpec& spec)
{
    currentSampleRate = spec.sampleRate;
    for (auto& stage : stages)
        stage.prepare(spec);

    // prepare() rebuilds the per-channel filters, so the next update must design
    // rather than trust its cache. Cheap, and it means nothing here depends on
    // whether a re-prepare happened to keep the same sample rate.
    haveDesign = false;

    const size_t numChannels = juce::jmax((size_t) 1, (size_t) spec.numChannels);
    harmonicShapers.assign(numChannels, HarmonicShaper());

    // Each channel needs its own isolation filter instance (own state), even though
    // they all share the same coefficients. Built in place - juce::dsp::IIR::Filter
    // isn't copy-assignable, so assign()/resize() won't work here.
    auto monoSpec = spec;
    monoSpec.numChannels = 1;
    harmonicBandFilters.clear();
    harmonicBandFilters.reserve(numChannels);
    for (size_t i = 0; i < numChannels; ++i)
        harmonicBandFilters.emplace_back();
    for (auto& f : harmonicBandFilters)
        f.prepare(monoSpec);

    // Force a redesign on the next update() now that the sample rate may have changed.
    lastIsolationFreq = 0.0f;
    lastIsolationQ = 0.0f;
}

void EQBand::reset()
{
    for (auto& stage : stages)
        stage.reset();
    for (auto& shaper : harmonicShapers)
        shaper.reset();
    for (auto& f : harmonicBandFilters)
        f.reset();
}

EQBand::Coeffs::Ptr EQBand::designRegionIsolationFilter(FilterType type, float freqHz, float q, double sampleRate)
{
    freqHz = juce::jlimit(20.0f, (float) (sampleRate * 0.49), freqHz);
    q = juce::jmax(0.1f, q);

    switch (type)
    {
        case FilterType::LowShelf:  return Coeffs::makeLowPass(sampleRate, freqHz, q);
        case FilterType::HighShelf: return Coeffs::makeHighPass(sampleRate, freqHz, q);
        case FilterType::Bell:
        case FilterType::TiltShelf:
        default:                    return Coeffs::makeBandPass(sampleRate, freqHz, q);
    }
}

float EQBand::proportionalQ(float baseQ, float gainDb, FilterCharacter character)
{
    if (character != FilterCharacter::Vintage)
        return baseQ;

    // Musical/console-style EQs tend to widen bandwidth as more gain is applied.
    // This is an approximation of that interactive behaviour, not a circuit model
    // of any specific hardware or plugin.
    const float k = 0.045f;
    const float widened = baseQ / (1.0f + k * std::abs(gainDb));
    return juce::jlimit(0.1f, 18.0f, widened);
}

std::vector<EQBand::Coeffs::Ptr> EQBand::design(FilterType type, float freqHz, float gainDb, float q,
                                                 FilterCharacter character, FilterSlope slope, double sampleRate)
{
    freqHz = juce::jlimit(20.0f, (float) (sampleRate * 0.49), freqHz);
    const float gainFactor = juce::Decibels::decibelsToGain(gainDb);
    const float effectiveQ = proportionalQ(q, gainDb, character);

    std::vector<Coeffs::Ptr> result;

    switch (type)
    {
        case FilterType::Bell:
            result.push_back(Coeffs::makePeakFilter(sampleRate, freqHz, effectiveQ, gainFactor));
            break;

        case FilterType::LowShelf:
            result.push_back(Coeffs::makeLowShelf(sampleRate, freqHz, effectiveQ, gainFactor));
            break;

        case FilterType::HighShelf:
            result.push_back(Coeffs::makeHighShelf(sampleRate, freqHz, effectiveQ, gainFactor));
            break;

        case FilterType::Notch:
            result.push_back(Coeffs::makeNotch(sampleRate, freqHz, juce::jmax(0.1f, q)));
            break;

        case FilterType::BandPass:
            result.push_back(Coeffs::makeBandPass(sampleRate, freqHz, juce::jmax(0.1f, q)));
            break;

        case FilterType::TiltShelf:
            result.push_back(Coeffs::makeLowShelf(sampleRate, freqHz, 0.5f, juce::Decibels::decibelsToGain(-gainDb * 0.5f)));
            result.push_back(Coeffs::makeHighShelf(sampleRate, freqHz, 0.5f, juce::Decibels::decibelsToGain(gainDb * 0.5f)));
            break;

        case FilterType::HighPass:
        case FilterType::LowPass:
        {
            const int order = 2 * ((int) slope + 1); // Slope12->2, Slope24->4, Slope36->6, Slope48->8
            auto designed = (type == FilterType::HighPass)
                ? juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod(freqHz, sampleRate, order)
                : juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod(freqHz, sampleRate, order);

            const int count = juce::jmin(designed.size(), maxStages);
            for (int i = 0; i < count; ++i)
                result.push_back(designed[i]);
            break;
        }

        default:
            result.push_back(Coeffs::makeAllPass(sampleRate, freqHz, effectiveQ));
            break;
    }

    return result;
}

void EQBand::update(FilterType type, float freqHz, float gainDb, float q,
                     FilterCharacter character, FilterSlope slope, float harmonicBlend)
{
    // Only redesign when something that shapes the filter has actually moved.
    // Every Coeffs::make* allocates, and this runs on the audio thread; the stages
    // below are written in place, so an unchanged band already holds the right
    // coefficients and there is nothing to recompute. Same guard the isolation
    // filter below has always had, applied to the design it was protecting the
    // wrong half of.
    //
    // The comparison is against the values as passed — which are the SMOOTHED
    // ones, and for a dynamic band include its gain delta. That is deliberate:
    // those genuinely change the filter, so they must redesign. What this removes
    // is the redesign of a band that is not moving at all, which was happening on
    // every single block.
    const bool designChanged = ! haveDesign
                               || type != lastDesignType
                               || character != lastDesignCharacter
                               || slope != lastDesignSlope
                               || ! juce::approximatelyEqual(freqHz, lastDesignFreq)
                               || ! juce::approximatelyEqual(gainDb, lastDesignGainDb)
                               || ! juce::approximatelyEqual(q, lastDesignQ)
                               || ! juce::approximatelyEqual(currentSampleRate, lastDesignSampleRate);

    if (designChanged)
    {
        auto designed = design(type, freqHz, gainDb, q, character, slope, currentSampleRate);
        activeStageCount = juce::jmin((int) designed.size(), maxStages);
        for (int i = 0; i < activeStageCount; ++i)
        {
            // Mutate the shared Coefficients object's contents in place rather than
            // replacing the pointer: ProcessorDuplicator's per-channel Filters each hold
            // their own reference to the coefficients captured at prepare() time, so
            // reassigning stages[i].state here would only update the Duplicator's own
            // bookkeeping field and never reach the actual per-channel filters.
            if (stages[(size_t) i].state == nullptr)
                stages[(size_t) i].state = designed[(size_t) i];
            else
                *stages[(size_t) i].state = *designed[(size_t) i];
        }

        haveDesign           = true;
        lastDesignType       = type;
        lastDesignFreq       = freqHz;
        lastDesignGainDb     = gainDb;
        lastDesignQ          = q;
        lastDesignCharacter  = character;
        lastDesignSlope      = slope;
        lastDesignSampleRate = currentSampleRate;
    }

    harmonicEnabled = (character == FilterCharacter::Harmonic) && filterTypeHasGain(type);
    // Drive scales with how much gain the band is applying - a band left at 0dB stays
    // essentially transparent even in Harmonic mode; up to +/-12dB reaches full drive.
    harmonicDriveAmount = harmonicEnabled ? juce::jlimit(0.0f, 1.0f, std::abs(gainDb) / 12.0f) : 0.0f;
    harmonicBlendAmount = harmonicBlend;

    if (harmonicEnabled)
    {
        // Only redesign when the isolated region actually moves - Coeffs::make* allocates,
        // and this runs on the audio thread.
        const bool changed = type != lastIsolationType
                             || ! juce::approximatelyEqual(freqHz, lastIsolationFreq)
                             || ! juce::approximatelyEqual(q, lastIsolationQ);

        if (changed || harmonicBandFilters.empty() || harmonicBandFilters[0].coefficients == nullptr)
        {
            auto isolation = designRegionIsolationFilter(type, freqHz, q, currentSampleRate);
            for (auto& f : harmonicBandFilters)
            {
                if (f.coefficients == nullptr)
                    f.coefficients = isolation;
                else
                    *f.coefficients = *isolation;
            }

            lastIsolationType = type;
            lastIsolationFreq = freqHz;
            lastIsolationQ = q;
        }
    }
}

void EQBand::process(const juce::dsp::ProcessContextReplacing<float>& context)
{
    if (! isActive)
        return;

    for (int i = 0; i < activeStageCount; ++i)
        stages[(size_t) i].process(context);

    if (harmonicEnabled && harmonicDriveAmount > 0.0f && ! harmonicBandFilters.empty()
        && harmonicBandFilters[0].coefficients != nullptr)
    {
        auto block = context.getOutputBlock();
        const size_t numChannels = juce::jmin(block.getNumChannels(),
                                               juce::jmin(harmonicShapers.size(), harmonicBandFilters.size()));
        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* samples = block.getChannelPointer(ch);
            auto& isolate = harmonicBandFilters[ch];
            auto& shaper = harmonicShapers[ch];

            for (size_t n = 0; n < block.getNumSamples(); ++n)
            {
                const float dry = samples[n];

                // Isolate this band's spectral region, generate harmonics from ONLY
                // that, then sum the generated content back in parallel with the dry
                // signal. Subtracting `region` leaves just the harmonics the shaper
                // added, so the band's own level is untouched - a Harmonic band adds
                // harmonic content to the frequencies it targets rather than boosting
                // the fundamental or saturating the full-range signal.
                const float region = isolate.processSample(dry);
                const float shaped = shaper.processSample(region, harmonicDriveAmount, harmonicBlendAmount);

                samples[n] = dry + (shaped - region);
            }
        }
    }
}

float EQBand::getMagnitudeForFrequency(double frequencyHz) const
{
    if (! isActive)
        return 1.0f;

    double magnitude = 1.0;
    for (int i = 0; i < activeStageCount; ++i)
        if (stages[(size_t) i].state != nullptr)
            magnitude *= stages[(size_t) i].state->getMagnitudeForFrequency(frequencyHz, currentSampleRate);

    return (float) magnitude;
}

float EQBand::computeMagnitudeForFrequency(FilterType type, float freqHz, float gainDb, float q,
                                            FilterCharacter character, FilterSlope slope,
                                            double frequencyHz, double sampleRate)
{
    auto designed = design(type, freqHz, gainDb, q, character, slope, sampleRate);
    double magnitude = 1.0;
    for (auto& c : designed)
        if (c != nullptr)
            magnitude *= c->getMagnitudeForFrequency(frequencyHz, sampleRate);

    return (float) magnitude;
}

} // namespace ZeroEQ
