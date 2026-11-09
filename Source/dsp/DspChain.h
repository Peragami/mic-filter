#pragma once
#include <JuceHeader.h>

// Mic filter chain, in order:
//   InputGain -> HighPass -> NoiseGate -> 3-band EQ -> Compressor -> Limiter -> OutputGain
// All state is per-channel where it matters (gate/comp use a mono detector on ch 0,
// which is right for a mic: usually 1 real channel, the other is a copy/silent).
class DspChain
{
public:
    struct Params
    {
        float inputGainDb  = 0.0f;
        float outputGainDb = 0.0f;
        bool  hpfOn = true;   float hpfFreq = 80.0f;
        bool  gateOn = true;  float gateThreshDb = -50.0f; // below this, duck
        bool  eqOn = true;    float eqLowDb = 0.0f, eqMidDb = 0.0f, eqHighDb = 0.0f;
        bool  compOn = true;  float compThreshDb = -18.0f; float compRatio = 3.0f;
        bool  limiterOn = true;
    };

    void prepare (double sr, int maxBlock)
    {
        sampleRate = sr;
        juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxBlock, 2 };
        hpf.prepare (spec);  lowEq.prepare (spec); midEq.prepare (spec); highEq.prepare (spec);
        comp.prepare (spec);
        comp.setAttack (10.0f); comp.setRelease (120.0f); comp.setThreshold (-18.0f); comp.setRatio (3.0f);
        limiter.prepare (spec);
        limiter.setThreshold (-1.0f); limiter.setRelease (100.0f);
        updateFilters();
        gateEnv = 0.0f; gateGain = 1.0f;
    }

    void setParams (const Params& p) { params = p; updateFilters(); }

    void process (juce::AudioBuffer<float>& buffer)
    {
        const int nCh = buffer.getNumChannels();
        const int n   = buffer.getNumSamples();
        buffer.applyGain (0, n, juce::Decibels::decibelsToGain (params.inputGainDb));

        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        if (params.hpfOn)  hpf.process (ctx);
        if (params.eqOn)   { lowEq.process (ctx); midEq.process (ctx); highEq.process (ctx); }

        if (params.gateOn) applyGate (buffer);
        if (params.compOn) { comp.setThreshold (params.compThreshDb); comp.setRatio (params.compRatio); comp.process (ctx); }
        if (params.limiterOn) limiter.process (ctx);

        buffer.applyGain (0, n, juce::Decibels::decibelsToGain (params.outputGainDb));
        juce::ignoreUnused (nCh);
    }

    float getLevel() const { return level.load(); }

private:
    void updateFilters()
    {
        if (sampleRate <= 0) return;
        *hpf.state   = *juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, params.hpfFreq);
        *lowEq.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sampleRate, 200.0f, 0.7f, juce::Decibels::decibelsToGain (params.eqLowDb));
        *midEq.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(sampleRate, 1000.0f, 0.9f, juce::Decibels::decibelsToGain (params.eqMidDb));
        *highEq.state= *juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, 4000.0f, 0.7f, juce::Decibels::decibelsToGain (params.eqHighDb));
    }

    void applyGate (juce::AudioBuffer<float>& buffer)
    {
        // Simple expander-style gate: smooth gain down to -24 dB when below threshold.
        const float thresh = juce::Decibels::decibelsToGain (params.gateThreshDb);
        const float attackCoef  = 0.5f;    // open fast
        const float releaseCoef = 0.002f;  // close slowly (per sample @48k ~ smooth)
        const float floorGain = 0.06f;     // -24 dB, not full mute (avoids chopping word starts)
        auto* ch0 = buffer.getReadPointer (0);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            gateEnv = juce::jmax (std::abs (ch0[i]), gateEnv * 0.9995f);
            const float target = (gateEnv >= thresh) ? 1.0f : floorGain;
            gateGain += (target - gateGain) * (target > gateGain ? attackCoef : releaseCoef);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.getWritePointer (ch)[i] *= gateGain;
            level.store (gateEnv);
        }
    }

    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    Filter hpf, lowEq, midEq, highEq;
    juce::dsp::Compressor<float> comp;
    juce::dsp::Limiter<float> limiter;
    Params params; double sampleRate = 48000.0;
    float gateEnv = 0.0f, gateGain = 1.0f;
    std::atomic<float> level { 0.0f };
};
