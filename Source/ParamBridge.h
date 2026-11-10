// ParamBridge.h -- single source of truth for mapping MicFilterParams
// plain values (indexed by ParamID) onto the DSP chain parameters.
// Used by BOTH the VST3 processor and the standalone client's embedded
// engine, so the two builds can never drift apart in sound.
#pragma once

#include "Source/dsp/Effects2.h"
#include "vst3/source/paramids.h"

namespace MicFilterBridge {

inline float plainOf (const double* v, int id) { return (float) v[id]; }
inline bool onOf (const double* v, int id) { return v[id] > 0.5; }

inline void fillDefaults (double* values)
{
    for (int id = 0; id < (int) MicFilterParams::kNumParams; ++id)
        values[id] = MicFilterParams::rangeFor (id).def;
}

inline pure2::Params2 paramsFromPlainValues (const double* values)
{
    pure2::Params2 p;
    p.base.inputGainDb = plainOf (values, MicFilterParams::kInGain);
    p.base.outputGainDb = plainOf (values, MicFilterParams::kOutGain);
    p.base.hpfOn = onOf (values, MicFilterParams::kHpfOn);
    p.base.hpfFreq = plainOf (values, MicFilterParams::kHpfFreq);
    p.base.gateOn = onOf (values, MicFilterParams::kGateOn);
    p.base.gateThreshDb = plainOf (values, MicFilterParams::kGateThresh);
    p.base.eqOn = onOf (values, MicFilterParams::kEqOn);
    p.base.eqLowDb = plainOf (values, MicFilterParams::kEqLow);
    p.base.eqMidDb = plainOf (values, MicFilterParams::kEqMid);
    p.base.eqHighDb = plainOf (values, MicFilterParams::kEqHigh);
    p.base.compOn = onOf (values, MicFilterParams::kCompOn);
    p.base.compThreshDb = plainOf (values, MicFilterParams::kCompThresh);
    p.base.compRatio = plainOf (values, MicFilterParams::kCompRatio);
    p.base.limiterOn = onOf (values, MicFilterParams::kLimiterOn);
    // v2 effects
    p.hum.on = onOf (values, MicFilterParams::kHumOn);
    p.hum.baseHz = plainOf (values, MicFilterParams::kHumBase) >= 55.0f ? 60.0f : 50.0f;
    p.declip.on = onOf (values, MicFilterParams::kDeclipOn);
    p.declick.on = onOf (values, MicFilterParams::kDeclickOn);
    p.declick.thresh = plainOf (values, MicFilterParams::kDeclickThresh);
    p.multiband.on = onOf (values, MicFilterParams::kMultibandOn);
    p.deesser.on = onOf (values, MicFilterParams::kDeesserOn);
    p.deesser.threshDb = plainOf (values, MicFilterParams::kDeesserThresh);
    p.exciter.on = onOf (values, MicFilterParams::kExciterOn);
    p.exciter.mix = plainOf (values, MicFilterParams::kExciterMix);
    p.spectral.on = onOf (values, MicFilterParams::kSpectralOn);
    p.spectral.strength = plainOf (values, MicFilterParams::kSpectralStrength);
    p.duck.on = onOf (values, MicFilterParams::kDuckOn);
    p.breath.on = onOf (values, MicFilterParams::kBreathOn);
    p.rnnoise.on = onOf (values, MicFilterParams::kRnnoiseOn);
    p.bwe.on = onOf (values, MicFilterParams::kBweOn);
    p.bwe.amount = plainOf (values, MicFilterParams::kBweAmount);
    p.autoeq.on = onOf (values, MicFilterParams::kAutoEqOn);
    p.autoeq.amount = plainOf (values, MicFilterParams::kAutoEqAmount);
    return p;
}

} // namespace MicFilterBridge
