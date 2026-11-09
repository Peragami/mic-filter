// Test: BweStage (lightweight AI bandwidth extension).
// Feeds a band-limited harmonic "voice" (simulating a cheap mic) and
// checks the stage restores HF band energies toward the full-band
// reference, and stays quiet during silence.
// Build: g++ -std=c++17 -O2 -I../Source/dsp test_bwe.cpp -o test_bwe
#include "BweStage.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check (bool ok, const char* name, double value)
{
    std::printf ("[%s] %s (%.3f)\n", ok ? "PASS" : "FAIL", name, value);
    if (!ok) ++failures;
}
double bandRms (const float* b, int n, double sr, float lo, float hi)
{
    pure::Biquad bq;
    const float fc = std::sqrt (lo * hi);
    bq.setBandPass (sr, fc, fc / (hi - lo));
    double s = 0;
    for (int i = 0; i < n; ++i) { const float y = bq.process (b[i]); s += (double) y * y; }
    return std::sqrt (s / n);
}
constexpr int SR = 48000;
} // namespace

int main()
{
    constexpr int N = SR * 3;
    static float full[N], degr[N], out[N];
    // Full-band harmonic voice, f0 gliding 110 -> 160 Hz, HF-rich envelope.
    double phase = 0;
    for (int i = 0; i < N; ++i)
    {
        const double t = (double) i / SR;
        const double f0 = 110 + 50 * t / 3;
        phase += 2 * 3.141592653589793 * f0 / SR;
        const float env = (t > 0.3 && t < 2.7) ? 1.0f : 0.0f;
        float v = 0;
        for (int h = 1; h * f0 < 16000; ++h)
        {
            const double f = h * f0;
            const double a = 0.55 / h * (1.0 + 0.8 * std::exp (-std::pow ((f - 2500) / 900, 2)));
            v += (float) (a * std::sin (phase * h));
        }
        full[i] = env * v * 0.5f;
    }
    // Cheap-mic degradation: cascade of lowpasses (steep rolloff above ~5k).
    {
        pure::Biquad lp1, lp2, lp3, lp4;
        lp1.setLowPass (SR, 5800); lp2.setLowPass (SR, 5800);
        lp3.setLowPass (SR, 6400); lp4.setLowPass (SR, 6400);
        for (int i = 0; i < N; ++i) degr[i] = lp4.process (lp3.process (lp2.process (lp1.process (full[i]))));
    }
    {
        pure2::BweStage st;
        st.prepare (SR, { true, 1.0f });
        for (int i = 0; i < N; ++i) out[i] = st.process (degr[i]);
    }
    const int a = (int) (1.0 * SR), len = (int) (1.5 * SR); // steady voiced region
    const float bands[3][2] = {{7000, 9500}, {9500, 12000}, {12000, 16000}};
    double worstErr = 0;
    for (auto& bd : bands)
    {
        const double ref = bandRms (full + a, len, SR, bd[0], bd[1]);
        const double got = bandRms (out + a + pure2::BweStage::kFrame, len, SR, bd[0], bd[1]);
        const double errDb = std::fabs (20 * std::log10 (got / std::max (ref, 1e-9)));
        worstErr = std::max (worstErr, errDb);
        std::printf ("  band %.0f-%.0f: ref %.4f got %.4f err %.2f dB\n", (double) bd[0], (double) bd[1], ref, got, errDb);
    }
    check (worstErr < 7.0, "HF band energy error vs full-band reference (dB)", worstErr);
    // Silence in -> HF must stay essentially silent out (last 0.2 s tail region
    // was quiet in the source after 2.7 s; check the final 200 ms).
    const double tail = bandRms (out + N - (int) (0.2 * SR), (int) (0.2 * SR), SR, 7000, 16000);
    check (tail < 0.004, "silence stays silent in HF (rms)", tail);
    std::printf (failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
