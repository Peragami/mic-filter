#include "PluginEditor.h"

MicFilterEditor::MicFilterEditor (MicFilterProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    addRow (inGain,     "inGain",     "Input");
    addRow (outGain,    "outGain",    "Output");
    addRow (hpfFreq,    "hpfFreq",    "HPF Hz");
    addRow (gateThresh, "gateThresh", "Gate dB");
    addRow (eqLow,      "eqLow",      "EQ Low");
    addRow (eqMid,      "eqMid",      "EQ Mid");
    addRow (eqHigh,     "eqHigh",     "EQ High");
    addRow (compThresh, "compThresh", "Comp dB");
    addRow (compRatio,  "compRatio",  "Ratio");
    setSize (520, 460);
}

void MicFilterEditor::addRow (Row& r, const char* id, const char* name)
{
    r.label.setText (name, juce::dontSendNotification);
    addAndMakeVisible (r.label);
    r.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    r.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
    addAndMakeVisible (r.slider);
    r.att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, r.slider);
}

void MicFilterEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1c1f24));
    g.setColour (juce::Colours::white);
    g.setFont (18.0f);
    g.drawText ("MicFilter  -  in standalone: Options (top-left) = input / output device",
                getLocalBounds().removeFromTop (36), juce::Justification::centred, true);
}

void MicFilterEditor::resized()
{
    auto area = getLocalBounds().withTrimmedTop (40);
    auto* rows[] = { &inGain, &outGain, &hpfFreq, &gateThresh, &eqLow, &eqMid, &eqHigh, &compThresh, &compRatio };
    const int cols = 3, cellW = area.getWidth() / cols, cellH = area.getHeight() / 3;
    int i = 0;
    for (auto* r : rows)
    {
        auto cell = juce::Rectangle<int> ((i % cols) * cellW, area.getY() + (i / cols) * cellH, cellW, cellH);
        r->label.setBounds (cell.removeFromTop (20));
        r->slider.setBounds (cell.reduced (8));
        ++i;
    }
}
