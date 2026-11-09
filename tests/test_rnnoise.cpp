// Test: RNNoise stage on synthetic hiss + fan noise + voice-like signal,
// compared against the non-AI spectral stage on the same material.
// Build (from tests/):
//   gcc -O2 -I../thirdparty/rnnoise-build -I../thirdparty/rnnoise/include -I../thirdparty/rnnoise/src \
//       -c ../thirdparty/rnnoise/src/{denoise,rnn,pitch,kiss_fft,celt_lpc,nnet,nnet_default,parse_lpcnet_weights,rnnoise_data,rnnoise_tables}.c
//   g++ -std=c++17 -O2 -I../Source/dsp -I../thirdparty/rnnoise/include test_rnnoise.cpp *.o -o test_rnnoise
#include "RnnoiseStage.h"
#include "SpectralSub.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check (bool ok, const char* name, double value)
{
    std::printf ("[%s] %s (%.3f)\n", ok ? "PASS" : "FAIL", name, value);
    if (!ok) ++failures;
}
unsigned int lcg = 777;
float frand()
{
    lcg = lcg * 1664525u + 1013904223u;
    return (float) ((lcg >> 9) & 0xFFFF) / 32768.0f - 1.0f;
}
double rms (const float* b, int n)
{
    double s = 0;
    for (int i = 0; i < n; ++i) s += (double) b[i] * b[i];
    return std::sqrt (s / n);
}
constexpr int SR = 48000;
constexpr int TOTAL = SR * 4;
float in[TOTAL];
void buildSignal()
{
    float fanLp = 0;
    for (int i = 0; i < TOTAL; ++i)
    {
        const double t = (double) i / SR;
        fanLp += (frand() - fanLp) * 0.05f;                    // fan-like lowpassed noise
        float v = 0.0f;
        if (t >= 1.5 && t < 3.0)                               // voice-like harmonic bursts
            for (int h = 1; h <= 8; ++h)
                v += (float) (0.08 / h * std::sin (2.0 * 3.141592653589793 * 120.0 * h * t));
        in[i] = v + 0.030f * frand() + 2.2f * fanLp * 0.05f;   // hiss + fan
    }
}
} // namespace

int main()
{
    buildSignal();
    static float outR[TOTAL], outS[TOTAL];
    {
        pure2::RnnoiseStage st;
        st.setParams ({ true });
        for (int i = 0; i < TOTAL; ++i) outR[i] = st.process (in[i]);
        st.release();
    }
    {
        pure2::SpectralSub sp;
        sp.setParams ({ true, 1.2f, 0.08f });
        for (int i = 0; i < TOTAL; ++i) outS[i] = sp.process (in[i]);
    }
    const int a = (int) (3.3 * SR), b = (int) (3.9 * SR); // noise-only tail
    const double inR = rms (in + a, b - a);
    const double redR = 20.0 * std::log10 (inR / std::max (rms (outR + a, b - a), 1e-9));
    const double redS = 20.0 * std::log10 (inR / std::max (rms (outS + a, b - a), 1e-9));
    check (redR > 10.0, "RNNoise noise-only reduction (dB)", redR);
    std::printf ("       (reference: spectral subtraction reduction %.2f dB)\n", redS);

    const int c = (int) (2.0 * SR), d = (int) (2.8 * SR); // voice segment
    const double vin = rms (in + c, d - c);
    const double ratio = rms (outR + c, d - c) / vin;
    check (ratio > 0.45, "RNNoise voice retained (out/in)", ratio);

    check (std::fabs (outR[100]) < 1e-6f, "priming silence (480 latency)", outR[100]);
    std::printf (failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
