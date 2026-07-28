#include "HarmonicShaper.h"

namespace ZeroEQ
{

// Clears the one-sample memories so a fresh start (or a transport stop) can't leak the
// tail of the previous audio into the next block as a click.
void HarmonicShaper::reset()
{
    xPrevDriven = 0.0f;
    dcBlockerXPrev = 0.0f;
    dcBlockerYPrev = 0.0f;
}

// The even-harmonic shaping curve. Adding a squared term treats positive and negative
// halves of the waveform differently (squaring makes both halves positive), and that
// lopsidedness is what produces even harmonics - the "tube-like warmth" of the pair.
float HarmonicShaper::evenFunction(float x)
{
    return x + evenCurve * x * x;
}

// The exact integral of evenFunction. ADAA (see below) works with the area under the
// shaping curve rather than the curve itself, so each shaper has to supply its own
// antiderivative; these are worked out on paper, not approximated.
float HarmonicShaper::evenAntiderivative(float x)
{
    return x * x * 0.5f + evenCurve * x * x * x / 3.0f;
}

// The odd-harmonic shaping curve. tanh is a symmetric S-shape that squashes loud peaks
// identically whichever way they point, and that symmetry yields odd harmonics - the
// "transistor-like grit" of the pair.
float HarmonicShaper::oddFunction(float x)
{
    return std::tanh(x);
}

// The exact integral of tanh, which happens to be log(cosh(x)).
float HarmonicShaper::oddAntiderivative(float x)
{
    return std::log(std::cosh(x));
}

// The anti-aliasing trick that lets this run without oversampling.
//
// Distorting a signal invents new frequencies. Any that land above half the sample rate
// can't be represented, and fold back down as inharmonic "aliasing" whine. The usual fix
// is to oversample - run at a multiple of the sample rate - but that costs latency, which
// this plugin doesn't have to spend.
//
// Instead: rather than asking "what does the curve output at this instant", ask "what is
// the curve's *average* output as the input travelled from the previous sample to this
// one". That average is the area under the curve across the step divided by the width of
// the step, which is exactly (F(x) - F(xPrev)) / dx. Averaging over the step smooths off
// the sharp corners that generate the worst aliasing.
//
// The guard matters: when two consecutive samples are nearly identical, dx approaches
// zero and the division becomes 0/0 - numerically explosive. In that case the step is so
// small that simply evaluating the curve at the midpoint is both accurate and stable.
float HarmonicShaper::adaaStep(float x, float xPrev, float (*F)(float), float (*f)(float))
{
    const float dx = x - xPrev;
    if (std::abs(dx) < 1.0e-5f)
        return f(0.5f * (x + xPrev));
    return (F(x) - F(xPrev)) / dx;
}

// Runs one audio sample through the harmonic stage.
//
// The shape of the whole operation: push the sample harder into a deliberately imperfect
// curve so the curve distorts it, undo the level change that pushing introduced, remove
// the DC offset the even curve leaves behind, then mix that coloured version back against
// the untouched original.
float HarmonicShaper::processSample(float x, float driveAmount, float blend)
{
    // Fully backed off: pass the audio through untouched. The previous-sample memory is
    // still updated, so re-engaging the drive later doesn't jump from a stale value.
    if (driveAmount <= 0.0005f)
    {
        xPrevDriven = x;
        return x;
    }

    // Drive the signal into the curve. A louder input reaches the bending part of the
    // curve, so more drive means more harmonics rather than simply more volume.
    const float driveGain = 1.0f + driveAmount * driveScale;
    // Clamp before shaping: the even curve is a parabola and grows without limit, so an
    // extreme input could otherwise produce a huge or non-finite value.
    const float xd = juce::jlimit(-8.0f, 8.0f, x * driveGain);

    // Run both flavours of harmonics for this sample, each anti-aliased.
    const float evenOut = adaaStep(xd, xPrevDriven, evenAntiderivative, evenFunction);
    const float oddOut  = adaaStep(xd, xPrevDriven, oddAntiderivative, oddFunction);
    xPrevDriven = xd;

    // Crossfade between the two characters, then divide the drive back out so turning
    // drive up changes the tone rather than just making everything louder.
    const float wet = (1.0f - blend) * evenOut + blend * oddOut;
    const float wetNormalized = wet / driveGain;

    // One-pole DC blocker: the even (asymmetric) shaper introduces a DC offset that
    // needs removing; harmless no-op for the odd (symmetric) component.
    const float dcOut = wetNormalized - dcBlockerXPrev + dcBlockR * dcBlockerYPrev;
    dcBlockerXPrev = wetNormalized;
    dcBlockerYPrev = dcOut;

    // Final dry/wet mix, using the same control that set the drive: backing the control
    // off both softens the distortion and pulls the coloured signal back down in the mix.
    return (1.0f - driveAmount) * x + driveAmount * dcOut;
}

} // namespace ZeroEQ
