// Synthetic-signal tests for Effects2.h (+ legacy PureDsp bench reference).
// Build: g++ -O2 -std=c++17 -o /tmp/test_effects2 tests/test_effects2.cpp && /tmp/test_effects2
#include "../Source/dsp/Effects2.h"
#include <cstdio>
#include <vector>
#include <chrono>
#include <complex>

static constexpr double SR = 48000.0, PI = 3.141592653589793;
static int failures = 0;
static void check (bool ok, const char* name)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
static float sineAt (double freq, int n, float amp=1.0f) { return amp * float(std::sin (2*PI*freq*n/SR)); }
// Goertzel amplitude at an exact (coherent) frequency over x[a..b)
static double goertzel (const std::vector<float>& x, int a, int b, double freq)
{
    double w = 2*PI*freq/SR, cw = std::cos(w), coef = 2*cw;
    double s0=0, s1=0, s2=0;
    for (int n=a;n<b;++n){ s0 = x[n] + coef*s1 - s2; s2=s1; s1=s0; }
    double re = s1 - s2*std::cos(w), im = s2*std::sin(w);
    return 2.0/(b-a) * std::sqrt(re*re + im*im);
}
static double rmsRange (const std::vector<float>& x, int a, int b)
{
    double s=0; for (int n=a;n<b;++n) s += double(x[n])*x[n];
    return std::sqrt(s/(b-a));
}

int main()
{
    const int N = int(SR); // 1 s, all test freqs are integer Hz => coherent

    //------------------------------------------------ multiband unity
    std::printf ("== MultibandExpander unity (gains forced to 1) ==\n");
    {
        pure2::MultibandParams mp; mp.on = true;
        for (auto& t: mp.threshDb) t = -120.0f; // expander never engages -> gain 1
        pure2::MultibandExpander mb; mb.prepare (SR, mp);
        double maxErr = 0;
        for (int n=0;n<N;++n)
        {
            float in = 0.4f*sineAt(440,n) + 0.25f*sineAt(3000,n) + 0.15f*sineAt(90,n);
            float out = mb.process (in);
            maxErr = std::max (maxErr, double(std::fabs (out-in)));
        }
        std::printf ("  max |out-in| = %.3g\n", maxErr);
        check (maxErr < 1e-4, "subtractive split sums back to input");
    }

    //------------------------------------------------ notch
    std::printf ("== HumNotch 50 Hz ==\n");
    {
        // 2 s long, measured over 1-2 s: a Q=30 notch at 50 Hz rings with a
        // ~0.19 s time constant, and the first second is still transient.
        std::vector<float> in(2*N), out(2*N);
        for (int n=0;n<2*N;++n) in[n] = 0.5f*sineAt(50,n) + 0.5f*sineAt(1000,n);
        pure2::HumNotchParams hp; hp.baseHz = 50; hp.harmonics = 2;
        pure2::HumNotch hn; hn.prepare (SR, hp);
        for (int n=0;n<2*N;++n) out[n] = hn.process (in[n]);
        int a = N; // steady state
        const int M = 2*N;
        double att50 = 20*std::log10 (goertzel(in,a,M,50)  / goertzel(out,a,M,50));
        double att1k = 20*std::log10 (goertzel(in,a,M,1000)/ goertzel(out,a,M,1000));
        std::printf ("  50 Hz attenuation = %.1f dB, 1 kHz change = %.2f dB\n", att50, att1k);
        check (att50 > 40, "50 Hz strongly attenuated");
        check (std::fabs (att1k) < 1.0, "1 kHz essentially untouched");
    }

    //------------------------------------------------ de-click (+ latency)
    std::printf ("== DeClicker ==\n");
    {
        // latency: smooth ramp (no trigger) must come out shifted by exactly 128
        pure2::DeClicker dc; dc.prepare (pure2::DeClickParams{});
        bool lagOk = true; float firstOut = 1;
        for (int n=0;n<2000;++n)
        {
            float out = dc.process (n * 0.0001f);
            if (n==0) firstOut = out;
            if (n >= 128 && std::fabs (out - (n-128)*0.0001f) > 1e-6f) lagOk = false;
        }
        std::printf ("  latency exact-128: %s (first sample out %.3f)\n", lagOk?"yes":"NO", firstOut);
        check (lagOk && firstOut == 0.0f, "latency is exactly 128 samples");

        // click removal: 1 kHz sine + a 4-sample +0.9 click and a 1-sample -0.8 click
        std::vector<float> clean(N), dirty(N), out(N);
        for (int n=0;n<N;++n){ clean[n]=0.5f*sineAt(1000,n); dirty[n]=clean[n]; }
        for (int k=0;k<4;++k) dirty[5000+k] += 0.9f;
        dirty[9000] -= 0.8f;
        pure2::DeClicker dc2; dc2.prepare (pure2::DeClickParams{});
        for (int n=0;n<N;++n) out[n] = dc2.process (dirty[n]);
        auto winErr = [&](const std::vector<float>& x, int center, int shift){
            double s=0; int c=0;
            for (int n=center-6;n<center+10;++n){ double d = x[n+shift]-clean[n]; s+=d*d; ++c; }
            return std::sqrt(s/c);
        };
        double before = (winErr(dirty,5000,0) + winErr(dirty,9000,0)) / 2;
        double after  = (winErr(out,5000,128) + winErr(out,9000,128)) / 2;
        std::printf ("  click-window RMS error: dirty %.4f -> repaired %.4f (%.1f%% of before)\n",
                     before, after, 100.0*after/before);
        check (after < before * 0.25, "click error reduced by >75%");
    }

    //------------------------------------------------ de-clip
    std::printf ("== DeClipper ==\n");
    {
        auto thd = [&](const std::vector<float>& x, int a, int b){
            double f = goertzel (x,a,b,1000), h=0;
            for (int k=2;k<=7;++k){ double hk = goertzel (x,a,b,1000.0*k); h += hk*hk; }
            return std::sqrt(h)/f;
        };
        std::vector<float> clipped(N), out(N);
        for (int n=0;n<N;++n) clipped[n] = pure::clampf (1.3f*sineAt(1000,n), -1.0f, 1.0f);
        pure2::DeClipper dcl; dcl.prepare (pure2::DeClipParams{});
        for (int n=0;n<N;++n) out[n] = dcl.process (clipped[n]);
        int a = N/4; double tIn = thd (clipped, a, N), tOut = thd (out, a+64, N);
        std::printf ("  THD: clipped %.2f%% -> repaired %.2f%%\n", 100*tIn, 100*tOut);
        check (tOut < tIn * 0.4, "THD improved by >60%");
    }

    //------------------------------------------------ de-esser
    std::printf ("== DeEsser ==\n");
    {
        auto runAmp = [&](double freq){
            pure2::DeEsser de; de.prepare (SR, pure2::DeEsserParams{});
            std::vector<float> out(N);
            for (int n=0;n<N;++n) out[n] = de.process (0.6f*sineAt(freq,n));
            return rmsRange (out, N/2, N) / 0.6 * std::sqrt(2.0); // peak-ish ratio vs input amp
        };
        double r1k = runAmp (1000), r6k = runAmp (6000);
        std::printf ("  steady-state amplitude ratio: 1 kHz %.3f, 6 kHz %.3f (%.1f dB down)\n",
                     r1k, r6k, 20*std::log10 (r1k/r6k));
        check (r1k > 0.94, "1 kHz essentially untouched");
        check (r6k < r1k * 0.7, "6 kHz sibilance band pulled down");
    }

    //------------------------------------------------ exciter
    std::printf ("== Exciter ==\n");
    {
        std::vector<float> in(N), out(N);
        for (int n=0;n<N;++n) in[n] = 0.4f*sineAt(7000,n);
        pure2::ExciterParams ep; ep.mix = 0.6f; ep.drive = 3.0f;
        pure2::Exciter ex; ex.prepare (SR, ep);
        for (int n=0;n<N;++n) out[n] = ex.process (in[n]);
        int a=N/2;
        double hIn  = goertzel (in, a, N, 21000), hOut = goertzel (out, a, N, 21000);
        double fOut = goertzel (out, a, N, 7000);
        std::printf ("  21 kHz (3rd harmonic): in %.5f -> out %.5f (%.1f%% of fundamental)\n",
                     hIn, hOut, 100.0*hOut/fOut);
        check (hOut > 0.01 && hOut > hIn * 10, "new high harmonics generated");
    }

    //------------------------------------------------ chain bench
    std::printf ("== Chain bench (48 kHz, 128-sample blocks) ==\n");
    {
        const int block=128, blocksPerRun=3750, runs=40; // 10 s of audio per run
        std::vector<float> buf(block);
        for (int i=0;i<block;++i) buf[i]=0.3f*float(std::sin(i*0.1));
        pure::Chain legacy; legacy.prepare (SR);
        auto bench = [&](auto& chain){
            auto t0=std::chrono::steady_clock::now(); double sink=0;
            for (int r=0;r<runs;++r) for (int b=0;b<blocksPerRun;++b){ chain.processBlock (buf.data(), block); }
            sink += buf[0];
            double el=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
            std::printf ("    %.2f us/block, CPU %.3f%% of one core (sink %g)\n",
                         el/(runs*blocksPerRun)*1e6, 100.0*el/(runs*10.0), sink);
            return el/(runs*blocksPerRun)*1e6;
        };
        std::printf ("  legacy pure::Chain:\n"); double tLegacy = bench (legacy);
        pure2::ChainV2 v2; v2.prepare (SR);
        std::printf ("  pure2::ChainV2 (all effects on, latency %d samples):\n", v2.latencySamples());
        double tV2 = bench (v2);
        std::printf ("  ratio vs legacy: %.1fx\n", tV2/tLegacy);
        check (tV2 < 200, "ChainV2 still far inside the 2667 us block budget");
    }

    std::printf ("%s (%d failures)\n", failures==0 ? "ALL PASS" : "FAILURES PRESENT", failures);
    return failures==0 ? 0 : 1;
}
