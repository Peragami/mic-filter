#pragma once
// Effects2: second-stage effects for the mic filter, built ON TOP of PureDsp.h
// without modifying it (legacy pure::Chain / pure::Params stay untouched and
// API-compatible). Everything lives in namespace pure2 and is framework-free
// C++17 (only <cmath> / <algorithm>, same as PureDsp).
//
// Real-time safety: no allocation, no locks, no exceptions in any process
// path. All buffers are fixed-size members. DeClip and DeClick carry an
// inherent lookahead delay (documented per class); every other effect here
// is zero-latency.
//
// ChainV2 order:
//   inGain -> DeClip -> DeClick -> HPF -> HumNotch -> EQ -> MultibandExpander
//          -> Gate -> DeEsser -> Comp -> Exciter -> Limiter -> outGain
// Latency of ChainV2 = (declipOn ? 64 : 0) + (declickOn ? 128 : 0) samples.
#include "PureDsp.h"
#include "SpectralSub.h"
#include "RnnoiseStage.h"
#include "BweStage.h"
#include <cmath>
#include <algorithm>

namespace pure2 {

using pure::dbToGain;
using pure::clampf;
static constexpr double kPi = 3.141592653589793;

// Cubic Hermite segment between endpoint (y0, slope m0) and (y1, slope m1),
// h samples apart. Returns the value t in (0,1) along the segment.
// Shared by DeClipper and DeClicker repairs: with the slopes taken from the
// clean signal on both sides, the curve bulges past the endpoints, which is
// exactly the peak estimate a clipped / spiked section needs.
static inline float hermite (float y0, float m0, float y1, float m1, float h, float t)
{
    float t2=t*t, t3=t2*t;
    return (2*t3-3*t2+1)*y0 + (t3-2*t2+t)*h*m0 + (-2*t3+3*t2)*y1 + (t3-t2)*h*m1;
}

//======================================================================
// 1. Hum notch: fundamental (50 or 60 Hz, selectable) + up to 2 harmonics
//    (2f, 3f), narrow RBJ notches in series. Zero latency.
struct HumNotchParams
{
    bool  on = true;
    float baseHz = 60.0f;   // 50 or 60
    int   harmonics = 2;    // 0..2 (adds 2f, 3f)
    float q = 30.0f;
};
struct HumNotch
{
    // Double-precision biquads on purpose: at 50 Hz / 48 kHz a float32
    // direct-form notch loses depth to coefficient cancellation and
    // stalls around -30 dB (measured). Three double biquads cost nothing
    // measurable and restore the theoretical depth.
    struct DNotch
    {
        double b0=1, b1=0, b2=0, a1=0, a2=0, x1=0, x2=0, y1=0, y2=0;
        void setNotch (double sr, double freq, double q)
        {
            double w=2*kPi*freq/sr, cw=std::cos(w), sw=std::sin(w), al=sw/(2*q);
            double a0=1+al; b0=1/a0; b1=(-2*cw)/a0; b2=b0; a1=b1; a2=(1-al)/a0;
        }
        void reset(){ x1=x2=y1=y2=0; }
        inline float process (float xin)
        {
            double x = xin;
            double y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
            x2=x1; x1=x; y2=y1; y1=y; return float(y);
        }
    };
    DNotch notch[3];
    HumNotchParams p;
    void prepare (double sr, const HumNotchParams& np) { setParams (sr, np); }
    void setParams (double sr, const HumNotchParams& np)
    {
        p = np;
        for (int i=0;i<3;++i) notch[i].setNotch (sr, p.baseHz * float(i+1), p.q);
    }
    void reset(){ for (auto& n: notch) n.reset(); }
    inline float process (float x)
    {
        if (!p.on) return x;
        x = notch[0].process (x);
        if (p.harmonics >= 1) x = notch[1].process (x);
        if (p.harmonics >= 2) x = notch[2].process (x);
        return x;
    }
};

//======================================================================
// 2. DeClipper: repairs runs of samples pinned at >= +/-threshold by
//    Hermite interpolation between the clean slopes on both sides.
//    Needs the post-run slope, so it delays the stream by kDelay samples
//    (only while switched on). Runs longer than maxRun are left alone.
struct DeClipParams
{
    bool  on = true;
    float threshold = 0.99f;
    int   maxRun = 40;
};
struct DeClipper
{
    static constexpr int kDelay = 64;
    static constexpr int kRing = 192; // > kDelay + maxRun + slack
    float ring[kRing] = {};
    long absIn = 0;
    DeClipParams p;
    // detector state (input timeline)
    float prev1=0, prev2=0;
    bool inRun=false, pendingPost=false;
    long runStart=0; int runLen=0; float runSign=1;
    float preVal=0, preSlope=0, postVal=0;
    long pendStart=0; int pendLen=0; float pendPreVal=0, pendPreSlope=0, pendPostVal=0;

    void prepare (const DeClipParams& np) { p = np; reset(); }
    void setParams (const DeClipParams& np) { p = np; }
    void reset()
    {
        for (auto& v: ring) v=0;
        absIn=0; prev1=prev2=0; inRun=pendingPost=false; runLen=0;
    }
    void patch (long start, int len, float y0, float m0, float y1, float m1)
    {
        float h = float(len + 1);
        for (int k=1;k<=len;++k)
            ring[(start + k - 1) % kRing] = hermite (y0, m0, y1, m1, h, float(k)/h);
    }
    inline float process (float x)
    {
        if (!p.on) return x;
        const long a = absIn;
        // --- detector / repair scheduling (runs on the input timeline) ---
        if (pendingPost)
        {
            // x is the 2nd clean sample after the run: post slope now known.
            patch (pendStart, pendLen, pendPreVal, pendPreSlope, pendPostVal, x - pendPostVal);
            pendingPost = false;
            prev2 = pendPostVal; prev1 = x;
        }
        else if (inRun)
        {
            if (runSign * x < p.threshold) // first clean sample after the run
            {
                pendStart = runStart; pendLen = runLen;
                pendPreVal = preVal; pendPreSlope = preSlope; pendPostVal = x;
                pendingPost = true; inRun = false;
            }
            else if (++runLen > p.maxRun) inRun = false; // too long: give up
        }
        else
        {
            if (std::fabs (x) >= p.threshold)
            {
                inRun = true; runStart = a; runLen = 1;
                runSign = (x >= 0 ? 1.0f : -1.0f);
                preVal = prev1; preSlope = prev1 - prev2;
            }
            prev2 = prev1; prev1 = x;
        }
        ring[a % kRing] = x;
        ++absIn;
        long outIdx = a - kDelay;
        return outIdx >= 0 ? ring[outIdx % kRing] : 0.0f;
    }
};

//======================================================================
// 3. Multiband expander, 4 bands, subtractive filterbank:
//      b0 = LP1(x); r1 = x-b0; b1 = LP2(r1); r2 = r1-b1;
//      b2 = LP3(r2); b3 = r2-b2;
//    The band gains are the only thing between input and output, and the
//    split telescopes algebraically: with all gains = 1 the output equals
//    the input per-sample regardless of filter state (tested).
//    Each band has its own envelope and a gentle downward expander with a
//    gain floor. Zero latency.
struct MultibandParams
{
    bool  on = true;
    float threshDb[4] = { -48.0f, -45.0f, -42.0f, -48.0f };
    float ratio = 1.5f;     // gentle
    float floorDb = 18.0f;  // max extra attenuation per band
};
struct MultibandExpander
{
    static constexpr float kCross[3] = { 250.0f, 1000.0f, 4000.0f };
    pure::Biquad lp[3];
    MultibandParams p;
    float env[4] = {}, gain[4] = {1,1,1,1};
    float atkCoef=0.5f, relCoef=0.005f, envDecay=0.999f;
    void prepare (double sr, const MultibandParams& np)
    {
        p = np;
        for (int i=0;i<3;++i) lp[i].setLowPass (sr, kCross[i]);
        atkCoef  = float(1.0 - std::exp (-1.0/(sr*0.002)));  // 2 ms gain attack
        relCoef  = float(1.0 - std::exp (-1.0/(sr*0.080)));  // 80 ms gain release
        envDecay = float(std::exp (-1.0/(sr*0.050)));        // 50 ms env decay
        reset();
    }
    void setParams (const MultibandParams& np) { p = np; }
    void reset(){ for (auto& f: lp) f.reset(); for (int i=0;i<4;++i){ env[i]=0; gain[i]=1; } }
    // Split only (no expansion) — exposed for the unity test / reuse.
    inline void split (float x, float bands[4])
    {
        float b0 = lp[0].process (x);   float r1 = x - b0;
        float b1 = lp[1].process (r1);  float r2 = r1 - b1;
        float b2 = lp[2].process (r2);  float b3 = r2 - b2;
        bands[0]=b0; bands[1]=b1; bands[2]=b2; bands[3]=b3;
    }
    inline float process (float x)
    {
        float bands[4]; split (x, bands);
        if (!p.on) return bands[0]+bands[1]+bands[2]+bands[3];
        float out = 0;
        for (int i=0;i<4;++i)
        {
            float ax = std::fabs (bands[i]);
            env[i] = std::max (ax, env[i] * envDecay);
            float target = 1.0f;
            if (env[i] > 1e-6f)
            {
                float envDb = 20.0f * std::log10 (env[i]);
                if (envDb < p.threshDb[i])
                {
                    float redDb = std::min (p.floorDb, (p.threshDb[i] - envDb) * (p.ratio - 1.0f));
                    target = dbToGain (-redDb);
                }
            }
            gain[i] += (target - gain[i]) * (target < gain[i] ? atkCoef : relCoef);
            out += bands[i] * gain[i];
        }
        return out;
    }
};

//======================================================================
// 4. DeEsser: sidechain = high band (~freq) envelope; when sibilance is
//    hot, only the high band is turned down (subtractive split, so the
//    rest of the spectrum is bit-transparent when no reduction happens).
struct DeEsserParams
{
    bool  on = true;
    float freq = 6000.0f;
    float threshDb = -22.0f;
    float maxRedDb = 10.0f;
};
struct DeEsser
{
    pure::Biquad lpSplit, hpSide;
    DeEsserParams p;
    float env=0, gain=1, envDecay=0.999f, atkCoef=0.5f, relCoef=0.01f;
    void prepare (double sr, const DeEsserParams& np)
    {
        p = np;
        lpSplit.setLowPass (sr, p.freq); hpSide.setHighPass (sr, p.freq);
        envDecay = float(std::exp (-1.0/(sr*0.030)));
        atkCoef  = float(1.0 - std::exp (-1.0/(sr*0.001)));
        relCoef  = float(1.0 - std::exp (-1.0/(sr*0.060)));
        reset();
    }
    void setParams (double sr, const DeEsserParams& np) { prepare (sr, np); }
    void reset(){ lpSplit.reset(); hpSide.reset(); env=0; gain=1; }
    inline float process (float x)
    {
        if (!p.on) return x;
        float low  = lpSplit.process (x);
        float high = x - low;
        float det  = std::fabs (hpSide.process (x));
        env = std::max (det, env * envDecay);
        float target = 1.0f;
        if (env > 1e-6f)
        {
            float envDb = 20.0f * std::log10 (env);
            if (envDb > p.threshDb)
                target = dbToGain (-std::min (p.maxRedDb, (envDb - p.threshDb) * 0.7f));
        }
        gain += (target - gain) * (target < gain ? atkCoef : relCoef);
        return low + high * gain;
    }
};

//======================================================================
// 5. DeClicker / lip-noise removal. Lookahead is EXACTLY kLookahead
//    samples: a sample fed at index n leaves at n + kLookahead.
//    Detector: quadratic extrapolation from the last clean samples; a
//    sample that deviates far beyond what the local slope/curvature can
//    explain starts a "run". The run ends when the signal rejoins the
//    extrapolated trajectory; the whole run (<= maxRun) is then replaced
//    by Hermite interpolation between the clean slopes on both sides,
//    patched into the ring before its samples reach the output.
struct DeClickParams
{
    bool  on = true;
    float thresh = 0.22f;     // min deviation (abs) to suspect a click
    float adapt = 2.5f;       // deviation must also exceed adapt * local slope scale
    float returnTol = 0.15f;  // |x - extrapolated| below this = back on track
    int   maxRun = 32;
};
struct DeClicker
{
    static constexpr int kLookahead = 128;
    static constexpr int kRing = 512;
    float ring[kRing] = {};
    long absIn = 0;
    DeClickParams p;
    float prev1=0, prev2=0, prev3=0; // clean history only
    bool inRun=false, pendingPost=false;
    long runStart=0; int runLen=0;
    float preVal=0, preSlope=0, preCurve=0;
    long pendStart=0; int pendLen=0; float pendPreVal=0, pendPreSlope=0, pendPostVal=0;

    void prepare (const DeClickParams& np) { p = np; reset(); }
    void setParams (const DeClickParams& np) { p = np; }
    void reset()
    {
        for (auto& v: ring) v=0;
        absIn=0; prev1=prev2=prev3=0; inRun=pendingPost=false; runLen=0;
    }
    void patch (long start, int len, float y0, float m0, float y1, float m1)
    {
        float h = float(len + 1);
        for (int k=1;k<=len;++k)
            ring[(start + k - 1) % kRing] = hermite (y0, m0, y1, m1, h, float(k)/h);
    }
    inline float process (float x)
    {
        if (!p.on) return x;
        const long a = absIn;
        if (pendingPost)
        {
            patch (pendStart, pendLen, pendPreVal, pendPreSlope, pendPostVal, x - pendPostVal);
            pendingPost = false;
            // resync detector history onto the repaired stream:
            float lastRepaired = ring[(pendStart + pendLen - 1) % kRing];
            prev3 = lastRepaired; prev2 = pendPostVal; prev1 = x;
        }
        else if (inRun)
        {
            int k = runLen + 1; // distance of x from the pre-run sample
            float expected = preVal + preSlope*float(k) + 0.5f*preCurve*float(k*k);
            if (std::fabs (x - expected) <= p.returnTol)
            {
                pendStart = runStart; pendLen = runLen;
                pendPreVal = preVal; pendPreSlope = preSlope; pendPostVal = x;
                pendingPost = true; inRun = false;
            }
            else if (++runLen > p.maxRun)
            {
                inRun = false; prev1 = prev2 = prev3 = x; // abandon, resync flat
            }
        }
        else
        {
            float pred = 3*prev1 - 3*prev2 + prev3; // quadratic extrapolation
            float dev = std::fabs (x - pred);
            float scale = std::fabs (prev1 - prev2) + std::fabs ((prev1 - prev2) - (prev2 - prev3)) * 4.0f + 0.02f;
            if (dev > p.thresh && dev > p.adapt * scale)
            {
                inRun = true; runStart = a; runLen = 1;
                preVal = prev1; preSlope = prev1 - prev2;
                preCurve = (prev1 - prev2) - (prev2 - prev3);
            }
            else { prev3 = prev2; prev2 = prev1; prev1 = x; }
        }
        ring[a % kRing] = x;
        ++absIn;
        long outIdx = a - kLookahead;
        return outIdx >= 0 ? ring[outIdx % kRing] : 0.0f;
    }
};

//======================================================================
// 6. Exciter: the >= freq band is soft-saturated (tanh) at 2x rate
//    (linear-interpolation up, average down — deliberately simple), and
//    only the newly generated harmonic content (wet minus its linear
//    counterpart) is mixed back, scaled by mix. The wet branch lags the
//    dry path by half a sample; at exciter frequencies that is inaudible
//    comb-wise for an additive harmonics layer.
struct ExciterParams
{
    bool  on = true;
    float freq = 6000.0f;
    float drive = 2.0f;
    float mix = 0.25f;   // 0..1, keep modest — more reads as cheap, not airy
};
struct Exciter
{
    pure::Biquad lpSplit;
    ExciterParams p;
    float prevHigh = 0;
    void prepare (double sr, const ExciterParams& np) { p = np; lpSplit.setLowPass (sr, p.freq); reset(); }
    void setParams (double sr, const ExciterParams& np) { prepare (sr, np); }
    void reset(){ lpSplit.reset(); prevHigh = 0; }
    inline float process (float x)
    {
        if (!p.on) return x;
        float high = x - lpSplit.process (x);
        float mid  = 0.5f * (prevHigh + high);          // interpolated OS sample
        float norm = std::tanh (p.drive);
        float wet  = 0.5f * (std::tanh (p.drive * mid) + std::tanh (p.drive * high)) / (norm > 1e-6f ? norm : 1.0f);
        float lin  = 0.5f * (mid + high);               // linear counterpart, same timing
        prevHigh = high;
        return x + p.mix * (wet - lin);
    }
};

//======================================================================
// ChainV2: legacy chain + all of the above. PureDsp itself is untouched.
// TransientDuck: click-noise suppressor for mouse clicks / keyboard hits.
// Level alone cannot separate them from breath (clicks are LOUDER than
// breath), so detection uses character instead: energy above 5 kHz that
// rises far faster than its own recent average. On a trigger the gain dips
// for ~70 ms around the event. A 64-sample lookahead lets the dip start
// just before the transient lands. Speech protection: while broadband
// level is high, the trigger ratio is raised (plosives are transients too).
struct TransientDuckParams
{
    bool  on = true;
    float duckDb = 18.0f;    // depth of the dip
    float ratio = 4.0f;      // fast env must exceed slow env * ratio
    float minLevel = 0.05f;  // absolute floor for the HF envelope
};
struct TransientDuck
{
    static constexpr int kLookahead = 64;
    pure::Biquad hp;
    TransientDuckParams p;
    float ring[kLookahead] = {};
    int ringPos = 0;
    float fastEnv = 0, slowEnv = 0, broadEnv = 0;
    float gain = 1.0f;
    int duckLeft = 0;
    double sr = 48000;

    void prepare (double sampleRate, const TransientDuckParams& np)
    {
        sr = sampleRate;
        p = np;
        hp.setHighPass (sr, 5000.0f);
    }
    void setParams (const TransientDuckParams& np) { prepare (sr, np); }
    void reset()
    {
        for (int i = 0; i < kLookahead; ++i) ring[i] = 0.0f;
        ringPos = 0; fastEnv = slowEnv = broadEnv = 0; gain = 1.0f; duckLeft = 0;
        hp.reset();
    }
    inline float process (float x)
    {
        if (!p.on) return x;
        // Detector runs on the live (lookahead) signal.
        const float hf = std::fabs (hp.process (x));
        fastEnv = std::max (hf, fastEnv * 0.94f);          // ~5 ms fall
        slowEnv += (hf - slowEnv) * 0.001f;                // ~200 ms average
        broadEnv = std::max (std::fabs (x), broadEnv * 0.999f);
        const float ratioNeed = p.ratio * (broadEnv > 0.18f ? 1.6f : 1.0f); // protect loud speech
        if (duckLeft <= 0 && fastEnv > slowEnv * ratioNeed && fastEnv > p.minLevel)
            duckLeft = (int) (0.070 * sr);                 // 70 ms window
        // Gain envelope.
        const float target = duckLeft > 0 ? dbToGain (-p.duckDb) : 1.0f;
        gain += (target - gain) * (target < gain ? 0.35f : 0.004f);
        if (duckLeft > 0) --duckLeft;
        // Delay line: output the sample from kLookahead ago, ducked.
        const float out = ring[ringPos] * gain;
        ring[ringPos] = x;
        ringPos = (ringPos + 1) % kLookahead;
        return out;
    }
};

// Breath mode for the gate (Params2-level flag, applied in ChainV2):
// keeps quiet sustained sounds (breath) that a normal gate would cut, by
// lowering the threshold and raising the gate floor. Clicks are unaffected
// here (they are loud transients - TransientDuck's job, not the gate's).
struct BreathParams
{
    bool on = false;
};

// AutoEq (v0.21): slowly tracks the long-term spectral balance of the
// voice and applies a gentle corrective EQ toward a target curve
// ("broadcast voice"). It compensates the fixed coloration of the mic
// (cheap mics: weak top end, boomy proximity lows). Analysis integrates
// only while plausible voice is present; correction gains move over
// seconds, are capped, and add zero latency (plain IIR filtering).
struct AutoEqParams
{
    bool  on = true;
    float amount = 1.0f;  // 0..1.5 correction strength
    float maxDb = 5.0f;   // per-band correction cap
};

struct AutoEq
{
    static constexpr int kBands = 5; // LP250, BP500, BP1500 (anchor), BP4000, HP7000
    pure::Biquad ana[kBands];
    pure::Biquad cor[4];             // lowshelf 200, peak 500, peak 2500, highshelf 6000
    float bandPow[kBands] = {};
    float gainDb[4] = {};
    float shortPow = 0;
    double voiceSec = 0;
    int tick = 0;
    double sr = 48000;
    AutoEqParams p;
    // Target band levels relative to the 1.5 kHz anchor (dB), a typical
    // long-term speech spectrum as seen through these analysis filters.
    static constexpr float kTargetDb[kBands] = { 6.5f, 3.0f, 0.0f, -2.5f, -10.5f };
    static constexpr int kCorBand[4] = { 0, 1, 3, 4 }; // correction i reads band kCorBand[i]

    void prepare (double sampleRate, const AutoEqParams& np)
    {
        sr = sampleRate;
        p = np;
        ana[0].setLowPass (sr, 250);
        ana[1].setBandPass (sr, 500, 0.8f);
        ana[2].setBandPass (sr, 1500, 0.8f);
        ana[3].setBandPass (sr, 4000, 0.8f);
        ana[4].setHighPass (sr, 7000);
        reset();
    }
    void setParams (double sampleRate, const AutoEqParams& np) { sr = sampleRate; p = np; }
    void reset()
    {
        for (auto& b : ana) b.reset();
        for (auto& c : cor) c.reset();
        for (auto& v : bandPow) v = 0;
        for (auto& v : gainDb) v = 0;
        shortPow = 0;
        voiceSec = 0;
        tick = 0;
        refreshCoeffs();
    }
    void refreshCoeffs()
    {
        cor[0].setShelf (sr, 200, gainDb[0], false);
        cor[1].setPeak (sr, 500, 0.9f, gainDb[1]);
        cor[2].setPeak (sr, 2500, 0.9f, gainDb[2]);
        cor[3].setShelf (sr, 6000, gainDb[3], true);
    }
    inline float process (float x)
    {
        if (!p.on) return x;
        // Analysis always runs (cheap); integration is voice-gated.
        float y[kBands];
        for (int i = 0; i < kBands; ++i) y[i] = ana[i].process (x);
        shortPow += (x * x - shortPow) * 0.0005f; // ~40 ms level
        const float db = 10.0f * std::log10 (shortPow + 1e-12f);
        const bool voice = db > -52.0f && db < -1.0f && std::fabs (x) < 0.97f;
        if (voice)
        {
            voiceSec += 1.0 / sr;
            const float a = 1.0f / (3.5f * (float) sr); // ~3.5 s averaging
            for (int i = 0; i < kBands; ++i) bandPow[i] += (y[i] * y[i] - bandPow[i]) * a;
        }
        // Correction targets refresh at ~20 Hz, gains ease toward them.
        if (++tick >= 2400)
        {
            tick = 0;
            if (voiceSec > 2.0 && bandPow[2] > 1e-10f)
            {
                const float fade = (float) std::min (1.0, (voiceSec - 2.0) / 3.0);
                for (int c = 0; c < 4; ++c)
                {
                    const int m = kCorBand[c];
                    const float diffDb = 10.0f * std::log10 (bandPow[m] / bandPow[2] + 1e-12f);
                    float want = (kTargetDb[m] - diffDb) * p.amount;
                    want = std::max (-p.maxDb, std::min (p.maxDb, want)) * fade;
                    gainDb[c] += (want - gainDb[c]) * 0.04f; // ~1.2 s easing per tick rate
                }
                refreshCoeffs();
            }
        }
        float out = x;
        for (auto& c : cor) out = c.process (out);
        return out;
    }
    float currentGainDb (int c) const { return gainDb[c]; } // for tests/UI
};

struct Params2
{
    pure::Params base;             // gain / hpf / eq / gate / comp / limiter
    HumNotchParams hum;
    DeClipParams declip { false, 0.99f, 40 }; // ChainV2 default OFF (parent decision):
                                              // keeps latency at declick lookahead only
                                              // (128 samples); DeClip would add +64.
    DeClickParams declick;
    MultibandParams multiband;
    DeEsserParams deesser;
    ExciterParams exciter;
    SpectralParams spectral; // default OFF (opt-in; +512 samples latency)
    TransientDuckParams duck;
    BreathParams breath;
    RnnoiseParams rnnoise; // default OFF (opt-in AI noise removal; +480 samples latency)
    BweParams bwe;         // default OFF (opt-in AI bandwidth extension; +480 samples)
    AutoEqParams autoeq;   // default ON (slow mic-coloration correction; 0 latency)
};

struct ChainV2
{
    pure::Biquad hpf, low, mid, high;
    HumNotch hum; DeClipper declip; DeClicker declick;
    MultibandExpander multiband; DeEsser deesser; Exciter exciter;
    SpectralSub spectral;
    TransientDuck duck;
    RnnoiseStage rnnoiseStage;
    BweStage bweStage;
    AutoEq autoEq;
    Params2 p; double sr = 48000;
    float gateEnv=0, gateGain=1, compEnv=0;

    void prepare (double sampleRate) { sr = sampleRate; update(); declip.reset(); declick.reset(); spectral.reset(); duck.reset(); rnnoiseStage.reset(); bweStage.reset(); autoEq.prepare (sr, p.autoeq); }
    ~ChainV2() { rnnoiseStage.release(); }
    void setParams (const Params2& np) { p = np; update(); }
    void update()
    {
        hpf.setHighPass (sr, p.base.hpfFreq);
        low.setShelf (sr, 200, p.base.eqLowDb, false);
        mid.setPeak (sr, 1000, 0.9f, p.base.eqMidDb);
        high.setShelf (sr, 4000, p.base.eqHighDb, true);
        hum.prepare (sr, p.hum);
        declip.setParams (p.declip);
        declick.setParams (p.declick);
        multiband.prepare (sr, p.multiband);
        deesser.prepare (sr, p.deesser);
        exciter.prepare (sr, p.exciter);
        spectral.setParams (p.spectral);
        duck.prepare (sr, p.duck);
        rnnoiseStage.setParams (p.rnnoise);
        bweStage.prepare (sr, p.bwe);
        autoEq.setParams (sr, p.autoeq); // no state reset: adaptation survives tweaks
    }
    int latencySamples() const
    {
        return (p.declip.on ? DeClipper::kDelay : 0) + (p.declick.on ? DeClicker::kLookahead : 0)
             + spectral.latencySamples() + (p.duck.on ? TransientDuck::kLookahead : 0)
             + rnnoiseStage.latencySamples() + bweStage.latencySamples();
    }
    // --- Reorderable stages -------------------------------------------------
    // The client can reorder stages per preset (setStageOrder); the VST
    // build never calls it and keeps kDefaultStageOrder. Stage ids:
    enum StageId
    {
        STAGE_DECLIP = 0, STAGE_DECLICK, STAGE_DUCK, STAGE_HPF, STAGE_HUM,
        STAGE_RNNOISE, STAGE_SPECTRAL, STAGE_EQ, STAGE_MULTIBAND, STAGE_GATE,
        STAGE_DEESSER, STAGE_COMP, STAGE_EXCITER, STAGE_BWE, STAGE_LIMITER,
        STAGE_AUTOEQ, // v0.21: appended (ids of existing stages unchanged)
        STAGE_COUNT
    };
    static constexpr int kDefaultStageOrder[STAGE_COUNT] = {
        STAGE_DECLIP, STAGE_DECLICK, STAGE_DUCK, STAGE_HPF, STAGE_HUM,
        STAGE_RNNOISE, STAGE_SPECTRAL, STAGE_EQ, STAGE_AUTOEQ, STAGE_MULTIBAND,
        STAGE_GATE, STAGE_DEESSER, STAGE_COMP, STAGE_EXCITER, STAGE_BWE, STAGE_LIMITER
    };
    int stageOrder[STAGE_COUNT] = {
        STAGE_DECLIP, STAGE_DECLICK, STAGE_DUCK, STAGE_HPF, STAGE_HUM,
        STAGE_RNNOISE, STAGE_SPECTRAL, STAGE_EQ, STAGE_AUTOEQ, STAGE_MULTIBAND,
        STAGE_GATE, STAGE_DEESSER, STAGE_COMP, STAGE_EXCITER, STAGE_BWE, STAGE_LIMITER
    };
    // Accept only an exact permutation of all stages; anything else is
    // ignored so a corrupt preset cannot break the chain.
    void setStageOrder (const int* order, int n)
    {
        if (n != STAGE_COUNT) return;
        bool seen[STAGE_COUNT] = {};
        for (int i = 0; i < n; ++i)
        {
            if (order[i] < 0 || order[i] >= STAGE_COUNT || seen[order[i]]) return;
            seen[order[i]] = true;
        }
        for (int i = 0; i < n; ++i) stageOrder[i] = order[i];
    }

    inline float processStage (int stage, float x)
    {
        switch (stage)
        {
            case STAGE_DECLIP: return declip.process (x);
            case STAGE_DECLICK: return declick.process (x);
            case STAGE_DUCK: return duck.process (x);
            case STAGE_HPF: return p.base.hpfOn ? hpf.process (x) : x;
            case STAGE_HUM: return hum.process (x);
            case STAGE_RNNOISE: return rnnoiseStage.process (x);
            case STAGE_SPECTRAL: return spectral.process (x);
            case STAGE_EQ:
                if (p.base.eqOn) { x = low.process (x); x = mid.process (x); x = high.process (x); }
                return x;
            case STAGE_AUTOEQ: return autoEq.process (x);
            case STAGE_MULTIBAND: return multiband.process (x);
            case STAGE_GATE:
            {
                if (!p.base.gateOn) return x;
                const float ax = std::fabs (x);
                gateEnv = std::max (ax, gateEnv * 0.9995f);
                // Breath mode: lower threshold + higher floor + slower close, so
                // quiet sustained breath survives instead of being gated out.
                const float threshDb = p.base.gateThreshDb + (p.breath.on ? -10.0f : 0.0f);
                const float floorGain = p.breath.on ? 0.30f : 0.06f;
                const float closeRate = p.breath.on ? 0.0008f : 0.002f;
                float target = (gateEnv >= dbToGain (threshDb)) ? 1.0f : floorGain;
                gateGain += (target - gateGain) * (target > gateGain ? 0.5f : closeRate);
                return x * gateGain;
            }
            case STAGE_DEESSER: return deesser.process (x);
            case STAGE_COMP:
            {
                if (!p.base.compOn) return x;
                const float ax = std::fabs (x);
                compEnv = std::max (ax, compEnv * 0.999f);
                float over = dbToGain (p.base.compThreshDb);
                if (compEnv > over && compEnv > 1e-6f)
                {
                    float excessDb = 20.0f * std::log10 (compEnv / over);
                    float reduceDb = excessDb * (1.0f - 1.0f / p.base.compRatio);
                    x *= dbToGain (-reduceDb);
                }
                return x;
            }
            case STAGE_EXCITER: return exciter.process (x);
            case STAGE_BWE: return bweStage.process (x);
            case STAGE_LIMITER: return p.base.limiterOn ? clampf (x, -0.89f, 0.89f) : x;
            default: return x;
        }
    }

    inline float processSample (float x)
    {
        x *= dbToGain (p.base.inputGainDb);
        for (int i = 0; i < STAGE_COUNT; ++i) x = processStage (stageOrder[i], x);
        return x * dbToGain (p.base.outputGainDb);
    }

    void processBlock (float* buf, int n) { for (int i=0;i<n;++i) buf[i]=processSample(buf[i]); }
};

} // namespace pure2
