#include "controller.h"
#include "processor.h"
#include "paramids.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "public.sdk/source/vst/vstparameters.h"

using namespace Steinberg;

namespace MicFilter {
namespace MP = MicFilterParams;

tresult PLUGIN_API Controller::initialize (FUnknown* context)
{
    tresult result = EditControllerEx1::initialize (context);
    if (result != kResultOk)
        return result;

    auto addRange = [&] (int id, const char16_t* name, const char16_t* shortName, const char16_t* unit) {
        const auto& r = MP::rangeFor (id);
        parameters.addParameter (new Vst::RangeParameter (name, id, unit, r.min, r.max, r.def,
                                                          0, Vst::ParameterInfo::kCanAutomate,
                                                          Vst::kRootUnitId, shortName));
    };
    auto addSwitch = [&] (int id, const char16_t* name, int32 flags = Vst::ParameterInfo::kCanAutomate) {
        // StringListParameter ("Off"/"On") so the validator's string round-trip works.
        auto* p = new Vst::StringListParameter (name, id, nullptr, flags);
        p->appendString (STR16 ("Off"));
        p->appendString (STR16 ("On"));
        if (MP::rangeFor (id).def > 0.5f)
            p->setNormalized (1.0);
        parameters.addParameter (p);
    };

    addRange (MP::kInGain, STR16 ("Input Gain"), STR16 ("InGain"), STR16 ("dB"));
    addRange (MP::kOutGain, STR16 ("Output Gain"), STR16 ("OutGain"), STR16 ("dB"));
    addSwitch (MP::kHpfOn, STR16 ("HPF On"));
    addRange (MP::kHpfFreq, STR16 ("HPF Freq"), STR16 ("HPF"), STR16 ("Hz"));
    addSwitch (MP::kGateOn, STR16 ("Gate On"));
    addRange (MP::kGateThresh, STR16 ("Gate Threshold"), STR16 ("Gate"), STR16 ("dB"));
    addSwitch (MP::kEqOn, STR16 ("EQ On"));
    addRange (MP::kEqLow, STR16 ("EQ Low"), STR16 ("Low"), STR16 ("dB"));
    addRange (MP::kEqMid, STR16 ("EQ Mid"), STR16 ("Mid"), STR16 ("dB"));
    addRange (MP::kEqHigh, STR16 ("EQ High"), STR16 ("High"), STR16 ("dB"));
    addSwitch (MP::kCompOn, STR16 ("Comp On"));
    addRange (MP::kCompThresh, STR16 ("Comp Threshold"), STR16 ("Comp"), STR16 ("dB"));
    addRange (MP::kCompRatio, STR16 ("Comp Ratio"), STR16 ("Ratio"), STR16 (""));
    addSwitch (MP::kLimiterOn, STR16 ("Limiter On"));
    addSwitch (MP::kHumOn, STR16 ("Hum Notch On"));
    addRange (MP::kHumBase, STR16 ("Hum Base"), STR16 ("HumHz"), STR16 ("Hz"));
    addSwitch (MP::kDeclipOn, STR16 ("DeClip On"));
    addSwitch (MP::kDeclickOn, STR16 ("DeClick On"));
    addRange (MP::kDeclickThresh, STR16 ("DeClick Threshold"), STR16 ("DClick"), STR16 (""));
    addSwitch (MP::kMultibandOn, STR16 ("Multiband Expander On"));
    addSwitch (MP::kDeesserOn, STR16 ("DeEsser On"));
    addRange (MP::kDeesserThresh, STR16 ("DeEsser Threshold"), STR16 ("DeEss"), STR16 ("dB"));
    addSwitch (MP::kExciterOn, STR16 ("Exciter On"));
    addRange (MP::kExciterMix, STR16 ("Exciter Mix"), STR16 ("Excite"), STR16 (""));
    addSwitch (MP::kSpectralOn, STR16 ("Hiss Removal On"));
    addRange (MP::kSpectralStrength, STR16 ("Hiss Removal Strength"), STR16 ("Hiss"), STR16 (""));
    addSwitch (MP::kDuckOn, STR16 ("Click Duck On"));
    addSwitch (MP::kBreathOn, STR16 ("Breath Keep On"));
    addSwitch (MP::kRnnoiseOn, STR16 ("AI Noise Removal On"));
    // Bypass: flagged so hosts wire their own bypass button to it.
    {
        auto* p = new Vst::StringListParameter (STR16 ("Bypass"), MP::kBypass, nullptr,
                                                Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsBypass);
        p->appendString (STR16 ("Off"));
        p->appendString (STR16 ("On"));
        parameters.addParameter (p);
    }
    return kResultOk;
}

tresult PLUGIN_API Controller::setComponentState (IBStream* state)
{
    if (! state)
        return kResultFalse;
    IBStreamer streamer (state, kLittleEndian);
    for (int id = 0; id < (int) MP::kNumParams; ++id)
    {
        double v = 0;
        if (! streamer.readDouble (v))
            return kResultFalse;
        if (id == MP::kBypass)
            setParamNormalized (id, v > 0.5 ? 1.0 : 0.0);
        else if (MP::isSwitch (id))
            setParamNormalized (id, v > 0.5 ? 1.0 : 0.0);
        else
            setParamNormalized (id, MP::toNormalized (id, (float) v));
    }
    return kResultOk;
}

} // namespace MicFilter
