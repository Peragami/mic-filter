// test_passive_gain.cpp -- host-side tests for the passive ("ながら")
// mic-level controller (client/src/passive_gain.h). Simulates 50 ms UI
// ticks: silence must never raise, quiet speech raises slowly, a clip
// drops the level immediately, healthy speech is left alone.
#include <cmath>
#include <cstdio>

#include "../client/src/passive_gain.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static float lin (float db) { return std::pow (10.0f, db / 20.0f); }

using Action = PassiveGain::Action;

// Feed `seconds` of a constant speech peak (dB), optionally clipping.
static int run (PassiveGain& pg, float peakDb, bool clip, double seconds,
                Action want, float wantDelta)
{
    int acted = 0;
    const int ticks = (int) (seconds / 0.05);
    for (int i = 0; i < ticks; ++i)
    {
        auto d = pg.update (lin (peakDb), clip, 0.05);
        if (d.action != Action::None)
        {
            ++acted;
            CHECK (d.action == want);
            CHECK (std::fabs (d.deltaDb - wantDelta) < 0.01f);
        }
    }
    return acted;
}

int main()
{
    // 1) Silence for a minute: never any action (no raising into hiss).
    {
        PassiveGain pg;
        int acted = 0;
        for (int i = 0; i < 1200; ++i)
            if (pg.update (0.0f, false, 0.05).action != Action::None) ++acted;
        CHECK (acted == 0);
    }
    // 2) Quiet speech (-20 dB) for 60 s: raises happen, but spaced >= 15 s
    //    (so in 60 s at most ~4) and always +1.5 dB.
    {
        PassiveGain pg;
        // First action should come quickly once speech is seen (controller
        // starts "never adjusted"), then the 15 s spacing applies.
        const int acted = run (pg, -20.0f, false, 60.0, Action::Up, +1.5f);
        CHECK (acted >= 2 && acted <= 4);
    }
    // 3) Clipping speech: immediate Down on the very first clipping tick.
    {
        PassiveGain pg;
        auto d = pg.update (lin (-1.0f), true, 0.05);
        CHECK (d.action == Action::Down && std::fabs (d.deltaDb + 3.0f) < 0.01f);
        // Continued clipping: downs repeat, but not faster than 0.5 s
        // (60 s of clipping -> at most 120 downs, at least 60).
        const int acted = run (pg, -1.0f, true, 60.0, Action::Down, -3.0f);
        CHECK (acted >= 60 && acted <= 120);
    }
    // 4) Healthy speech in the -12..-6 window: untouched for 60 s.
    {
        PassiveGain pg;
        CHECK (run (pg, -9.0f, false, 60.0, Action::None, 0.0f) == 0);
    }
    // 5) A single loud shout does not raise or (without clip) lower; and
    //    after a clip-down, no Up is allowed for 15 s even if now quiet.
    {
        PassiveGain pg;
        auto d = pg.update (lin (-0.5f), true, 0.05);
        CHECK (d.action == Action::Down);
        CHECK (run (pg, -20.0f, false, 10.0, Action::None, 0.0f) == 0);
    }
    // 6) Speech that stops: no Up decisions on stale statistics.
    {
        PassiveGain pg;
        pg.reset();
        for (int i = 0; i < 20; ++i) // 1 s of quiet speech (actions ignored here)
            (void) pg.update (lin (-20.0f), false, 0.05);
        int acted = 0;
        for (int i = 0; i < 600; ++i) // then 30 s of silence
            if (pg.update (0.0f, false, 0.05).action != Action::None) ++acted;
        CHECK (acted == 0);
    }

    if (failures == 0) std::printf ("test_passive_gain: ALL PASS\n");
    else std::printf ("test_passive_gain: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
