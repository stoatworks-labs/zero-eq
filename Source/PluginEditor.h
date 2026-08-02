#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "GUI/ZeroEQLookAndFeel.h"
#include "GUI/EQCurveComponent.h"
#include "GUI/BandControlPanel.h"
#include "GUI/CompressorPanel.h"
#include "GUI/IOPanel.h"
#include "GUI/PresetBar.h"
#include "StoatworksAboutPanel.h"

class ZeroEQAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit ZeroEQAudioProcessorEditor(ZeroEQAudioProcessor&);
    ~ZeroEQAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    ZeroEQAudioProcessor& audioProcessor;

    ZeroEQ::ZeroEQLookAndFeel lookAndFeel;

    ZeroEQ::PresetBar presetBar;
    ZeroEQ::IOPanel ioPanel;
    ZeroEQ::EQCurveComponent eqCurve;
    ZeroEQ::BandControlPanel bandControlPanel;
    ZeroEQ::CompressorPanel compressorPanel;

    /* Vendored from stoatworks-backend/about - see StoatworksAboutPanel.h.
       A child of the editor rather than a window of its own: a plugin must not
       put a second top-level window on a host's screen. */
    juce::TextButton aboutButton { "i" };
    stoatworks::AboutPanel aboutPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroEQAudioProcessorEditor)
};
