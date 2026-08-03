#pragma once

// JUCE-compatible shim for the WebAssembly build of Zero EQ's DSP core.
//
// The plugin's Source/DSP files compile against this header COMPLETELY UNMODIFIED -
// it sits on the include path in place of the real <JuceHeader.h> and provides only
// the slice of the JUCE API those files actually touch. The filter math (coefficient
// formulas, DF2T state updates, Butterworth cascade Qs, SmoothedValue ramp semantics)
// is transcribed from the exact JUCE sources the plugin builds against
// (build/_deps/juce-src, JUCE 8), so the web build and the shipped plugin produce
// the same output. If a DSP file starts using a JUCE symbol this shim lacks, the
// wasm build fails loudly at compile time - extend the shim from the real JUCE
// source rather than approximating from memory.
//
// Intentional semantic match with a JUCE subtlety this project depends on:
// ProcessorDuplicator's per-channel filters capture the SAME shared Coefficients
// object at prepare() time. Replacing `duplicator.state` afterwards does NOT reach
// them; mutating `*duplicator.state` does. EQBand::update() relies on exactly that
// (see the v0.2.0 silence-bug notes), so the shim reproduces it via shared_ptr.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace juce
{

//==============================================================================
// Math basics
template <typename T>
struct MathConstants
{
    static constexpr T pi    = static_cast<T> (3.141592653589793238L);
    static constexpr T twoPi = static_cast<T> (2.0L * 3.141592653589793238L);
};

template <typename A, typename B>
constexpr std::common_type_t<A, B> jmin (A a, B b)
{
    using C = std::common_type_t<A, B>;
    return static_cast<C> (b) < static_cast<C> (a) ? static_cast<C> (b) : static_cast<C> (a);
}

template <typename A, typename B>
constexpr std::common_type_t<A, B> jmax (A a, B b)
{
    using C = std::common_type_t<A, B>;
    return static_cast<C> (a) < static_cast<C> (b) ? static_cast<C> (b) : static_cast<C> (a);
}

template <typename T>
constexpr T jlimit (T lower, T upper, T value)
{
    return value < lower ? lower : (upper < value ? upper : value);
}

template <typename T>
inline bool approximatelyEqual (T a, T b)
{
    const T scale = jmax (std::abs (a), std::abs (b));
    return std::abs (a - b) <= jmax (scale * static_cast<T> (1.0e-6), static_cast<T> (1.0e-12));
}

//==============================================================================
struct Decibels
{
    template <typename T>
    static T decibelsToGain (T decibels, T minusInfinityDb = static_cast<T> (-100.0))
    {
        return decibels > minusInfinityDb ? std::pow (static_cast<T> (10.0), decibels * static_cast<T> (0.05))
                                          : T();
    }

    template <typename T>
    static T gainToDecibels (T gain, T minusInfinityDb = static_cast<T> (-100.0))
    {
        return gain > T() ? jmax (minusInfinityDb, static_cast<T> (std::log10 (gain)) * static_cast<T> (20.0))
                          : minusInfinityDb;
    }

    template <typename T>
    static T gainWithLowerBound (T gain, T lowerBoundDb)
    {
        return jmax (gain, decibelsToGain (lowerBoundDb, lowerBoundDb - static_cast<T> (1.0)));
    }
};

//==============================================================================
class String
{
public:
    String() = default;
    String (const char* s) : text (s != nullptr ? s : "") {}
    String (const std::string& s) : text (s) {}
    explicit String (int value) : text (std::to_string (value)) {}

    String operator+ (const String& other) const { return String (text + other.text); }
    String operator+ (const char* other) const   { return String (text + (other != nullptr ? other : "")); }

    bool operator== (const String& other) const { return text == other.text; }
    bool operator!= (const String& other) const { return text != other.text; }
    bool operator<  (const String& other) const { return text <  other.text; }

    const std::string& toStdString() const { return text; }
    const char* toRawUTF8() const { return text.c_str(); }

private:
    std::string text;
};

inline String operator+ (const char* a, const String& b) { return String (a) + b; }

struct StringArray : public std::vector<String>
{
    StringArray() = default;
    StringArray (std::initializer_list<String> items) : std::vector<String> (items) {}
    int size() const { return (int) std::vector<String>::size(); }
};

//==============================================================================
template <typename T>
class AudioBuffer
{
public:
    AudioBuffer() = default;

    // Owning: allocates channel storage.
    AudioBuffer (int numChannelsToAllocate, int numSamplesToAllocate)
    {
        setSize (numChannelsToAllocate, numSamplesToAllocate);
    }

    // Non-owning view over externally-owned channel pointers (the same pattern the
    // plugin's test harnesses use for host-buffer-shape testing).
    AudioBuffer (T* const* dataToReferTo, int numChannelsToUse, int numSamplesToUse)
        : numSamples (numSamplesToUse)
    {
        channels.assign ((size_t) jmax (0, numChannelsToUse), nullptr);
        for (int ch = 0; ch < numChannelsToUse; ++ch)
            channels[(size_t) ch] = dataToReferTo[ch];
    }

    void setSize (int newNumChannels, int newNumSamples)
    {
        storage.assign ((size_t) newNumChannels, std::vector<T> ((size_t) newNumSamples, T()));
        channels.resize ((size_t) newNumChannels);
        for (int ch = 0; ch < newNumChannels; ++ch)
            channels[(size_t) ch] = storage[(size_t) ch].data();
        numSamples = newNumSamples;
    }

    int getNumChannels() const { return (int) channels.size(); }
    int getNumSamples() const  { return numSamples; }

    const T* getReadPointer (int ch) const { return channels[(size_t) ch]; }
    T* getWritePointer (int ch)            { return channels[(size_t) ch]; }

    const T* const* getArrayOfReadPointers() const { return const_cast<const T* const*> (channels.data()); }
    T* const* getArrayOfWritePointers()            { return channels.data(); }

    void clear()
    {
        for (auto* ch : channels)
            std::fill (ch, ch + numSamples, T());
    }

    void clear (int channel, int startSample, int count)
    {
        std::fill (channels[(size_t) channel] + startSample,
                   channels[(size_t) channel] + startSample + count, T());
    }

    void applyGain (T gain)
    {
        for (auto* ch : channels)
            for (int n = 0; n < numSamples; ++n)
                ch[n] *= gain;
    }

    T getMagnitude (int startSample, int count) const
    {
        T mag = T();
        for (auto* ch : channels)
            for (int n = startSample; n < startSample + count; ++n)
                mag = jmax (mag, std::abs (ch[n]));
        return mag;
    }

private:
    std::vector<T*> channels;
    std::vector<std::vector<T>> storage;
    int numSamples = 0;
};

//==============================================================================
// Linear SmoothedValue - semantics transcribed from juce_SmoothedValue.h.
template <typename FloatType>
class SmoothedValue
{
public:
    SmoothedValue() = default;
    SmoothedValue (FloatType initialValue) : currentValue (initialValue), target (initialValue) {}

    void reset (double sampleRate, double rampLengthInSeconds)
    {
        stepsToTarget = (int) std::floor (rampLengthInSeconds * sampleRate);
        setCurrentAndTargetValue (target);
    }

    void setCurrentAndTargetValue (FloatType newValue)
    {
        target = currentValue = newValue;
        countdown = 0;
    }

    void setTargetValue (FloatType newValue)
    {
        if (approximatelyEqual (newValue, target))
            return;

        if (stepsToTarget <= 0)
        {
            setCurrentAndTargetValue (newValue);
            return;
        }

        target = newValue;
        countdown = stepsToTarget;
        step = (target - currentValue) / (FloatType) countdown;
    }

    FloatType getNextValue()
    {
        if (countdown <= 0)
            return target;

        --countdown;
        currentValue += step;
        return currentValue;
    }

    FloatType skip (int numSamples)
    {
        if (numSamples >= countdown)
        {
            setCurrentAndTargetValue (target);
            return target;
        }

        currentValue += step * (FloatType) numSamples;
        countdown -= numSamples;
        return currentValue;
    }

    FloatType getCurrentValue() const { return countdown <= 0 ? target : currentValue; }
    FloatType getTargetValue() const  { return target; }
    bool isSmoothing() const          { return countdown > 0; }

private:
    FloatType currentValue {}, target {}, step {};
    int countdown = 0, stepsToTarget = 0;
};

//==============================================================================
namespace dsp
{

struct ProcessSpec
{
    double sampleRate;
    uint32_t maximumBlockSize;
    uint32_t numChannels;
};

template <typename T>
class AudioBlock
{
public:
    AudioBlock (AudioBuffer<T>& buffer)
        : channelPointers (buffer.getArrayOfWritePointers()),
          numChannels ((size_t) buffer.getNumChannels()),
          numSamples ((size_t) buffer.getNumSamples())
    {}

    size_t getNumChannels() const { return numChannels; }
    size_t getNumSamples() const  { return numSamples; }
    T* getChannelPointer (size_t ch) const { return channelPointers[ch]; }

private:
    T* const* channelPointers;
    size_t numChannels, numSamples;
};

template <typename T>
class ProcessContextReplacing
{
public:
    ProcessContextReplacing (AudioBlock<T>& blockToUse) : block (blockToUse) {}
    AudioBlock<T>& getInputBlock() const  { return block; }
    AudioBlock<T>& getOutputBlock() const { return block; }

private:
    AudioBlock<T>& block;
};

//==============================================================================
namespace IIR
{

// Coefficient storage matches JUCE: [b0..bOrder, a1..aOrder], normalised so a0 == 1
// (a0 == 0 in the default-constructed case, which stores all zeros - i.e. silence,
// the exact behaviour the plugin's ProcessorDuplicator lesson depends on).
template <typename NumericType>
struct Coefficients
{
    using Ptr = std::shared_ptr<Coefficients>;

    std::vector<NumericType> coefficients;

    Coefficients() { assign6 (0, 0, 0, 0, 0, 0); }

    Coefficients (NumericType b0, NumericType b1, NumericType a0, NumericType a1)
    {
        assign4 (b0, b1, a0, a1);
    }

    Coefficients (NumericType b0, NumericType b1, NumericType b2,
                  NumericType a0, NumericType a1, NumericType a2)
    {
        assign6 (b0, b1, b2, a0, a1, a2);
    }

    size_t getFilterOrder() const { return (coefficients.size() - 1) / 2; }
    const NumericType* getRawCoefficients() const { return coefficients.data(); }

    // Transcribed from juce_IIRFilter.cpp (ArrayCoefficients<NumericType>).
    static Ptr makeLowPass (double sampleRate, NumericType frequency, NumericType Q)
    {
        const auto n = 1 / std::tan (MathConstants<NumericType>::pi * frequency / (NumericType) sampleRate);
        const auto nSquared = n * n;
        const auto invQ = 1 / Q;
        const auto c1 = 1 / (1 + invQ * n + nSquared);
        return std::make_shared<Coefficients> (c1, c1 * 2, c1,
                                               (NumericType) 1, c1 * 2 * (1 - nSquared),
                                               c1 * (1 - invQ * n + nSquared));
    }

    static Ptr makeHighPass (double sampleRate, NumericType frequency, NumericType Q)
    {
        const auto n = std::tan (MathConstants<NumericType>::pi * frequency / (NumericType) sampleRate);
        const auto nSquared = n * n;
        const auto invQ = 1 / Q;
        const auto c1 = 1 / (1 + invQ * n + nSquared);
        return std::make_shared<Coefficients> (c1, c1 * -2, c1,
                                               (NumericType) 1, c1 * 2 * (nSquared - 1),
                                               c1 * (1 - invQ * n + nSquared));
    }

    static Ptr makeBandPass (double sampleRate, NumericType frequency, NumericType Q)
    {
        const auto n = 1 / std::tan (MathConstants<NumericType>::pi * frequency / (NumericType) sampleRate);
        const auto nSquared = n * n;
        const auto invQ = 1 / Q;
        const auto c1 = 1 / (1 + invQ * n + nSquared);
        return std::make_shared<Coefficients> (c1 * n * invQ, (NumericType) 0, -c1 * n * invQ,
                                               (NumericType) 1, c1 * 2 * (1 - nSquared),
                                               c1 * (1 - invQ * n + nSquared));
    }

    static Ptr makeNotch (double sampleRate, NumericType frequency, NumericType Q)
    {
        const auto n = 1 / std::tan (MathConstants<NumericType>::pi * frequency / (NumericType) sampleRate);
        const auto nSquared = n * n;
        const auto invQ = 1 / Q;
        const auto c1 = 1 / (1 + n * invQ + nSquared);
        const auto b0 = c1 * (1 + nSquared);
        const auto b1 = 2 * c1 * (1 - nSquared);
        return std::make_shared<Coefficients> (b0, b1, b0,
                                               (NumericType) 1, b1, c1 * (1 - n * invQ + nSquared));
    }

    static Ptr makeAllPass (double sampleRate, NumericType frequency, NumericType Q)
    {
        const auto n = 1 / std::tan (MathConstants<NumericType>::pi * frequency / (NumericType) sampleRate);
        const auto nSquared = n * n;
        const auto invQ = 1 / Q;
        const auto c1 = 1 / (1 + invQ * n + nSquared);
        const auto b0 = c1 * (1 - n * invQ + nSquared);
        const auto b1 = c1 * 2 * (1 - nSquared);
        return std::make_shared<Coefficients> (b0, b1, (NumericType) 1,
                                               (NumericType) 1, b1, b0);
    }

    static Ptr makeFirstOrderLowPass (double sampleRate, NumericType frequency)
    {
        const auto n = std::tan (MathConstants<NumericType>::pi * frequency / (NumericType) sampleRate);
        return std::make_shared<Coefficients> (n, n, n + 1, n - 1);
    }

    static Ptr makeFirstOrderHighPass (double sampleRate, NumericType frequency)
    {
        const auto n = std::tan (MathConstants<NumericType>::pi * frequency / (NumericType) sampleRate);
        return std::make_shared<Coefficients> ((NumericType) 1, (NumericType) -1, n + 1, n - 1);
    }

    static Ptr makeLowShelf (double sampleRate, NumericType cutOffFrequency, NumericType Q, NumericType gainFactor)
    {
        const auto A = std::sqrt (Decibels::gainWithLowerBound (gainFactor, (NumericType) -300.0));
        const auto aminus1 = A - 1;
        const auto aplus1 = A + 1;
        const auto omega = (2 * MathConstants<NumericType>::pi * jmax (cutOffFrequency, (NumericType) 2.0)) / (NumericType) sampleRate;
        const auto coso = std::cos (omega);
        const auto beta = std::sin (omega) * std::sqrt (A) / Q;
        const auto aminus1TimesCoso = aminus1 * coso;
        return std::make_shared<Coefficients> (A * (aplus1 - aminus1TimesCoso + beta),
                                               A * 2 * (aminus1 - aplus1 * coso),
                                               A * (aplus1 - aminus1TimesCoso - beta),
                                               aplus1 + aminus1TimesCoso + beta,
                                               -2 * (aminus1 + aplus1 * coso),
                                               aplus1 + aminus1TimesCoso - beta);
    }

    static Ptr makeHighShelf (double sampleRate, NumericType cutOffFrequency, NumericType Q, NumericType gainFactor)
    {
        const auto A = std::sqrt (Decibels::gainWithLowerBound (gainFactor, (NumericType) -300.0));
        const auto aminus1 = A - 1;
        const auto aplus1 = A + 1;
        const auto omega = (2 * MathConstants<NumericType>::pi * jmax (cutOffFrequency, (NumericType) 2.0)) / (NumericType) sampleRate;
        const auto coso = std::cos (omega);
        const auto beta = std::sin (omega) * std::sqrt (A) / Q;
        const auto aminus1TimesCoso = aminus1 * coso;
        return std::make_shared<Coefficients> (A * (aplus1 + aminus1TimesCoso + beta),
                                               A * -2 * (aminus1 + aplus1 * coso),
                                               A * (aplus1 + aminus1TimesCoso - beta),
                                               aplus1 - aminus1TimesCoso + beta,
                                               2 * (aminus1 - aplus1 * coso),
                                               aplus1 - aminus1TimesCoso - beta);
    }

    static Ptr makePeakFilter (double sampleRate, NumericType frequency, NumericType Q, NumericType gainFactor)
    {
        const auto A = std::sqrt (Decibels::gainWithLowerBound (gainFactor, (NumericType) -300.0));
        const auto omega = (2 * MathConstants<NumericType>::pi * jmax (frequency, (NumericType) 2.0)) / (NumericType) sampleRate;
        const auto alpha = std::sin (omega) / (Q * 2);
        const auto c2 = -2 * std::cos (omega);
        const auto alphaTimesA = alpha * A;
        const auto alphaOverA = alpha / A;
        return std::make_shared<Coefficients> (1 + alphaTimesA, c2, 1 - alphaTimesA,
                                               1 + alphaOverA, c2, 1 - alphaOverA);
    }

    double getMagnitudeForFrequency (double frequency, double sampleRate) const
    {
        const size_t order = getFilterOrder();
        if (order == 0)
            return 0.0;

        const double omega = MathConstants<double>::twoPi * frequency / sampleRate;
        const std::complex<double> zMinus1 = std::polar (1.0, -omega);

        std::complex<double> numerator (0.0, 0.0), denominator (1.0, 0.0), factor (1.0, 0.0);
        for (size_t k = 0; k <= order; ++k)
        {
            numerator += (double) coefficients[k] * factor;
            factor *= zMinus1;
        }

        factor = zMinus1;
        for (size_t k = 1; k <= order; ++k)
        {
            denominator += (double) coefficients[order + k] * factor;
            factor *= zMinus1;
        }

        return std::abs (numerator / denominator);
    }

private:
    void assign4 (NumericType b0, NumericType b1, NumericType a0, NumericType a1)
    {
        const NumericType a0Inv = ! approximatelyEqual (a0, NumericType()) ? 1 / a0 : NumericType();
        coefficients = { b0 * a0Inv, b1 * a0Inv, a1 * a0Inv };
    }

    void assign6 (NumericType b0, NumericType b1, NumericType b2,
                  NumericType a0, NumericType a1, NumericType a2)
    {
        const NumericType a0Inv = ! approximatelyEqual (a0, NumericType()) ? 1 / a0 : NumericType();
        coefficients = { b0 * a0Inv, b1 * a0Inv, b2 * a0Inv, a1 * a0Inv, a2 * a0Inv };
    }
};

// Mono IIR filter, Direct Form II transposed - transcribed from juce_IIRFilter_Impl.h.
template <typename SampleType>
class Filter
{
public:
    using CoefficientsPtr = typename Coefficients<SampleType>::Ptr;

    CoefficientsPtr coefficients;

    Filter() : coefficients (std::make_shared<Coefficients<SampleType>> ((SampleType) 1, (SampleType) 0,
                                                                         (SampleType) 1, (SampleType) 0))
    {
        reset();
    }

    Filter (CoefficientsPtr c) : coefficients (std::move (c)) { reset(); }

    void prepare (const ProcessSpec&) { reset(); }

    void reset (SampleType resetToValue = SampleType())
    {
        order = coefficients != nullptr ? coefficients->getFilterOrder() : 0;
        state.assign (jmax (order, (size_t) 3) + 1, resetToValue);
    }

    SampleType processSample (SampleType sample)
    {
        check();
        if (order == 0)
            return SampleType();

        const auto* c = coefficients->getRawCoefficients();
        auto output = c[0] * sample + state[0];

        for (size_t j = 0; j < order - 1; ++j)
            state[j] = c[j + 1] * sample - c[order + j + 1] * output + state[j + 1];

        state[order - 1] = c[order] * sample - c[order * 2] * output;
        return output;
    }

    void snapToZero()
    {
        for (auto& s : state)
            if (! (std::abs (s) >= (SampleType) 1.0e-30))
                s = SampleType();
    }

private:
    void check()
    {
        if (coefficients != nullptr && order != coefficients->getFilterOrder())
            reset();
    }

    std::vector<SampleType> state;
    size_t order = 0;
};

} // namespace IIR

//==============================================================================
// Multi-channel wrapper around a mono processor. Matches JUCE's aliasing semantics:
// per-channel filters share the SAME Coefficients object grabbed at prepare() time
// via shared_ptr - replacing `state` later never reaches them, mutating `*state` does.
template <typename ProcessorType, typename StateType>
struct ProcessorDuplicator
{
    typename StateType::Ptr state { std::make_shared<StateType>() };

    void prepare (const ProcessSpec& spec)
    {
        processors.clear();
        for (uint32_t ch = 0; ch < spec.numChannels; ++ch)
        {
            processors.emplace_back();
            processors.back().coefficients = state;
            processors.back().reset();
        }
    }

    void reset()
    {
        for (auto& p : processors)
            p.reset();
    }

    template <typename SampleType>
    void process (const ProcessContextReplacing<SampleType>& context)
    {
        auto& block = context.getOutputBlock();
        const size_t numChannels = jmin (block.getNumChannels(), processors.size());
        const size_t numSamples = block.getNumSamples();

        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* samples = block.getChannelPointer (ch);
            auto& p = processors[ch];
            for (size_t n = 0; n < numSamples; ++n)
                samples[n] = p.processSample (samples[n]);
            p.snapToZero();
        }
    }

private:
    std::vector<ProcessorType> processors;
};

//==============================================================================
template <typename FloatType>
struct FilterDesign
{
    // Minimal stand-in for juce::ReferenceCountedArray as used by EQBand.
    struct CoefficientsArray
    {
        std::vector<typename IIR::Coefficients<FloatType>::Ptr> items;
        int size() const { return (int) items.size(); }
        const typename IIR::Coefficients<FloatType>::Ptr& operator[] (int i) const { return items[(size_t) i]; }
    };

    // Transcribed from juce_FilterDesign.cpp.
    static CoefficientsArray designIIRLowpassHighOrderButterworthMethod (FloatType frequency, double sampleRate, int order)
    {
        CoefficientsArray result;
        if (order % 2 == 1)
        {
            result.items.push_back (IIR::Coefficients<FloatType>::makeFirstOrderLowPass (sampleRate, frequency));
            for (int i = 0; i < order / 2; ++i)
            {
                const auto Q = 1.0 / (2.0 * std::cos ((i + 1.0) * MathConstants<double>::pi / order));
                result.items.push_back (IIR::Coefficients<FloatType>::makeLowPass (sampleRate, frequency, (FloatType) Q));
            }
        }
        else
        {
            for (int i = 0; i < order / 2; ++i)
            {
                const auto Q = 1.0 / (2.0 * std::cos ((2.0 * i + 1.0) * MathConstants<double>::pi / (order * 2.0)));
                result.items.push_back (IIR::Coefficients<FloatType>::makeLowPass (sampleRate, frequency, (FloatType) Q));
            }
        }
        return result;
    }

    static CoefficientsArray designIIRHighpassHighOrderButterworthMethod (FloatType frequency, double sampleRate, int order)
    {
        CoefficientsArray result;
        if (order % 2 == 1)
        {
            result.items.push_back (IIR::Coefficients<FloatType>::makeFirstOrderHighPass (sampleRate, frequency));
            for (int i = 0; i < order / 2; ++i)
            {
                const auto Q = 1.0 / (2.0 * std::cos ((i + 1.0) * MathConstants<double>::pi / order));
                result.items.push_back (IIR::Coefficients<FloatType>::makeHighPass (sampleRate, frequency, (FloatType) Q));
            }
        }
        else
        {
            for (int i = 0; i < order / 2; ++i)
            {
                const auto Q = 1.0 / (2.0 * std::cos ((2.0 * i + 1.0) * MathConstants<double>::pi / (order * 2.0)));
                result.items.push_back (IIR::Coefficients<FloatType>::makeHighPass (sampleRate, frequency, (FloatType) Q));
            }
        }
        return result;
    }
};

} // namespace dsp

//==============================================================================
// Parameter-declaration classes: dummies that RECORD what the plugin registers.
// createParameterLayout() in PluginParameters.h runs against these unmodified, and
// the wasm wrapper harvests every parameter's id/default/range/skew from the result -
// so the web build's parameter set and defaults come from the plugin's own source,
// never from a hand-maintained copy.

struct ParameterID
{
    ParameterID (const String& id, int versionHint = 1) : paramID (id), version (versionHint) {}
    String paramID;
    int version;
};

template <typename T>
struct NormalisableRange
{
    NormalisableRange() = default;
    NormalisableRange (T rangeStart, T rangeEnd, T intervalValue = T(), T skewFactor = (T) 1)
        : start (rangeStart), end (rangeEnd), interval (intervalValue), skew (skewFactor) {}

    T start {}, end {}, interval {}, skew { (T) 1 };
};

struct AudioParameterFloatAttributes
{
    AudioParameterFloatAttributes withLabel (const String&) const { return *this; }
};

class RangedAudioParameter
{
public:
    virtual ~RangedAudioParameter() = default;

    String paramID;
    float defaultValue = 0.0f;
    float rangeStart = 0.0f, rangeEnd = 1.0f, interval = 0.0f, skew = 1.0f;
    int numSteps = 0; // > 0 for choice params (the number of choices), 2 for bools
};

class AudioParameterFloat : public RangedAudioParameter
{
public:
    AudioParameterFloat (const ParameterID& id, const String&, const NormalisableRange<float>& range,
                         float def, const AudioParameterFloatAttributes& = {})
    {
        paramID = id.paramID;
        defaultValue = def;
        rangeStart = range.start;
        rangeEnd = range.end;
        interval = range.interval;
        skew = range.skew;
    }
};

class AudioParameterChoice : public RangedAudioParameter
{
public:
    AudioParameterChoice (const ParameterID& id, const String&, const StringArray& choices, int def)
    {
        paramID = id.paramID;
        defaultValue = (float) def;
        rangeStart = 0.0f;
        rangeEnd = (float) jmax (0, choices.size() - 1);
        interval = 1.0f;
        numSteps = choices.size();
    }
};

class AudioParameterBool : public RangedAudioParameter
{
public:
    AudioParameterBool (const ParameterID& id, const String&, bool def)
    {
        paramID = id.paramID;
        defaultValue = def ? 1.0f : 0.0f;
        rangeStart = 0.0f;
        rangeEnd = 1.0f;
        interval = 1.0f;
        numSteps = 2;
    }
};

//==============================================================================
// Just enough APVTS for EQEngine: getRawParameterValue() backed by a flat map of
// atomics. The wasm wrapper owns population (from the harvested ParameterLayout)
// and setting values from the UI thread.
class AudioProcessorValueTreeState
{
public:
    struct ParameterLayout
    {
        std::vector<std::unique_ptr<RangedAudioParameter>> params;

        ParameterLayout() = default;

        template <typename Iterator>
        ParameterLayout (Iterator first, Iterator last)
        {
            for (; first != last; ++first)
                params.push_back (std::move (*first));
        }
    };

    std::atomic<float>* getRawParameterValue (const String& paramID)
    {
        auto it = values.find (paramID.toStdString());
        return it != values.end() ? it->second.get() : &missingParam;
    }

    // Shim-only helpers used by the wasm wrapper (not part of the JUCE API).
    std::atomic<float>* addParameter (const std::string& id, float initialValue)
    {
        auto& slot = values[id];
        if (slot == nullptr)
            slot = std::make_unique<std::atomic<float>> (initialValue);
        else
            slot->store (initialValue);
        return slot.get();
    }

private:
    std::unordered_map<std::string, std::unique_ptr<std::atomic<float>>> values;
    std::atomic<float> missingParam { 0.0f };
};

} // namespace juce
