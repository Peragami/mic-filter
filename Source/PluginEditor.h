#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// One editor shared by the VST3 and the standalone client.
// In the standalone client, input/output device choice is the wrapper's
// "Options" button (top-left) -> Audio device selector. That is intentional:
// JUCE's selector already handles ASIO/WASAPI device, sample rate and buffer.
class MicFilterEditor : public juce::AudioProcessorEditor
{
public:
    explicit MicFilterEditor (MicFilterProcessor&);
    ~MicFilterEditor() override = default;
    void paint (juce::Graphics&) override;
    void resized() override;
private:
    struct Row { juce::Slider slider; juce::Label label; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att; };
    void addRow (Row& r, const char* id, const char* name);
    MicFilterProcessor& proc;
    Row inGain, outGain, hpfFreq, gateThresh, eqLow, eqMid, eqHigh, compThresh, compRatio;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicFilterEditor)
};
