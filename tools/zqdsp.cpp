// Audio-thread checks for the DSP, with no plugin host and no audio device.
//
// The invariant this exists for is stated in CLAUDE.md and AGENTS.md section 4:
// "Keep the audio thread lock-free and allocation-free." Nothing checked it, and
// it was not true — EQBand::update ran the whole coefficient design on every
// block, and DynamicEQDetector::process redesigned its analysis filter on every
// block, both through juce::dsp::IIR::Coefficients::make*, which heap-allocates.
//
// Counting allocations is the only way to see that from outside: the plugin
// sounds identical either way, right up to the moment the allocator's lock is
// contended and a buffer is late.
//
// Build: configured as part of the normal CMake build; run ./build/zqdsp.

#include "Source/DSP/EQBand.h"
#include "Source/DSP/DynamicEQDetector.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace
{
std::atomic<long> allocations { 0 };
std::atomic<bool> counting { false };

int checks = 0;
int failures = 0;

void check (bool condition, const char* what)
{
    ++checks;
    std::printf (condition ? "  ok   %s\n" : "  FAIL %s\n", what);
    if (! condition)
        ++failures;
}
} // namespace

// Counted, not blocked: a failure here should be a number in the report, not a
// crash in something that might legitimately allocate during setup.
void* operator new (std::size_t size)
{
    if (counting.load (std::memory_order_relaxed))
        allocations.fetch_add (1, std::memory_order_relaxed);

    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;

    throw std::bad_alloc();
}

void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
constexpr double sr = 48000.0;
constexpr int blockSize = 128;
constexpr int blocks = 400;

juce::AudioBuffer<float> makeSignal (int numChannels, int numSamples)
{
    juce::AudioBuffer<float> b (numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int n = 0; n < numSamples; ++n)
            b.setSample (ch, n, 0.25f * std::sin (juce::MathConstants<float>::twoPi * 220.0f
                                                  * (float) n / (float) sr));
    return b;
}

/// Runs a band for `blocks` blocks and returns how many allocations happened
/// while it was doing so, writing the output into `captured` for comparison.
long runBand (ZeroEQ::FilterType type, ZeroEQ::FilterCharacter character,
              ZeroEQ::FilterSlope slope, float gainDb, std::vector<float>& captured,
              bool moveFrequency)
{
    ZeroEQ::EQBand band;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) blockSize, 2 };
    band.prepare (spec);

    auto signal = makeSignal (2, blockSize);
    captured.clear();
    captured.reserve ((size_t) (blocks * blockSize));

    // One warm-up block outside the count: the first design is a real change and
    // is supposed to allocate.
    {
        auto b = signal;
        juce::dsp::AudioBlock<float> blk (b);
        juce::dsp::ProcessContextReplacing<float> ctx (blk);
        band.update (type, 1000.0f, gainDb, 1.0f, character, slope, 0.5f);
        band.process (ctx);
    }

    allocations.store (0);
    counting.store (true);

    for (int i = 0; i < blocks; ++i)
    {
        auto b = signal;
        juce::dsp::AudioBlock<float> blk (b);
        juce::dsp::ProcessContextReplacing<float> ctx (blk);

        const float freq = moveFrequency ? 1000.0f + (float) i : 1000.0f;
        band.update (type, freq, gainDb, 1.0f, character, slope, 0.5f);
        band.process (ctx);

        for (int n = 0; n < blockSize; ++n)
            captured.push_back (b.getSample (0, n));
    }

    counting.store (false);
    return allocations.load();
}

long runDetector (std::vector<float>& deltas)
{
    ZeroEQ::DynamicEQDetector detector;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) blockSize, 2 };
    detector.prepare (spec);

    ZeroEQ::DynamicEQDetector::Params params;
    params.thresholdDb = -30.0f;
    params.ratio       = 4.0f;
    params.attackMs    = 10.0f;
    params.releaseMs   = 80.0f;
    params.rangeDb     = 6.0f;

    auto signal = makeSignal (2, blockSize);
    deltas.clear();
    deltas.reserve ((size_t) blocks);   // or the harness's own vector growth is counted

    detector.process (signal, ZeroEQ::FilterType::Bell, 4000.0f, 2.0f, params);   // warm-up

    allocations.store (0);
    counting.store (true);

    for (int i = 0; i < blocks; ++i)
        deltas.push_back (detector.process (signal, ZeroEQ::FilterType::Bell, 4000.0f, 2.0f, params));

    counting.store (false);
    return allocations.load();
}
} // namespace

int main()
{
    std::printf ("\nzero-eq DSP checks\n\n== the audio thread does not allocate ==\n");

    std::vector<float> out;

    const long bell = runBand (ZeroEQ::FilterType::Bell, ZeroEQ::FilterCharacter::Modern,
                               ZeroEQ::FilterSlope::Slope12, 6.0f, out, false);
    std::printf ("  a settled Bell band: %ld allocations over %d blocks\n", bell, blocks);
    check (bell == 0, "a settled Bell band allocates nothing per block");

    const long hp = runBand (ZeroEQ::FilterType::HighPass, ZeroEQ::FilterCharacter::Modern,
                             ZeroEQ::FilterSlope::Slope48, 0.0f, out, false);
    std::printf ("  a settled 48 dB/oct High Pass: %ld allocations over %d blocks\n", hp, blocks);
    check (hp == 0, "a settled High Pass allocates nothing per block (the worst case)");

    const long tilt = runBand (ZeroEQ::FilterType::TiltShelf, ZeroEQ::FilterCharacter::Vintage,
                               ZeroEQ::FilterSlope::Slope12, -4.0f, out, false);
    std::printf ("  a settled Tilt Shelf, Vintage: %ld allocations over %d blocks\n", tilt, blocks);
    check (tilt == 0, "a settled Tilt Shelf allocates nothing per block");

    const long harmonic = runBand (ZeroEQ::FilterType::Bell, ZeroEQ::FilterCharacter::Harmonic,
                                   ZeroEQ::FilterSlope::Slope12, 8.0f, out, false);
    std::printf ("  a settled Harmonic Bell: %ld allocations over %d blocks\n", harmonic, blocks);
    check (harmonic == 0, "a settled Harmonic band allocates nothing per block");

    std::vector<float> deltas;
    const long dyn = runDetector (deltas);
    std::printf ("  a working dynamic detector: %ld allocations over %d blocks\n", dyn, blocks);
    check (dyn == 0, "the dynamic detector allocates nothing per block");

    // A band whose frequency is genuinely moving still has to be redesigned, and
    // that design allocates. Recorded rather than asserted at zero, so the number
    // is visible and a future allocation-free design would show up here.
    std::vector<float> moving;
    const long glide = runBand (ZeroEQ::FilterType::Bell, ZeroEQ::FilterCharacter::Modern,
                                ZeroEQ::FilterSlope::Slope12, 6.0f, moving, true);
    std::printf ("  a band whose frequency moves every block: %ld allocations (known: the\n"
                 "     design itself allocates, so a moving control still does)\n", glide);

    std::printf ("\n== the cache does not change the sound ==\n");

    // The whole risk of a cache is that it serves a stale answer. Same settings,
    // run twice, must be sample-identical; and a band that is moved and then
    // returned must land on exactly what it had before.
    std::vector<float> a, b;
    runBand (ZeroEQ::FilterType::Bell, ZeroEQ::FilterCharacter::Vintage,
             ZeroEQ::FilterSlope::Slope12, 5.0f, a, false);
    runBand (ZeroEQ::FilterType::Bell, ZeroEQ::FilterCharacter::Vintage,
             ZeroEQ::FilterSlope::Slope12, 5.0f, b, false);

    bool identical = a.size() == b.size();
    if (identical)
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) { identical = false; break; }

    check (identical, "two identical runs are sample-identical");

    {
        // Move the band away and back: the cache must notice both. Compared at
        // STEADY STATE — these are IIR filters, so a single block after a
        // different history differs for reasons that have nothing to do with the
        // coefficients. Fifty blocks of a fixed sine is far past settling.
        ZeroEQ::EQBand band;
        juce::dsp::ProcessSpec spec { sr, (juce::uint32) blockSize, 2 };
        band.prepare (spec);
        auto signal = makeSignal (2, blockSize);

        const auto settleAt = [&] (float gainDb)
        {
            float last = 0.0f;
            for (int i = 0; i < 50; ++i)
            {
                auto buf = signal;
                juce::dsp::AudioBlock<float> blk (buf);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                band.update (ZeroEQ::FilterType::Bell, 1000.0f, gainDb, 1.0f,
                             ZeroEQ::FilterCharacter::Modern, ZeroEQ::FilterSlope::Slope12, 0.5f);
                band.process (ctx);
                last = buf.getSample (0, blockSize - 1);
            }
            return last;
        };

        const float atSix     = settleAt (6.0f);
        const float atMinus   = settleAt (-6.0f);
        const float backAtSix = settleAt (6.0f);

        std::printf ("  settled at +6 dB: %.9f, at -6 dB: %.9f, back at +6 dB: %.9f\n",
                     atSix, atMinus, backAtSix);

        check (std::abs (atSix - atMinus) > 1.0e-4f,
               "moving the gain actually changes the output (the cache is not stuck)");
        check (std::abs (atSix - backAtSix) < 1.0e-6f,
               "a band moved away and back lands on exactly what it had before");
    }

    std::printf (failures == 0 ? "\n%d checks, 0 failures\n\n" : "\n%d checks, FAILURES\n\n", checks);
    return failures == 0 ? 0 : 1;
}
