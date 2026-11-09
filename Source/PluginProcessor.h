#pragma once
#include <JuceHeader.h>
#include "dsp/DspChain.h"

class MicFilterProcessor : public juce::AudioProcessor
{
public:
    MicFilterProcessor();
    ~MicFilterProcessor() override = default;

    void prepareToPlay (double sr, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MicFilter"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int size) override;

    juce::AudioProcessorValueTreeState apvts;
    float getInputLevel() const { return chain.getLevel(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    DspChain::Params readParams() const;
    DspChain chain;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicFilterProcessor)
};
