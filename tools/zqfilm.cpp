// zqfilm — renders the Zero EQ editor to a PNG sequence for the project video.
//
// Deliberately not a screen recording, for the same reason mixerreturn's mrshot is not:
// driving a window server to photograph a window is unreliable (blank frames, and captures
// of whatever happened to be in front), and it makes the take depend on a quiet machine for
// the whole of its length. This instead instantiates the real ZeroEQAudioProcessor, streams
// real audio through it, and renders the real editor component offscreen, one frame at a
// time.
//
// Everything on screen is therefore genuinely the plugin: the curve is the filter response
// the DSP computed, the analyser is the spectrum of the audio that actually went through it,
// and the gain-reduction meters are what the compressor actually did. Nothing is drawn for
// the camera.
//
// The editor is set to 1600x900 — inside its own resize limits (950..1700 x 830..1230) and
// exactly 16:9 — and snapshotted at 1.2x, which lands on 1920x1080 with no scaling and no
// letterbox.
//
// The frame loop is: apply this frame's automation, process exactly one frame's worth of
// audio, pump the message loop so the editor's timers repaint against that new state, then
// snapshot. Doing those in any other order photographs the previous frame's meters.
//
//   zqfilm --script cues.txt --audio mix.wav --out frames/ [--fps 30] [--frames 1260]
//
// Automation script format, shared with porthole's phtest so the two can be read the same
// way — except that values here are in REAL UNITS (Hz, dB, ratio), not 0..1, because a
// script full of normalised positions is unreadable and unreviewable:
//
//     0     band3_freq   1000
//     120   band3_gain   6.0

#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
constexpr int blockSize = 256;

using Track = std::vector<std::pair<int, float>>;

/** Frame-keyed automation. Held before the first key and after the last, linear between. */
std::map<std::string, Track> loadScript (const juce::File& file, juce::String& error)
{
    std::map<std::string, Track> tracks;

    if (! file.existsAsFile())
    {
        error = "cannot open " + file.getFullPathName();
        return tracks;
    }

    juce::StringArray lines;
    file.readLines (lines);

    for (int n = 0; n < lines.size(); ++n)
    {
        auto line = lines[n].upToFirstOccurrenceOf ("#", false, false).trim();

        if (line.isEmpty())
            continue;

        auto tokens = juce::StringArray::fromTokens (line, " \t", "");
        tokens.removeEmptyStrings();

        if (tokens.size() < 3)
        {
            error = file.getFileName() + ":" + juce::String (n + 1)
                  + ": expected `frame parameter value`";
            return {};
        }

        tracks[tokens[1].toStdString()].emplace_back (tokens[0].getIntValue(),
                                                      tokens[2].getFloatValue());
    }

    for (auto& entry : tracks)
        std::sort (entry.second.begin(), entry.second.end());

    return tracks;
}

/** @param stepped  hold the previous key instead of interpolating towards the next.

    Discrete parameters — a filter type, a character, an on/off — must step, and this is
    not a nicety. Interpolating them silently invents states the script never asked for:
    a Character track keyed Modern at frame 420 and Harmonic at 660 spends the frames
    between them ramping 0 -> 2, which rounds to Vintage across the middle of the beat.
    The take then shows a mode the caption is not talking about, and it looks entirely
    plausible while doing it. Asking scripts to add holding keys everywhere would work
    until the first time someone forgot.
*/
float valueAt (const Track& track, int frame, bool stepped)
{
    if (track.empty())
        return 0.0f;
    if (frame <= track.front().first)
        return track.front().second;
    if (frame >= track.back().first)
        return track.back().second;

    for (size_t i = 1; i < track.size(); ++i)
    {
        if (frame <= track[i].first)
        {
            const auto& a = track[i - 1];
            const auto& b = track[i];

            if (stepped)
                return frame >= b.first ? b.second : a.second;

            const auto span = (float) (b.first - a.first);
            const auto t = span > 0.0f ? ((float) (frame - a.first) / span) : 1.0f;
            return a.second + (b.second - a.second) * t;
        }
    }

    return track.back().second;
}

bool writePng (const juce::Image& image, const juce::File& target)
{
    target.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (target.createOutputStream());

    if (stream == nullptr)
        return false;

    juce::PNGImageFormat png;
    return png.writeImageToStream (image, *stream);
}

juce::String argValue (int argc, char** argv, const juce::String& flag,
                       const juce::String& fallback = {})
{
    for (int i = 1; i + 1 < argc; ++i)
        if (flag == argv[i])
            return juce::String (argv[i + 1]);

    return fallback;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File scriptFile (argValue (argc, argv, "--script"));
    const juce::File audioFile (argValue (argc, argv, "--audio"));
    const juce::File outDir (argValue (argc, argv, "--out"));
    const int fps = argValue (argc, argv, "--fps", "30").getIntValue();
    const int totalFrames = argValue (argc, argv, "--frames", "0").getIntValue();

    if (outDir.getFullPathName().isEmpty() || ! audioFile.existsAsFile())
    {
        std::fprintf (stderr, "usage: zqfilm --script cues.txt --audio mix.wav --out dir "
                              "[--fps 30] [--frames N]\n");
        return 2;
    }

    outDir.createDirectory();

    // ---- the audio that will drive the analyser and the meters -------------------------
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (audioFile));

    if (reader == nullptr)
    {
        std::fprintf (stderr, "zqfilm: cannot read %s\n",
                      audioFile.getFullPathName().toRawUTF8());
        return 1;
    }

    const auto sampleRate = reader->sampleRate;
    const auto sourceLength = (int) reader->lengthInSamples;

    juce::AudioBuffer<float> source (2, sourceLength);
    reader->read (&source, 0, sourceLength, 0, true, true);

    // ---- the plugin, exactly as a host would build it -----------------------------------
    ZeroEQAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    juce::String error;
    auto tracks = loadScript (scriptFile, error);

    if (error.isNotEmpty())
    {
        std::fprintf (stderr, "zqfilm: %s\n", error.toRawUTF8());
        return 2;
    }

    // Fail on a name that does not exist rather than silently animating nothing for the
    // length of the reel — the failure mode this is guarding against is a video that looks
    // fine and shows the wrong thing.
    for (const auto& entry : tracks)
    {
        if (processor.apvts.getParameter (entry.first) == nullptr)
        {
            std::fprintf (stderr, "zqfilm: script names '%s', which is not a parameter\n",
                          entry.first.c_str());
            return 2;
        }
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

    if (editor == nullptr)
    {
        std::fprintf (stderr, "zqfilm: the processor returned no editor\n");
        return 1;
    }

    // 1600x900 is inside the editor's own resize limits and exactly 16:9, so the 1.2x
    // snapshot below is 1920x1080 with nothing scaled or padded.
    editor->setSize (1600, 900);
    editor->setBounds (0, 0, 1600, 900);

    const int samplesPerFrame = (int) (sampleRate / (double) fps);
    const int frames = totalFrames > 0 ? totalFrames : (sourceLength / samplesPerFrame);

    juce::AudioBuffer<float> block (2, blockSize);
    juce::MidiBuffer midi;
    int readPos = 0;

    // Let the editor settle before the first frame, so frame 0 is not the initial state of
    // every meter.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);

    for (int frame = 0; frame < frames; ++frame)
    {
        for (const auto& entry : tracks)
        {
            if (auto* p = processor.apvts.getParameter (entry.first))
            {
                const auto stepped = p->isDiscrete() || p->isBoolean();
                p->setValueNotifyingHost (p->convertTo0to1 (valueAt (entry.second, frame, stepped)));
            }
        }

        for (int done = 0; done < samplesPerFrame;)
        {
            const int n = std::min (blockSize, samplesPerFrame - done);
            block.setSize (2, n, false, false, true);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                    block.setSample (ch, i, source.getSample (ch % source.getNumChannels(),
                                                              (readPos + i) % sourceLength));

            midi.clear();
            processor.processBlock (block, midi);

            readPos = (readPos + n) % sourceLength;
            done += n;
        }

        // The timers repaint against the state the audio just produced. Without this the
        // snapshot is of the previous frame's analyser.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1000 / fps);

        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.2f);

        if (! writePng (image, outDir.getChildFile (juce::String::formatted ("f%05d.png", frame))))
        {
            std::fprintf (stderr, "zqfilm: could not write frame %d\n", frame);
            return 1;
        }

        if (frame % 30 == 0)
        {
            std::printf ("\r  %d / %d frames", frame, frames);
            std::fflush (stdout);
        }
    }

    std::printf ("\r  %d / %d frames\n", frames, frames);
    return 0;
}
