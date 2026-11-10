#pragma once

// Shared parameter definitions for processor + controller.
// Plain ranges live here so both sides can never disagree.
// Linear mapping: normalized = (plain - min) / (max - min).
namespace MicFilterParams {

enum ParamID : unsigned int
{
    kInGain = 0,
    kOutGain,
    kHpfOn,
    kHpfFreq,
    kGateOn,
    kGateThresh,
    kEqOn,
    kEqLow,
    kEqMid,
    kEqHigh,
    kCompOn,
    kCompThresh,
    kCompRatio,
    kLimiterOn,
    kBypass, // uses Vst::kIsBypass in the controller
    // --- v2 effects (ChainV2). Appended after v1 ids: existing ids never move. ---
    kHumOn,
    kHumBase,       // 50 or 60 (Hz)
    kDeclipOn,      // default Off (adds +64 samples latency when On)
    kDeclickOn,
    kDeclickThresh,
    kMultibandOn,
    kDeesserOn,
    kDeesserThresh,
    kExciterOn,
    kExciterMix,
    kSpectralOn,      // hiss removal (spectral subtraction), default Off: +512 latency
    kSpectralStrength,
    kDuckOn,          // click/keyboard transient ducking
    kBreathOn,        // gate breath mode (keep quiet breath sounds)
    kRnnoiseOn,       // AI noise removal (RNNoise), default Off: +480 samples latency
    kBweOn,           // AI bandwidth extension, default Off: +480 samples latency
    kBweAmount,
    kAutoEqOn,        // auto EQ (slow mic-coloration correction), default On: 0 latency
    kAutoEqAmount,
    kNumParams
};

struct Range
{
    float min, max, def;
};

// Order must match the enum (except kBypass, handled separately).
inline const Range& rangeFor (int id)
{
    static const Range ranges[] = {
        { -24.0f,  24.0f,   0.0f }, // kInGain     dB
        { -24.0f,  24.0f,   0.0f }, // kOutGain    dB
        {   0.0f,   1.0f,   1.0f }, // kHpfOn
        {  40.0f, 200.0f,  80.0f }, // kHpfFreq    Hz
        {   0.0f,   1.0f,   1.0f }, // kGateOn
        { -80.0f, -10.0f, -50.0f }, // kGateThresh dB
        {   0.0f,   1.0f,   1.0f }, // kEqOn
        { -12.0f,  12.0f,   0.0f }, // kEqLow      dB
        { -12.0f,  12.0f,   0.0f }, // kEqMid      dB
        { -12.0f,  12.0f,   0.0f }, // kEqHigh     dB
        {   0.0f,   1.0f,   1.0f }, // kCompOn
        { -40.0f,   0.0f, -18.0f }, // kCompThresh dB
        {   1.0f,  10.0f,   3.0f }, // kCompRatio
        {   0.0f,   1.0f,   1.0f }, // kLimiterOn
        {   0.0f,   1.0f,   0.0f }, // kBypass
        {   0.0f,   1.0f,   1.0f }, // kHumOn
        {  50.0f,  60.0f,  60.0f }, // kHumBase Hz (60 = western Japan incl. Aichi)
        {   0.0f,   1.0f,   1.0f }, // kDeclipOn (default ON since 2026-11-08: first-shout
                                    // safety net per user expectation; costs +64 samples latency)
        {   0.0f,   1.0f,   1.0f }, // kDeclickOn
        {  0.05f,   0.5f,  0.22f }, // kDeclickThresh
        {   0.0f,   1.0f,   1.0f }, // kMultibandOn
        {   0.0f,   1.0f,   1.0f }, // kDeesserOn
        { -40.0f,   0.0f, -22.0f }, // kDeesserThresh dB
        {   0.0f,   1.0f,   1.0f }, // kExciterOn
        {   0.0f,   0.6f,  0.25f }, // kExciterMix
        {   0.0f,   1.0f,   0.0f }, // kSpectralOn
        {   0.5f,   2.0f,   1.2f }, // kSpectralStrength
        {   0.0f,   1.0f,   1.0f }, // kDuckOn
        {   0.0f,   1.0f,   0.0f }, // kBreathOn
        {   0.0f,   1.0f,   0.0f }, // kRnnoiseOn
        {   0.0f,   1.0f,   0.0f }, // kBweOn
        {   0.0f,   1.5f,   1.0f }, // kBweAmount
        {   0.0f,   1.0f,   1.0f }, // kAutoEqOn
        {   0.0f,   1.5f,   1.0f }, // kAutoEqAmount
    };
    return ranges[id];
}

inline float toNormalized (int id, float plain)
{
    const Range& r = rangeFor (id);
    float n = (plain - r.min) / (r.max - r.min);
    return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
}

inline float toPlain (int id, float normalized)
{
    const Range& r = rangeFor (id);
    return r.min + normalized * (r.max - r.min);
}

inline bool isSwitch (int id)
{
    return id == kHpfOn || id == kGateOn || id == kEqOn || id == kCompOn || id == kLimiterOn
        || id == kBypass || id == kHumOn || id == kDeclipOn || id == kDeclickOn
        || id == kMultibandOn || id == kDeesserOn || id == kExciterOn || id == kSpectralOn
        || id == kDuckOn || id == kBreathOn || id == kRnnoiseOn || id == kBweOn
        || id == kAutoEqOn;
}

} // namespace MicFilterParams
