#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "paramids.h"
#include "Source/dsp/Effects2.h" // ChainV2 (includes PureDsp.h); include dir = mic-filter/

namespace MicFilter {

// Fixed class IDs (generated once, do not change: hosts identify the plugin by these).
// Processor : {190DD1F9-C10A4B07-95D6805C-70B33BBB}
// Controller: {3FD2EF38-07944434-B303F5A6-B1C4E1F1}
inline const Steinberg::FUID kProcessorUID (0x190DD1F9, 0xC10A4B07, 0x95D6805C, 0x70B33BBB);
inline const Steinberg::FUID kControllerUID (0x3FD2EF38, 0x07944434, 0xB303F5A6, 0xB1C4E1F1);

class Processor : public Steinberg::Vst::AudioEffect
{
public:
    Processor ();
    ~Processor () override = default;

    static Steinberg::FUnknown* createInstance (void*) { return (Steinberg::Vst::IAudioProcessor*) new Processor; }

    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API setBusArrangements (Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                                       Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;
    Steinberg::tresult PLUGIN_API setupProcessing (Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) override;
    Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) override;

    // ChainV2 latency = declick lookahead (128) + declip delay (64, only when On).
    Steinberg::uint32 PLUGIN_API getLatencySamples () override { return (Steinberg::uint32) chain_.latencySamples (); }
    Steinberg::uint32 PLUGIN_API getTailSamples () override { return Steinberg::Vst::kNoTail; }

private:
    void syncChainParams ();
    // Plain (real-world) values, indexed by MicFilterParams::ParamID.
    double values_[MicFilterParams::kNumParams] {};
    bool paramsDirty_ = true;
    pure2::ChainV2 chain_;
};

} // namespace MicFilter
