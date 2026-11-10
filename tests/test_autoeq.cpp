// test_autoeq.cpp -- host tests for the AutoEq stage (Effects2.h):
// silence leaves the signal untouched, exaggerated spectral tilts get
// corrected in the right direction, correction stays within its cap.
#include <cmath>
#include <cstdio>
#include <vector>

#include "../Source/dsp/Effects2.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

// Voice-like harmonic stack (f0 = 130 Hz) with slow amplitude wobble,
// scaled to roughly -18 dBFS RMS.
std::vector<float> MakeVoice (double seconds, double sr)
{
    const int n = (int) (seconds * sr);
    std::vector<float> out ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        double v = 0;
        for (int k = 1; k <= 60; ++k)
        {
            const double f = 130.0 * k;
            if (f > 9000.0) break;
            const double amp = std::pow ((double) k, -0.85);
            v += amp * std::sin (2 * 3.141592653589793 * f * t + 0.3 * k);
        }
        const double am = 0.75 + 0.25 * std::sin (2 * 3.141592653589793 * 3.7 * t);
        out[(size_t) i] = (float) (v * am * 0.11);
    }
    return out;
}

void ApplyShelf (std::vector<float>& x, double sr, float freq, float db, bool high)
{
    pure::Biquad bq;
    bq.setShelf (sr, freq, db, high);
    for (auto& s : x) s = bq.process (s);
}

int main()
{
    const double sr = 48000;

    // Silence: no adaptation, signal passes (near-)untouched.
    {
        pure2::AutoEq eq;
        pure2::AutoEqParams prm;
        eq.prepare (sr, prm);
        for (int i = 0; i < 48000; ++i)
        {
            const float y = eq.process (0.0f);
            CHECK (y == 0.0f);
        }
        for (int c = 0; c < 4; ++c) CHECK (eq.currentGainDb (c) == 0.0f);
    }

    // Bright voice (+10 dB high shelf): the high-shelf correction must
    // go clearly negative; amount = 0 must disable correction entirely.
    {
        auto sig = MakeVoice (14.0, sr);
        ApplyShelf (sig, sr, 5000, 10.0f, true);
        pure2::AutoEq eq;
        pure2::AutoEqParams prm;
        eq.prepare (sr, prm);
        for (float s : sig) eq.process (s);
        std::printf ("bright: gains = %.2f %.2f %.2f %.2f dB\n",
                     eq.currentGainDb (0), eq.currentGainDb (1),
                     eq.currentGainDb (2), eq.currentGainDb (3));
        CHECK (eq.currentGainDb (3) < -2.5f);
        CHECK (eq.currentGainDb (0) > -4.0f); // lows not dragged along
        for (int c = 0; c < 4; ++c) CHECK (std::fabs (eq.currentGainDb (c)) <= 5.0f);
    }

    // Boomy voice (+10 dB low shelf): the low-shelf correction must go
    // clearly negative.
    {
        auto sig = MakeVoice (14.0, sr);
        ApplyShelf (sig, sr, 200, 10.0f, false);
        pure2::AutoEq eq;
        pure2::AutoEqParams prm;
        eq.prepare (sr, prm);
        for (float s : sig) eq.process (s);
        std::printf ("boomy:  gains = %.2f %.2f %.2f %.2f dB\n",
                     eq.currentGainDb (0), eq.currentGainDb (1),
                     eq.currentGainDb (2), eq.currentGainDb (3));
        CHECK (eq.currentGainDb (0) < -2.5f);
        for (int c = 0; c < 4; ++c) CHECK (std::fabs (eq.currentGainDb (c)) <= 5.0f);
    }

    // Neutral voice: corrections stay bounded and finite.
    {
        auto sig = MakeVoice (14.0, sr);
        pure2::AutoEq eq;
        pure2::AutoEqParams prm;
        eq.prepare (sr, prm);
        float peak = 0;
        for (float s : sig) peak = std::max (peak, std::fabs (eq.process (s)));
        std::printf ("neutral: gains = %.2f %.2f %.2f %.2f dB, out peak %.3f\n",
                     eq.currentGainDb (0), eq.currentGainDb (1),
                     eq.currentGainDb (2), eq.currentGainDb (3), peak);
        CHECK (std::isfinite (peak) && peak > 0.05f && peak < 0.95f);
        for (int c = 0; c < 4; ++c) CHECK (std::fabs (eq.currentGainDb (c)) <= 5.0f);
    }

    // amount = 0: no correction even for a bright voice.
    {
        auto sig = MakeVoice (8.0, sr);
        ApplyShelf (sig, sr, 5000, 10.0f, true);
        pure2::AutoEq eq;
        pure2::AutoEqParams prm;
        prm.amount = 0.0f;
        eq.prepare (sr, prm);
        for (float s : sig) eq.process (s);
        for (int c = 0; c < 4; ++c) CHECK (eq.currentGainDb (c) == 0.0f);
    }

    // Chain integration: default chain stays finite with autoeq on,
    // and autoeq adds no latency.
    {
        pure2::ChainV2 chain;
        chain.prepare (sr);
        const int lat = chain.latencySamples();
        pure2::Params2 only = chain.p;
        only.declip.on = false; only.declick.on = false; only.duck.on = false;
        only.spectral.on = false; only.rnnoise.on = false; only.bwe.on = false;
        chain.setParams (only);
        CHECK (chain.latencySamples() == 0);
        chain.setParams (chain.p);
        std::printf ("chain latency (defaults) = %d\n", lat);
        auto sig = MakeVoice (2.0, sr);
        bool finite = true;
        for (float s : sig) finite = finite && std::isfinite (chain.processSample (s));
        CHECK (finite);
    }

    if (failures == 0) std::printf ("test_autoeq: ALL PASS\n");
    else std::printf ("test_autoeq: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
