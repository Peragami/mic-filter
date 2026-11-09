// test_stage_order.cpp -- ChainV2 stage reordering (client presets).
// Default order must reproduce the pre-refactor chain exactly; a permuted
// order must run, stay finite, keep total latency, and invalid orders
// must be rejected.
#include <cmath>
#include <cstdio>
#include <vector>

#include "../Source/dsp/Effects2.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static std::vector<float> render (pure2::ChainV2& chain, const std::vector<float>& in)
{
    std::vector<float> out = in;
    chain.processBlock (out.data(), (int) out.size());
    return out;
}

int main()
{
    const int N = 48000;
    std::vector<float> in (N);
    for (int n = 0; n < N; ++n)
        in[n] = 0.30f * std::sin (2 * M_PI * 440 * n / 48000.0) +
                0.10f * std::sin (2 * M_PI * 3000 * n / 48000.0) +
                0.05f * std::sin (2 * M_PI * 60 * n / 48000.0);

    pure2::Params2 params; // defaults: most stages on
    params.rnnoise.on = true;
    params.spectral.on = true;
    params.bwe.on = true;

    // 1) Two fresh chains, one explicitly set to the default order:
    //    outputs must be bit-identical.
    pure2::ChainV2 a; a.setParams (params); a.prepare (48000);
    pure2::ChainV2 b; b.setParams (params); b.prepare (48000);
    b.setStageOrder (pure2::ChainV2::kDefaultStageOrder, pure2::ChainV2::STAGE_COUNT);
    const auto outA = render (a, in);
    const auto outB = render (b, in);
    CHECK (outA == outB);
    CHECK (a.latencySamples() == b.latencySamples());

    // 2) Reversed order: runs, finite, different result, same latency.
    int rev[pure2::ChainV2::STAGE_COUNT];
    for (int i = 0; i < pure2::ChainV2::STAGE_COUNT; ++i)
        rev[i] = pure2::ChainV2::kDefaultStageOrder[pure2::ChainV2::STAGE_COUNT - 1 - i];
    pure2::ChainV2 c; c.setParams (params); c.prepare (48000);
    c.setStageOrder (rev, pure2::ChainV2::STAGE_COUNT);
    const auto outC = render (c, in);
    bool finite = true, differs = false;
    for (int n = 0; n < N; ++n)
    {
        if (!std::isfinite (outC[n])) finite = false;
        if (outC[n] != outA[n]) differs = true;
    }
    CHECK (finite);
    CHECK (differs);
    CHECK (c.latencySamples() == a.latencySamples());

    // 3) Invalid orders are ignored (duplicate stage, wrong length).
    pure2::ChainV2 d; d.setParams (params); d.prepare (48000);
    int bad[pure2::ChainV2::STAGE_COUNT];
    for (int i = 0; i < pure2::ChainV2::STAGE_COUNT; ++i) bad[i] = 0; // all duplicates
    d.setStageOrder (bad, pure2::ChainV2::STAGE_COUNT);
    d.setStageOrder (rev, 3); // too short
    bool stillDefault = true;
    for (int i = 0; i < pure2::ChainV2::STAGE_COUNT; ++i)
        if (d.stageOrder[i] != pure2::ChainV2::kDefaultStageOrder[i]) stillDefault = false;
    CHECK (stillDefault);

    if (failures == 0) std::printf ("test_stage_order: ALL PASS\n");
    else std::printf ("test_stage_order: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
