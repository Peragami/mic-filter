#include "processor.h"
#include "paramids.h"
#include "Source/ParamBridge.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <cstring>

using namespace Steinberg;

namespace MicFilter {

Processor::Processor ()
{
    setControllerClass (kControllerUID);
    // Defaults in *plain* units; switches stored as 0/1 plain as well.
    for (int id = 0; id < (int) MicFilterParams::kNumParams; ++id)
        values_[id] = MicFilterParams::rangeFor (id).def;
}

tresult PLUGIN_API Processor::initialize (FUnknown* context)
{
    tresult result = AudioEffect::initialize (context);
    if (result != kResultOk)
        return result;
    addAudioInput (STR16 ("Stereo In"), Vst::BusTypes::kMain, Vst::SpeakerArr::kStereo);
    addAudioOutput (STR16 ("Stereo Out"), Vst::BusTypes::kMain, Vst::SpeakerArr::kStereo);
    return kResultOk;
}

tresult PLUGIN_API Processor::setBusArrangements (Vst::SpeakerArrangement* inputs, int32 numIns,
                                                  Vst::SpeakerArrangement* outputs, int32 numOuts)
{
    // Mic use case: accept mono or stereo, but in and out must match in count.
    if (numIns != 1 || numOuts != 1)
        return kResultFalse;
    auto ok = [] (Vst::SpeakerArrangement a) {
        return a == Vst::SpeakerArr::kMono || a == Vst::SpeakerArr::kStereo;
    };
    if (! ok (inputs[0]) || ! ok (outputs[0]) || inputs[0] != outputs[0])
        return kResultFalse;
    return AudioEffect::setBusArrangements (inputs, numIns, outputs, numOuts);
}

tresult PLUGIN_API Processor::canProcessSampleSize (int32 symbolicSampleSize)
{
    return symbolicSampleSize == Vst::kSample32 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API Processor::setupProcessing (Vst::ProcessSetup& setup)
{
    chain_.prepare (setup.sampleRate);
    paramsDirty_ = true;
    return AudioEffect::setupProcessing (setup);
}

void Processor::syncChainParams ()
{
    chain_.setParams (MicFilterBridge::paramsFromPlainValues (values_));
    paramsDirty_ = false;
}

tresult PLUGIN_API Processor::process (Vst::ProcessData& data)
{
    // --- parameter changes -------------------------------------------------
    if (data.inputParameterChanges)
    {
        int32 numParams = data.inputParameterChanges->getParameterCount ();
        for (int32 i = 0; i < numParams; ++i)
        {
            auto* queue = data.inputParameterChanges->getParameterData (i);
            if (! queue)
                continue;
            Vst::ParamValue value;
            int32 sampleOffset;
            int32 numPoints = queue->getPointCount ();
            int32 id = (int32) queue->getParameterId ();
            if (id >= 0 && id < (int32) MicFilterParams::kNumParams
                && queue->getPoint (numPoints - 1, sampleOffset, value) == kResultTrue)
            {
                values_[id] = MicFilterParams::isSwitch (id)
                                  ? (value > 0.5 ? 1.0 : 0.0)
                                  : MicFilterParams::toPlain (id, (float) value);
                paramsDirty_ = true;
            }
        }
    }

    if (data.numInputs == 0 || data.numOutputs == 0 || data.numSamples == 0)
        return kResultOk;
    if (data.symbolicSampleSize != Vst::kSample32)
        return kResultOk; // canProcessSampleSize() only advertises float32

    auto& in = data.inputs[0];
    auto& out = data.outputs[0];
    const int32 inCh = in.numChannels;
    const int32 outCh = out.numChannels;
    const int32 n = data.numSamples;

    // Some hosts/tests hand us null channel buffers: never dereference blindly.
    if (! in.channelBuffers32 || ! out.channelBuffers32 || inCh == 0 || outCh == 0)
        return kResultOk;
    float* const outBuf = out.channelBuffers32[0];
    const float* const inBuf = in.channelBuffers32[0];
    if (! outBuf)
        return kResultOk;
    auto zeroOutputs = [&] {
        for (int32 ch = 0; ch < outCh; ++ch)
            if (out.channelBuffers32[ch])
                std::memset (out.channelBuffers32[ch], 0, sizeof (float) * (size_t) n);
    };

    // Silent (or missing) input: stay silent (and mark it), nothing to filter.
    const uint64 allSilent = (inCh >= 2) ? 0x3 : 0x1;
    if (! inBuf || (in.silenceFlags & allSilent) == allSilent)
    {
        zeroOutputs ();
        out.silenceFlags = allSilent;
        return kResultOk;
    }

    // Copy mic source (ch0) to out ch0, process in place, mirror to other out channels.
    if (outBuf != inBuf)
        std::memcpy (outBuf, inBuf, sizeof (float) * (size_t) n);

    const bool bypass = MicFilterBridge::onOf (values_, MicFilterParams::kBypass);
    if (! bypass)
    {
        if (paramsDirty_)
            syncChainParams ();
        chain_.processBlock (outBuf, n);
    }
    for (int32 ch = 1; ch < outCh; ++ch)
        if (out.channelBuffers32[ch])
            std::memcpy (out.channelBuffers32[ch], outBuf, sizeof (float) * (size_t) n);
    out.silenceFlags = 0;
    return kResultOk;
}

tresult PLUGIN_API Processor::getState (IBStream* state)
{
    if (! state)
        return kResultFalse;
    IBStreamer streamer (state, kLittleEndian);
    for (int id = 0; id < (int) MicFilterParams::kNumParams; ++id)
        if (! streamer.writeDouble (values_[id]))
            return kResultFalse;
    return kResultOk;
}

tresult PLUGIN_API Processor::setState (IBStream* state)
{
    if (! state)
        return kResultFalse;
    IBStreamer streamer (state, kLittleEndian);
    for (int id = 0; id < (int) MicFilterParams::kNumParams; ++id)
    {
        double v = 0;
        if (! streamer.readDouble (v))
            return kResultFalse;
        values_[id] = v;
    }
    paramsDirty_ = true;
    return kResultOk;
}

} // namespace MicFilter
