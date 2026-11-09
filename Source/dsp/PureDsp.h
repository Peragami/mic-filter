#pragma once
// Framework-free DSP core: no JUCE, no allocations, no locks, no exceptions
// in the audio path. This is the whole cost of the filter chain — the
// VST3 wrapper and the WASAPI client around it add only a function call
// per block. Plain C++17, depends only on <cmath>.
#include <cmath>
#include <algorithm>

namespace pure {

static inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
static inline float clampf (float v, float lo, float hi) { return std::min (hi, std::max (lo, v)); }

struct Biquad
{
    float b0=1, b1=0, b2=0, a1=0, a2=0, x1=0, x2=0, y1=0, y2=0;
    inline float process (float x)
    {
        float y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2=x1; x1=x; y2=y1; y1=y; return y;
    }
    void reset(){ x1=x2=y1=y2=0; }
    void setHighPass (double sr, float freq, float q=0.7071f)
    {
        double w=2*3.141592653589793*freq/sr, cw=std::cos(w), sw=std::sin(w), al=sw/(2*q);
        double a0=1+al; b0=float(((1+cw)/2)/a0); b1=float((-(1+cw))/a0); b2=b0;
        a1=float((-2*cw)/a0); a2=float((1-al)/a0);
    }
    void setPeak (double sr, float freq, float q, float gainDb)
    {
        double A=std::pow(10.0, gainDb/40.0), w=2*3.141592653589793*freq/sr, cw=std::cos(w), sw=std::sin(w), al=sw/(2*q);
        double a0=1+al/A; b0=float((1+al*A)/a0); b1=float((-2*cw)/a0); b2=float((1-al*A)/a0);
        a1=b1; a2=float((1-al/A)/a0);
    }
    void setShelf (double sr, float freq, float gainDb, bool high)
    {
        double A=std::pow(10.0, gainDb/40.0), w=2*3.141592653589793*freq/sr, cw=std::cos(w), sw=std::sin(w);
        double al=sw/2*std::sqrt(2.0), sq=2*std::sqrt(A)*al, a0, bb0, bb1, bb2, aa1, aa2;
        if (high) { a0=(A+1)-(A-1)*cw+sq; bb0=A*((A+1)+(A-1)*cw+sq); bb1=-2*A*((A-1)+(A+1)*cw); bb2=A*((A+1)+(A-1)*cw-sq); aa1=2*((A-1)-(A+1)*cw); aa2=(A+1)-(A-1)*cw-sq; }
        else      { a0=(A+1)+(A-1)*cw+sq; bb0=A*((A+1)-(A-1)*cw+sq); bb1=2*A*((A-1)-(A+1)*cw); bb2=A*((A+1)-(A-1)*cw-sq); aa1=-2*((A-1)+(A+1)*cw); aa2=(A+1)+(A-1)*cw-sq; }
        b0=float(bb0/a0); b1=float(bb1/a0); b2=float(bb2/a0); a1=float(aa1/a0); a2=float(aa2/a0);
    }
};

struct Params
{
    float inputGainDb=0, outputGainDb=0;
    bool hpfOn=true;  float hpfFreq=80;
    bool gateOn=true; float gateThreshDb=-50;
    bool eqOn=true;   float eqLowDb=0, eqMidDb=0, eqHighDb=0;
    bool compOn=true; float compThreshDb=-18, compRatio=3;
    bool limiterOn=true;
};

// Mono mic chain (duplicate the sample to ch1 in the wrapper if stereo out is needed).
struct Chain
{
    Biquad hpf, low, mid, high;
    Params p; double sr=48000;
    float gateEnv=0, gateGain=1, compEnv=0;

    void prepare (double sampleRate) { sr=sampleRate; update(); }
    void setParams (const Params& np) { p=np; update(); }
    void update()
    {
        hpf.setHighPass (sr, p.hpfFreq);
        low.setShelf (sr, 200, p.eqLowDb, false);
        mid.setPeak (sr, 1000, 0.9f, p.eqMidDb);
        high.setShelf (sr, 4000, p.eqHighDb, true);
    }
    inline float processSample (float x)
    {
        x *= dbToGain (p.inputGainDb);
        if (p.hpfOn) x = hpf.process (x);
        if (p.eqOn) { x = low.process (x); x = mid.process (x); x = high.process (x); }
        float ax = std::fabs (x);
        if (p.gateOn)
        {
            gateEnv = std::max (ax, gateEnv * 0.9995f);
            float target = (gateEnv >= dbToGain (p.gateThreshDb)) ? 1.0f : 0.06f;
            gateGain += (target - gateGain) * (target > gateGain ? 0.5f : 0.002f);
            x *= gateGain;
        }
        if (p.compOn)
        {
            compEnv = std::max (ax, compEnv * 0.999f);
            float over = dbToGain (p.compThreshDb);
            if (compEnv > over && compEnv > 1e-6f)
            {
                float excessDb = 20.0f * std::log10 (compEnv / over);
                float reduceDb = excessDb * (1.0f - 1.0f / p.compRatio);
                x *= dbToGain (-reduceDb);
            }
        }
        if (p.limiterOn) x = clampf (x, -0.89f, 0.89f); // -1 dB ceiling, hard clip last-resort
        return x * dbToGain (p.outputGainDb);
    }
    void processBlock (float* buf, int n) { for (int i=0;i<n;++i) buf[i]=processSample(buf[i]); }
};

} // namespace pure
