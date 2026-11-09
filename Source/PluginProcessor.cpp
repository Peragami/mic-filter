#include "PluginProcessor.h"
#include "PluginEditor.h"

MicFilterProcessor::MicFilterProcessor()
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", makeLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout MicFilterProcessor::makeLayout()
{
    using P = juce::AudioParameterFloat; using B = juce::AudioParameterBool;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto addF = [&] (auto id, auto name, float lo, float hi, float def) {
        p.push_back (std::make_unique<P> (id, name, juce::NormalisableRange<float> (lo, hi, 0.1f), def)); };
    auto addB = [&] (auto id, auto name, bool def) { p.push_back (std::make_unique<B> (id, name, def)); };

    addF ("inGain",  "Input Gain",  -24.0f, 24.0f, 0.0f);
    addF ("outGain", "Output Gain", -24.0f, 24.0f, 0.0f);
    addB ("hpfOn", "HPF On", true);          addF ("hpfFreq", "HPF Freq", 40.0f, 200.0f, 80.0f);
    addB ("gateOn", "Gate On", true);        addF ("gateThresh", "Gate Thresh", -80.0f, -10.0f, -50.0f);
    addB ("eqOn", "EQ On", true);
    addF ("eqLow", "EQ Low", -12.0f, 12.0f, 0.0f);
    addF ("eqMid", "EQ Mid", -12.0f, 12.0f, 0.0f);
    addF ("eqHigh","EQ High",-12.0f, 12.0f, 0.0f);
    addB ("compOn", "Comp On", true);
    addF ("compThresh", "Comp Thresh", -40.0f, 0.0f, -18.0f);
    addF ("compRatio", "Comp Ratio", 1.0f, 10.0f, 3.0f);
    addB ("limiterOn", "Limiter On", true);
    return { p.begin(), p.end() };
}

bool MicFilterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
        || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void MicFilterProcessor::prepareToPlay (double sr, int samplesPerBlock)
{
    chain.prepare (sr, samplesPerBlock);
    chain.setParams (readParams());
}

DspChain::Params MicFilterProcessor::readParams() const
{
    auto getF = [&] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    auto getB = [&] (const char* id) { return getF (id) > 0.5f; };
    DspChain::Params p;
    p.inputGainDb = getF ("inGain");  p.outputGainDb = getF ("outGain");
    p.hpfOn = getB ("hpfOn");         p.hpfFreq = getF ("hpfFreq");
    p.gateOn = getB ("gateOn");       p.gateThreshDb = getF ("gateThresh");
    p.eqOn = getB ("eqOn");
    p.eqLowDb = getF ("eqLow"); p.eqMidDb = getF ("eqMid"); p.eqHighDb = getF ("eqHigh");
    p.compOn = getB ("compOn");       p.compThreshDb = getF ("compThresh"); p.compRatio = getF ("compRatio");
    p.limiterOn = getB ("limiterOn");
    return p;
}

void MicFilterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    chain.setParams (readParams());
    chain.process (buffer);
}

juce::AudioProcessorEditor* MicFilterProcessor::createEditor() { return new MicFilterEditor (*this); }

void MicFilterProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}
void MicFilterProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size)) apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MicFilterProcessor(); }
