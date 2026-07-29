#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Diag/Diag.h"

#include <mutex>

ZeroEQAudioProcessor::ZeroEQAudioProcessor()
    : AudioProcessor(BusesProperties()
                          .withInput("Input", juce::AudioChannelSet::stereo(), true)
                          .withInput("Sidechain", juce::AudioChannelSet::stereo(), false)
                          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", ZeroEQ::createParameterLayout()),
      presetManager(apvts)
{
    // Once per process, however many instances the host loads.
    //
    // `installCrashHandler = false` is the important part: this plugin lives
    // inside someone else's process. A process-wide signal handler here would
    // intercept crashes that are not ours and interfere with the host's own
    // handling — a plugin has no business deciding what happens when a DAW
    // dies. We still get the log, the ring and the diagnostics bundle.
    static std::once_flag diagOnce;
    std::call_once(diagOnce, []
    {
        cp::diag::Options options;
        options.appName = "ZeroEQ";
        options.envPrefix = "ZEROEQ";
        options.version = JucePlugin_VersionString;
        options.installCrashHandler = false;
        cp::diag::init(options);
        CP_LOG_INFO ("plugin loaded host=" + juce::String (juce::PluginHostType().getHostDescription()));
    });
}

void ZeroEQAudioProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= getNumPrograms())
        return;
    presetManager.loadFactoryPreset(index);
    currentProgramIndex = index;
}

const juce::String ZeroEQAudioProcessor::getProgramName(int index)
{
    const auto& presets = ZeroEQ::PresetManager::getFactoryPresets();
    if (index < 0 || index >= (int) presets.size())
        return {};
    return presets[(size_t) index].name;
}

void ZeroEQAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    spec.numChannels = (juce::uint32) juce::jmax(1, getTotalNumOutputChannels());

    eqEngine.prepare(spec);
    compressor.prepare(spec);
    preSpectrum.prepare(sampleRate);
    postSpectrum.prepare(sampleRate);
    inputMeter.prepare(sampleRate);
    outputMeter.prepare(sampleRate);
}

void ZeroEQAudioProcessor::releaseResources()
{
}

bool ZeroEQAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;

    if (mainIn != mainOut)
        return false;

    // Sidechain (input bus 1): disabled, mono, or stereo - never required to match
    // the main bus, and hosts are free to leave it unconnected entirely.
    const auto sidechain = layouts.getChannelSet(true, 1);
    if (! sidechain.isDisabled() && sidechain != juce::AudioChannelSet::mono()
        && sidechain != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

void ZeroEQAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // With the sidechain input bus, `buffer` may carry extra channels beyond the
    // main in/out pair (main in and main out alias the same channels in place;
    // sidechain shows up as additional read-only channels appended after them).
    // Every stage below must operate on the main bus view only, never the raw
    // `buffer`, or sidechain audio would leak into gain/metering/EQ/compression.
    auto mainBuffer = getBusBuffer(buffer, true, 0);
    const int numSamples = mainBuffer.getNumSamples();

    const int mainOutChannels = getMainBusNumOutputChannels();
    for (int ch = getMainBusNumInputChannels(); ch < mainOutChannels; ++ch)
        mainBuffer.clear(ch, 0, numSamples);

    // --- Input gain + metering ---
    const float inputGainDb = apvts.getRawParameterValue(ZeroEQ::ParamIDs::inputGain)->load();
    mainBuffer.applyGain(juce::Decibels::decibelsToGain(inputGainDb));
    inputMeter.process(mainBuffer);

    preSpectrum.pushBuffer(mainBuffer);

    // Sidechain view: 0 channels whenever the host has the bus disabled or
    // unconnected. EQEngine falls back to internal (main-buffer) detection per
    // band whenever a band asks for sidechain but this is empty.
    auto sidechainBuffer = getBusBuffer(buffer, true, 1);

    // --- EQ ---
    const bool eqActive = apvts.getRawParameterValue(ZeroEQ::ParamIDs::eqActive)->load() > 0.5f;
    if (eqActive)
        eqEngine.updateAndProcess(mainBuffer, apvts, sidechainBuffer);

    // --- Compressor ---
    const bool compActive = apvts.getRawParameterValue(ZeroEQ::ParamIDs::compActive)->load() > 0.5f;
    if (compActive)
    {
        ZeroEQ::Compressor::Params p;
        p.thresholdDb = apvts.getRawParameterValue(ZeroEQ::ParamIDs::compThreshold)->load();
        p.ratio       = apvts.getRawParameterValue(ZeroEQ::ParamIDs::compRatio)->load();
        p.attackMs    = apvts.getRawParameterValue(ZeroEQ::ParamIDs::compAttack)->load();
        p.releaseMs   = apvts.getRawParameterValue(ZeroEQ::ParamIDs::compRelease)->load();
        p.kneeDb      = apvts.getRawParameterValue(ZeroEQ::ParamIDs::compKnee)->load();
        p.makeupDb    = apvts.getRawParameterValue(ZeroEQ::ParamIDs::compMakeup)->load();
        p.autoMakeup  = apvts.getRawParameterValue(ZeroEQ::ParamIDs::compAutoMakeup)->load() > 0.5f;
        p.detector    = (ZeroEQ::DetectorType) (int) apvts.getRawParameterValue(ZeroEQ::ParamIDs::compDetector)->load();
        compressor.process(mainBuffer, p);
    }
    else
    {
        compressor.reset();
    }

    // --- Output gain + metering ---
    const float outputGainDb = apvts.getRawParameterValue(ZeroEQ::ParamIDs::outputGain)->load();
    mainBuffer.applyGain(juce::Decibels::decibelsToGain(outputGainDb));
    outputMeter.process(mainBuffer);

    postSpectrum.pushBuffer(mainBuffer);
}

juce::AudioProcessorEditor* ZeroEQAudioProcessor::createEditor()
{
    return new ZeroEQAudioProcessorEditor(*this);
}

void ZeroEQAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void ZeroEQAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

// This creates the platform-specific instance of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ZeroEQAudioProcessor();
}
