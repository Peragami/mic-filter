// Test: TransientDuck (click suppressor) + gate breath mode.
// Build: g++ -std=c++17 -O2 -I../Source/dsp test_duck.cpp -o test_duck
#include "Effects2.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check (bool ok, const char* name, double value)
{
    std::printf ("[%s] %s (%.3f)\n", ok ? "PASS" : "FAIL", name, value);
    if (!ok) ++failures;
}
unsigned int lcg = 999;
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
double peak (const float* b, int n)
{
    double m = 0;
    for (int i = 0; i < n; ++i) m = std::max (m, (double) std::fabs (b[i]));
    return m;
}
constexpr int SR = 48000;
} // namespace

int main()
{
    // ---- 1) Isolated click between silences: must be ducked hard.
    {
        constexpr int N = SR;
        static float in[N], out[N];
        for (int i = 0; i < N; ++i) in[i] = 0.004f * frand(); // quiet room tone
        const int clickAt = SR / 2;
        for (int i = 0; i < 400; ++i) // 8 kHz burst, exp decay (~mouse click)
            in[clickAt + i] += 0.7f * std::exp (-(float) i / 60.0f) *
                               (float) std::sin (2.0 * 3.141592653589793 * 8000.0 * i / SR);
        pure2::TransientDuck duck;
        duck.prepare (SR, pure2::TransientDuckParams{});
        for (int i = 0; i < N; ++i) out[i] = duck.process (in[i]);
        const int lat = pure2::TransientDuck::kLookahead;
        const double inPk = peak (in + clickAt, 300);
        const double outPk = peak (out + clickAt + lat, 300);
        const double dropDb = 20.0 * std::log10 (inPk / std::max (outPk, 1e-9));
        check (dropDb > 8.0, "isolated click peak reduction (dB)", dropDb);
    }
    // ---- 2) Voice-like burst with a natural onset: must mostly survive.
    {
        constexpr int N = SR;
        static float in[N], out[N];
        for (int i = 0; i < N; ++i)
        {
            const int t0 = SR / 4, t1 = SR * 3 / 4;
            float v = 0.0f;
            if (i >= t0 && i < t1)
            {
                const float env = std::min (1.0f, (float) (i - t0) / (0.010f * SR));
                for (int h = 1; h <= 6; ++h)
                    v += env * (float) (0.09 / h * std::sin (2.0 * 3.141592653589793 * 130.0 * h * i / SR));
            }
            in[i] = v + 0.004f * frand();
        }
        pure2::TransientDuck duck;
        duck.prepare (SR, pure2::TransientDuckParams{});
        for (int i = 0; i < N; ++i) out[i] = duck.process (in[i]);
        const int lat = pure2::TransientDuck::kLookahead;
        const int a = SR / 4 + 2000, b = SR * 3 / 4 - 2000; // steady middle of the burst
        const double ratio = rms (out + a + lat, b - a) / rms (in + a, b - a);
        check (ratio > 0.75, "voice burst retained (out/in)", ratio);
    }
    // ---- 3) Breath mode: quiet sustained noise (breath-like) survives the
    //         ChainV2 gate much better with breath mode ON than OFF.
    {
        auto runChain = [] (bool breathOn) {
            constexpr int N = SR * 2;
            static float buf[N];
            unsigned int l2 = 4242;
            float smooth = 0.0f;
            for (int i = 0; i < N; ++i)
            {
                l2 = l2 * 1664525u + 1013904223u;
                const float n = (float) ((l2 >> 9) & 0xFFFF) / 32768.0f - 1.0f;
                smooth += (n - smooth) * 0.03f; // smoothed noise: steadier envelope, like breath
                const double t = (double) i / SR;
                const float swell = (t > 1.0 && t < 1.6) ? 0.16f : 0.0f;
                buf[i] = swell * smooth; // ~ -31 dB RMS, peaks ~ -24 dB
            }
            pure2::ChainV2 chain;
            chain.prepare (SR);
            pure2::Params2 pp;
            pp.base.gateOn = true;
            pp.base.gateThreshDb = -22.0f; // breath peaks sit below this: OFF gates it out
            pp.breath.on = breathOn;
            pp.declip.on = false; pp.declick.on = false; pp.duck.on = false;
            pp.hum.on = false; pp.multiband.on = false; pp.deesser.on = false; pp.exciter.on = false;
            chain.setParams (pp);
            chain.processBlock (buf, N);
            return rms (buf + (int) (1.2 * SR), (int) (0.3 * SR));
        };
        const double off = runChain (false), on = runChain (true);
        check (on > off * 1.8, "breath-mode gate retention (on/off ratio)", on / std::max (off, 1e-9));
    }

    std::printf (failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
