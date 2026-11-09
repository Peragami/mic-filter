// Test: spectral subtraction (SpectralSub) on synthetic hiss + voice-like signal.
// Build: g++ -std=c++17 -O2 -I../Source/dsp test_spectral.cpp -o test_spectral
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
unsigned int lcg = 12345;
float noiseSample() // deterministic white noise in [-1, 1)
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
} // namespace

int main()
{
    constexpr int sr = 48000;
    constexpr int total = sr * 3;
    static float in[total], out[total];
    // 0.0-1.0s hiss only (learning), 1.0-2.0s voice+hiss, 2.0-3.0s hiss only.
    for (int i = 0; i < total; ++i)
    {
        const double t = (double) i / sr;
        float v = 0.0f;
        if (t >= 1.0 && t < 2.0)
            for (int h = 1; h <= 8; ++h)
                v += (float) (0.10 / h * std::sin (2.0 * 3.141592653589793 * 120.0 * h * t));
        in[i] = v + 0.05f * noiseSample();
    }

    pure2::SpectralSub sub;
    pure2::SpectralParams sp;
    sp.on = true;
    sp.strength = 1.2f;
    sub.setParams (sp);
    for (int i = 0; i < total; ++i) out[i] = sub.process (in[i]);

    // Latency check: first kFft outputs are the priming silence.
    check (std::fabs (out[100]) < 1e-6f, "priming silence during latency window", out[100]);

    // Noise-only tail (2.3-2.9s): hiss should be well down vs the input there.
    const int a = (int) (2.3 * sr), b = (int) (2.9 * sr);
    const double inR = rms (in + a, b - a), outR = rms (out + a, b - a);
    const double reductionDb = 20.0 * std::log10 (inR / std::max (outR, 1e-9));
    check (reductionDb > 8.0, "hiss reduction in noise-only segment (dB)", reductionDb);

    // Voice segment (1.3-1.9s, allow the 512-sample shift): output must keep
    // most of the voice energy (subtraction must not eat the signal).
    const int c = (int) (1.3 * sr), d = (int) (1.9 * sr);
    const double vin = rms (in + c, d - c), vout = rms (out + c, d - c);
    check (vout > 0.55 * vin, "voice energy retained (out/in ratio)", vout / vin);

    std::printf (failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
